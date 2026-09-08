#include "5gone/latency.hpp"
#include "5gone/nr_capture.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace gone;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    // Marker = a known transmit burst (an SSB slot stands in for the real PUSCH
    // burst; the latency math is burst-content independent).
    nr::Ofdm tx(23.04e6, 30000, 51);
    const SampleBuffer marker = nr::synth_ssb_slot(tx, 1);

    // RX buffer = noise + marker injected at a known absolute offset.
    const size_t inject_at = 150000u;
    SampleBuffer rx = nr::synth_noise(300000);
    for (size_t t = 0; t < marker.size(); ++t)
        rx[inject_at + t] += 25.0f * marker[t];

    // 1) whole-buffer search finds the exact offset.
    auto r1 = measure_latency(marker, rx, 23.04e6, 0, rx.size());
    printf("[loopback] whole-buffer: found=%d at=%zu corr=%.3f delay=%.1fus\n",
           (int)r1.found, r1.arrival_sample, r1.corr, r1.delay_sec * 1e6);
    CHECK(r1.found, "burst located");
    CHECK(r1.arrival_sample == inject_at,
          "arrival offset exact (injected at 150000)");
    CHECK(r1.corr > 0.8f, "correlation high");
    const double expect_us = static_cast<double>(inject_at) / 23.04e6 * 1e6;
    CHECK(std::abs(r1.delay_sec * 1e6 - expect_us) < 1.0, "delay consistent with offset");

    // 2) restricted window that excludes the burst -> not found.
    auto r2 = measure_latency(marker, rx, 23.04e6, 0, 1000);
    printf("[loopback] window-excl: found=%d\n", (int)r2.found);
    CHECK(!r2.found, "burst excluded by window is not found");

    // 3) pure noise -> not found.
    const SampleBuffer noise = nr::synth_noise(300000);
    auto r3 = measure_latency(marker, noise, 23.04e6, 0, noise.size());
    printf("[loopback] noise: found=%d\n", (int)r3.found);
    CHECK(!r3.found, "pure noise has no marker");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}