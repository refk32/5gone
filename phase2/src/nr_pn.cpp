#include "5gone/nr_pn.hpp"
#include "5gone/nr_constants.hpp"   // for Nc and gold_sequence_length
#include <cstddef>

namespace gone::nr {

std::vector<uint8_t> PnSequence::pseudo_random_sequence(int seq_length, uint32_t c_init)
{
    // The Gold sequence is built from two binary m-sequences x1 and x2,
    // each pushed forward by Nc (=1600) samples. We need enough terms so
    // that after shifting by Nc we still have `seq_length` usable bits.
    const int size_x = seq_length + gold_sequence_length + Nc;

    std::vector<uint8_t> x1(static_cast<size_t>(size_x), 0);
    std::vector<uint8_t> x2(static_cast<size_t>(size_x), 0);
    std::vector<uint8_t> c(static_cast<size_t>(seq_length), 0);

    // --- Initialize x1 ---
    // x1 is ALWAYS seeded with x1(0) = 1, everything else 0.
    x1[0] = 1;

    // --- Initialize x2 ---
    // x2's seed is the binary representation of c_init (LSB first).
    for (int n = 0; n < gold_sequence_length; ++n) {
        x2[n] = (c_init >> n) & 0x1;
    }

    // --- Advance both sequences forward ---
    // Each new bit is a XOR (mod-2 sum) of earlier bits, per the standard's
    // two feedback polynomials:
    //   x1(n+31) = x1(n+3) + x1(n)
    //   x2(n+31) = x2(n+3) + x2(n+2) + x2(n+1) + x2(n)
    for (int n = 0; n < (Nc + seq_length); ++n) {
        x1[n + 31] = (x1[n + 3] + x1[n]) & 0x1;
        x2[n + 31] = (x2[n + 3] + x2[n + 2] + x2[n + 1] + x2[n]) & 0x1;
    }

    // --- Combine into the output sequence ---
    // c(n) = x1(n + Nc) XOR x2(n + Nc). Because Nc=1600 we skip the first
    // 1600 terms of each, giving the final pseudo-random bit stream.
    for (int n = 0; n < seq_length; ++n) {
        c[n] = (x1[n + Nc] + x2[n + Nc]) & 0x1;
    }

    return c;
}

} // namespace gone::nr
