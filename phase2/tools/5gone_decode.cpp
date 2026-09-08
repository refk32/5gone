#include "tools/decode_capture.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// 5gone-decode — decode a single raw IQ capture file with the 5Gone RAR decoder
// (and optionally the real CellSync). Console tool for the live-decode PoC.
//
// Usage:
//   5gone-decode <file.cf32> [--rate HZ] [--scs KHZ] [--pci N]
//                [--prbs N] [--no-sync] [--start SAMP] [--slots N]
//                [--quiet]
//
// Reads interleaved float32 I/Q (8 bytes per {I,Q} sample), CellSync- locks on
// the SSB (unless --no-sync), and runs the DL decode chain (PDCCH DM-RS
// correlation -> RAR DCI -> PDSCH -> MAC RAR parse) over the capture.
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
        "  --no-sync        skip CellSync SSB lock; decode from --start\n"
        "  --start  SAMP    decode window start    (default 0)\n"
        "  --slots  N       decode N slots from start (default whole file)\n"
        "  --quiet          suppress per-RAR logging\n"
        "\n"
        "exit codes: 0 ok (no RAR = idle cell), 1 file/CLI error, 2 not locked\n",
        argv0);
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

    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if      (k == "--rate")  p.sample_rate = std::atof(v.c_str());
        else if (k == "--scs")   p.scs_hz      = (uint32_t)std::atoi(v.c_str()) * 1000u;
        else if (k == "--pci")   p.pci         = (uint16_t)std::atoi(v.c_str());
        else if (k == "--prbs")  p.bwp_prbs    = (uint16_t)std::atoi(v.c_str());
        else if (k == "--no-sync") { p.cell_sync = false; --i; }
        else if (k == "--quiet") { p.verbose    = false;  --i; }
        else if (k == "--start") { p.start_sample = (size_t)std::atoll(v.c_str()); }
        else if (k == "--slots") { p.num_slots    = (size_t)std::atoll(v.c_str()); }
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
    std::printf("[5gone-decode] decoded %zu samples -> %zu RAR DCI observation(s)\n",
                r.decoded_samples, r.rars.size());
    return 0;
}