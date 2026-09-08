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
constexpr float kMinFdCorr = 0.5f;
constexpr double kMinPeakRatio = 6.0;

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
                                          static_cast<uint32_t>(cfg_.scs_khz) * 1000u);
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
  if (peak_mag < kMinPeakRatio * mean_mag) return false;

  // 2) Map the PSS timing peak back to the start of its slot.
  if (peak < pss_body_offset) return false;               // slot tucks before buffer
  const uint64_t slot_start = peak - pss_body_offset;
  if (slot_start + static_cast<uint64_t>(ofdm_.samples_per_slot()) > iq.size()) {
    return false;                                         // slot would overrun
  }

  // 3) Frequency-domain verification of the slot (lab PSS/SSS + PCI).
  const SampleBuffer slot(
      iq.begin() + static_cast<std::ptrdiff_t>(slot_start),
      iq.begin() + static_cast<std::ptrdiff_t>(slot_start) + ofdm_.samples_per_slot());
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
  if (strength < kMinFdCorr) return false;

  // 4) Fractional CFO from the cyclic-prefix phase rotation across the slot.
  // Accumulate the CP correlations into one complex sum so symbols with no
  // energy (non-SSB symbols in our synthetic slot) contribute nothing.
  std::complex<double> cp_corr(0.0, 0.0);
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

  // 5) Lock the frame on the SSB slot.
  frame_start_sample_ = slot_start;
  locked_ = true;

  out.found = true;
  out.pss_sample = peak;
  out.slot_start = slot_start;
  out.strength = strength;
  out.cfo_hz = cfo_hz;
  out.subcarrier_offset = (std::abs(best_offset_pss) > std::abs(best_offset_sss))
                              ? best_offset_pss : best_offset_sss;
  return true;
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