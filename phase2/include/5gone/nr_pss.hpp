#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_pss.hpp
 * ==========
 * NR cell-search primitives (TS 38.211 5.2.1 / 5.2.2) for the lab gNB.
 *
 * The gNB is srsRAN Project on n78, 20 MHz, PCI 1, 30 kHz SCS, 51 PRBs.
 * We search only for THAT cell: PCI 1 -> N_ID^2 = 1, N_ID^1 = 0.
 *
 * Frequency-domain layout (identity mapping, k_ssb = 0):
 *   - The SSB block (240 subcarriers) occupies BWP subcarriers     0..239
 *   - PSS lives on SSB symbol 2 = slot symbol 4, at subcarriers 56..182
 *   - SSS lives on SSB symbol 0 = slot symbol 2, at subcarriers 56..182
 */

constexpr uint32_t kPssLen        = 127;
constexpr uint32_t kPssFirstSub   = 56;   // first subcarrier of PSS/SSS (SSB = 0..239)
constexpr uint8_t  kPssSlotSymbol = 4;    // slot symbol where PSS sits (SSB symbol 2)
constexpr uint8_t  kSssSlotSymbol = 2;    // slot symbol where SSS sits (SSB symbol 0)

// TS 38.211 5.2.1: NR-PSS, 127 x {+1,-1}. n_id2 = cell_id % 3.
std::vector<float> nr_pss_sequence(uint16_t n_id2);

// TS 38.211 5.2.2: NR-SSS, 127 x {+1,-1}. n_id1 in 0..335, n_id2 in 0..2.
std::vector<float> nr_sss_sequence(uint16_t n_id1, uint16_t n_id2);

// Write the PSS (resp. SSS) sequence into `symbol`'s active subcarriers at
// kPssFirstSub..+127. `symbol.samples` must have at least 183 entries.
void place_pss_in_symbol(std::vector<std::complex<float>>& symbol, uint16_t n_id2);
void place_sss_in_symbol(std::vector<std::complex<float>>& symbol,
                         uint16_t n_id1, uint16_t n_id2);

// Time-domain body (length fft_size) of one PSS symbol, generated through our
// own Ofdm (NULL -> pure-Frequency-Domain reference is just the IFFT). Used to
// correlate the raw IQ stream for timing.
std::vector<std::complex<float>> pss_time_reference(uint16_t n_id2, uint32_t fft_size,
                                                    double sample_rate = 23.04e6,
                                                    uint32_t scs_hz = 30000);

// Normalized frequency-domain correlation of one demodulated symbol's active
// subcarriers against PSS (resp. SSS) at subcarriers kPssFirstSub+off.. Search
// integer subcarrier offsets in [-search_bins..search_bins]; returns the best
// |corr| in [0,1] and the offset that achieved it.
float pss_correlate_fd(const std::vector<std::complex<float>>& symbol,
                       uint16_t n_id2, int search_bins, int& best_offset);
float sss_correlate_fd(const std::vector<std::complex<float>>& symbol,
                       uint16_t n_id1, uint16_t n_id2,
                       int search_bins, int& best_offset);

// Sliding cross-correlation of `iq` with the length-fft `ref` body. Result has
// iq.size() - ref.size() + 1 complex values; the PSS timing peak = argmax |z|.
std::vector<std::complex<float>> pss_sliding_corr(
    const std::vector<std::complex<float>>& iq,
    const std::vector<std::complex<float>>& ref);

} // namespace gone::nr