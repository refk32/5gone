// test_sample_clock.cpp
// =====================
// Portable unit test for the Rx sample-clock conversion (gone::rx_sample_from_time
// / rx_time_from_sample). Pure math — no UHD, no srsRAN — so it builds and runs
// on the Mac workspace and the Latte equally.
//
// It validates the exact property live mode relies on: with one synced device
// epoch (RadioUhd::sync_time(0.0)), every RX packet's UHD stamp maps to a
// deterministic global sample index and a continuous stream stays contiguous.

#include "5gone/sample_clock.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace gone;

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

int main()
{
    const double srate = 23.04e6; // lab B210 master rate (23.04 MHz)

    std::printf("test_sample_clock: Rx sample-clock conversions @ %.2f MHz\n", srate / 1e6);

    // 1) Whole-second epochs map to exact sample counts.
    {
        CHECK(rx_sample_from_time(0.0, srate) == 0);
        CHECK(rx_sample_from_time(1.0, srate) == static_cast<uint64_t>(srate));
        CHECK(rx_sample_from_time(2.0, srate) == 2u * static_cast<uint64_t>(srate));
        std::printf("[ok] whole-second epochs\n");
    }

    // 2) Fractional stamps land on the exact sample (rounding <= 0.5 sample).
    {
        const double t = 0.000123456789;
        const uint64_t idx = rx_sample_from_time(t, srate);
        const double t_back = rx_time_from_sample(idx, srate);
        CHECK(std::fabs(t_back - t) <= 0.5 / srate + 1e-9);
        std::printf("[ok] fractional stamp round-trips within half a sample (idx=%llu)\n",
                    static_cast<unsigned long long>(idx));
    }

    // 3) A continuous UHD stream stays sample-contiguous.
    //    Packet i starts at 0.1 s + i * (packet samples / rate); the mapped
    //    indices must be exactly base + i * packet_len (no drift, no gaps).
    {
        const uint64_t packet_len = 2304;            // 100 us packets @ 23.04 MHz
        const uint64_t base = 0.1 * srate;           // 2304000 (exact multiple)
        bool contiguous = true;
        uint64_t expect = base;
        for (uint64_t i = 0; i < 5000; ++i) {
            const double t_i = 0.1 + static_cast<double>(i * packet_len) / srate;
            const uint64_t got = rx_sample_from_time(t_i, srate);
            if (got != expect) {
                std::printf("    drift @ packet %llu: got %llu want %llu\n",
                            static_cast<unsigned long long>(i),
                            static_cast<unsigned long long>(got),
                            static_cast<unsigned long long>(expect));
                contiguous = false;
                break;
            }
            expect += packet_len;
        }
        CHECK(contiguous);
        std::printf("[ok] stream contiguity (5000 packets)\n");
    }

    // 4) Sample-aligned math inside a packet: stamp of sample k within packet P.
    {
        const uint64_t packet_len = 2304;
        const uint64_t base = 2304000;
        const uint64_t idx = rx_sample_from_time(0.1 + 1000.0 / srate, srate);
        CHECK(idx == base + 1000);
        std::printf("[ok] in-packet offset\n");
    }

    // 5) Seconds-inverse sanity for TX scheduling (slot offsets).
    {
        const uint64_t slot = 7680; // samples per slot @ 23.04 MHz / 30 kHz
        const double t_slot = rx_time_from_sample(slot, srate);
        CHECK(std::fabs(t_slot - 1.0 / 3000.0) < 1e-12);
        std::printf("[ok] slot-duration inverse (%.9f s)\n", t_slot);
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED (%d)\n", failures);
    return 1;
}