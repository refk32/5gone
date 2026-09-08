// Step 9 test: nr_rar_decoder — full DL decode chain wiring smoke test.
//
//   g++ -std=c++17 -I ../include test_rar.cpp ../src/nr_rar_decoder.cpp \
//       ../src/nr_pdcch.cpp ../src/nr_dci.cpp ../src/nr_dmrs.cpp ../src/nr_pn.cpp \
//       ../src/nr_dsp.cpp ../src/nr_symbol.cpp ../src/nr_ofdm.cpp \
//       -o test_rar && ./test_rar
//
// decode() takes RAW TIME-DOMAIN IQ samples (like the B210 delivers), so this
// smoke test feeds pure noise through the entire chain: OFDM demux -> PDCCH
// DM-RS correlation -> (no DCI bits without srsRAN) -> logging. With no DM-RS
// present there must be zero false positives, and it must not crash.
//
// (Synthesizing a REAL time-domain PDCCH to confirm a positive detection needs
// an IFFT/OFDM modulator which is out of scope for this port; the positive
// path is covered at the frequency-domain level by test_pdcch.cpp.)

#include "5gone/nr_rar_decoder.hpp"

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
    // Build the decoder exactly as live mode would: 23.04 MHz, 30 kHz, PCI 1.
    RarDecoder decoder(23.04e6, 30000, 1, 51);   // prints its init line

    // Feed several OFDM symbols' worth of pure noise (looks like an idle cell).
    // 30 kHz SCS -> 1 ms subframe = 23040 samples; give it ~5 ms.
    std::mt19937 rng(7);
    std::normal_distribution<float> nz(0.f, sqrt(0.5f));
    std::vector<std::complex<float>> iq(23040u * 5u);
    for (auto& v : iq) v = std::complex<float>(nz(rng), nz(rng));

    std::vector<RarDciObs> obs = decoder.decode(iq);
    printf("noise buffer -> %zu RAR DCI observations (expect 0)\n", obs.size());
    CHECK(obs.empty(), "idle/noise buffer yields no false-positive RAR DCI");

    // Also make sure an empty/short buffer is handled (no crash).
    std::vector<std::complex<float>> iq2;
    auto obs2 = decoder.decode(iq2);
    CHECK(obs2.empty(), "empty buffer handled");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
