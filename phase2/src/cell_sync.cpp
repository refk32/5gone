#include "5gone/cell_sync.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gone {

namespace {
// Free-running detection gates (see find_ssb).
// Calibrated to OTA evidence: rx_probe locks the lab gNB SSB at corr ~0.16-0.18
// (gate 0.15). kedomom: the previous 0.5/6.0 thresholds were tuned for the clean
// synthetic loopback and never fired on the near-field RF path (live logged
// "TD PSS peak ratio=3.3 (<6.0)"). Keep a margin over pure-noise (FD corr ~0);
// the FD gate is the real disambiguator, the TD ratio gate is only a pre-filter.
constexpr float kMinFdCorr = 0.15f;
constexpr double kMinPeakRatio = 3.0;
// TD PSS peak/mean floor for lock VERIFICATION. Much lower than acquisition's:
// verify additionally requires the hit on the known grid position AND the FD
// sequence correlation (kMinFdCorr), so a real-but-marginal SSB that dips below
// the blink-and-you-miss-it acquisition pre-filter still registers a reproduce
// hit. A dead cell never lands on the grid 3x, so confidence is preserved.
constexpr double kMinVerifyPeakRatio = 1.5;

const double kTwoPi = 2.0 * std::acos(-1.0);
} // namespace

CellSync::CellSync(const AttackConfig& cfg)
    : cfg_(cfg),
      ofdm_(cfg_.sample_rate, static_cast<double>(cfg_.scs_khz) * 1000.0, nr::bwp_num_prbs)
{
  samples_per_slot_ = static_cast<double>(ofdm_.samples_per_slot());
}

void CellSync::lock_from_ssb_peak(std::size_t sample_offset)
{
  frame_start_sample_ = sample_offset;
  locked_ = true;
}

bool CellSync::find_ssb(const SampleBuffer& iq, SsbResult& out)
{
  out = SsbResult{};
  std::complex<double> cp_corr(0.0, 0.0);
  if (!locate_ssb(iq, out, cp_corr, kMinPeakRatio)) return false;

  // 5) Lock the frame on the SSB slot and seed the CFO accumulator with the
  //    first estimate. The caller shifts frame_start_sample_ into the global
  //    sample clock via set_frame_start_global().
  frame_start_sample_ = out.slot_start;
  locked_             = true;
  cfo_acc_            = cp_corr;
  cfo_frames_         = 1;
  start_lock_verify();   // a fresh lock is a CANDIDATE until it reproduces
  return true;
}

bool CellSync::locate_ssb(const SampleBuffer& iq, SsbResult& out,
                          std::complex<double>& cp_corr, double min_peak_ratio)
{
  out = SsbResult{};

  // Sample offset from the start of a slot to the PSS body start, given the
  // lab numerology: lengths of slot symbols 0..3 plus the CP of symbol 4.
  uint64_t pss_body_offset = 0;
  for (uint32_t l = 0; l < nr::kPssSlotSymbol; ++l) {
    pss_body_offset += ofdm_.sym_len(l);
  }
  pss_body_offset += ofdm_.cp_len(nr::kPssSlotSymbol);

  // 1) Time-domain search: slide the reflected PSS body against the buffer.
  const uint16_t n_id2 = cfg_.pci % 3;
  const uint16_t n_id1 = static_cast<uint16_t>(cfg_.pci / 3);
  const auto ref = nr::pss_time_reference(n_id2, ofdm_.fft_size(),
                                          cfg_.sample_rate,
                                          static_cast<uint32_t>(cfg_.scs_khz) * 1000u,
                                          cfg_.pss_bin_shift);
  if (iq.size() <= ref.size()) return false;
  const auto corr = nr::pss_sliding_corr(iq, ref);

  // Locate the peak and the correlation background around it.
  std::size_t peak = 0;
  double peak_mag = -1.0;
  double sum_mag = 0.0;
  for (std::size_t j = 0; j < corr.size(); ++j) {
    const double m = std::abs(corr[j]);
    sum_mag += m;
    if (m > peak_mag) { peak_mag = m; peak = j; }
  }
  const double mean_mag = sum_mag / static_cast<double>(corr.size());
  const double peak_ratio = mean_mag > 0.0 ? peak_mag / mean_mag : 0.0;
  if (peak_mag < min_peak_ratio * mean_mag) {
    // Nothing SSB-like in this window. Only print when there is *some* energy
    // (so pure-silence windows stay quiet).
    if (peak_ratio > 1.5) {
      std::printf("[cell-sync] no lock: TD PSS peak ratio=%.2f (< %.1f gate) — "
                  "check freq/antennas/%zu-sample window\n", peak_ratio, min_peak_ratio, iq.size());
    }
    return false;
  }

  // 2) Map the PSS timing peak back to the start of its slot.
  if (peak < pss_body_offset) return false;               // slot tucks before buffer
  const uint64_t slot_start = peak - pss_body_offset;
  if (slot_start + static_cast<uint64_t>(ofdm_.samples_per_slot()) > iq.size()) {
    return false;                                         // slot would overrun
  }

  // 3) Frequency-domain verification of the slot (lab PSS/SSS + PCI).
  // The attacker RX is tuned to the gNB CARRIER, so the SSB block arrives
  // offset by cfg_.pss_bin_shift subcarriers from DC (e.g. -187 for this cell).
  // demodulate() keeps only FFT bins 0..num_subcarriers, so an un-shifted
  // PSS (at bin -187, wrapped to ~581) is invisible to the nominal +56..+182
  // FD correlation. Rotate the slot by (119 - bin_shift) bins first: the
  // reference identity maps PSS center to +119, so this lands the received
  // PSS/SSS back on the nominal subcarriers and makes FD verification + the
  // CP-CFO estimate real (non-zero shift only; bin_shift==0 is the sim case).
  SampleBuffer slot(
      iq.begin() + static_cast<std::ptrdiff_t>(slot_start),
      iq.begin() + static_cast<std::ptrdiff_t>(slot_start) + ofdm_.samples_per_slot());
  if (cfg_.pss_bin_shift != 0) {
    const int rot_bins = static_cast<int>(nr::kPssFirstSub + nr::kPssLen / 2) -
                         cfg_.pss_bin_shift;   // 119 - bin_shift
    const double twopi = 2.0 * std::acos(-1.0);
    const double step_ph = twopi * rot_bins / static_cast<double>(ofdm_.fft_size());
    std::complex<double> ph(1.0, 0.0);
    const std::complex<double> step(std::cos(step_ph), std::sin(step_ph));
    for (auto& v : slot) {
      const std::complex<float> c(v);
      v = std::complex<float>(
          static_cast<float>(c.real() * ph.real() - c.imag() * ph.imag()),
          static_cast<float>(c.real() * ph.imag() + c.imag() * ph.real()));
      ph *= step;
    }
  }
  const auto syms = ofdm_.demodulate(slot);

  const nr::Symbol* pss_sym = nullptr;
  const nr::Symbol* sss_sym = nullptr;
  for (const auto& s : syms) {
    if (s.slot_index != 0) continue;
    if (s.symbol_index == nr::kPssSlotSymbol) pss_sym = &s;
    if (s.symbol_index == nr::kSssSlotSymbol) sss_sym = &s;
  }
  if (!pss_sym || !sss_sym) return false;

  int best_offset_pss = 0;
  int best_offset_sss = 0;
  const float corr_pss = nr::pss_correlate_fd(pss_sym->samples, n_id2, 2, best_offset_pss);
  const float corr_sss = nr::sss_correlate_fd(sss_sym->samples, n_id1, n_id2, 2, best_offset_sss);
  const float strength = std::max(corr_pss, corr_sss);
  if (strength < kMinFdCorr) {
    // TD says 'here be an SSB' but the frequency-domain sequences don't verify
    // (likely off by >±2 subcarriers, or a spurious TD peak). Show the numbers.
    std::printf("[cell-sync] TD lock but FD weak: pss=%.3f(off=%d) sss=%.3f(off=%d) "
                "ratio=%.2f symbols: pss@%u sss@%u\n",
                corr_pss, best_offset_pss, corr_sss, best_offset_sss, peak_ratio,
                (unsigned)pss_sym->symbol_index, (unsigned)sss_sym->symbol_index);
    return false;
  }

  // 4) Fractional CFO from the cyclic-prefix phase rotation across the slot.
  // Accumulate the CP correlations into one complex sum so symbols with no
  // energy (non-SSB symbols in our synthetic slot) contribute nothing. The
  // caller clears `cp_corr`; find_ssb/refine_cfo fold it into their sums.
  cp_corr = std::complex<double>(0.0, 0.0);
  std::size_t pos = 0;
  for (uint32_t l = 0; l < nr::symbols_per_slot; ++l) {
    const uint32_t cp = ofdm_.cp_len(l);
    const uint32_t sps = ofdm_.sym_len(l);
    if (pos + sps > slot.size()) break;
    std::complex<double> acc(0.0, 0.0);
    for (uint32_t i = 0; i < cp; ++i) {
      acc += std::conj(slot[pos + i]) * slot[pos + ofdm_.fft_size() + i];
    }
    cp_corr += acc;
    pos += sps;
  }
  double cfo_hz = 0.0;
  if (std::abs(cp_corr) > 0.0) {
    double phase = std::arg(cp_corr);
    // Wrap into (-pi, pi]: per-symbol phase = 2pi*cfo/scs, ambiguous at cfo =
    // scs/2 (15 kHz for the lab cell).
    while (phase >  std::acos(-1.0)) phase -= kTwoPi;
    while (phase < -std::acos(-1.0)) phase += kTwoPi;
    cfo_hz = phase * static_cast<double>(cfg_.scs_khz) * 1000.0 / kTwoPi;
  }

  out.found = true;
  out.pss_sample = peak;
  out.slot_start = slot_start;
  out.strength = strength;
  out.cfo_hz = cfo_hz;   // single-shot estimate (diagnostics; use cfo_avg_hz() for timing)
  out.cfo_mag = std::abs(cp_corr);
  out.pss_corr = corr_pss;
  out.pss_off = best_offset_pss;
  out.sss_corr = corr_sss;
  out.sss_off = best_offset_sss;
  out.subcarrier_offset = (std::abs(best_offset_pss) > std::abs(best_offset_sss))
                              ? best_offset_pss : best_offset_sss;
  return true;
}

bool CellSync::refine_cfo(const SampleBuffer& iq, SsbResult& out)
{
  out = SsbResult{};
  std::complex<double> cp_corr(0.0, 0.0);
  if (!locate_ssb(iq, out, cp_corr, kMinPeakRatio)) return false;
  // The phase of the running sum is the mean CFO over ALL refined frames, so
  // a single unlucky noisy slot no longer dictates the carrier we transmit on.
  cfo_acc_ += cp_corr;
  ++cfo_frames_;
  out.cfo_hz = cfo_avg_hz();
  return true;
}

CellSync::VerifyEvent CellSync::verify_frame(const SampleBuffer& iq,
                                             uint64_t buf_global_start,
                                             uint64_t tol_samples)
{
  if (!locked_) return VerifyEvent::Idle;
  SsbResult vr;
  std::complex<double> cp_corr(0.0, 0.0);

  // The verify buffer is a rolling, sub-frame-sized RX window, so the locked
  // grid SSB slot is only occasionally fully inside it. Only judge the frame
  // when the slot actually fits; otherwise skip WITHOUT a miss so a healthy
  // cell is not rejected just because the rolling window missed the slot.
  const uint64_t slot = static_cast<uint64_t>(ofdm_.samples_per_slot());
  if (!grid_slot_contained(buf_global_start, iq.size())) {
    if (getenv("GONE_VERIFY_DBG")) {
      const uint64_t S = next_grid_slot_after(buf_global_start);
      std::printf("[verify] skip: buf_gs=%llu len=%zu slot@%llu not contained "
                  "(needs [%llu,%llu]) hits=%d misses=%d\n",
                  static_cast<unsigned long long>(buf_global_start), iq.size(),
                  static_cast<unsigned long long>(S),
                  static_cast<unsigned long long>(S),
                  static_cast<unsigned long long>(S + slot),
                  verify_hits_, verify_misses_);
    }
    return VerifyEvent::Pending;   // wait for a frame whose slot is in-buffer
  }

  const bool found = locate_ssb(iq, vr, cp_corr, kMinVerifyPeakRatio);
  // The SSB must land on the locked grid position (frame_start_sample() +
  // k * frame period). A real cell reproduces there every frame; a dead cell
  // (or a shifted/noise hit) drifts off it and the lock must NOT be trusted.
  //
  // Three outcomes are distinguished:
  //  - on-grid hit:        proof of life; accumulates toward CONFIRMED.
  //  - found but off-grid: genuine drift / wrong cell / shifted noise; the hit
  //                        streak is wiped (the grid hypothesis is falsified).
  //  - not found:          frame absence only. The gNB's chronic RF underflow
  //                        drops entire frames, so absence must NOT wipe the
  //                        hits already accrued; it only counts toward the drop
  //                        limit so a dead cell still gets rejected.
  const bool on_grid = found &&
                       ssb_reproduced_here(buf_global_start + vr.slot_start, tol_samples);
  if (getenv("GONE_VERIFY_DBG")) {
    std::printf("[verify] buf_gs=%llu found=%d str=%.3f slot_start=%llu fs=%llu "
                "grid=%d cur_hits=%d/%d misses=%d/%d\n",
                static_cast<unsigned long long>(buf_global_start), found ? 1 : 0,
                vr.strength, static_cast<unsigned long long>(vr.slot_start),
                static_cast<unsigned long long>(frame_start_sample_),
                on_grid ? 1 : 0, verify_hits_, kVerifyHitsNeeded,
                verify_misses_, kVerifyMissLimit);
  }
  if (on_grid) {
    verify_hits_   += 1;
    verify_misses_  = 0;
    cfo_acc_       += cp_corr;   // folds into the same mean ref as refine_cfo()
    ++cfo_frames_;
    return (verify_hits_ >= kVerifyHitsNeeded) ? VerifyEvent::Confirmed
                                               : VerifyEvent::Pending;
  }
  if (found) verify_hits_ = 0;          // off-grid finding: grid falsified
  verify_misses_ += 1;
  if (verify_misses_ >= kVerifyMissLimit) {
    drop_verify();
    return VerifyEvent::Broken;
  }
  return VerifyEvent::Pending;
}

bool CellSync::ssb_reproduced_here(uint64_t cand_abs, uint64_t tol_samples) const
{
  if (!locked_) return false;
  const uint64_t period = frame_samples();
  if (period == 0) return false;
  const uint64_t fs = frame_start_sample_;
  const uint64_t d  = (cand_abs >= fs) ? (cand_abs - fs) % period
                                       : (fs - cand_abs) % period;
  return (d <= tol_samples) || (period - d <= tol_samples);
}

uint64_t CellSync::next_grid_slot_after(uint64_t sample) const
{
  const uint64_t fs = frame_start_sample_;
  const uint64_t frame = frame_samples();
  if (frame == 0) return sample;
  if (sample <= fs) return fs;
  const uint64_t d = sample - fs;
  const uint64_t k = d / frame + (d % frame != 0 ? 1u : 0u);
  return fs + k * frame;
}

bool CellSync::grid_slot_contained(uint64_t buf_gs, uint64_t buf_len) const
{
  if (!locked_) return false;
  const uint64_t slot = static_cast<uint64_t>(ofdm_.samples_per_slot());
  if (slot == 0) return false;
  const uint64_t S = next_grid_slot_after(buf_gs);
  return (S >= buf_gs) && (S + slot <= buf_gs + buf_len);
}

double CellSync::cfo_avg_hz() const
{
  if (cfo_frames_ == 0 || std::abs(cfo_acc_) == 0.0) return 0.0;
  double phase = std::arg(cfo_acc_);
  // Wrap into (-pi, pi] (same scale as the single-shot estimate).
  while (phase >  std::acos(-1.0)) phase -= kTwoPi;
  while (phase < -std::acos(-1.0)) phase += kTwoPi;
  return phase * static_cast<double>(cfg_.scs_khz) * 1000.0 / kTwoPi;
}

std::optional<uint64_t> CellSync::current_slot() const
{
  if (!locked_) return std::nullopt;
  if (rx_now_ < frame_start_sample_) return 0;
  return static_cast<uint64_t>((rx_now_ - frame_start_sample_) / samples_per_slot_);
}

double CellSync::samples_to_next_ul_slot(uint64_t target_slot) const
{
  if (!locked_) return static_cast<double>(target_slot) * samples_per_slot_;
  const uint64_t cur = current_slot().value_or(0);
  if (target_slot <= cur) return 0.0;
  return static_cast<double>(target_slot - cur) * samples_per_slot_;
}

} // namespace gone