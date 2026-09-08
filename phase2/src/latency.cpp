#include "5gone/latency.hpp"

#include <cmath>
#include <complex>

namespace gone {

static constexpr float  kLatencyGate  = 0.5f;  // normalized |corr| peak gate
static constexpr size_t kDecimFactor  = 8;     // coarse-search decimation
static constexpr double kFineMargin   = 24.0;  // fine window = coarse off +/- L*? (samples)

// Naive normalized sliding correlation over an exact window. O(W*L); used only
// on the (decimated) coarse pass and a tiny full-rate fine pass, so it stays
// cheap while remaining exact for arbitrary marker shapes.
static float corr_at(const SampleBuffer& rx, size_t off, const SampleBuffer& m,
                     double rx_energy, double marker_energy)
{
  const size_t L = m.size();
  if (off + L > rx.size() || rx_energy <= 1e-12) return 0.0f;
  std::complex<double> dot = 0.0;
  for (size_t t = 0; t < L; ++t)
    dot += rx[off + t] * std::conj(m[t]);
  return static_cast<float>(std::abs(dot) /
                            (std::sqrt(rx_energy) * std::sqrt(marker_energy)));
}

// RX-window energy for the exact window at `off`.
static double window_energy(const SampleBuffer& rx, size_t off, size_t L)
{
  double e = 0.0;
  for (size_t t = 0; t < L; ++t) e += std::norm(rx[off + t]);
  return e;
}

// Decimate a buffer by keeping every D-th sample (phase 0). Cheap short-averaging
// would alias; plain point-sampling is plenty for a coarse location.
static SampleBuffer decimate(const SampleBuffer& in, size_t D)
{
  SampleBuffer o;
  o.reserve(in.size() / D + 1);
  for (size_t i = 0; i + D - 1 < in.size(); i += D) o.push_back(in[i]);
  return o;
}

LatencyResult measure_latency(const SampleBuffer& marker, const SampleBuffer& rx,
                              double sample_rate, size_t search_from, size_t search_len)
{
  LatencyResult r;
  const size_t L = marker.size();
  if (L == 0 || rx.empty()) return r;

  if (search_from + search_len > rx.size())
    search_len = rx.size() - search_from;
  if (search_len < L) return r;                 // window too small for one burst

  double em = 0.0;                              // marker energy
  for (size_t t = 0; t < L; ++t) em += std::norm(marker[t]);

  // --- Coarse pass: decimate both signals, naive correlation, best peak. ---
  const SampleBuffer md = decimate(marker, kDecimFactor);
  const SampleBuffer rxd = decimate(SampleBuffer(rx.begin() + static_cast<std::ptrdiff_t>(search_from),
                                                 rx.begin() + static_cast<std::ptrdiff_t>(search_from + search_len)),
                                    kDecimFactor);
  const size_t Ld = md.size();
  if (Ld == 0 || rxd.size() < Ld) return r;

  double ed = 0.0;
  for (size_t t = 0; t < Ld; ++t) ed += std::norm(md[t]);   // decimated marker energy

  float  best_c = 0.0f;
  size_t best_off_d = 0;
  for (size_t k = 0; k + Ld <= rxd.size(); ++k) {
    const float sk = corr_at(rxd, k, md, window_energy(rxd, k, Ld), ed);
    if (sk > best_c) { best_c = sk; best_off_d = k; }
  }
  if (best_c < kLatencyGate) return r;          // nothing above the gate anywhere

  // --- Fine pass: full-rate search over a small neighbourhood of the coarse hit. ---
  const size_t coarse_off = search_from + best_off_d * kDecimFactor;
  const size_t lo = coarse_off > kFineMargin * static_cast<double>(kDecimFactor * 2)
                        ? coarse_off - static_cast<size_t>(kFineMargin * kDecimFactor * 2)
                        : 0;
  const size_t hi = std::min(coarse_off + static_cast<size_t>(kFineMargin * kDecimFactor * 2),
                             search_from + search_len - L);

  for (size_t k = lo; k <= hi; ++k) {
    const float sk = corr_at(rx, k, marker, window_energy(rx, k, L), em);
    if (sk > best_c) {
      best_c = sk;
      r.arrival_sample = k;
      r.corr = sk;
      r.found = (sk >= kLatencyGate);
    }
  }

  if (r.found) r.delay_sec = static_cast<double>(r.arrival_sample) / sample_rate;
  return r;
}

} // namespace gone