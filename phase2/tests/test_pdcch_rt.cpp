// test_pdcch_rt.cpp — PDCCH polar round trip (REQUIRES srsRAN-4G).
//
// Encodes random DCI payloads with srsRAN's own TX (reference implementation),
// lays the symbols onto OUR candidate RE layout, and runs OUR full
// Pdcch::process() (DMRS channel est + LLRs + polar + CRC + RNTI search).
// Proves the decode glue correct on clean data — the one path no test has
// ever exercised (every OTA "CRC fail" so far blames the signal; an untested
// decoder is an equally good explanation).
//
//   - AL1 (the level every real capture hits): MUST come back with crc_ok,
//     exact payload and rnti == 267. Pre-fix this failed: the keep-gate
//     dropped all AL-1 successes (`> 1`), returning correlation shells.
//   - AL2 control: passes pre- and post-fix.
//   - Noise control: no crc_ok on garbage.
//
// Build ONLY with GONE_HAVE_SRSRAN_OLD=1 + srsRAN includes/libs (CMake gates
// on TARGET test_rar, the reliable srsRAN-presence signal; skipped elsewhere
// like test_rar).

#include "5gone/nr_constants.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_dci.hpp"
#include "5gone/nr_pdcch.hpp"

#include <algorithm>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#ifndef GONE_HAVE_SRSRAN_OLD
#error "test_pdcch_rt requires GONE_HAVE_SRSRAN_OLD (srsRAN-4G present)"
#endif

#include <srsran/phy/phch/pdcch_nr.h>

namespace {

using gone::nr::bwp_num_prbs;

int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

// Deterministic bits (own LCG: zero srsRAN dependency for stimulus).
static uint32_t lcg = 0xA53A5A35u;
static uint8_t rand_bit()
{
    lcg = lcg * 1664525u + 1013904223u;
    return (uint8_t)((lcg >> 16) & 1u);
}

gone::nr::Coreset default_coreset(uint16_t pci)
{
    gone::nr::Coreset cs;
    cs.frequency_domain_resources = bwp_num_prbs;
    cs.duration = 1;
    cs.cell_id = pci;
    cs.cce_reg_mapping_type = "non-interleaved";
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = 14;
    cs.num_slots_per_frame = 20;
    cs.candidates_search_space = {1, 2, 4, 8, 16};
    return cs;
}

// Round trip one aggregation level: srsRAN-TX encodes `bits` under `rnti`,
// symbols land on OUR data REs (+ OUR DMRS, perfect channel), OUR process()
// must recover bits + rnti. Returns true on full match.
bool round_trip(uint8_t al, uint16_t rnti, const std::vector<uint8_t>& bits)
{
    const uint16_t pci = 1;
    const uint32_t L = (al == 1) ? 0u : 1u;   // srsRAN log-AL
    const size_t n_data = (size_t)al * 6u * 9u; // data REs per candidate

    // --- srsRAN TX side (reference encoder) ---
    srsran_pdcch_nr_args_t args = {};
    args.disable_simd = false;
    args.measure_evm = false;
    args.measure_time = false;
    srsran_pdcch_nr_t tx = {};
    if (srsran_pdcch_nr_init_tx(&tx, &args) != SRSRAN_SUCCESS) {
        printf("  tx init failed\n");
        return false;
    }
    srsran_carrier_nr_t carrier = SRSRAN_DEFAULT_CARRIER_NR;
    carrier.nof_prb = bwp_num_prbs;
    carrier.pci = pci;
    srsran_coreset_t coreset = {};
    coreset.mapping_type = srsran_coreset_mapping_type_non_interleaved;
    coreset.duration = 1;
    coreset.precoder_granularity = srsran_coreset_precoder_granularity_reg_bundle;
    for (uint32_t i = 0; i < 9; ++i) coreset.freq_resources[i] = true;  // 54 REGs
    coreset.shift_index = 0;
    if (srsran_pdcch_nr_set_carrier(&tx, &carrier, &coreset) != SRSRAN_SUCCESS) {
        printf("  tx set_carrier failed\n");
        srsran_pdcch_nr_free(&tx);
        return false;
    }
    srsran_dci_msg_nr_t msg = {};
    msg.ctx.format = srsran_dci_format_nr_1_0;
    msg.ctx.rnti_type = srsran_rnti_type_ra;
    msg.ctx.ss_type = srsran_search_space_type_common_1;  // ra-SearchSpace -> CSS
    msg.ctx.location.L = L;
    msg.ctx.location.ncce = 0;
    msg.ctx.coreset_id = 1;
    msg.ctx.coreset_start_rb = 0;
    msg.ctx.rnti = rnti;
    for (size_t i = 0; i < bits.size() && i < 50; ++i) msg.payload[i] = bits[i];
    msg.nof_bits = (uint32_t)bits.size();
    // Scratch grid for the encoder (mapping bypassed: only q.symbols used).
    // Oversized deliberately: mapping writes stay inside srsRAN's layout.
    const size_t grid_sz = 64u * 12u * 14u;
    cf_t* grid = new (std::nothrow) cf_t[grid_sz]();
    if (!grid) {
        srsran_pdcch_nr_free(&tx);
        return false;
    }
    if (srsran_pdcch_nr_encode(&tx, &msg, grid) != SRSRAN_SUCCESS) {
        printf("  tx encode failed\n");
        delete[] grid;
        srsran_pdcch_nr_free(&tx);
        return false;
    }
    // Flat data-symbol sequence out (E/2 symbols for this AL).
    const size_t n_sym = (size_t)(1u << L) * 9u * 6u;
    std::vector<std::complex<float>> tx_syms;
    tx_syms.reserve(n_sym);
    for (size_t i = 0; i < n_sym; ++i) {
        // cf_t is C99 _Complex float: contiguous [real, imag] by definition.
        const float* p = reinterpret_cast<const float*>(&tx.symbols[i]);
        tx_syms.push_back(std::complex<float>(p[0], p[1]));
    }
    srsran_pdcch_nr_free(&tx);
    delete[] grid;

    // --- OUR side: candidate RE layout + full process() ---
    gone::nr::Pdcch pdcch;
    pdcch.set_coreset_info(default_coreset(pci));
    pdcch.scrambling_id_start = pci;
    pdcch.scrambling_id_end = pci;
    pdcch.set_decode_enabled(true);
    pdcch.dci_sizes_list = {(uint8_t)bits.size()};
    pdcch.initialize_dmrs_seq();

    const uint8_t num_cands = (al == 1) ? 1u : 2u;
    auto dmrs_sc = pdcch.get_dmrs_sc_indices(al, 0, num_cands, 0, false);
    auto dmrs_sym = pdcch.get_dmrs_symbols(al, 0, num_cands, 0, 0);
    auto data_sc = pdcch.get_data_sc_indices(al, 0, num_cands, 0, false);
    if (dmrs_sc.size() != dmrs_sym.size() || data_sc.size() != tx_syms.size()) {
        printf("  RE count mismatch: dmrs %zu/%zu data %zu vs tx %zu\n",
               dmrs_sc.size(), dmrs_sym.size(), data_sc.size(), tx_syms.size());
        return false;
    }
    gone::nr::Symbol sym;
    sym.samples.assign(bwp_num_prbs * 12u, std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < dmrs_sc.size(); ++i) sym.samples[dmrs_sc[i]] = dmrs_sym[i];
    for (size_t i = 0; i < data_sc.size(); ++i) sym.samples[data_sc[i]] = tx_syms[i];
    sym.slot_index = 0;
    sym.symbol_index = 0;
    std::vector<gone::nr::Symbol> syms{sym};
    auto found = pdcch.process(syms, 0);

    bool match = false;
    size_t crc_ok_count = 0;
    float best_corr = 0.0f;
    for (const auto& d : found) {
        best_corr = std::max(best_corr, d.correlation);
        if (!d.crc_ok) continue;
        ++crc_ok_count;
        if (d.rnti == rnti && d.found_aggregation_level == al &&
            d.payload.size() == bits.size()) {
            bool same = true;
            for (size_t i = 0; i < bits.size(); ++i)
                if (!!d.payload[i] != !!bits[i]) { same = false; break; }
            if (same) match = true;
        }
    }
    printf("  AL=%u rnti=%u: found=%zu best_corr=%.3f crc_ok=%zu match=%d\n",
           (unsigned)al, (unsigned)rnti, found.size(), best_corr,
           crc_ok_count, (int)match);
    return match;
}

} // namespace

int main()
{
    std::vector<uint8_t> bits39;
    for (int i = 0; i < 39; ++i) bits39.push_back(rand_bit());

    CHECK(round_trip(1, 267, bits39), "AL1 round trip: bits + RNTI 267 recovered");
    CHECK(round_trip(2, 267, bits39), "AL2 round trip: bits + RNTI 267 recovered");

    // Noise control: garbage symbols must never CRC-pass.
    {
        gone::nr::Pdcch pdcch;
        pdcch.set_coreset_info(default_coreset(1));
        pdcch.scrambling_id_start = 1;
        pdcch.scrambling_id_end = 1;
        pdcch.set_decode_enabled(true);
        pdcch.dci_sizes_list = {39};
        pdcch.initialize_dmrs_seq();
        gone::nr::Symbol sym;
        sym.samples.assign(bwp_num_prbs * 12u, std::complex<float>(0.0f, 0.0f));
        for (size_t i = 0; i < sym.samples.size(); ++i) {
            const float v = ((float)(i % 7) - 3.0f) / 3.0f;
            sym.samples[i] = std::complex<float>(v, -v);
        }
        sym.slot_index = 0;
        sym.symbol_index = 0;
        std::vector<gone::nr::Symbol> syms{sym};
        auto found = pdcch.process(syms, 0);
        size_t crc_ok_count = 0;
        for (const auto& d : found) crc_ok_count += d.crc_ok ? 1 : 0;
        CHECK(crc_ok_count == 0, "noise control: no CRC pass on garbage");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
