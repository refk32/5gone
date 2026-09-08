#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_dmrs.hpp
 * ===========
 * Generates the PDCCH DM-RS (Demodulation Reference Signal).
 *
 * What is DM-RS and why do we need it?
 * ------------------------------------
 * The gNB intersperses KNOWN symbols (DM-RS) among the PDCCH data symbols.
 * If we can generate an EXACT copy of those known symbols locally, we can
 * correlate them against the received signal to:
 *   1. FIND where the PDCCH (and a RAR) is (Step 7), and
 *   2. provide the reference for channel estimation (Step 3).
 *
 * The DM-RS is deterministic: its bits come from the Gold sequence (Step 2),
 * seeded by a special number c_init. c_init depends on:
 *   - n_id      : the PDCCH DM-RS scrambling ID (for a common search space
 *                 this is just the cell id = PCI).
 *   - n_slot    : slot number within the frame.
 *   - n_ofdm    : OFDM symbol number within the slot.
 *
 * Formula (TS 38.211 7.4.1.3.1):
 *   c_init = (((num_symbols_per_slot * n_slot + n_ofdm + 1) << 17)
 *             * (2*n_id + 1) + 2*n_id) mod 2^32
 *
 * The bits then get QPSK-modulated into complex symbols (same idea as the
 * data, but the bits are known to us).
 */
class Dmrs {
public:
    // Gold-sequence BITS (0/1) of length pdcch_dmrs_length_bits.
    static std::vector<uint8_t> generate_pdcch_dmrs_seq(uint16_t n_id,
                                                        uint8_t  n_slot,
                                                        uint8_t  n_ofdm,
                                                        uint8_t  num_symbols_per_slot,
                                                        int      pdcch_dmrs_length_bits);

    // QPSK-modulated complex reference symbols (floor(length/2) of them).
    static std::vector<std::complex<float>> generate_pdcch_dmrs_symb(uint16_t n_id,
                                                                     uint8_t  n_slot,
                                                                     uint8_t  n_ofdm,
                                                                     uint8_t  num_symbols_per_slot,
                                                                     int      pdcch_dmrs_length_bits);
};

/*
 * QPSK modulation of a 2-bit value -> one complex symbol.
 *
 * 3GPP Table 5.1.4-1 (QPSK):
 *   bits 00 ->  +1/√2 + j·1/√2   ("+,+"  quadrant)
 *   bits 01 ->  +1/√2 - j·1/√2   ("+,-")
 *   bits 10 ->  -1/√2 + j·1/√2   ("-,+")
 *   bits 11 ->  -1/√2 - j·1/√2   ("-,-")
 *
 * `bits` is the packed 2-bit value (b0 + 2*b1). This must EXACTLY match what
 * the gNB transmits, otherwise our local reference won't correlate. (Verified
 * to match both TS 38.211 and 5GSniffer's liquid-dsp QPSK mapping.)
 */
inline std::complex<float> qpsk_modulate(uint8_t bits)
{
    // bit0 -> I (real), bit1 -> Q (imag). sign -1 if the bit is set.
    const float sign_i = (bits & 0x2) ? -1.0f : 1.0f;
    const float sign_q = (bits & 0x1) ? -1.0f : 1.0f;
    constexpr float scale = 1.0f / 1.41421356f;  // 1/sqrt(2): unit-energy
    return {sign_i * scale, sign_q * scale};
}

} // namespace gone::nr