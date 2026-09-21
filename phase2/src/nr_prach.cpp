#include "5gone/nr_prach.hpp"

#include <cmath>

namespace gone::nr {

namespace {

constexpr unsigned kLraB4       = 139;        // short preamble sequence length
constexpr double   kRaScsHz     = 30000.0;    // B4 PRACH subcarrier spacing
constexpr double   kKappaSec    = 1.0 / (2048.0 * 15000.0); // kappa basic unit
constexpr unsigned kCpKappa     = 468;        // B4 CP at 30 kHz: 936k >> 1
constexpr unsigned kNofSymbols  = 12;         // B4 preamble duration
constexpr unsigned kKBar        = 2;          // TS 38.211 Tab 6.3.3.2-1, 30k/30k
constexpr uint32_t kSlotsPerFrame = 20;       // 30 kHz

// Ncs for L_RA = 139, unrestricted set (TS 38.211 Tab 6.3.3.1-5).
unsigned ncs_format_b4_l139(unsigned zcz)
{
  static const unsigned kRows[16] = {0, 2, 4, 6, 8, 10, 12, 13,
                                     15, 18, 22, 26, 32, 38, 46, 59};
  return kRows[zcz & 0xF];
}

// DFT of the ZC root: TS 38.211 6.3.3.1, y_u_v(k) = sum_n x_u(n) e^{-j2pi k n / L}.
std::vector<std::complex<float>> zc_frequency_domain(const std::vector<std::complex<float>>& x)
{
  const unsigned L = static_cast<unsigned>(x.size());
  std::vector<std::complex<float>> y(L, std::complex<float>(0.0f, 0.0f));
  const double w = 2.0 * std::acos(-1.0) / static_cast<double>(L);
  for (unsigned k = 0; k < L; ++k) {
    std::complex<double> acc(0.0, 0.0);
    for (unsigned n = 0; n < L; ++n) {
      const double ph = -w * static_cast<double>(k) * static_cast<double>(n);
      acc += static_cast<std::complex<double>>(x[n]) *
             std::complex<double>(std::cos(ph), std::sin(ph));
    }
    y[k] = static_cast<std::complex<float>>(acc);
  }
  return y;
}

} // namespace

unsigned prach_ncs(unsigned zero_correlation_zone)
{
  return ncs_format_b4_l139(zero_correlation_zone);
}

unsigned sequence_number_short(unsigned root_sequence_index)
{
  // srsRAN get_sequence_number_short LUT: index i (0..137) maps to
  // i even -> i/2 + 1, i odd -> 138 - (i-1)/2.  ({1,138,2,137,3,136,...})
  const unsigned i = root_sequence_index % (kLraB4 - 1);
  return (i % 2 == 0) ? (i / 2 + 1) : ((kLraB4 - 1) - (i - 1) / 2);
}

std::vector<std::complex<float>> zc_root_sequence(unsigned l_ra, unsigned root_index)
{
  std::vector<std::complex<float>> seq(l_ra);
  const double u = static_cast<double>(root_index % l_ra);
  for (unsigned n = 0; n < l_ra; ++n) {
    const double phase = -std::acos(-1.0) * u * static_cast<double>(n) *
                         static_cast<double>(n + 1) / static_cast<double>(l_ra);
    seq[n] = std::complex<float>(std::cos(phase), std::sin(phase));
  }
  return seq;
}

PrachPreamble synth_prach_b4(unsigned root_sequence_index, unsigned rapid,
                             unsigned n_prbs, double scs_hz, double srate,
                             uint16_t msg1_frequency_start_prb,
                             double cfo_comp_hz)
{
  // Time-domain geometry at the radio rate.
  const unsigned sym_len = static_cast<unsigned>(std::llround(srate / kRaScsHz)); // 768
  const double   cp_sec  = static_cast<double>(kCpKappa) * kKappaSec;
  const unsigned cp_len  = static_cast<unsigned>(std::llround(cp_sec * srate));   // 351

  // PRACH SC0 in 30 kHz bins vs the tuned carrier. Point A = carrier -
  // n_prbs*12*scs/2 (BWP centered on the ARFCN); sequence starts k_bar SCs in.
  // PRACH SC0 in 30 kHz bins vs the tuned carrier. Point A = carrier -
  // n_prbs*12*scs/2 (BWP centered on the ARFCN); sequence starts k_bar SCs in.
  const double point_a_hz   = -static_cast<double>(n_prbs) * 12.0 * scs_hz * 0.5;
  const double prach_sc0_hz = point_a_hz +
                              static_cast<double>(msg1_frequency_start_prb) * 12.0 * scs_hz +
                              static_cast<double>(kKBar) * kRaScsHz;
  // The integer bin takes the placement; the SUB-BIN remainder of the CFO
  // comp is kept as a continuous NCO below. (An earlier revision rounded the
  // whole CFO to bins, silently discarding anything within +-15 kHz — the
  // entire dither span emitted bit-identical bursts.)
  const double total_shift_hz = prach_sc0_hz + cfo_comp_hz;
  const int64_t f0_bin = static_cast<int64_t>(std::llround(total_shift_hz / kRaScsHz));
  const double cfo_residual_hz = total_shift_hz -
                                 static_cast<double>(f0_bin) * kRaScsHz;

  // Physical ZC for the requested RAPID: ZCZ 0 -> Ncs 0, so RAPID k selects
  // logical root (root_sequence_index + k) and no cyclic shift (srsRAN
  // prach_generator_impl).
  std::vector<std::complex<float>> x =
      zc_root_sequence(kLraB4, sequence_number_short(root_sequence_index + rapid));
  if (x.empty()) return {};
  std::vector<std::complex<float>> y = zc_frequency_domain(x);

  // One 768-sample symbol = 139-subcarrier IFFT of y at bins f0_bin..f0_bin+138:
  //   s[n] = exp(j2pi f0_bin n / sym_len) * sum_k y[k] exp(j2pi k n / sym_len)
  const double ph_f0 = 2.0 * std::acos(-1.0) * static_cast<double>(f0_bin) /
                       static_cast<double>(sym_len);
  const double ph_k = 2.0 * std::acos(-1.0) / static_cast<double>(sym_len);

  SampleBuffer sym(sym_len, Sample(0.0f, 0.0f));
  for (unsigned k = 0; k < kLraB4; ++k) {
    const std::complex<double> w(std::cos(ph_k * static_cast<double>(k)),
                                 std::sin(ph_k * static_cast<double>(k)));
    std::complex<double> z = static_cast<std::complex<double>>(y[k]);
    for (Sample& s : sym) {
      s += static_cast<Sample>(z);
      z *= w;
    }
  }

  // Rotate to the PRACH band, then normalize: loudest DAC-safe burst. The DFT
  // ZC has |y(k)| = sqrt(139), so the raw symbol rms ~ sqrt(139): normalize by
  // a global scale so the peak sits at 0.92 (rms ~0.3 due to the inherent PAPR).
  // The rotation combines the integer-bin placement (w0) with the continuous
  // fractional-CFO NCO (wres), so sub-bin comps actually shift the carrier.
  const std::complex<double> w0(std::cos(ph_f0), std::sin(ph_f0));
  const double ph_res = 2.0 * std::acos(-1.0) * cfo_residual_hz / srate;
  const std::complex<double> wres(std::cos(ph_res), std::sin(ph_res));
  std::complex<double> rot(1.0, 0.0);
  double rms = 0.0, peak = 0.0;
  for (Sample& s : sym) {
    s = static_cast<Sample>(rot) * s;
    rot *= w0 * wres;
    rms += std::norm(s);
    peak = std::max(peak, static_cast<double>(std::abs(s)));
  }
  rms = std::sqrt(rms / sym.size());
  const float g = static_cast<float>(0.92 / std::max(peak, 1e-9));
  for (Sample& s : sym) s *= g;

  // B4 burst: CP (last cp_len samples of the symbol) + 12 repeated symbols.
  PrachPreamble out;
  out.f0_bin = f0_bin;
  out.cfo_residual_hz = cfo_residual_hz;
  out.samples.resize(cp_len + static_cast<size_t>(kNofSymbols) * sym_len);
  for (unsigned m = 0; m < kNofSymbols; ++m) {
    std::copy(sym.end() - static_cast<std::ptrdiff_t>(cp_len),
              sym.end(),
              out.samples.begin() + static_cast<std::ptrdiff_t>(m) * sym_len);
    std::copy(sym.begin(), sym.end(),
              out.samples.begin() + static_cast<std::ptrdiff_t>(m) * sym_len + cp_len);
  }
  out.duration_sec = out.samples.size() / srate;
  return out;
}

bool is_prach_occasion_slot(uint64_t abs_slot, uint8_t occasion_slot)
{
  // Default config 159 / B4: x = 1, y = {0} -> every frame; subframe 9; the
  // single PRACH slot at 30 kHz is the second one (slot 19 of the 20-slot
  // frame). occasion_slot is overridable so the frame phase can be swept (the
  // CellSync frame clock is anchored on the SSB slot, which need not be slot 0).
  return (abs_slot % kSlotsPerFrame) == occasion_slot;
}

uint64_t next_prach_occasion_start(uint64_t frame_start_sample, double samples_per_slot,
                                   uint64_t now_sample, uint8_t occasion_slot)
{
  const int64_t rel = static_cast<int64_t>(now_sample) - static_cast<int64_t>(frame_start_sample);
  const int64_t now_slot = rel < 0 ? 0 : static_cast<int64_t>(
      std::floor(static_cast<double>(rel) / samples_per_slot));
  int64_t slot = now_slot;
  for (unsigned guard = 0; guard < 2 * kSlotsPerFrame + 4; ++guard, ++slot) {
    if (is_prach_occasion_slot(static_cast<uint64_t>(slot), occasion_slot)) {
      const uint64_t start = frame_start_sample + static_cast<uint64_t>(std::llround(
          static_cast<double>(slot) * samples_per_slot));
      // Never hand back an occasion that is already in the past.
      if (start >= now_sample) return start;
    }
  }
  return frame_start_sample;
}

} // namespace gone::nr