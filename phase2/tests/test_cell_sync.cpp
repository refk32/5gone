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
        CHECK(res.pss_sample == lead + 3354, "PSS body start == lead + 3354");
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

    // ---- 5) Global-frame lock: buffer-local SSB lock shifted into the
    //          absolute RX-clock (live-mode Step 3 handshake) ----
    {
        Ofdm tx(srate, 30000, 51);
        SampleBuffer buf = make_ssb_tx(tx, pci, 100, 4096, 0.0);   // SSB slot at 100

        AttackConfig cfg;
        CellSync sync(cfg);
        SsbResult res;
        CHECK(sync.find_ssb(buf, res), "clean slot detected");
        CHECK(sync.frame_start_sample() == 100, "find_ssb locks buffer-local first");

        // The receive stream told us this buffer began at absolute sample X.
        const uint64_t rx_start = 123456789ull;
        sync.set_frame_start_global(rx_start + res.slot_start);

        const double sps = sync.samples_per_slot();
        CHECK(sync.frame_start_sample() == rx_start + 100,
              "frame start shifted into the global clock");
        CHECK(sync.locked(), "lock survives the shift");

        sync.set_rx_now(rx_start + 100);
        CHECK(sync.current_slot().has_value() && *sync.current_slot() == 0,
              "global frame start -> slot 0");
        sync.set_rx_now(rx_start + 100 + (uint64_t)(3.0 * sps));
        CHECK(sync.current_slot().has_value() && *sync.current_slot() == 3,
              "3 slots later in absolute samples -> slot 3");
        CHECK(std::fabs(sync.samples_to_next_ul_slot(4) - sps) < 1e-6,
              "one slot of samples to slot 4");
        sync.set_rx_now(rx_start + 100 + (uint64_t)(9.0 * sps));
        CHECK(sync.samples_to_next_ul_slot(4) == 0.0,
              "already past slot 4 -> 0 samples");
        printf("[case5] global lock OK (frame_start=%llu)\n",
               (unsigned long long)sync.frame_start_sample());
    }

    // ---- 6) Two-phase lock: a candidate must reproduce on the frame grid
    //          for kVerifyHitsNeeded frames before it is confirmed (live TX
    //          safety; a dead cell hands a spurious single-buffer lock) ----
    {
        Ofdm tx(srate, 30000, 51);
        const uint64_t rx_base = 1ull << 33;
        const std::size_t lead = 200;

        AttackConfig cfg;
        CellSync sync(cfg);
        SsbResult res;
        CHECK(sync.find_ssb(make_ssb_tx(tx, pci, lead, 4096, 0.0), res), "clean slot detected");
        const double sps = sync.samples_per_slot();
        const uint64_t F = sync.frame_samples();
        CHECK(F == (uint64_t)(20.0 * sps), "frame period == 20 slots");
        CHECK(sync.verify_in_progress(), "fresh lock starts as unconfirmed candidate");
        CHECK(sync.locked(), "candidate keeps cell lock active");

        // Shift the lock into the global clock, then feed three frame-aligned
        // buffers that reproduce the SSB at the same absolute position.
        sync.set_frame_start_global(rx_base + lead);
        CellSync::VerifyEvent ev = CellSync::VerifyEvent::Idle;
        for (unsigned k = 1; k <= 3; ++k) {
            const uint64_t buf_start = rx_base + k * F;
            ev = sync.verify_frame(make_ssb_tx(tx, pci, lead, 4096, 0.0),
                                   buf_start, 3ull * (uint64_t)sps);
            CHECK(ev != CellSync::VerifyEvent::Broken, "grid hit must not break the lock");
        }
        CHECK(ev == CellSync::VerifyEvent::Confirmed, "3 reproductions confirm the lock");
        CHECK(!sync.verify_in_progress(), "confirmed lock leaves the pending stage");
        CHECK(sync.locked(), "confirmed lock is active");
        CHECK(sync.cfo_frames() >= 4, "verify frames folded into the CFO average");

        // ssb_reproduced_here() grid arithmetic, in isolation.
        CHECK(sync.ssb_reproduced_here(rx_base + lead, 10), "SSB at frame start");
        CHECK(sync.ssb_reproduced_here(rx_base + lead + 3 * F, 10), "SSB k frames later");
        CHECK(sync.ssb_reproduced_here(rx_base + lead - 5, 10), "just before frame start (wrap)");
        CHECK(!sync.ssb_reproduced_here(rx_base + lead + F / 2, 10), "half-frame off grid");
        CHECK(!sync.ssb_reproduced_here(rx_base + lead + 7 * (uint64_t)sps, 3ull * (uint64_t)sps),
              "7 slots off-grid beyond tolerance");
        printf("[case6] two-phase lock verified: F=%llu sps=%.0f hits confirmed after 3 frames\n",
               (unsigned long long)F, sps);
    }

    // ---- 7) A candidate that never reproduces (no cell / noise) is dropped ----
    {
        Ofdm tx(srate, 30000, 51);
        const uint64_t rx_base = 1ull << 29;
        const std::size_t lead = 100;

        AttackConfig cfg;
        CellSync sync(cfg);
        SsbResult res;
        CHECK(sync.find_ssb(make_ssb_tx(tx, pci, lead, 4096, 0.0), res), "clean slot detected");
        sync.set_frame_start_global(rx_base + lead);
        const uint64_t F = sync.frame_samples();
        const double sps = sync.samples_per_slot();

        CellSync::VerifyEvent ev;
        ev = sync.verify_frame(make_noise(16384), rx_base + 2 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Pending && sync.locked(),
              "first noise frame: still pending");
        ev = sync.verify_frame(make_noise(16384), rx_base + 3 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Pending && sync.locked(),
              "second noise frame: still pending");
        ev = sync.verify_frame(make_noise(16384), rx_base + 4 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Broken, "third noise frame drops the lock");
        CHECK(!sync.locked(), "lock dropped after kVerifyMissLimit misses");
        CHECK(!sync.verify_in_progress(), "no pending verify after drop");
        CHECK(sync.verify_frame(make_noise(16384), rx_base + 5 * F, 3ull * (uint64_t)sps) ==
              CellSync::VerifyEvent::Idle, "verify after drop is idle");
        printf("[case7] noise frame reject OK (lock dropped, back to rescan)\n");
    }

    // ---- 8) An SSB that lands off the locked grid position is a miss ----
    {
        Ofdm tx(srate, 30000, 51);
        const uint64_t rx_base = 1ull << 31;
        const std::size_t lead = 300;
        const double sps = (double)tx.samples_per_slot();

        AttackConfig cfg;
        CellSync sync(cfg);
        SsbResult res;
        CHECK(sync.find_ssb(make_ssb_tx(tx, pci, lead, 4096, 0.0), res), "clean slot detected");
        sync.set_frame_start_global(rx_base + lead);
        const uint64_t F = sync.frame_samples();
        // SSB shifted 7 slots forward in the frame -> outside the 3-slot tolerance.
        const std::size_t shift_lead = lead + (std::size_t)(7.0 * sps);

        CellSync::VerifyEvent ev;
        ev = sync.verify_frame(make_ssb_tx(tx, pci, shift_lead, 4096, 0.0),
                               rx_base + 1 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Pending, "off-grid frame 1: pending");
        ev = sync.verify_frame(make_ssb_tx(tx, pci, shift_lead, 4096, 0.0),
                               rx_base + 2 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Pending, "off-grid frame 2: pending");
        ev = sync.verify_frame(make_ssb_tx(tx, pci, shift_lead, 4096, 0.0),
                               rx_base + 3 * F, 3ull * (uint64_t)sps);
        CHECK(ev == CellSync::VerifyEvent::Broken, "off-grid frame 3 drops the lock");
        CHECK(!sync.locked(), "off-grid SSB refused");
        printf("[case8] off-grid SSB reject OK\n");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}