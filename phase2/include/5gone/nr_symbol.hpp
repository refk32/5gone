#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_symbol.hpp
 * =============
 * Represents ONE OFDM symbol after the FFT (frequency domain).
 *
 * Before this, the radio gives us IQ samples in the TIME domain. The OFDM
 * demodulator (Step 5) turns each symbol's time samples into subcarrier
 * values. That produces a `Symbol`. Each subcarrier value is a complex number:
 *   - magnitude ~ how strong that subcarrier is
 *   - phase     ~ where on the constellation it is
 *
 * Why channel estimation?
 * -----------------------
 * Radio channel twists the signal (attenuation + phase rotation per
 * subcarrier). The receiver doesn't know that twist, so it must ESTIMATE it
 * from a known reference (the DM-RS). `channel_estimate()` figures out the
 * twist, then UNDOES it (equalization) so the symbols are clean again.
 */
struct Symbol {
    // Which radio sample this symbol started at (whole stream).
    uint64_t sample_index = 0;

    // The received subcarrier values (one complex per subcarrier).
    // Size = number of active subcarriers in the BWP (bwp_num_prbs * 12).
    std::vector<std::complex<float>> samples;

    // Same subcarriers, but EQ-equalized (channel twist removed).
    std::vector<std::complex<float>> samples_eq;

    // Estimated instrumentation noise per subcarrier (for debug only).
    std::vector<std::complex<float>> noise;

    // The estimated channel per subcarrier (how much each was twisted).
    std::vector<std::complex<float>> channel_filter;

    // Where this symbol sits in the frame (for the DCI's timing info).
    uint8_t symbol_index = 0;  // OFDM symbol number within the slot (0..13)
    uint8_t slot_index   = 0;  // slot number within the frame (0..19)
    bool    is_equalized = false;

    /*
     * channel_estimate
     * ----------------
     * Uses a small set of KNOWN DM-RS subcarriers to figure out + undo the
     * channel across the full symbol.
     *
     *   dmrs_reference : the exact DM-RS complex values we expect (Step 4)
     *   dmrs_indices   : which subcarrier positions those DM-RS live on
     *   subcarrier_start/end : subcarrier range the PDCCH candidate occupies,
     *                         used to bound the equalization.
     *
     * How it works (simplified):
     *   1. At each DM-RS position, the channel is simply
     *          received_DMRS * conj(expected_DMRS)
     *      because we know what was sent, so anything extra is the channel.
     *   2. Between DM-RS positions we INTERPOLATE (linear) to fill in the gaps.
     *   3. We divide each received subcarrier by the (normalized) channel
     *      estimate to remove the twist -> samples_eq.
     */
    void channel_estimate(const std::vector<std::complex<float>>& dmrs_reference,
                          const std::vector<uint64_t>&            dmrs_indices,
                          uint64_t subcarrier_start,
                          uint64_t subcarrier_end);
};

} // namespace gone::nr
