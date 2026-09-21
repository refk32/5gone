#pragma once

#include "5gone/cell_sync.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_ofdm.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace gone::tool {

// One CORESET config from the sweep that produced a strong DM-RS correlation.
struct CoresetSweepHit {
    double corr = 0.0;          // best DM-RS correlation of this config
    uint32_t hits = 0;          // candidates above the sweep threshold
    uint8_t  offset = 0;        // slot-in-frame offset that matched
    uint8_t  slot = 0;          // slot-in-frame of the best hit
    uint8_t  symbol = 0;        // OFDM symbol within that slot
    uint8_t  al = 0;            // aggregation level of the best hit
    uint8_t  candidate = 0;
    uint16_t start_prb = 0;     // CORESET PRB offset within the BWP
    uint16_t freq_prbs = 0;     // CORESET PRB count
    uint8_t  duration = 0;      // CORESET OFDM-symbol span (1..3)
    uint16_t shift_index = 0;   // interleaver n_shift
    bool     interleaved = false;
};

struct DecodeParams {
    std::string path;
    double sample_rate = 23.04e6;
    uint32_t scs_hz    = 30000;
    uint16_t pci       = 1;
    uint16_t bwp_prbs  = 51;
    int pss_bin_shift  = 0;     // RX is carrier-tuned: SSB sits at bin_shift bin
    bool cell_sync     = true;
    size_t start_sample = 0;    // used when cell_sync==false
    size_t num_slots    = 0;    // 0 = entire capture from start point
    bool verbose        = true;
    uint32_t starting_slot = 0; // absolute slot-in-frame of the slice start (DM-RS)
    double cfo_hz          = 0.0; // residual CFO to correct before demod

    // --- CORESET sweep / override ---
    // Our decode chain assumes CORESET = full BWP, duration 1, anchored at PRB
    // 0, non-interleaved. A real gNB's CORESET #0 (pdcchConfigSIB1) may differ,
    // which makes the default systematically blind. `coreset_sweep` searches a
    // grid of CORESET options over the capture and re-decodes with the winner.
    bool coreset_sweep     = false;
    size_t sweep_slots     = 100;  // window (slots) the sweep scans
    float  sweep_min_corr  = 0.6f; // only accept a config at/above this corr
    bool   sweep_wide      = false;// add 32/24-PRB CORESETs and duration 2
    // Single explicit CORESET override (when !coreset_sweep):
    uint16_t coreset_prbs  = 0;    // PRB count; 0 = BWP size
    uint8_t  coreset_dur   = 0;    // 0 = 1
    uint16_t coreset_offset = 0;   // start_prb within the BWP
    uint16_t coreset_shift = 0;    // interleaver shift
    bool     coreset_interleaved = false;
};

struct DecodeResult {
    bool ok = false;            // file parsed successfully
    std::string error;
    bool locked = false;        // CellSync found the SSB
    SsbResult ssb;
    std::vector<nr::RarDciObs> rars;
    size_t total_samples    = 0;
    size_t decoded_samples  = 0;
    size_t decoded_start    = 0;

    // CORESET sweep results (coreset_sweep=true). `sweep` is best-first;
    // `coreset_applied` says the winner was strong enough to re-decode with.
    bool sweep_completed = false;
    bool sweep_accepted  = false;
    std::vector<CoresetSweepHit> sweep;
    size_t sweep_configs = 0;   // number of CORESET variants tried
};

// Build the Coreset for an explicit (non-sweep) override, or the default.
inline nr::Coreset coreset_from_params(const DecodeParams& p)
{
    nr::Coreset cs;
    cs.control_resourceset_id = 1;
    cs.frequency_domain_resources = p.coreset_prbs ? p.coreset_prbs : p.bwp_prbs;
    cs.start_prb = p.coreset_offset;
    cs.duration = p.coreset_dur ? p.coreset_dur : 1;
    cs.cce_reg_mapping_type = p.coreset_interleaved ? "interleaved" : "non-interleaved";
    cs.reg_bundlesize = 6;
    cs.interleaver_size = 2;
    cs.shift_index = p.coreset_shift;
    cs.cell_id = p.pci;
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = 14;
    cs.num_slots_per_frame = 20;
    cs.candidates_search_space = {1, 2, 4, 8, 16};
    return cs;
}

// Build the Coreset for a sweep hit (shared by the accept + fallback paths).
inline nr::Coreset coreset_from_hit(const CoresetSweepHit& h, const DecodeParams& p)
{
    nr::Coreset cs;
    cs.control_resourceset_id = 1;
    cs.frequency_domain_resources = h.freq_prbs;
    cs.start_prb = h.start_prb;
    cs.duration = h.duration;
    cs.cce_reg_mapping_type = h.interleaved ? "interleaved" : "non-interleaved";
    cs.reg_bundlesize = 6;
    cs.interleaver_size = 2;
    cs.shift_index = h.shift_index;
    cs.cell_id = p.pci;
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = 14;
    cs.num_slots_per_frame = 20;
    cs.candidates_search_space = {1, 2, 4, 8, 16};
    return cs;
}

// Enumerate the CORESET config grid. `wide` adds the smaller (32/24-PRB, or
// 38.213-type off-center) CORESETs plus 2-symbol durations; the lean grid keeps
// the runtime of a full sweep manageable.
inline std::vector<nr::Coreset> coreset_sweep_grid(const DecodeParams& p, bool wide)
{
    std::vector<nr::Coreset> out;
    const uint16_t bwp = p.bwp_prbs;
    std::vector<uint16_t> freqs = wide ? std::vector<uint16_t>{bwp, 48, 32, 24}
                                       : std::vector<uint16_t>{bwp, 48};
    std::vector<uint8_t> durs = wide ? std::vector<uint8_t>{1, 2, 3} : std::vector<uint8_t>{1};
    std::vector<bool> maps = {false, true};   // non-interleaved, interleaved

    for (uint16_t f : freqs) {
        if (f > bwp) continue;
        // CORESET start_prb must keep the CORESET inside the BWP. Step 1, not
        // 3: the 38.213 CORESET0 tables include non-multiple-of-3 offsets, and
        // a step-3 grid turns a true off-grid PDCCH into eternal 0.89 AL1-only
        // sidelobes at neighboring points (observed across three captures).
        const uint16_t max_off = bwp - f;
        for (uint16_t off = 0; off <= max_off; ++off) {
            for (uint8_t dur : durs) {
                for (bool interleaved : maps) {
                    for (uint16_t shift = 0; shift < 3; ++shift) {
                        nr::Coreset cs;
                        cs.control_resourceset_id = 1;
                        cs.frequency_domain_resources = f;
                        cs.start_prb = off;
                        cs.duration = dur;
                        cs.cce_reg_mapping_type = interleaved ? "interleaved" : "non-interleaved";
                        cs.reg_bundlesize = 6;
                        cs.interleaver_size = 2;
                        cs.shift_index = shift;
                        cs.cell_id = p.pci;
                        cs.starting_ofdm_symbol_within_slot = 0;
                        cs.num_symbols_per_slot = 14;
                        cs.num_slots_per_frame = 20;
                        cs.candidates_search_space = {1, 2, 4, 8, 16};
                        out.push_back(cs);
                    }
                }
            }
        }
    }
    return out;
}

inline DecodeResult decode_capture(const DecodeParams& p)
{
    DecodeResult r{};

    // --- Load interleaved cf32 (two floats per complex sample, 8 bytes each) ---
    {
        std::ifstream f(p.path, std::ios::binary | std::ios::ate);
        if (!f) { r.error = "cannot open " + p.path; return r; }
        const auto sz = f.tellg();
        if (sz % 8 != 0 && sz != 0) {
            r.error = "file size (" + std::to_string(static_cast<long long>(sz))
                      + ") not a multiple of 8";
            return r;
        }
        r.total_samples = static_cast<size_t>(sz) / 8;
        f.seekg(0);
        if (r.total_samples == 0) { r.ok = true; return r; }
        std::vector<float> raw(r.total_samples * 2);
        f.read(reinterpret_cast<char*>(raw.data()), sz);
        SampleBuffer iq(r.total_samples);
        for (size_t i = 0; i < r.total_samples; ++i)
            iq[i] = Sample(raw[i * 2], raw[i * 2 + 1]);
        r.ok = true;

        // --- CellSync (optional) ---
        size_t slice_start = p.start_sample;
        size_t slice_len   = r.total_samples - slice_start;

        if (p.cell_sync) {
            AttackConfig cfg;   // defaults: pci=1, scs=30, srate=23.04MHz
            cfg.sample_rate = p.sample_rate;
            cfg.scs_khz     = static_cast<uint8_t>(p.scs_hz / 1000u);
            cfg.pci         = p.pci;
            cfg.pss_bin_shift = p.pss_bin_shift;
            CellSync sync(cfg);
            SsbResult ssb;
            if (sync.find_ssb(iq, ssb)) {
                r.locked = true;
                r.ssb    = ssb;
                slice_start = ssb.slot_start;
                slice_len   = r.total_samples - slice_start;
            }
        }

        const nr::Ofdm ofdm(p.sample_rate, static_cast<double>(p.scs_hz), p.bwp_prbs);
        const size_t slot_samples = ofdm.samples_per_slot();

        size_t decode_len = slice_len;
        if (p.num_slots > 0) {
            const size_t slot_span = slot_samples * p.num_slots;
            if (slot_span < decode_len) decode_len = slot_span;
        }
        if (slice_start + decode_len > r.total_samples)
            decode_len = r.total_samples - slice_start;
        r.decoded_samples = decode_len;
        r.decoded_start   = slice_start;

        SampleBuffer slice(iq.begin() + static_cast<std::ptrdiff_t>(slice_start),
                           iq.begin() + static_cast<std::ptrdiff_t>(slice_start + decode_len));

        nr::RarDecoder dec(p.sample_rate, p.scs_hz, p.pci, p.bwp_prbs, p.verbose);

        if (p.coreset_sweep) {
            // ---- CORESET blind sweep ----
            // Stage 1: demodulate a bounded window ONCE (slot labels 0..19
            // cycling), then re-scan it under many CORESET configs and slot
            // offsets. The offset that aligns DM-RS scrambling tells us the
            // absolute slot number of the slice start; the config that gives
            // ~1.0 correlation tells us the gNB's real CORESET.
            r.sweep_completed = true;
            const size_t sweep_len = slot_samples *
                                     (p.sweep_slots ? p.sweep_slots : 100);
            size_t win_len = (sweep_len < decode_len) ? sweep_len : decode_len;
            win_len -= win_len % slot_samples;   // whole slots (slice starts slot-aligned)
            const SampleBuffer win(slice.begin(),
                                   slice.begin() + static_cast<std::ptrdiff_t>(win_len));
            auto symbols = dec.demodulate(win, 0);
            std::vector<uint8_t> orig_slot(symbols.size());
            for (size_t i = 0; i < symbols.size(); ++i) orig_slot[i] = symbols[i].slot_index;

            auto grid = coreset_sweep_grid(p, p.sweep_wide);
            r.sweep_configs = grid.size();

            std::vector<CoresetSweepHit> hits;
            hits.reserve(grid.size() * 4);
            for (const nr::Coreset& cs : grid) {
                dec.set_coreset(cs);
                for (uint32_t off20 = 0; off20 < 20; ++off20) {
                    for (size_t i = 0; i < symbols.size(); ++i)
                        symbols[i].slot_index = static_cast<uint8_t>((orig_slot[i] + off20) % 20);
                    auto found = dec.scan_pdcch(symbols);
                    CoresetSweepHit best;
                    best.offset = static_cast<uint8_t>(off20);
                    best.start_prb = cs.start_prb;
                    best.freq_prbs = cs.frequency_domain_resources;
                    best.duration = cs.duration;
                    best.shift_index = cs.shift_index;
                    best.interleaved = cs.cce_reg_mapping_type == "interleaved";
                    for (const auto& d : found) {
                        if (d.correlation >= p.sweep_min_corr) best.hits++;
                        if (d.correlation > best.corr) {
                            best.corr = d.correlation;
                            best.slot = d.n_slot;
                            best.symbol = d.n_ofdm;
                            best.al = d.found_aggregation_level;
                            best.candidate = d.found_candidate;
                        }
                    }
                    if (best.corr > 0.0) hits.push_back(best);
                }
            }

            std::sort(hits.begin(), hits.end(),
                      [](const CoresetSweepHit& a, const CoresetSweepHit& b) { return a.corr > b.corr; });
            const size_t ntop = std::min<size_t>(12, hits.size());
            r.sweep.assign(hits.begin(), hits.begin() + ntop);

            CoresetSweepHit best{};
            if (!hits.empty()) best = hits.front();

            // Acceptance requires a STRONG hit (corr >= 0.75) or a BROAD one
            // (corr >= min AND hits >= 5 candidates). A lone 0.6x AL1 hit is
            // the noise-floor maximum over ~100M correlations (observed floor
            // 0.65-0.69 with hits=1-2); accepting it then decoding nothing is
            // worse than reporting no detection.
            const bool strong = best.corr >= 0.75;
            const bool broad =
                best.corr >= p.sweep_min_corr && best.hits >= 5;
            r.sweep_accepted = false;
            if (strong || broad) {
                dec.set_coreset(coreset_from_hit(best, p));
                r.sweep_accepted = true;
                // The winning offset is the absolute slot-in-frame of the slice
                // start, so a full decode relabels the whole slice identically.
                r.rars = dec.decode(slice, best.offset, p.cfo_hz);
            } else if (!hits.empty()) {
                // Below the accept gate — but the gate is about labels, not
                // evidence: a weak-but-true RAR dies under the default
                // full-BWP CORESET just as surely. Decode with the best
                // config, then also with the default for comparison (deduped).
                dec.set_coreset(coreset_from_hit(best, p));
                r.rars = dec.decode(slice, best.offset, p.cfo_hz);
                nr::RarDecoder dec0(p.sample_rate, p.scs_hz, p.pci, p.bwp_prbs,
                                    p.verbose);
                for (const auto& o : dec0.decode(slice, p.starting_slot, p.cfo_hz)) {
                    bool dup = false;
                    for (const auto& e : r.rars)
                        if (e.slot == o.slot && e.symbol == o.symbol &&
                            e.aggregation_level == o.aggregation_level &&
                            e.candidate == o.candidate) { dup = true; break; }
                    if (!dup) r.rars.push_back(o);
                }
            } else {
                // Nothing strong enough: fall back to the default full-BWP
                // CORESET so the output reproduces the old (blind) behaviour
                // for comparison.
                r.rars = dec.decode(slice, p.starting_slot, p.cfo_hz);
            }
        } else {
            if (p.coreset_prbs || p.coreset_dur || p.coreset_offset || p.coreset_shift || p.coreset_interleaved) {
                dec.set_coreset(coreset_from_params(p));
            }
            r.rars = dec.decode(slice, p.starting_slot, p.cfo_hz);
        }
    }
    return r;
}

} // namespace gone::tool