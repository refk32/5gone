#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_capture.hpp"
#include "tools/decode_capture.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gone;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string pth = tmp + "/5gone_decode_test.cf32";

    const uint16_t pci = 1;
    // synth_capture(pci, slots) = [noise preamble] + `slots` SSB slots.
    auto cap = nr::synth_capture(pci, 2);          // preamble + 2 SSB slots
    CHECK(nr::write_cf32(pth, cap), "wrote cf32");

    nr::CaptureInfo info = nr::scan_cf32(pth);
    printf("[cap] samples=%zu info.samples=%zu ok=%d\n", cap.size(), info.samples, (int)info.ok);
    CHECK(info.ok, "capture scanned");
    CHECK(info.samples == cap.size(), "scan reports correct sample count");

    auto iq = nr::read_cf32(pth);
    CHECK(iq.size() == cap.size(), "read_cf32 restores all samples");

    // PASS 1 — cell_sync OFF, explicit 2-slot window from 0.
    auto d1 = tool::decode_capture({
        .path = pth, .sample_rate = 23.04e6, .scs_hz = 30000, .pci = pci,
        .bwp_prbs = 51, .cell_sync = false, .start_sample = 0,
        .num_slots = 2, .verbose = false,
    });
    printf("[pass1] ok=%d samples=%zu slots=2\n", d1.ok, d1.decoded_samples);
    CHECK(d1.ok, "override captures decode");
    CHECK(d1.decoded_samples == 2 * 11514u, "2-slot window decoded");

    // PASS 2 — cell_sync ON with a real SSB slot mid-buffer.
    auto cap2 = nr::synth_capture(0, 0);            // pure noise preamble
    auto tail = nr::synth_capture(pci, 2);          // preamble + 2 SSB slots
    cap2.insert(cap2.end(), tail.begin() + 900, tail.end());  // append just the SSB slots
    CHECK(nr::write_cf32(pth, cap2), "wrote pass2 capture");

    auto d2 = tool::decode_capture({
        .path = pth, .sample_rate = 23.04e6, .scs_hz = 30000, .pci = pci,
        .bwp_prbs = 51, .cell_sync = true, .start_sample = 0,
        .num_slots = 0, .verbose = true,
    });
    printf("[pass2] ok=%d locked=%d slot_start=%zu strength=%.3f samples=%zu\n",
           d2.ok, d2.locked, (size_t)d2.ssb.slot_start, d2.ssb.strength, d2.decoded_samples);
    CHECK(d2.ok, "noise-then-ssb capture parses");
    CHECK(d2.locked, "CellSync found the SSB in the capture");
    CHECK(d2.ssb.strength > 0.70f, "SSB strength high");

    // PASS 3 — pure noise must not lock, still parses, 0 RAR.
    CHECK(nr::write_cf32(pth, nr::synth_capture(0, 0)), "wrote noise capture");
    auto d3 = tool::decode_capture({
        .path = pth, .sample_rate = 23.04e6, .scs_hz = 30000, .pci = pci,
        .bwp_prbs = 51, .cell_sync = true, .start_sample = 0,
        .num_slots = 4, .verbose = false,
    });
    printf("[pass3] ok=%d locked=%d rars=%zu\n", d3.ok, d3.locked, d3.rars.size());
    CHECK(d3.ok, "pure-noise capture parses");
    CHECK(!d3.locked, "pure noise does not lock");
    CHECK(d3.rars.empty(), "no RAR on noise");

    // PASS 4 — missing file.
    auto d4 = tool::decode_capture({
        .path = tmp + "/does_not_exist.cf32", .sample_rate = 23.04e6,
        .scs_hz = 30000, .pci = pci, .bwp_prbs = 51,
        .cell_sync = true, .start_sample = 0, .num_slots = 0, .verbose = false,
    });
    CHECK(!d4.ok && !d4.error.empty(), "missing file reported");

    std::remove(pth.c_str());
    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}