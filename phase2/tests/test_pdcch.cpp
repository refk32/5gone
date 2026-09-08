// Step 7 test: nr_pdcch — PDCCH DM-RS correlation finds a RAR DCI candidate.
//
//   g++ -std=c++17 -I ../include test_pdcch.cpp ../src/nr_pdcch.cpp ../src/nr_dci.cpp \
//       ../src/nr_dmrs.cpp ../src/nr_pn.cpp ../src/nr_dsp.cpp ../src/nr_symbol.cpp \
//       -o test_pdcch && ./test_pdcch

#include "5gone/nr_pdcch.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_dmrs.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace gone::nr;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    // Configure a CORESET over the full 51-RB BWP, 1 OFDM symbol, CSS,
    // DM-RS scrambling id = cell id = 1 (matches the lab gNB).
    Coreset cs;
    cs.frequency_domain_resources = bwp_num_prbs;            // 51 PRBs
    cs.duration = 1;
    cs.cell_id = 1;
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = symbols_per_slot;              // 14
    cs.num_slots_per_frame = slots_per_frame;                // 20
    cs.candidates_search_space = {1, 2, 4, 8, 16};           // CSS candidates per AL

    Pdcch pdcch;
    pdcch.set_coreset_info(cs);
    pdcch.scrambling_id_start = 1;
    pdcch.scrambling_id_end   = 1;          // only search PCI 1
    pdcch.rnti_start = ra_rnti_min;         // 1
    pdcch.rnti_end   = ra_rnti_max;         // 71
    pdcch.dci_sizes_list = {39};            // DCI 1_0 @ 51 RB
    pdcch.AL_corr_thresholds = {0.9f, 0.8f, 0.7f, 0.5f, 0.5f};
    pdcch.initialize_dmrs_seq();

    // Pick a specific candidate to "transmit" on: AL=2, candidate 0, slot 0.
    const uint8_t al = 2;
    const uint8_t cand = 0;
    const uint8_t num_cand = cs.candidates_search_space[1]; // log2(2)=1
    const uint8_t slot = 0;
    const uint8_t n_ofdm = 0;

    // Query the exact DM-RS reference for this candidate.
    auto dmrs_sc = pdcch.get_dmrs_sc_indices(al, cand, num_cand, slot, false);
    auto dmrs_sym = pdcch.get_dmrs_symbols(al, cand, num_cand, slot, n_ofdm);
    printf("AL=%u cand=%u: %zu DMRS subcarriers, %zu reference symbols\n",
           (unsigned)al, (unsigned)cand, dmrs_sc.size(), dmrs_sym.size());
    CHECK(!dmrs_sc.empty() && dmrs_sc.size() == dmrs_sym.size(), "dmrs indices match symbols");

    // Build a received Symbol with noise + the DM-RS placed at its locations,
    // run through a channel (scale + rotation).
    Symbol sym;
    sym.samples.assign(bwp_num_prbs * PRB_RE, {0.f, 0.f});   // 612 subcarriers
    sym.slot_index = slot;
    sym.symbol_index = n_ofdm;

    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0.f, 0.05f);
    for (auto& v : sym.samples) v = {noise(rng), noise(rng)};   // low noise

    const std::complex<float> H(0.8f, 0.6f);                    // channel
    for (size_t i = 0; i < dmrs_sc.size(); ++i)
        sym.samples[dmrs_sc[i]] = dmrs_sym[i] * H;

    // Run the decoder.
    std::vector<Symbol> syms = {sym};
    auto found = pdcch.process(syms, 0);

    printf("found %zu candidate DCIs via DM-RS correlation\n", found.size());
    bool matched = false;
    for (auto& d : found) {
        printf("  AL=%u candidate=%u corr=%.3f scrambling_id=%u slot=%u sym=%u\n",
               (unsigned)d.found_aggregation_level, (unsigned)d.found_candidate,
               d.correlation, (unsigned)d.pdcch_scrambling_id,
               (unsigned)d.n_slot, (unsigned)d.n_ofdm);
        if (d.found_aggregation_level == al && d.found_candidate == cand &&
            d.pdcch_scrambling_id == 1) matched = true;
    }
    CHECK(matched, "the transmitted AL=2 candidate was detected");

    // Sanity: a symbol with NO DM-RS must not produce a match (all noise).
    Symbol empty;
    empty.samples.assign(bwp_num_prbs * PRB_RE, {0.f, 0.f});
    empty.slot_index = slot;
    empty.symbol_index = n_ofdm;
    for (auto& v : empty.samples) v = std::complex<float>(noise(rng), noise(rng));
    std::vector<Symbol> empty_vec = {empty};
    auto none = pdcch.process(empty_vec, 0);
    printf("noise-only symbol found %zu candidates (want 0)\n", none.size());
    CHECK(none.empty(), "all-noise symbol yields no candidate");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
