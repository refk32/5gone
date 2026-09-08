// Step 2 test: real CellSync (SSB/PSS detection + slot clock + CFO).
//
// Build WITHOUT liquid (Mac / fallback path):
//   g++ -std=c++17 -I ../include test_cell_sync.cpp \
//       ../src/cell_sync.cpp ../src/nr_pss.cpp ../src/nr_ofdm.cpp -o test_cell_sync
//
// Build WITH liquid-dsp (your Linux box, the real fast path):
//   g++ -std=c++17 -DHAVE_LIQUID -I ../include ../src/nr_ofdm.cpp ../src/nr_pss.cpp \
//       ../src/cell_sync.cpp test_cell_sync.cpp -lliquid -o test_cell_sync
//
// Run: ./test_cell_sync     (return code 0 = all checks passed)

#include "5gone/cell_sync.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace gone;
using namespace gone::nr;

static const double kTwoPi = 2.0 * std::acos(-1.0);
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

// Deterministic "noise-like" samples (LCG-free) so the test is reproducible.
static SampleBuffer make_noise(std::size_t n)
{
    SampleBuffer b(n);
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = Sample(0.05f * std::sin(0.13 * i), 0.05f * std::cos(0.071 * i));
    }
    return b;
}

// One clean lab SSB-slot transmission: [lead] [SSB slot] [trailing].
static SampleBuffer make_ssb_tx(Ofdm& tx, uint16_t pci,
                                std::size_t lead, std::size_t trailing,
                                double cfo_hz)
{
    const uint16_t n_id2 = pci % 3;
    const uint16_t n_id1 = static_cast<uint16_t>(pci / 3);

    std::vector<Symbol> slot(14);
    for (auto& s : slot) s.samples.assign(612, {0, 0});
    place_sss_in_symbol(slot[kSssSlotSymbol].samples, n_id1, n_id2);
    place_pss_in_symbol(slot[kPssSlotSymbol].samples, n_id2);

    SampleBuffer buf = make_noise(lead);
    auto body = tx.modulate(slot);
    buf.insert(buf.end(), body.begin(), body.end());
    buf.insert(buf.end(), trailing, Sample(0.0f, 0.0f));

    if (std::fabs(cfo_hz) > 1e-9) {
        const double srate = 23.04e6;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const double ph = kTwoPi * cfo_hz * static_cast<double>(i) / srate;
            const Sample rot(std::cos(ph), std::sin(ph));
            buf[i] = buf[i] * rot;
        }
    }
    return buf;
}

int main()
{
    const double srate = 23.04e6;
    const uint16_t pci = 1;

    // ---- 1) Find + lock on a clean SSB slot sitting mid-buffer, with CFO ----
    {
        Ofdm tx(srate, 30000, 51);
        const std::size_t lead = 1024;
        const std::size_t trailing = 4096;
        const double cfo = 1234.5;

        SampleBuffer buf = make_ssb_tx(tx, pci, lead, trailing, cfo);

        AttackConfig cfg;              // pci=1, scs=30 kHz, srate=23.04 MHz (defaults)
        CellSync sync(cfg);
        SsbResult res;
        CHECK(!sync.locked(), "initial state: unlocked");
        CHECK(!sync.current_slot().has_value(), "no slot before lock");

        bool ok = sync.find_ssb(buf, res);
        printf("[case1] found=%d pss_sample=%zu slot_start=%zu strength=%.3f cfo=%.1fHz\n",
               ok, (size_t)res.pss_sample, (size_t)res.slot_start,
               res.strength, res.cfo_hz);
        CHECK(ok, "SSB found in clean slot");
        CHECK(res.slot_start == lead, "slot_start == lead-in length");
        CHECK(res.pss_sample == lead + 3348, "PSS body start == lead + 3348");
        CHECK(res.strength > 0.70f, "PSS/SSS FD correlation strong");
        CHECK(std::fabs(res.cfo_hz - cfo) < 50.0, "CFO estimated within 50 Hz");
        CHECK(sync.locked(), "lock set after find_ssb");
        CHECK(sync.frame_start_sample() == lead, "frame_start_sample == slot_start");
    }

    // ---- 2) Slot-clock math after lock ----
    {
        Ofdm tx(srate, 30000, 51);
        SampleBuffer buf = make_ssb_tx(tx, pci, 100, 4096, 0.0);

        AttackConfig cfg;
        CellSync sync(cfg);
        SsbResult res;
        CHECK(sync.find_ssb(buf, res), "clean slot detected (no CFO)");
        CHECK(std::fabs(res.cfo_hz) < 1.0, "CFO ~ 0 when none applied");

        const double sps = sync.samples_per_slot();
        CHECK(std::fabs(sps - (double)tx.samples_per_slot()) < 1e-6,
              "samples_per_slot matches Ofdm");

        sync.set_rx_now(res.slot_start);
        CHECK(sync.current_slot().has_value() && *sync.current_slot() == 0,
              "at slot_start -> slot 0");

        sync.set_rx_now(res.slot_start + (uint64_t)(2.0 * sps));
        CHECK(sync.current_slot().has_value() && *sync.current_slot() == 2,
              "2 slots later -> slot 2");
        CHECK(std::fabs(sync.samples_to_next_ul_slot(3) - sps) < 1e-6,
              "samples to slot 3 == one slot");
        CHECK(sync.samples_to_next_ul_slot(2) == 0.0, "past target -> 0 samples");
        CHECK(sync.slot_from_rar(3, 4) == 7, "RAR scheduled 4 slots after Msg1 (3)");
    }

    // ---- 3) Noise must NOT produce a lock ----
    {
        AttackConfig cfg;
        CellSync sync(cfg);
        SampleBuffer noise = make_noise(16384);
        SsbResult res;
        bool ok = sync.find_ssb(noise, res);
        printf("[case3] noise found=%d strength=%.3f\n", ok, res.strength);
        CHECK(!ok, "no lock on pure noise");
        CHECK(!sync.locked(), "still unlocked after noise");
    }

    // ---- 4) Wrong-cell config must NOT produce a lock against the lab SSB ----
    {
        Ofdm tx(srate, 30000, 51);
        SampleBuffer buf = make_ssb_tx(tx, pci, 200, 4096, 0.0);

        AttackConfig cfg;             // defaults -> pci=1 (lab cell)
        cfg.pci = 2;                  // different cell -> different PSS/SSS
        CellSync sync(cfg);
        SsbResult res;
        bool ok = sync.find_ssb(buf, res);
        printf("[case4] wrong-pci found=%d strength=%.3f\n", ok, res.strength);
        CHECK(!ok, "no lock when configured for a different PCI");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}