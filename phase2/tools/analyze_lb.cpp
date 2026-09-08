// analyze_lb: offline post-processing for the loopback RX dump (cf32).
//
// Regenerates the exact marker burst the harness transmits (same Ofdm/SSB
// synthesis + tx_scale as run_loopback) and scans the ENTIRE captured buffer
// for correlation peaks — not just the harness's [-0.25w,+0.75w] window — so
// we can tell whether (a) the burst never arrived in RF, or (b) it arrived
// but slid outside the search window / below the gate because of scheduling
// slippage or a timing offset between the TX schedule and RX timestamps.
// Mirrors measure_latency()'s math (decimated naive correlation, no DC
// removal) so results are directly comparable.
//
// Usage: analyze_lb <dump.cf32> [sample_rate=23040000] [scs_khz=30] [pci=1] [tx_scale=5.0]

#include "5gone/nr_capture.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gone;

namespace {
constexpr size_t kDecim = 8;

struct Peak {
  size_t off;
  float corr;
};

float corr_at(const std::vector<std::complex<float>>& rx, size_t off,
              const std::vector<std::complex<float>>& m, size_t L,
              double rx_energy, double marker_energy)
{
  if (off + L > rx.size() || rx_energy <= 1e-12) return 0.0f;
  const std::complex<float>* r = rx.data() + off;
  const std::complex<float>* c = m.data();
  std::complex<double> dot = 0.0;
  for (size_t t = 0; t < L; ++t) dot += r[t] * std::conj(c[t]);
  return static_cast<float>(std::abs(dot) /
                            (std::sqrt(rx_energy) * std::sqrt(marker_energy)));
}

double window_energy(const std::vector<std::complex<float>>& rx, size_t off, size_t L)
{
  double e = 0.0;
  for (size_t t = 0; t < L; ++t) e += std::norm(rx[off + t]);
  return e;
}

std::vector<std::complex<float>> decimate(const std::vector<std::complex<float>>& in,
                                          size_t D)
{
  std::vector<std::complex<float>> o;
  o.reserve(in.size() / D + 1);
  for (size_t i = 0; i + D - 1 < in.size(); i += D) o.push_back(in[i]);
  return o;
}
} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <dump.cf32> [sample_rate=23040000] [scs_khz=30] [pci=1] [tx_scale=5.0]\n",
                 argv[0]);
    return 2;
  }
  const double sample_rate = argc > 2 ? std::atof(argv[2]) : 23.04e6;
  const double scs_khz = argc > 3 ? std::atof(argv[3]) : 30.0;
  const uint32_t pci = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 1;
  const double tx_scale = argc > 5 ? std::atof(argv[5]) : 5.0;

  std::vector<std::complex<float>> rx = nr::read_cf32(argv[1]);
  if (rx.size() < 2 * 4096) {
    std::fprintf(stderr, "bad or empty capture at %s\n", argv[1]);
    return 1;
  }
  std::printf("[analyze] %s: %zu samples = %.3f ms at %.1f MSPS\n",
              argv[1], rx.size(), rx.size() / sample_rate * 1e3, sample_rate / 1e6);

  nr::Ofdm tx_ofdm(sample_rate, scs_khz * 1000.0, 51);
  std::vector<std::complex<float>> marker = nr::synth_ssb_slot(tx_ofdm, pci);
  if (tx_scale > 1.0)
    for (auto& v : marker) v *= static_cast<float>(tx_scale);
  std::printf("[analyze] marker: %zu samples rms=%.5f (tx_scale=%.1f)\n",
              marker.size(),
              std::sqrt(window_energy(marker, 0, marker.size()) / marker.size()),
              tx_scale);

  double rms = 0.0, pk = 0.0;
  size_t pk_off = 0;
  for (size_t i = 0; i < rx.size(); ++i) {
    const double a = std::abs(rx[i]);
    rms += std::norm(rx[i]);
    if (a > pk) { pk = a; pk_off = i; }
  }
  rms = std::sqrt(rms / rx.size());
  std::printf("[analyze] rx levels: rms=%.5f peak=%.5f @ %zu (%.3f ms) pk/rms=%.1f dB\n",
              rms, pk, pk_off, pk_off / sample_rate * 1e3,
              20.0 * std::log10(pk / (rms + 1e-12)));

  std::complex<double> mean = 0;
  for (const auto& v : rx) mean += v;
  mean /= static_cast<double>(rx.size());
  std::printf("[analyze] rx mean (DC): %+.6f %+.6f\n", mean.real(), mean.imag());

  // Coarse scan over the whole buffer (same decimated naive correlation the
  // harness uses; no DC removal, to match its gate).
  const std::vector<std::complex<float>> md = decimate(marker, kDecim);
  const std::vector<std::complex<float>> rxd = decimate(rx, kDecim);
  const size_t Ld = md.size();
  const size_t Nd = rxd.size();
  const double md_energy = window_energy(md, 0, Ld);

  std::vector<float> corr(Nd > Ld ? Nd - Ld + 1 : 0, 0.0f);
  for (size_t k = 0; k + Ld <= Nd; ++k)
    corr[k] = corr_at(rxd, k, md, Ld, window_energy(rxd, k, Ld), md_energy);

  std::vector<Peak> coarse;
  for (size_t k = 0; k < corr.size(); ++k) {
    const bool is_peak = (k == 0 || corr[k] >= corr[k - 1]) &&
                         (k + 1 >= corr.size() || corr[k] >= corr[k + 1]);
    if (!is_peak || corr[k] < 0.30f) continue;
    if (!coarse.empty() && k < coarse.back().off + Ld / 2) {
      if (corr[k] > coarse.back().corr) coarse.back() = {k, corr[k]};
      continue;
    }
    coarse.push_back({k, corr[k]});
  }
  std::sort(coarse.begin(), coarse.end(),
            [](const Peak& a, const Peak& b) { return a.corr > b.corr; });
  if (coarse.size() > 8) coarse.resize(8);

  std::printf("\n[analyze] top coarse correlation peaks (decim=%zu, gate shown at 0.30):\n",
              kDecim);
  if (coarse.empty()) std::printf("  none — marker energy not detectable anywhere\n");
  for (const auto& p : coarse)
    std::printf("  coarse off=%zu (%.3f ms) corr=%.3f\n",
                p.off * kDecim, p.off * kDecim / sample_rate * 1e3, p.corr);

  // Fine pass: full-rate search around each coarse peak (±32 samples).
  std::vector<Peak> fine;
  const size_t Lfull = marker.size();
  const double m_energy = window_energy(marker, 0, Lfull);
  for (const auto& p : coarse) {
    const size_t c0 = p.off * kDecim;
    const size_t lo = c0 > 64 ? c0 - 64 : 0;
    const size_t hi = lo + 128 < rx.size() - Lfull ? lo + 128 : rx.size() - Lfull;
    float best_c = 0.0f;
    size_t best_o = lo;
    for (size_t k = lo; k <= hi; ++k) {
      const float c = corr_at(rx, k, marker, Lfull, window_energy(rx, k, Lfull), m_energy);
      if (c > best_c) { best_c = c; best_o = k; }
    }
    if (best_c >= 0.30f) fine.push_back({best_o, best_c});
  }
  std::sort(fine.begin(), fine.end(),
            [](const Peak& a, const Peak& b) { return a.corr > b.corr; });

  std::printf("\n[analyze] fine correlation (full rate, gate 0.50):\n");
  if (fine.empty()) std::printf("  none — burst ABSENT in capture\n");
  for (const auto& p : fine)
    std::printf("  off=%zu (%.3f ms) corr=%.3f  %s\n",
                p.off, p.off / sample_rate * 1e3, p.corr,
                p.corr >= 0.50f ? "MATCH — burst present" : "below gate");

  if (fine.empty() || fine.front().corr < 0.50f)
    std::printf("\n=> burst ABSENT in this capture. TX->RF->RX coupling or amplitude"
                " is the issue — check antennas/cabling, raise rx_gain and tx_scale.\n");
  else
    std::printf("\n=> burst PRESENT at off=%zu (%.3f ms) corr=%.3f. Timing slid "
                "outside the harness search window; widen the window and/or sync "
                "TX schedule to the RX timestamp clock.\n",
                fine.front().off, fine.front().off / sample_rate * 1e3,
                fine.front().corr);
  return 0;
}