#include "tools/decode_capture.hpp"

#include "5gone/nr_constants.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// 5gone-decode — decode a single raw IQ capture file with the 5Gone RAR decoder
// (and optionally the real CellSync). Console tool for the live-decode PoC.
//
// Usage:
//   5gone-decode <file.cf32> [--rate HZ] [--scs KHZ] [--pci N]
//                [--prbs N] [--shift BIN] [--no-sync] [--start SAMP] [--slots N]
//                [--slot N] [--cfo HZ] [--coreset-sweep] [--sweep-slots N]
//                [--sweep-min X] [--sweep-wide] [--coreset-prbs N]
//                [--coreset-dur N] [--coreset-offset N] [--coreset-shift N]
//                [--coreset-map non|int] [--quiet]
//
// Reads interleaved float32 I/Q (8 bytes per {I,Q} sample), CellSync- locks on
// the SSB (unless --no-sync), and runs the DL decode chain (PDCCH DM-RS
// correlation -> RAR DCI -> PDSCH -> MAC RAR parse) over the capture.
//
// `--slot N` names the absolute slot-in-frame (0..19) of the decode slice
// start (i.e. the PSS slot number when CellSync slicing). DM-RS scrambling is
// slot-dependent, so the wrong N yields no RAR PDCCH hit even when the timing
// is locked. Sweep N 0..19 to find the gNB's true slot numbering.
// `--cfo HZ` removes a residual carrier offset before demodulating.
//
// `--coreset-sweep`: our default CORESET assumption (full-BWP, 1 symbol, PRB 0,
// non-interleaved) may not match the gNB's real CORESET #0 (from
// pdcchConfigSIB1). The sweep re-scans the window under a grid of CORESET
// configurations AND all 20 slot offsets, reports the strongest DM-RS
// correlations, and re-decodes with the best config. If no config scores at or
// above `--sweep-min`, the default CORESET is used (so the output still shows
// what the old, blind decode saw).
//
// Returns 0 on success (even if no RAR is found — that just means the cell was
// idle), 1 on a CLI/file error, 2 if the file parsed but CellSync couldn't lock.
static void usage(const char* argv0)
{
    std::printf(
        "usage: %s <file.cf32> [options]\n"
        "\n"
        "  --rate   HZ      sample rate            (default 23.04e6)\n"
        "  --scs    KHZ     subcarrier spacing kHz (default 30)\n"
        "  --pci    N       physical cell id       (default 1)\n"
        "  --prbs   N       BWP PRBs               (default 51)\n"
        "  --shift  BIN     pss_bin_shift (SSB bin offset vs tuned carrier,\n"
        "                    e.g. -187 for a carrier-tuned capture)\n"
        "  --no-sync        skip CellSync SSB lock; decode from --start\n"
        "  --start  SAMP    decode window start    (default 0)\n"
        "  --slots  N        decode N slots from start (default whole file)\n"
        "  --slot   N        absolute slot-in-frame of slice start (0..19) for\n"
        "                    DM-RS scrambling; sweep 0..19 to find gNB numbering\n"
        "  --cfo    HZ       residual CFO to correct before demodulating\n"
        "  --coreset-sweep   search CORESET config x slot-offset grid (see readme)\n"
        "  --sweep-slots N   slots the sweep scans from slice start (default 100)\n"
        "  --sweep-min X     accept a CORESET config only >= X corr  (default 0.60)\n"
        "  --sweep-wide      also try 32/24-PRB and 2-symbol CORESETs (slower)\n"
        "  --coreset-prbs N  explicit CORESET PRB count  (default BWP=%u)\n"
        "  --coreset-dur N   explicit CORESET symbols   (default 1)\n"
        "  --coreset-offset N explicit CORESET start PRB (default 0)\n"
        "  --coreset-shift N explicit interleaver shift (default 0)\n"
        "  --coreset-map M   explicit mapping: non | int (default non)\n"
        "  --quiet           suppress per-RAR logging\n"
        "\n"
        "exit codes: 0 ok (no RAR = idle cell), 1 file/CLI error, 2 not locked\n",
        argv0, (unsigned)gone::nr::bwp_num_prbs);
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        usage(argv[0]);
        return 0;
    }

    gone::tool::DecodeParams p;
    p.path = argv[1];
    std::printf("[build] 5gone-decode built %s %s (predates-sync = stale binary)\n",
                __DATE__, __TIME__);

    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        if      (k == "--no-sync")      { p.cell_sync      = false; continue; }
        else if (k == "--quiet")        { p.verbose        = false; continue; }
        else if (k == "--coreset-sweep"){ p.coreset_sweep  = true;  continue; }
        else if (k == "--sweep-wide")   { p.sweep_wide     = true;  continue; }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "5gone-decode: option %s needs a value\n", k.c_str());
            return 1;
        }
        const std::string v = argv[++i];
        if      (k == "--rate")  p.sample_rate = std::atof(v.c_str());
        else if (k == "--scs")   p.scs_hz      = (uint32_t)std::atoi(v.c_str()) * 1000u;
        else if (k == "--pci")   p.pci         = (uint16_t)std::atoi(v.c_str());
        else if (k == "--prbs")  p.bwp_prbs    = (uint16_t)std::atoi(v.c_str());
        else if (k == "--shift") p.pss_bin_shift = std::atoi(v.c_str());
        else if (k == "--start") { p.start_sample = (size_t)std::atoll(v.c_str()); }
        else if (k == "--slots") { p.num_slots    = (size_t)std::atoll(v.c_str()); }
        else if (k == "--slot")  { p.starting_slot = (uint32_t)std::atoi(v.c_str()); }
        else if (k == "--cfo")   { p.cfo_hz        = std::atof(v.c_str()); }
        else if (k == "--sweep-slots")   { p.sweep_slots = (size_t)std::atoll(v.c_str()); }
        else if (k == "--sweep-min")     { p.sweep_min_corr = (float)std::atof(v.c_str()); }
        else if (k == "--coreset-prbs")  { p.coreset_prbs = (uint16_t)std::atoi(v.c_str()); }
        else if (k == "--coreset-dur")   { p.coreset_dur = (uint8_t)std::atoi(v.c_str()); }
        else if (k == "--coreset-offset"){ p.coreset_offset = (uint16_t)std::atoi(v.c_str()); }
        else if (k == "--coreset-shift") { p.coreset_shift = (uint16_t)std::atoi(v.c_str()); }
        else if (k == "--coreset-map") {
            if (v == "int" || v == "interleaved") p.coreset_interleaved = true;
            else if (v == "non" || v == "non-interleaved") p.coreset_interleaved = false;
            else {
                std::fprintf(stderr, "5gone-decode: --coreset-map must be non or int\n");
                return 1;
            }
        }
        else {
            std::fprintf(stderr, "unknown option: %s\n", k.c_str());
            return 1;
        }
    }

    auto r = gone::tool::decode_capture(p);
    if (!r.ok) {
        std::fprintf(stderr, "5gone-decode: %s\n", r.error.c_str());
        return 1;
    }
    if (p.cell_sync && !r.locked) {
        std::fprintf(stderr,
                     "5gone-decode: capture parsed (%zu samples) but no SSB found "
                     "(cell not present or wrong --pci/--rate/--scs)\n",
                     r.total_samples);
        return 2;
    }

    std::printf("[5gone-decode] %s: %zu samples (%.2f ms @ %.0f Hz), scs %u kHz, "
                "PCI %u, %u PRB\n",
                p.path.c_str(), r.total_samples,
                r.total_samples / p.sample_rate * 1000.0, p.sample_rate,
                p.scs_hz / 1000u, (unsigned)p.pci, (unsigned)p.bwp_prbs);
    if (p.cell_sync)
        std::printf("[5gone-decode] cell-sync: locked slot@sample %zu, "
                    "strength %.3f, CFO %.0f Hz, int-subcarrier %+d\n",
                    (size_t)r.ssb.slot_start, r.ssb.strength,
                    r.ssb.cfo_hz, r.ssb.subcarrier_offset);

    if (r.sweep_completed) {
        std::printf("[5gone-decode] CORESET sweep: %zu configs x 20 slot-offsets "
                    "over %zu slots -> accepted=%s",
                    r.sweep_configs, p.sweep_slots ? p.sweep_slots : 100,
                    r.sweep_accepted ? "yes" : "no");
        if (!r.sweep_accepted) {
            // Say exactly why: the accept gate needs corr >= 0.75 or
            // (corr >= min AND hits >= 5); a lone 0.6x AL1 max is the
            // noise floor over ~100M correlations, not a detection.
            if (!r.sweep.empty()) {
                const auto& b = r.sweep.front();
                std::printf(" (best corr=%.4f hits=%u; need >=0.75 or >=%.2f with >=5 hits)",
                            b.corr, (unsigned)b.hits, p.sweep_min_corr);
            } else {
                std::printf(" (no candidates above zero)");
            }
        }
        std::printf("\n");
        for (size_t i = 0; i < r.sweep.size(); ++i) {
            const auto& h = r.sweep[i];
            std::printf("  #%02zu corr=%.4f hits=%u %s PRBs=%u@%u dur=%u shift=%u "
                        "slot=%u sym=%u AL=%u cand=%u (offset=%u)\n",
                        i, h.corr, (unsigned)h.hits,
                        h.interleaved ? "interleaved" : "non-interleaved",
                        (unsigned)h.freq_prbs, (unsigned)h.start_prb,
                        (unsigned)h.duration, (unsigned)h.shift_index,
                        (unsigned)h.slot, (unsigned)h.symbol,
                        (unsigned)h.al, (unsigned)h.candidate,
                        (unsigned)h.offset);
        }
    }

    std::printf("[5gone-decode] decoded %zu samples -> %zu RAR DCI observation(s)\n",
                r.decoded_samples, r.rars.size());
    if (r.sweep_completed && r.sweep_accepted && r.rars.empty() && !r.sweep.empty()) {
        // The sweep winner still produced no DCI: the final decode enforces
        // per-AL correlation thresholds (AL1 needs ~0.9), so a weak accept
        // dies silently here without this note.
        const auto& b = r.sweep.front();
        std::printf("[5gone-decode] note: accepted config (corr=%.4f %s PRBs=%u@%u "
                    "dur=%u shift=%u) yielded no DCI — below the decoder's per-AL "
                    "correlation thresholds; treat as no confident detection\n",
                    b.corr, b.interleaved ? "interleaved" : "non-interleaved",
                    (unsigned)b.freq_prbs, (unsigned)b.start_prb,
                    (unsigned)b.duration, (unsigned)b.shift_index);
    }
    return 0;
}