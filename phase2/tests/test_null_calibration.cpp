// test_null_calibration.cpp — null-calibrated acceptance regression test.
// Portable (no UHD/srsRAN). Covers the R1/P2 defect where "empty" captures
// scored as high as a real PDCCH: the CORESET sweep and the RarMonitor grid
// accepted configurations on correlation maxima that pure noise reaches via
// max-of-extreme statistics over ~100M trials, then decoded zero DCI bits.
//
// The fix measures each grid's OWN noise floor on a synthetic noise window of
// the same span and requires the winner to clear it by a margin. This test
// pins that behaviour on both paths:
//
//   1. decode_capture() sweep on pure noise    -> not accepted, null floor
//      measured and reported, best hit below the gate.
//   2. decode_capture() sweep on a RAR slot    -> accepted (corr ~1.0 clears
//      the same gate), DCI observations present.
//   3. RarMonitor grid on pure noise windows   -> null floor measured, no
//      bucket ever fills (an empty link must not spam SIB buckets), and no
//      hit reaches fire_floor().
//   4. RarMonitor grid on a real RAR window    -> the RAR slot fires (corr
//      >0.95) and its SIB bucket is learned.
//
// Build:
//   (see phase2/CMakeLists.txt target test_null_calibration)

#include "5gone/nr_constants.hpp"
#include "5gone/nr_capture.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_rar_tx.hpp"
#include "5gone/rar_monitor.hpp"
#include "tools/decode_capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using gone::Sample;
using gone::SampleBuffer;
using gone::nr::bwp_num_prbs;

int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

gone::nr::Coreset make_coreset(uint16_t freq, uint16_t start, uint8_t dur,
                               bool interleaved, uint16_t shift, uint16_t pci)
{
    gone::nr::Coreset c;
    c.control_resourceset_id = 1;
    c.frequency_domain_resources = freq;
    c.start_prb = start;
    c.duration = dur;
    c.cce_reg_mapping_type = interleaved ? "interleaved" : "non-interleaved";
    c.reg_bundlesize = 6;
    c.interleaver_size = 2;
    c.shift_index = shift;
    c.cell_id = pci;
    c.starting_ofdm_symbol_within_slot = 0;
    c.num_symbols_per_slot = 14;
    c.num_slots_per_frame = 20;
    c.candidates_search_space = {1, 2, 4, 8, 16};
    return c;
}

} // namespace

int main(int argc, char** argv)
{
    const uint16_t pci = 1;
    const double kSrate = 23.04e6;
    const uint32_t kSlotSamples = 11520;
    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");

    gone::nr::Ofdm ofdm(kSrate, 30000, bwp_num_prbs);

    // ---- Shared fixtures ----
    // Pure-noise cf32 (4 slots of deterministic hash noise). No SSB, no PDCCH.
    const std::string noise_path = tmp + "/5gone_nullcal_noise.cf32";
    auto noise = gone::nr::synth_noise_hash(4u * kSlotSamples, 0.5f);
    CHECK(gone::nr::write_cf32(noise_path, noise), "wrote pure-noise cf32");

    // RAR-bearing file: 4 slots, real RAR (48 PRB @ PRB 2, duration 1,
    // interleaved, shift=PCI%3=1) embedded at slot 2, quiet hash noise else.
    const std::string rar_path = tmp + "/5gone_nullcal_rar.cf32";
    const auto tx_cs = make_coreset(48, 2, 1, true, 1, pci);
    const unsigned rar_slot = 2;
    std::vector<gone::nr::Symbol> file_syms;
    file_syms.reserve(4u * 14u);
    for (unsigned s = 0; s < 4; ++s) {
        for (unsigned l = 0; l < 14; ++l) {
            gone::nr::Symbol sym;
            sym.samples.assign(51u * 12u, Sample(0.0f, 0.0f));
            if (s == rar_slot) {
                auto tx = gone::nr::build_rar_slot(ofdm, pci, bwp_num_prbs,
                                                  0.5f, tx_cs, (uint8_t)(s % 20));
                sym.samples = tx.grid[l].samples;
            } else {
                auto slot_noise = gone::nr::synth_noise_hash(sym.samples.size(), 0.02f);
                for (size_t i = 0; i < sym.samples.size(); ++i)
                    sym.samples[i] = Sample(slot_noise[i].real(), slot_noise[i].imag());
            }
            file_syms.push_back(std::move(sym));
        }
    }
    const SampleBuffer file_iq = ofdm.modulate(file_syms);
    CHECK(gone::nr::write_cf32(rar_path, file_iq),
          "wrote RAR-bearing cf32 (RAR at slot 2)");

    // ============ 1. Sweep on PURE NOISE: must not accept ============
    {
        auto d = gone::tool::decode_capture({
            .path = noise_path, .sample_rate = kSrate, .scs_hz = 30000,
            .pci = pci, .bwp_prbs = 51, .cell_sync = false,
            .start_sample = 0, .num_slots = 4, .verbose = false,
            .coreset_sweep = true, .sweep_slots = 4,
        });
        printf("[1] noise: sweep_completed=%d accepted=%d null_floor=%.4f gate=%.4f\n",
               d.sweep_completed, d.sweep_accepted,
               d.sweep_null_floor, d.sweep_null_gate);
        CHECK(d.sweep_completed, "noise: sweep ran");
        CHECK(d.sweep_null_floor > 0.0f, "noise: null floor measured (>0)");
        CHECK(d.sweep_null_gate > d.sweep_null_floor, "noise: gate above floor");
        CHECK(!d.sweep_accepted, "noise: empty file is NOT accepted");
        if (!d.sweep.empty()) {
            printf("[1] noise best corr=%.4f (gate=%.4f)\n",
                   d.sweep.front().corr, d.sweep_null_gate);
            CHECK(d.sweep.front().corr < d.sweep_null_gate,
                  "noise: best hit sits below the null gate");
        }
        CHECK(d.rars.empty(), "noise: no RAR DCI observations");
    }

    // ============ 2. Sweep on the RAR slot: must accept AND decode ============
    {
        auto d = gone::tool::decode_capture({
            .path = rar_path, .sample_rate = kSrate, .scs_hz = 30000,
            .pci = pci, .bwp_prbs = 51, .cell_sync = false,
            .start_sample = 0, .num_slots = 4, .verbose = false,
            .coreset_sweep = true, .sweep_slots = 4,
        });
        printf("[2] rar: sweep_completed=%d accepted=%d null_floor=%.4f gate=%.4f rars=%zu\n",
               d.sweep_completed, d.sweep_accepted,
               d.sweep_null_floor, d.sweep_null_gate, d.rars.size());
        CHECK(d.sweep_completed, "rar: sweep ran");
        CHECK(d.sweep_accepted, "rar: real RAR slot IS accepted");
        if (!d.sweep.empty()) {
            printf("[2] rar best corr=%.4f (gate=%.4f)\n",
                   d.sweep.front().corr, d.sweep_null_gate);
            CHECK(d.sweep.front().corr > d.sweep_null_gate,
                  "rar: winner clears the null gate");
            CHECK(d.sweep.front().corr > 0.9,
                  "rar: winner is a ~1.0 correlation");
        }
        // The winning config must be the transmitted one (48 PRB @ 2), the
        // same separation property test_coreset_sweep checks.
        if (!d.sweep.empty() && d.sweep_accepted) {
            const auto& b = d.sweep.front();
            CHECK(b.freq_prbs == 48 && b.start_prb == 2,
                  "rar: winning config is the transmitted 48 PRB @ 2");
        }
        // The fix keeps decode-on-accept working: the winner re-decodes the
        // RAR PDCCH observation (see test_coreset_sweep case A step 4).
        CHECK(!d.rars.empty(), "rar: at least one RAR DCI observation");
    }

    // ============ 3. RarMonitor grid on PURE NOISE: no fire, no buckets ===
    {
        gone::AttackConfig cfg;
        cfg.pci = pci;
        gone::RarMonitor mon(cfg);
        mon.ensure_null_floor();
        printf("[3] monitor grid on noise-start: null_floor=%.4f fire_floor=%.4f\n",
               mon.null_floor(), mon.fire_floor());
        CHECK(mon.null_floor() > 0.0f, "monitor: null floor measured");
        CHECK(mon.fire_floor() >= 0.9f, "monitor: fire floor at least 0.9");

        const uint64_t frame0 = 0;
        double worst = 0.0;
        // Scan several pure-noise windows across the file (stream clock).
        const size_t step = 16384;
        for (size_t rel = 0; rel < noise.size() - step; rel += 2 * 11520) {
            SampleBuffer buf(noise.begin() + rel, noise.begin() + rel + step);
            auto hits = mon.scan_window(buf, rel, frame0, (double)kSlotSamples, 0.0);
            mon.note_hits_for_sib(hits);
            for (const auto& h : hits) worst = std::max<double>(worst, h.corr);
        }
        printf("[3] noise windows: worst corr=%.4f fire_floor=%.4f\n", worst, mon.fire_floor());
        CHECK(worst < mon.fire_floor(),
              "monitor: nothing on noise reaches the fire floor");
        size_t filled = 0;
        for (unsigned m = 0; m < 40; ++m)
            if (mon.sib_distinct((uint8_t)m) > 0) ++filled;
        printf("[3] SIB buckets filled: %zu / 40\n", filled);
        CHECK(filled == 0, "monitor: empty link fills zero SIB buckets");
    }

    // ============ 4. RarMonitor grid on the REAL RAR window: it fires =====
    {
        gone::AttackConfig cfg;
        cfg.pci = pci;
        gone::RarMonitor mon(cfg);
        mon.ensure_null_floor();
        printf("[4] monitor grid fire_floor=%.4f\n", mon.fire_floor());
        CHECK(mon.fire_floor() >= 0.9f, "monitor: fire floor at least 0.9");

        // Window starting just before the RAR slot (abs slot 2), like the
        // test_slot_scan harness — the only frame the grid understands.
        const uint64_t abs = 2u * kSlotSamples - 1000;
        SampleBuffer buf(file_iq.begin() + abs,
                         file_iq.begin() + abs + 16384);
        auto hits = mon.scan_window(buf, abs, 0, (double)kSlotSamples, 0.0);
        mon.note_hits_for_sib(hits);
        bool saw2 = false;
        double best = 0.0;
        for (const auto& h : hits) {
            best = std::max<double>(best, h.corr);
            if (h.abs_slot == 2) saw2 = true;
        }
        printf("[4] rar window: best corr=%.4f, fire_floor=%.4f, hit@2=%d\n",
               best, mon.fire_floor(), (int)saw2);
        CHECK(best > 0.95, "monitor: RAR window correlates ~1.0");
        CHECK(best >= mon.fire_floor(), "monitor: RAR window clears the fire floor");
        CHECK(saw2, "monitor: hit labelled to abs slot 2");
        CHECK(mon.sib_distinct(2) == 1, "monitor: SIB bucket 2 learned (1 distinct)");
    }

    std::remove(noise_path.c_str());
    std::remove(rar_path.c_str());

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}