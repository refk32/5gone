#include "5gone/nr_dmrs.hpp"
#include "5gone/nr_pn.hpp"

#include <cmath>

namespace gone::nr {

std::vector<uint8_t> Dmrs::generate_pdcch_dmrs_seq(uint16_t n_id,
                                                   uint8_t  n_slot,
                                                   uint8_t  n_ofdm,
                                                   uint8_t  num_symbols_per_slot,
                                                   int      pdcch_dmrs_length_bits)
{
    // Compute the seed c_init per TS 38.211 7.4.1.3.1 (see header comment).
    // The & 0xFFFFFFFF keeps it within 32 bits (the standard says 'mod 2^32').
    const uint32_t c_init =
        (((static_cast<uint32_t>(num_symbols_per_slot) * n_slot + n_ofdm + 1) << 17)
         * (2u * n_id + 1u) + 2u * n_id) & 0xFFFFFFFFu;

    // Feed the seed into the Gold sequence (Step 2) to get the DM-RS bits.
    return PnSequence::pseudo_random_sequence(pdcch_dmrs_length_bits, c_init);
}

std::vector<std::complex<float>> Dmrs::generate_pdcch_dmrs_symb(uint16_t n_id,
                                                                uint8_t  n_slot,
                                                                uint8_t  n_ofdm,
                                                                uint8_t  num_symbols_per_slot,
                                                                int      pdcch_dmrs_length_bits)
{
    // Get the raw bits...
    const auto seq = generate_pdcch_dmrs_seq(n_id, n_slot, n_ofdm,
                                             num_symbols_per_slot, pdcch_dmrs_length_bits);

    // ...then QPSK them in pairs: bit0 = first, bit1 = second.
    std::vector<std::complex<float>> out;
    out.reserve(static_cast<size_t>(pdcch_dmrs_length_bits) / 2);
    for (size_t n = 0; n + 1 < seq.size(); n += 2) {
        out.push_back(qpsk_modulate(seq[n] + 2 * seq[n + 1]));
    }
    return out;
}

} // namespace gone::nr
