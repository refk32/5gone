#pragma once

#include "5gone/types.hpp"
#include <array>
#include <cstdint>
#include <cmath>

namespace gone::nr {

/*
 * nr_prach.hpp
 * ============
 * Step 5: the PRACH (Msg1) sender. Generates the physical-random-access
 * preamble our attacker broadcasts on the UL so the lab gNB hands us a real
 * RAR (Msg2) — which the live loop then decodes and overshadows with the
 * false Msg3 (Step 4).
 *
 * Parameters follow what the lab gNB CONFIGURES in SIB1 (verified against the
 * gNB log and srsRAN Project's prach_configuration.cpp for release_24_10):
 *   - prach-ConfigurationIndex 159 -> short format B4, L_RA = 139, d_f_RA = 30 kHz
 *     - PRACH occasion EVERY system frame (x = 1, y = {0}), subframe 9
 *     - 1 PRACH slot per subframe, start_symbol 0, duration 12 symbols
 *     - at 30 kHz this is slot 19 of the frame (prach_starts_in_even_slot is
 *       false when nof_prach_slots_within_subframe == 1), symbols 0..11
 *   - prach-RootSequenceIndex l139 = 1, zeroCorrelationZoneConfig 0
 *     - short-format Ncs table: ZCZ 0 -> Ncs = 0, so RAPID k maps to the
 *       logical root 1+k directly (no intra-root cyclic shifts)
 *   - msg1-SubcarrierSpacing kHz30, msg1-FDM one, msg1-FrequencyStart 6
 *     -> nof_rb_ra = 12 RBs, k_bar = 2 (TS 38.211 Tab 6.3.3.2-1)
 *
 * Frequency placement:
 *   - B4 symbols live on a d_f_RA = 30 kHz grid, so the PRACH FFT size at the
 *     radio rate is srate / 30000 = 768 (no big-FFT dependency).
 *   - A 51-PRB BWP is centered on the cell ARFCN, so point A sits at
 *     carrier - nof_prbs*12*scs/2. PRACH SC0 therefore lands at
 *       point_A + msg1_frequency_start*12*scs + k_bar*scs
 *     = -232 * 30 kHz bins vs the tuned carrier (-6.96 MHz) for the lab cell.
 *
 * The burst is synthesized directly in the time domain: one 768-sample OFDM
 * symbol = 139-subcarrier IFFT of the ZC DFT sequence, repeated 12x with the
 * B4 CP (468 kappa-units = 351 samples @ 23.04 MS/s) in front.
 *
 * Pure DSP / clock math: no UHD, no srsRAN -> unit-testable on any host.
 */

// TS 38.211 6.3.3.1: Zadoff-Chu root sequence x_u(n) = exp(-j pi u n (n+1) / L_RA).
std::vector<std::complex<float>> zc_root_sequence(unsigned l_ra, unsigned root_index);

// Logical root index -> sequence number u for L_RA = 139 (short preambles).
// srsRAN's get_sequence_number_short LUT, TS 38.211 Tab 6.3.3.1-3/4/5 pattern:
// {1, 138, 2, 137, 3, 136, ...}. Physical root 1 (config) -> u = 138.
unsigned sequence_number_short(unsigned root_sequence_index);

// Ncs for short preambles (L_RA = 139), unrestricted set
// (TS 38.211 Tab 6.3.3.1-5). ZCZ 0 -> Ncs 0 (the lab gNB config).
unsigned prach_ncs(unsigned zero_correlation_zone);

// Full B4 preamble burst (CP + 12 repeated symbols) at the radio sample rate.
// `msg1_frequency_start_prb` is from PRB 0; `n_prbs` + `scs_hz` place point A
// (BWP centered on the tuned carrier). `cfo_comp_hz` shifts the whole band up
// by that amount (2-SDR LO skew: ref the gNB grid with the measured CFO):
// the integer-bin part lands in f0_bin, the sub-bin remainder in
// cfo_residual_hz (applied as a continuous NCO, so sub-bin comps are real).
struct PrachPreamble {
  SampleBuffer samples;      // CP + 12 x symbol, radio-rate
  double       duration_sec; // total burst time (~415.2 us @ 23.04e6)
  int64_t      f0_bin;       // PRACH SC0 vs carrier, in 30 kHz bins (=-232)
  double       cfo_residual_hz = 0.0; // fractional CFO actually applied (NCO)
};
PrachPreamble synth_prach_b4(unsigned root_sequence_index, unsigned rapid,
                             unsigned n_prbs, double scs_hz, double srate,
                             uint16_t msg1_frequency_start_prb,
                             double cfo_comp_hz = 0.0);

// Full 64-rapid bank, built ONCE per (root, n_prbs, cfo, ...) parameter set.
// With cycle_rapids every send claims the next rapid, so a naive per-send
// synth_prach_b4() rebuild (64-pt IFFT + NCO ramp) stalls the RX drain for
// ~ms per send and the RAR-window capture starves. Eagerly building all 64
// at frame-lock time turns each send into an O(1) copy.
class PrachBank {
 public:
  bool matches(unsigned root_sequence_index, unsigned n_prbs, double scs_hz,
               double srate, uint16_t msg1_frequency_start_prb,
               double cfo_comp_hz) const {
    return built_ && root_ == root_sequence_index && n_prbs_ == n_prbs &&
           scs_ == scs_hz && srate_ == srate &&
           fstart_ == msg1_frequency_start_prb &&
           std::abs(cfo_ - cfo_comp_hz) <= 250.0;
  }
  void build(unsigned root_sequence_index, unsigned n_prbs, double scs_hz,
             double srate, uint16_t msg1_frequency_start_prb,
             double cfo_comp_hz) {
    root_ = root_sequence_index;
    n_prbs_ = n_prbs;
    scs_ = scs_hz;
    srate_ = srate;
    fstart_ = msg1_frequency_start_prb;
    cfo_ = cfo_comp_hz;
    for (unsigned r = 0; r < 64; ++r)
      bank_[r] =
          synth_prach_b4(root_, r, n_prbs_, scs_, srate_, fstart_, cfo_);
    built_ = true;
  }
  const PrachPreamble& get(unsigned rapid) const { return bank_[rapid % 64]; }
  double cfo() const { return cfo_; }

 private:
  bool built_{false};
  unsigned root_{0}, n_prbs_{51};
  double scs_{0.0}, srate_{0.0}, fstart_{0.0}, cfo_{0.0};
  std::array<PrachPreamble, 64> bank_{};
};

// DP do-send helper (pure clock math over the CellSync frame lock).
// Absolute slot indices are unwrapped from frame 0 (frame_start_sample == slot 0).
bool is_prach_occasion_slot(uint64_t abs_slot, uint8_t occasion_slot = 19);

// Next occasion start sample at or after `now_sample` in the CellSync frame clock.
uint64_t next_prach_occasion_start(uint64_t frame_start_sample, double samples_per_slot,
                                   uint64_t now_sample, uint8_t occasion_slot = 19);

} // namespace gone::nr