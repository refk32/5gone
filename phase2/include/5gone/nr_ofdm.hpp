#pragma once

#include "5gone/nr_symbol.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_ofdm.hpp
 * ===========
 * The OFDM demodulator. The radio gives us IQ samples in the TIME domain
 * (thousands of samples per symbol, one after another). A 5G symbol is sent
 * as a sum of many subcarriers; the FFT separates that sum back into
 * individual subcarrier values -> one `Symbol` per OFDM symbol.
 *
 * Pipeline for each OFDM symbol:
 *   1. Drop the cyclic prefix (CP) — the guard the transmitter added.
 *   2. FFT the remaining `fft_size` time samples -> frequency bins.
 *   3. Keep only the active-BWP subcarriers (centered) -> Symbol.samples.
 *
 * Parameters match the lab gNB:
 *   sample_rate 23.04 MHz, scs 30 kHz  -> FFT size = 768
 *   51 PRBs active                      -> 612 subcarriers per symbol
 *
 * FFT implementation:
 *   - If built with liquid-dsp (HAVE_LIQUID, which you have on Linux):
 *     uses liquid's fft_create_plan/fft_execute like 5GSniffer.
 *   - Otherwise falls back to a correct-but-slow scalar DFT so the file
 *     still builds anywhere.
 */
class Ofdm {
public:
    Ofdm(double sample_rate, uint32_t scs_hz, uint16_t num_prbs);

    // Demodulate as many complete OFDM symbols as fit in `iq`. `iq` must start
    // on a slot boundary; `starting_slot_in_frame` names that slot (0..19) so
    // symbol/slot counters line up with the real gNB frame (DM-RS scrambling is
    // slot-dependent). Default 0 keeps the buffer-local convention.
    std::vector<Symbol> demodulate(const std::vector<std::complex<float>>& iq,
                                   uint32_t starting_slot_in_frame = 0);

    // Inverse of demodulate(): turn a stream of frequency-domain Symbols back
    // into one time-domain IQ buffer (IFFT + cyclic-prefix insertion). Used by
    // the round-trip test to synthesize the transmitted downlink slot.
    std::vector<std::complex<float>> modulate(const std::vector<Symbol>& symbols) const;

    uint32_t fft_size() const { return fft_size_; }
    uint32_t num_subcarriers() const { return num_subcarriers_; }
    uint32_t samples_per_slot() const { return samples_per_slot_; }

    // Per-symbol cyclic-prefix and total (CP+useful) lengths for the symbol in
    // a subframe (l = 0..27). Used by the cell-syncer to locate SSB/PSS timing.
    uint32_t cp_len(uint32_t symbol_in_subframe) const;
    uint32_t sym_len(uint32_t symbol_in_subframe) const;

private:
    // FFT of `in` (size fft_size_) -> frequency bins (size fft_size_).
    std::vector<std::complex<float>> fft(const std::vector<std::complex<float>>& in) const;

    // IFFT (normalized inverse of fft(), so round-tripping is the identity).
    std::vector<std::complex<float>> ifft(const std::vector<std::complex<float>>& in) const;

    double sample_rate_;
    uint32_t scs_hz_;
    uint16_t num_prbs_;
    uint32_t fft_size_;
    uint32_t num_subcarriers_;
    uint32_t symbols_per_slot_ = 14;
    uint32_t slots_per_frame_ = 20;
    uint32_t samples_per_slot_ = 0;

    // Per-symbol CP length and total length (within a subframe, 28 symbols).
    std::vector<uint32_t> samples_per_cp_;
    std::vector<uint32_t> samples_per_symbol_;
};

} // namespace gone::nr
