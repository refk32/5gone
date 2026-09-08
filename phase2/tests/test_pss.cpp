// Step 1 test: NR PSS/SSS generation + detection (nr_pss + nr_ofdm).
//
// Build WITHOUT liquid (Mac / fallback path):
//   g++ -std=c++17 -I ../include test_pss.cpp ../src/nr_pss.cpp ../src/nr_ofdm.cpp -o test_pss
//
// Build WITH liquid-dsp (your Linux box, the real fast path):
//   g++ -std=c++17 -DHAVE_LIQUID -I ../include ../src/nr_ofdm.cpp ../src/nr_pss.cpp \
//       test_pss.cpp -lliquid -o test_pss
//
// Run: ./test_pss     (return code 0 = all checks passed)

#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace gone::nr;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    // ---- 1) Sequence generation sanity ----
    auto p0 = nr_pss_sequence(0);
    auto p1 = nr_pss_sequence(1);
    CHECK(p0.size() == 127, "PSS is 127 long");
    bool all_unit = true;
    for (float v : p1) if (std::fabs(std::fabs(v) - 1.0f) > 1e-6f) all_unit = false;
    CHECK(all_unit, "PSS values are +-1");
    bool differ = false;
    for (int i = 0; i < 127; ++i) if (p0[i] != p1[i]) differ = true;
    CHECK(differ, "PSS differs between N_ID2 0 and 1");

    auto sss = nr_sss_sequence(0, 1);   // PCI 1 -> N_ID1 0, N_ID2 1
    CHECK(sss.size() == 127, "SSS is 127 long");
    all_unit = true;
    for (float v : sss) if (std::fabs(std::fabs(v) - 1.0f) > 1e-6f) all_unit = false;
    CHECK(all_unit, "SSS values are +-1");

    // ---- 2) Frequency-domain detection after a full OFDM round trip ----
    // Lab numerology. Build a 14-symbol slot, put SSS on slot symbol 2 and PSS
    // on slot symbol 4 (SSB block at slot symbols {2,3,4,5}, k_ssb=0).
    Ofdm ofdm(23.04e6, 30000, 51);
    CHECK(ofdm.fft_size() == 768, "fft_size 768");
    CHECK(ofdm.num_subcarriers() == 612, "612 active subcarriers");

    std::vector<Symbol> slot(14);
    for (auto& s : slot) s.samples.assign(612, {0, 0});
    place_sss_in_symbol(slot[kSssSlotSymbol].samples, 0, 1);
    place_pss_in_symbol(slot[kPssSlotSymbol].samples, 1);

    auto iq   = ofdm.modulate(slot);
    auto rx   = ofdm.demodulate(iq);
    CHECK(rx.size() == 14, "round trip -> 14 symbols");

    // Find the symbol with the PSS on it (should be symbol_index 4).
    bool found = false;
    size_t pss_idx = 0;
    for (size_t i = 0; i < rx.size(); ++i)
        if (rx[i].symbol_index == kPssSlotSymbol) { pss_idx = i; found = true; }
    CHECK(found, "PSS symbol (index 4) present after round trip");

    int    off = 0;
    float  corr = pss_correlate_fd(rx[pss_idx].samples, 1, 2, off);
    printf("[pss-fd] corr=%.3f offset=%d\n", corr, off);
    CHECK(corr > 0.90f, "PSS FD correlation high on correct cell (PCI 1)");
    CHECK(off == 0, "PSS detected at zero subcarrier offset");

    // Same with SSS.
    int off2 = 0; float corss = 0;
    for (size_t i = 0; i < rx.size(); ++i)
        if (rx[i].symbol_index == kSssSlotSymbol) { pss_idx = i; break; }
    corss = sss_correlate_fd(rx[pss_idx].samples, 0, 1, 2, off2);
    printf("[sss-fd] corr=%.3f offset=%d\n", corss, off2);
    CHECK(corss > 0.90f, "SSS FD correlation high on correct cell (PCI 1)");
    CHECK(off2 == 0, "SSS detected at zero subcarrier offset");

    // Wrong cell must NOT match: try N_ID2=0 against the (N_ID2=1) transmission.
    int offw = 0; float corrw = pss_correlate_fd(rx[pss_idx].samples, 0, 2, offw);
    printf("[pss-fd] wrong-cell corr=%.3f\n", corrw);
    CHECK(corrw < 0.60f, "PSS FD correlation low on wrong N_ID2");

    // ---- 3) Time-domain timing (matched filter peak = PSS symbol body start) ----
    auto ref = pss_time_reference(1, 768);
    CHECK(ref.size() == 768, "PSS time reference is one FFT body");

    auto tcorr = pss_sliding_corr(iq, ref);
    CHECK(tcorr.size() == iq.size() - 768 + 1, "sliding correlation length");
    size_t peak = 0; float pm = -1.0f;
    for (size_t i = 0; i < tcorr.size(); ++i) {
        float m = std::abs(tcorr[i]);
        if (m > pm) { pm = m; peak = i; }
    }

    // Expected body start of slot symbol 4, computed from TS 38.211 CP lengths
    // independently of Ofdm's internals (same formulas as nr_ofdm.cpp).
    const double kTc = 1.0 / (480000.0 * 4096.0);
    const double kK  = 64.0;
    const double srate = 23.04e6;
    const double useful = 2048.0 * kK / 2.0;            // mu = 1
    const double cp_norm = 144.0 * kK / 2.0;
    const double cp_long = (144.0 * kK + 16.0 * kK) / 2.0;
    auto sym_len = [&](unsigned l) {
        double cp = (l == 0 || l == 14) ? cp_long : cp_norm;
        return static_cast<size_t>(std::floor((cp + useful) * kTc * srate));
    };
    auto cp_len = [&](unsigned l) -> size_t {
        double cp = (l == 0 || l == 14) ? cp_long : cp_norm;
        return static_cast<size_t>(std::floor(cp * kTc * srate));
    };
    size_t expected = sym_len(0) + sym_len(1) + sym_len(2) + sym_len(3) + cp_len(4);
    printf("[pss-td] peak=%zu expected PSS body start=%zu strength=%.3f\n",
           peak, expected, pm);

    CHECK(peak == expected, "PSS time-domain correlation peaks at the PSS symbol body start");

    // Peak must be well above the average background.
    float mean_off_peak = 0.0f; size_t cnt = 0;
    for (size_t i = 0; i < tcorr.size(); ++i) {
        if (i == peak) continue;
        mean_off_peak += std::abs(tcorr[i]); ++cnt;
    }
    mean_off_peak /= cnt;
    printf("[pss-td] peak/mean-ratio=%.1f\n", pm / (mean_off_peak + 1e-9f));
    CHECK(pm > 8.0f * mean_off_peak + 1e-6f, "PSS timing peak well above background");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}