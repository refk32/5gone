// Step 5 test: nr_ofdm only (the OFDM demodulator).
//
// Build WITHOUT liquid (Mac / fallback path):
//   g++ -std=c++17 -I ../include test_ofdm.cpp ../src/nr_ofdm.cpp -o test_ofdm
//
// Build WITH liquid-dsp (your Linux box, the real fast path):
//   g++ -std=c++17 -DHAVE_LIQUID -I ../include ../src/nr_ofdm.cpp \
//       test_ofdm.cpp -lliquid -o test_ofdm
//
// Expected output is printed line by line. Return code 0 = all checks passed.

#include "5gone/nr_ofdm.hpp"

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
    // Matches the lab gNB: 23.04 MHz, 30 kHz, 51 PRBs.
    Ofdm ofdm(23.04e6, 30000, 51);
    printf("fft_size=%u num_subcarriers=%u samples_per_slot=%u\n",
           ofdm.fft_size(), ofdm.num_subcarriers(), ofdm.samples_per_slot());

    CHECK(ofdm.fft_size() == 768, "fft size 768 (23.04e6 / 30e3)");
    CHECK(ofdm.num_subcarriers() == 612, "612 active subcarriers (51 * 12)");

    // Build one slot worth of IQ with a single tone on absolute FFT bin 100.
    const uint32_t sps = ofdm.samples_per_slot();
    std::vector<std::complex<float>> iq(sps, {0,0});
    const uint32_t FFT = ofdm.fft_size();
    const float twopi = 8.0f * atan(1.0f);
    const int k = 100;
    for (uint32_t n = 0; n < sps; ++n)
        iq[n] = std::exp(std::complex<float>(0, twopi * k * n / FFT));

    auto syms = ofdm.demodulate(iq);
    printf("decoded %zu symbols (expect 14)\n", syms.size());
    CHECK(syms.size() == 14, "one slot -> 14 OFDM symbols");

    const auto& s0 = syms[0].samples;
    size_t peak = 0; float pm = -1;
    for (size_t i = 0; i < s0.size(); ++i) {
        float m = std::abs(s0[i]);
        if (m > pm) { pm = m; peak = i; }
    }
    printf("sym0 idx=%u peak active subcarrier %zu mag %.3f\n",
           (unsigned)syms[0].symbol_index, peak, pm);

    // Active subcarrier index = FFT bin index (bin 100 -> subcarrier 100).
    CHECK(peak == 100, "tone on fft bin 100 -> active subcarrier 100");
    CHECK(std::abs(pm - (float)FFT) < 1e-2f, "all energy on one bin (mag = 768)");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
