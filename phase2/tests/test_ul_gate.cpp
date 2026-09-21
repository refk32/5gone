// Step 4 test: TDD UL slot gate — RAR -> Msg3 window in the absolute frame clock.
//
// Build WITHOUT liquid (Mac / fallback path):
//   g++ -std=c++17 -I ../include test_ul_gate.cpp \
//       ../src/cell_sync.cpp ../src/nr_pss.cpp ../src/nr_ofdm.cpp \
//       ../src/tdd_gate.cpp ../src/sample_clock.cpp ../src/ul_gate.cpp \
//       -o test_ul_gate
//
// Run: ./test_ul_gate     (return code 0 = all checks passed)

#include "5gone/cell_sync.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/tdd_gate.hpp"
#include "5gone/ul_gate.hpp"

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

static SampleBuffer make_noise(std::size_t n)
{
    SampleBuffer b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = Sample(0.05f * std::sin(0.13 * i), 0.05f * std::cos(0.071 * i));
    return b;
}

// One clean lab SSB slot: [lead] [SSB slot] [trailing].
static SampleBuffer make_ssb_tx(Ofdm& tx, uint16_t pci,
                                std::size_t lead, std::size_t trailing)
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
    return buf;
}

int main()
{
    const double srate = 23.04e6;
    const uint16_t pci = 1;
    const double advance_us = 8.0;                     // cfg default

    // --- Lock a CellSync on a synthetic SSB, then pin it into a big absolute  ---
    // --- sample frame so the UL gate math runs against a known frame clock.  ---
    AttackConfig cfg;
    CellSync sync(cfg);
    {
        Ofdm tx(srate, 30000, 51);
        SampleBuffer buf = make_ssb_tx(tx, pci, 100, 4096);
        SsbResult res;
        const bool ok = sync.find_ssb(buf, res);
        printf("[setup] SSB found=%d slot_start=%zu strength=%.3f\n",
               ok, (size_t)res.slot_start, res.strength);
        CHECK(ok, "SSB locked");
    }
    const uint64_t FRAME = 1000000ull;                 // absolute slot 0 of frame 0
    sync.set_frame_start_global(FRAME);
    const double sps = sync.samples_per_slot();
    printf("[setup] frame_start=%llu sps=%.1f (%.3f ms/slot)\n",
           (unsigned long long)sync.frame_start_sample(), sps, sps / srate * 1e3);
    CHECK(sync.locked(), "locked after global frame pin");

    TddGate tdd(30);
    const double now = 0.5;                            // 500 ms into the epoch
    auto mk = [](uint8_t k, uint64_t rar_slot_offset) {
        RarEvent ev;
        ev.rapid = 7;
        ev.c_rnti = 0x1234;
        ev.grant.k = k;
        ev.rar_slot_offset = rar_slot_offset;
        return ev;
    };

    // ---- A: RAR in absolute slot 2, K2=2 -> Msg3 in slot 4 (DDDSU U) ----
    {
        const uint64_t buf_start = FRAME + 1ull * (uint64_t)sps;
        RarEvent ev = mk(/*k=*/2, /*rar_offset=*/(uint64_t)sps);   // RAR at abs slot 2
        const UlGrantWindow w = compute_ul_grant_window(sync, tdd, buf_start, ev, cfg, now);

        printf("[A] valid=%d ul_ok=%d rar_slot=%llu msg3_slot=%llu start=%llu "
               "tx=%.6f ahead=%.3f ms\n",
               w.valid, w.ul_ok, (unsigned long long)w.rar_abs_slot,
               (unsigned long long)w.msg3_abs_slot, (unsigned long long)w.start_sample,
               w.tx_abs_time_sec, w.tx_ahead_sec * 1e3);
        CHECK(w.valid, "A: window computed");
        CHECK(w.ul_ok, "A: Msg3 is on a UL slot");
        CHECK(w.rar_abs_slot == 2, "A: RAR absolute slot == 2");
        CHECK(w.msg3_abs_slot == 4, "A: Msg3 absolute slot == RAR + K2 == 4");
        CHECK(w.start_sample == FRAME + 4ull * (uint64_t)sps, "A: start == frame + 4 slots");
        CHECK(w.end_sample - w.start_sample == (uint64_t)sps, "A: window == one slot");
        const double exp_tx = (double)(FRAME + 4ull * (uint64_t)sps) / srate - advance_us * 1e-6;
        CHECK(std::fabs(w.tx_abs_time_sec - exp_tx) < 1e-12, "A: TX time = slot start - advance");
        CHECK(std::fabs(w.tx_ahead_sec - (exp_tx - now)) < 1e-12, "A: ahead == TX time - now");
    }

    // ---- B: K2 schedules Msg3 onto a DL slot -> computed but not UL ----
    {
        const uint64_t buf_start = FRAME + 1ull * (uint64_t)sps;
        RarEvent ev = mk(/*k=*/1, /*rar_offset=*/0);          // RAR at slot 1, K2=1 -> slot 2 (DL)
        const UlGrantWindow w = compute_ul_grant_window(sync, tdd, buf_start, ev, cfg, now);
        printf("[B] valid=%d ul_ok=%d msg3_slot=%llu\n",
               w.valid, w.ul_ok, (unsigned long long)w.msg3_abs_slot);
        CHECK(w.valid, "B: still computed (window math exists)");
        CHECK(!w.ul_ok, "B: slot 2 is DL under DDDSU -> not UL");
        CHECK(w.start_sample == FRAME + 2ull * (uint64_t)sps, "B: start still correct");
    }

    // ---- C: RAR maps before the frame lock -> invalid, no TX ----
    {
        const uint64_t buf_start = 0;                  // absolute sample 0 < FRAME
        RarEvent ev = mk(/*k=*/2, /*rar_offset=*/100);
        const UlGrantWindow w = compute_ul_grant_window(sync, tdd, buf_start, ev, cfg, now);
        printf("[C] valid=%d (expect 0)\n", w.valid);
        CHECK(!w.valid, "C: pre-lock RAR yields no window");
    }

    // ---- D: K2 crosses a frame boundary into a UL slot (abs 19 -> 23) ----
    {
        const uint64_t buf_start = FRAME + 19ull * (uint64_t)sps;
        RarEvent ev = mk(/*k=*/4, /*rar_offset=*/0);
        const UlGrantWindow w = compute_ul_grant_window(sync, tdd, buf_start, ev, cfg, now);
        printf("[D] valid=%d ul_ok=%d rar_slot=%llu msg3_slot=%llu\n",
               w.valid, w.ul_ok, (unsigned long long)w.rar_abs_slot,
               (unsigned long long)w.msg3_abs_slot);
        CHECK(w.valid && w.ul_ok, "D: frame-wrap Msg3 == slot 23 (UL, pos 3)");
        CHECK(w.rar_abs_slot == 19, "D: RAR at slot 19");
        CHECK(w.msg3_abs_slot == 23, "D: Msg3 at 19 + 4 == 23");
        CHECK(w.start_sample == FRAME + 23ull * (uint64_t)sps, "D: start unwraps into frame 2");
        CHECK(tdd.is_ul_slot(23), "D: TddGate agrees slot 23 is UL");
    }

    // ---- E: timeliness — a `now` past the target reads negative ahead ----
    {
        const uint64_t buf_start = FRAME + 1ull * (uint64_t)sps;
        RarEvent ev = mk(/*k=*/2, /*rar_offset=*/(uint64_t)sps);
        const double late_now = (double)(FRAME + 4ull * (uint64_t)sps) / srate + 1.0;
        const UlGrantWindow w = compute_ul_grant_window(sync, tdd, buf_start, ev, cfg, late_now);
        printf("[E] ahead=%.3f ms (expect negative: target already past)\n",
               w.tx_ahead_sec * 1e3);
        CHECK(w.valid && w.ul_ok, "E: window still computed");
        CHECK(w.tx_ahead_sec < 0.0, "E: target is in the past -> skip-worthy");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}