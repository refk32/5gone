// test_coreset_sweep.cpp — prove the RX side is blind to a real gNB-like
// CORESET #0 and that the config sweep (5gone-decode --coreset-sweep) recovers
// it.
//
// The decode chain defaults to a CORESET = full BWP (51 PRB), 1 symbol,
// anchored at PRB 0, non-interleaved. A real gNB CORESET #0 (from
// pdcchConfigSIB1) is usually narrower, offset from PRB 0, and interleaved
// with n_shift = PCI mod 3. This test:
//   1. synthesizes a RAR slot whose PDCCH DM-RS uses such a CORESET
//      (48 PRB @ PRB 2, interleaved, shift 1 — PCI 1);
//   2. shows the DEFAULT decoder finds nothing (the R1 blind spot);
//   3. runs the same (config x slot-offset) sweep the tool uses and shows it
//      recovers the true CORESET (corr ~1.0), plus a 2-symbol CORESET case;
//   4. decodes the RAR PDCCH with the recovered config.
//
//   g++ -std=c++17 -I ../include test_coreset_sweep.cpp ../src/nr_rar_tx.cpp \
//       ../src/nr_rar_decoder.cpp ../src/nr_pdcch.cpp ../src/nr_dci.cpp \
//       ../src/nr_dmrs.cpp ../src/nr_pn.cpp ../src/nr_dsp.cpp ../src/nr_symbol.cpp \
//       ../src/nr_ofdm.cpp ../src/nr_pdsch.cpp ../src/mac_rar.cpp ../src/nr_pss.cpp \
//       -o test_coreset_sweep

#include "5gone/nr_constants.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_dci.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_rar_tx.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace gone::nr;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

namespace {

Coreset make_coreset(uint16_t freq, uint16_t start_prb, uint8_t dur,
                     bool interleaved, uint16_t shift, uint16_t pci)
{
    Coreset cs;
    cs.control_resourceset_id = 1;
    cs.frequency_domain_resources = freq;
    cs.start_prb = start_prb;
    cs.duration = dur;
    cs.cce_reg_mapping_type = interleaved ? "interleaved" : "non-interleaved";
    cs.reg_bundlesize = 6;
    cs.interleaver_size = 2;
    cs.shift_index = shift;
    cs.cell_id = pci;
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = 14;
    cs.num_slots_per_frame = 20;
    cs.candidates_search_space = {1, 2, 4, 8, 16};
    return cs;
}

// The sweep used by 5gone-decode --coreset-sweep, reduced to the lemma grid
// this test cares about. Returns the config x-offset pair with the highest
// DM-RS correlation.
struct SweepResult {
    double corr = 0.0;
    Coreset coreset;
    int offset = 0;
    int al = 0;
    int slot = 0;
    int symbol = 0;
};

SweepResult run_sweep(const std::vector<Symbol>& symbols, uint16_t pci)
{
    SweepResult best;
    RarDecoder dec(23.04e6, 30000, pci, 51, /*verbose=*/false);

    // Grid: mapping {non, int} x shift {0,1,2} x freq {48,51} x start_prb {0,2,3} x dur {1,2}
    std::vector<Coreset> grid;
    for (bool interleaved : {false, true})
      for (uint16_t shift = 0; shift < 3; ++shift)
        for (uint16_t freq : {48u, 51u})
          for (uint16_t off : {0u, 2u, 3u})
            if (off + freq <= 51)
              for (uint8_t dur : {1, 2})
                grid.push_back(make_coreset(freq, off, dur, interleaved, shift, pci));

    std::vector<uint8_t> orig(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) orig[i] = symbols[i].slot_index;

    for (const Coreset& cs : grid) {
        dec.set_coreset(cs);
        for (int off20 = 0; off20 < 20; ++off20) {
            std::vector<Symbol> syms = symbols;           // shallow copies (shared samples)
            for (size_t i = 0; i < syms.size(); ++i)
                syms[i].slot_index = static_cast<uint8_t>((orig[i] + off20) % 20);
            auto found = dec.scan_pdcch(syms);
            for (const auto& d : found) {
                if (d.correlation > best.corr) {
                    best.corr = d.correlation;
                    best.coreset = cs;
                    best.offset = off20;
                    best.al = d.found_aggregation_level;
                    best.slot = d.n_slot;
                    best.symbol = d.n_ofdm;
                }
            }
        }
    }
    return best;
}

// Largest DM-RS correlation of the given config at the given slot-offset.
double corr_of(const std::vector<Symbol>& symbols, const Coreset& cs,
               uint16_t pci, int offset)
{
    RarDecoder dec(23.04e6, 30000, pci, 51, /*verbose=*/false);
    dec.set_coreset(cs);
    std::vector<Symbol> syms = symbols;
    for (size_t i = 0; i < syms.size(); ++i)
        syms[i].slot_index = static_cast<uint8_t>((syms[i].slot_index + offset) % 20);
    auto found = dec.scan_pdcch(syms);
    double m = 0.0;
    for (const auto& d : found) m = std::max<double>(m, d.correlation);
    return m;
}

} // namespace

int main()
{
    const uint16_t pci = 1;    // => CORESET0 n_shift = PCI mod 3 = 1
    Ofdm ofdm(23.04e6, 30000, bwp_num_prbs);

    // --- Case A: 48-PRB, offset 2, interleaved, shift 1 (the likely lab CORE0) ---
    const Coreset true_cs = make_coreset(48, 2, 1, /*interleaved=*/true, 1, pci);
    RarSlotTx tx_a = build_rar_slot(ofdm, pci, bwp_num_prbs, 0.5f, true_cs);
    auto syms_a = ofdm.demodulate(tx_a.legit, 0);
    printf("case A: tx slot demodulated to %zu symbols\n", syms_a.size());

    // 1. Default (blind) rewrite: must find nothing.
    {
        RarDecoder dec_default(23.04e6, 30000, pci, bwp_num_prbs, /*verbose=*/false);
        auto obs = dec_default.decode(tx_a.legit, 0);
        printf("case A: default CORESET decode found %zu obs (want 0 — the R1 bug)\n", obs.size());
        CHECK(obs.empty(), "default full-BWP CORESET must MISS the offset/interleaved CORESET");
    }

    // 2. Sweep recovers the true config.
    {
        auto best = run_sweep(syms_a, pci);
        printf("case A: sweep best corr=%.4f offset=%d AL=%d slot=%d sym=%d "
               "PRBs=%u@%u dur=%u shift=%u %s\n",
               best.corr, best.offset, best.al, best.slot, best.symbol,
               (unsigned)best.coreset.frequency_domain_resources, (unsigned)best.coreset.start_prb,
               (unsigned)best.coreset.duration, (unsigned)best.coreset.shift_index,
               best.coreset.cce_reg_mapping_type.c_str());
        CHECK(best.corr > 0.95, "sweep must find the transmitted CORESET with corr ~1.0");
        // A full-CORESET transmission occupies every REG bundle, so DM-RS
        // positions are unchanged across shift_index and mapping variants --
        // correlation cannot (and need not) resolve those two dimensions. It
        // MUST resolve the size/location (the thing that blinds the decoder).
        CHECK(best.coreset.frequency_domain_resources == 48, "best PRB count == 48");
        CHECK(best.coreset.start_prb == 2, "best start PRB == 2");
    }

    // 3. Default config must correlate much worse than the true one.
    {
        const Coreset default_cs = make_coreset(bwp_num_prbs, 0, 1, false, 0, pci);
        const double c_default = corr_of(syms_a, default_cs, pci, 0);
        const double c_true = corr_of(syms_a, true_cs, pci, 0);
        printf("case A: corr(default)=%.4f corr(true)=%.4f\n", c_default, c_true);
        CHECK(c_default < 0.5 && c_true > 0.9, "default config much weaker than true config");
    }

    // 4. Decode with the recovered config -> the RAR PDCCH observation appears.
    {
        RarDecoder dec(23.04e6, 30000, pci, bwp_num_prbs, /*verbose=*/false);
        dec.set_coreset(true_cs);
        auto obs = dec.decode(tx_a.legit, 0);
        printf("case A: decode (recovered CORESET) -> %zu RAR observation(s)\n", obs.size());
        CHECK(!obs.empty(), "with the right CORESET the RAR PDCCH is found");
    }

    // --- Case B: 2-symbol CORESET (duration slicing path) ---
    const Coreset dur2_cs = make_coreset(48, 2, 2, /*interleaved=*/false, 0, pci);
    RarSlotTx tx_b = build_rar_slot(ofdm, pci, bwp_num_prbs, 0.5f, dur2_cs);
    auto syms_b = ofdm.demodulate(tx_b.legit, 0);
    {
        auto best = run_sweep(syms_b, pci);
        const double c_dur2 = corr_of(syms_b, dur2_cs, pci, 0);
        const Coreset default_cs = make_coreset(bwp_num_prbs, 0, 1, false, 0, pci);
        const double c_default = corr_of(syms_b, default_cs, pci, 0);
        printf("case B: sweep best corr=%.4f PRBs=%u@%u dur=%u %s | corr(dur2)=%.4f corr(default)=%.4f\n",
               best.corr, (unsigned)best.coreset.frequency_domain_resources,
               (unsigned)best.coreset.start_prb, (unsigned)best.coreset.duration,
               best.coreset.cce_reg_mapping_type.c_str(), c_dur2, c_default);
        CHECK(c_dur2 > 0.95 && c_default < 0.5, "2-symbol CORESET decoded via duration slicing");
        CHECK(best.corr > 0.95, "sweep sees the 2-symbol CORESET");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}