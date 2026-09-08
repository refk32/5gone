#pragma once

#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_pn.hpp
 * =========
 * A "pseudo-random" bit sequence generator used all over 5G NR:
 *   - generating the PDCCH DM-RS reference signal (Step 4)
 *   - scrambling/descrambling PDCCH payload bits (Step 8)
 *
 * The standard calls this the "Gold sequence" (TS 38.211 section 5.2.1).
 * For our purposes: give it a number c_init and a length, and it returns
 * that many bits (0/1) that look random but are deterministic (the SAME
 * c_init always gives the SAME bits, on the phone AND on the network).
 *
 * That determinism is exactly what lets a receiver reconstruct the DM-RS:
 * if we know c_init we can create the exact reference the gNB transmitted.
 */
class PnSequence {
public:
    // Returns `seq_length` bits of the pseudo-random sequence, seeded by c_init.
    static std::vector<uint8_t> pseudo_random_sequence(int seq_length, uint32_t c_init);
};

} // namespace gone::nr
