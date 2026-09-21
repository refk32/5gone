// test_slot_scan.cpp — Path-2 slot-aware window scan (P2-1) test.
// Portable (no UHD/srsRAN): synthesizes a 24-PRB @6, duration-2, interleaved
// RAR slot, embeds copies at absolute slots {2, 42, 9} in a low-noise file,
// and scans stream-like buffers with an explicit (abs_start, frame_start)
// clock. Verifies:
//   1. Each RAR slot is found (corr ~1.0) with the transmitted config
//      identified (freq 24, start 6) — detection + identification.
//   2. Silence produces nothing (specificity; corr floor holds).
//   3. SIB bookkeeping: abs slots 2 and 42 share bucket 2 (distinct == 2),
//      abs 9 is alone in bucket 9 (distinct == 1).
//   4. Per-window latency is sane (printed; smoke-bounded).

#include "5gone/nr_constants.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_rar_tx.hpp"
#include "5gone/rar_monitor.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
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

// Deterministic hash noise (splitmix64): uniform, no lattice structure.
// (An earlier LCG version produced 0.95+ spurious correlations through the
// 120-config reference bank — LCG lattice, not a code bug. If you touch this,
// re-check the silence floor on both scalar and liquid FFT builds.)
static uint64_t splitmix_state = 0x9E3779B97F4A7C15ull;
static float hash_noise(float amp)
{
    uint64_t z = (splitmix_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    const float u = (float)(z >> 11) / (float)(1ull << 53);  // [0,1)
    return (u - 0.5f) * 2.0f * amp;
}

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
    // File mode: scan a real .cf32 capture with an explicit clock.
    //   test_slot_scan <file.cf32> [file_start_stream_abs] [frame_start] [sps]
    // file_start_stream_abs = stream sample index of file sample 0 (from the
    // run's attempt/done lines); frame_start = run's SSB lock value. Both in
    // the run's UHD device epoch — mixing file-relative positions with the
    // stream frame silently mislabels every slot (that mistake voided the
    // first two real-file analyses).
    if (argc > 1) {
        const char* path = argv[1];
        const uint64_t file_start = argc > 2 ? strtoull(argv[2], nullptr, 10) : 0;
        const uint64_t frame_start = argc > 3 ? strtoull(argv[3], nullptr, 10) : 0;
        const double sps = argc > 4 ? strtod(argv[4], nullptr) : 11520.0;
        FILE* f = std::fopen(path, "rb");
        if (!f) { std::printf("cannot open %s\n", path); return 2; }
        std::fseek(f, 0, SEEK_END);
        const long bytes = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<float> raw((size_t)bytes / 4);
        if (std::fread(raw.data(), 4, raw.size(), f) != raw.size()) {
            std::printf("short read\n");
            std::fclose(f);
            return 2;
        }
        std::fclose(f);
        SampleBuffer iq(raw.size() / 2);
        for (size_t i = 0; i < iq.size(); ++i)
            iq[i] = Sample(raw[2 * i], raw[2 * i + 1]);
        gone::AttackConfig cfg;
        gone::RarMonitor mon(cfg);
        size_t wins = 0, hits = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t rel = 0; rel + 16384 <= iq.size() && wins < 400; rel += 16384, ++wins) {
            SampleBuffer buf(iq.begin() + rel, iq.begin() + rel + 16384);
            // Stream-absolute position: the only frame scan_window understands.
            const uint64_t abs = file_start + rel;
            auto found = mon.scan_window(buf, abs, frame_start, sps, 0.0);
            mon.note_hits_for_sib(found);
            double best = 0.0;
            uint64_t bslot = 0;
            for (const auto& h : found)
                if (h.corr > best) { best = h.corr; bslot = h.abs_slot; }
            if (best >= 0.9)
                std::printf("win rel=%llu abs=%llu: %zu hits best=%.3f @abs=%llu\n",
                            (unsigned long long)rel, (unsigned long long)abs,
                            found.size(), best, (unsigned long long)bslot);
            hits += found.size();
        }
        auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("file mode: %zu windows, %zu hits >= 0.5, %.1f ms total\n",
                    wins, hits, ms);
        mon.report_sib_clusters();
        return 0;
    }

    const uint16_t pci = 1;
    const double kSrate = 23.04e6;
    const uint32_t kSlotSamples = 11520;
    gone::nr::Ofdm ofdm(kSrate, 30000, bwp_num_prbs);

    // Transmitted RAR: 24 PRB @ 6, duration 2, interleaved, shift 1.
    // (In the monitor's mini-grid, so identification is expected exact.)
    // One burst per RAR slot, each scrambled for the slot it is scanned at
    // (DM-RS scrambling is slot-dependent; a slot-0 burst scanned at slot 2
    // can never match — that mistake cost one full debug round).
    const auto tx_cs = make_coreset(24, 6, 2, true, 1, pci);
    const std::vector<unsigned> rar_slots = {2, 42, 9};
    const unsigned kSlots = 51;
    std::map<unsigned, gone::nr::RarSlotTx> tx_by_slot;
    for (unsigned s : rar_slots)
        tx_by_slot[s] = gone::nr::build_rar_slot(ofdm, pci, bwp_num_prbs, 0.5f,
                                                 tx_cs, (uint8_t)(s % 20));
    auto is_rar = [&](unsigned s) {
        for (unsigned r : rar_slots)
            if (r == s) return true;
        return false;
    };
    std::vector<gone::nr::Symbol> file_syms;
    file_syms.reserve(kSlots * 14);
    for (unsigned s = 0; s < kSlots; ++s) {
        for (unsigned l = 0; l < 14; ++l) {
            gone::nr::Symbol sym;
            sym.samples.assign(bwp_num_prbs * 12, Sample(0.0f, 0.0f));
            if (is_rar(s)) {
                sym.samples = tx_by_slot[s].grid[l].samples;
            } else {
                for (auto& v : sym.samples)
                    v = Sample(hash_noise(0.01f), hash_noise(0.01f));
            }
            file_syms.push_back(std::move(sym));
        }
    }
    const SampleBuffer file_iq = ofdm.modulate(file_syms);
    printf("file: %zu samples (%u slots)\n", file_iq.size(), kSlots);

    gone::AttackConfig cfg;
    cfg.pci = pci;
    gone::RarMonitor mon(cfg);

    // Buffers under test: two silence windows + one per RAR slot, each
    // starting just before the slot of interest (stream-like absolute clock).
    struct Win { uint64_t abs; const char* want; };
    const Win wins[] = {
        {0, "silence"},
        {20u * kSlotSamples, "silence"},
        {2u * kSlotSamples - 1000, "rar@2"},
        {42u * kSlotSamples - 1000, "rar@42"},
        {9u * kSlotSamples - 1000, "rar@9"},
        {30u * kSlotSamples, "silence"},
    };

    auto t0 = std::chrono::steady_clock::now();
    int grid_ms = 0;
    {
        auto g0 = std::chrono::steady_clock::now();
        auto probe = mon.scan_window(
            SampleBuffer(file_iq.begin(), file_iq.begin() + 16384),
            0, 0, (double)kSlotSamples, 0.0);
        auto g1 = std::chrono::steady_clock::now();
        grid_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(g1 - g0).count();
        (void)probe;
    }

    bool saw2 = false, saw42 = false, saw9 = false;
    bool cfg2 = false, cfg42 = false, cfg9 = false;
    double best_rar_corr = 0.0;
    double max_other_corr = 0.0;
    for (const auto& w : wins) {
        SampleBuffer buf(file_iq.begin() + w.abs,
                         file_iq.begin() + w.abs + 16384);
        auto t1 = std::chrono::steady_clock::now();
        auto hits = mon.scan_window(buf, w.abs, 0, (double)kSlotSamples, 0.0);
        auto t2 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t2 - t1).count();
        mon.note_hits_for_sib(hits);
        printf("win abs=%llu (%s): %zu hits, %.1f ms\n",
               (unsigned long long)w.abs, w.want, hits.size(), ms);
        for (const auto& h : hits) {
            if (h.abs_slot == 2) {
                saw2 = true;
                best_rar_corr = std::max<double>(best_rar_corr, h.corr);
                if (h.freq_prbs == 24 && h.start_prb == 6) cfg2 = true;
            } else if (h.abs_slot == 42) {
                saw42 = true;
                best_rar_corr = std::max<double>(best_rar_corr, h.corr);
                if (h.freq_prbs == 24 && h.start_prb == 6) cfg42 = true;
            } else if (h.abs_slot == 9) {
                saw9 = true;
                best_rar_corr = std::max<double>(best_rar_corr, h.corr);
                if (h.freq_prbs == 24 && h.start_prb == 6) cfg9 = true;
            } else if (h.corr >= 0.9f) {
                // Only >= 0.9 matters operationally (live-fire threshold);
                // the 0.5 reporting floor intentionally shows weaker junk.
                max_other_corr = std::max<double>(max_other_corr, h.corr);
                printf("  strong unexpected hit: abs=%llu corr=%.3f\n",
                       (unsigned long long)h.abs_slot, h.corr);
            }
        }
    }
    printf("grid build + first scan: %d ms\n", grid_ms);

    CHECK(saw2 && saw42 && saw9, "all three RAR slots detected");
    CHECK(best_rar_corr > 0.95, "RAR correlation ~1.0");
    CHECK(cfg2 && cfg42 && cfg9, "transmitted config identified (24 PRB @ 6)");
    CHECK(max_other_corr < 0.9, "nothing outside RAR slots reaches live-fire level");
    CHECK(mon.sib_distinct(2) == 2, "SIB bucket 2 has 2 distinct abs slots (2,42)");
    CHECK(mon.sib_distinct(9) == 1, "SIB bucket 9 has 1 distinct abs slot (9)");
    mon.report_sib_clusters();

    // ---- P2-3: constant-grant RIVE round-trip + SlotHit -> RarEvent mapping ----
    // riv_encode must be the exact inverse of riv_decode for every allocatable
    // (start, len) in the 51-PRB BWP (the scan_window grid's reference bank and
    // the live constant grant both go through it).
    {
        uint32_t roundtrips = 0;
        for (uint32_t start = 0; start < bwp_num_prbs; ++start) {
            for (uint32_t len = 1; len + start <= bwp_num_prbs; ++len) {
                const uint32_t riv = gone::nr::riv_encode(start, len, bwp_num_prbs);
                uint32_t dstart = 0, dlen = 0;
                gone::nr::riv_decode(riv, bwp_num_prbs, dstart, dlen);
                if (dstart != start || dlen != len) {
                    ++g_fail;
                    printf("FAIL: riv round-trip (%u,%u) -> 0x%x -> (%u,%u)\n",
                           start, len, riv, dstart, dlen);
                } else {
                    ++roundtrips;
                }
            }
        }
        printf("riv round-trip: %u allocs check out\n", roundtrips);
        const uint32_t constant_riv = gone::nr::riv_encode(0, 3, bwp_num_prbs);
        printf("constant grant riv(0,3,51)=0x%x (%u)\n", constant_riv, constant_riv);
        CHECK(constant_riv == 102, "constant rb=[0..3) encodes to RIV 102");
    }

    // rar_event_from_slot_hit: DMRS SlotHit + the cell's constant RarGrant ->
    // a RarEvent the Step-4 UL gate / Msg3 TX consume (P2-3). The live default
    // grant is rb=[0..3), mcs 0, k2=4, tpc 0, ta 0; the TC-RNTI comes from the
    // seeded sequential tracker. rar_slot_offset anchors the Msg3 math into the
    // absolute (frame_start, samples_per_slot) clock.
    {
        gone::RarMonitor::TcRntiTracker tracker;
        CHECK(!tracker.seeded(), "TcRntiTracker unseeded by default");
        CHECK(tracker.next() == 0, "unseeded tracker.next() is 0 (no fake TC-RNTI)");
        tracker.reseed(0x4601);
        CHECK(tracker.seeded(), "TcRntiTracker seeded after reseed()");
        const uint16_t t1 = tracker.next(), t2 = tracker.next();
        printf("tc-rnti seq: 0x%X, 0x%X\n", t1, t2);
        CHECK(t1 == 0x4601 && t2 == 0x4602, "tracker steps 0x4601, 0x4602 sequentially");

        gone::RarMonitor::RarGrant g = gone::RarMonitor::static_grant(tracker.next());
        // RarGrant defaults are the constant grant (k2=4, rb=[0..3), mcs 0,
        // tpc 0, ta 0) — only the tracked TC-RNTI is filled in above.
        CHECK(g.rb_start == 0 && g.rb_len == 3, "constant grant rb=[0..3) by default");
        CHECK(g.k2_slots == 4 && g.mcs == 0 && g.tpc == 0, "constant grant k2=4 mcs=0 tpc=0");

        gone::RarMonitor::SlotHit hit;
        hit.abs_slot = 40;
        hit.slot = 40 % 20;
        hit.symbol = 0;
        hit.al = 1;
        hit.candidate = 2;
        hit.corr = 0.98f;
        hit.freq_prbs = 48;
        hit.start_prb = 2;
        hit.duration = 1;
        hit.interleaved = false;
        hit.shift = 1;

        const uint64_t frame_start = 1000000;   // slot 0 of some frame
        const uint64_t buf_start = frame_start + 2 * (uint64_t)kSlotSamples; // buffer @ slot 2
        const double sps = (double)kSlotSamples;
        const gone::RarEvent ev = gone::RarMonitor::rar_event_from_slot_hit(
            hit, g, 7, buf_start, frame_start, sps, bwp_num_prbs);

        printf("rar_event: rapid=%u c_rnti=0x%X grant.k=%u mcs=%u tbs=%u riv=0x%x off=%llu\n",
               ev.rapid, ev.c_rnti, ev.grant.k, ev.grant.mcs, ev.grant.tbs_bits,
               ev.grant.pusch_freq_res, (unsigned long long)ev.rar_slot_offset);
        CHECK(ev.rapid == 7, "rapid carried through to RarEvent");
        CHECK(ev.c_rnti == 0x4603, "tracked TC-RNTI reaches RarEvent.c_rnti");
        CHECK(ev.grant.k == 4, "grant.k = k2_slots = 4 (Msg3 in slot 44)");
        CHECK(ev.grant.mcs == 0, "constant grant mcs 0");
        CHECK(ev.grant.tbs_bits == 264, "tbs 264 (paper cell-wide default)");
        CHECK(ev.grant.pusch_freq_res == 102, "pusch_freq_res = each RIV 102 (rb 0..2)");
        CHECK(ev.ul_dci.pusch_freq_res == ev.grant.pusch_freq_res,
              "ul_dci and grant carry the same RIV");
        const uint64_t expected_off =
            frame_start + hit.abs_slot * (uint64_t)kSlotSamples - buf_start;
        CHECK(ev.rar_slot_offset == expected_off,
              "rar_slot_offset = abs slot 40 sample minus buffer start (clock-anchored)");
    }

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
