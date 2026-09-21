// test_prach.cpp — portable PRACH (Msg1) generator test. No UHD, no srsRAN.
// Validates the B4 short-preamble math run_prach() uses:
//   1. ZC root sequence matches the closed form (TS 38.211 6.3.3.1)
//   2. Logical-root LUT for L_RA=139 (srsRAN get_sequence_number_short)
//   3. Ncs table (Tab 6.3.3.1-5, L=139, unrestricted): ZCZ 0 -> Ncs 0
//   4. Burst geometry: 351 CP + 12 x 768 @ 23.04 MS/s = 415.2 us
//   5. CP cyclicity (CP == cyclic extension of the symbol)
//   6. Frequency placement: f0_bin == -232 (6 PRBs + k_bar=2 into a 51-PRB
//      BWP centered on the carrier = -6.96 MHz), lifted by round(cfo_comp/30k)
//   7. Projecting the burst onto the 139 PRACH subcarriers recovers the DFT
//      ZC sequence (global phase only)
//   8. Occasion math: slot 19 of EVERY frame only, next-occasion lookup

#include "5gone/nr_prach.hpp"
#include "5gone/nr_ofdm.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using gone::Sample;
using gone::SampleBuffer;

bool check(bool cond, const std::string& what)
{
  std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
  return cond;
}

double dot_norm(const SampleBuffer& a, const SampleBuffer& b)
{
  std::complex<double> acc(0.0, 0.0);
  double ea = 0.0, eb = 0.0;
  const std::size_t L = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < L; ++i) {
    acc += static_cast<std::complex<double>>(a[i]) *
           std::conj(static_cast<std::complex<double>>(b[i]));
    ea += std::norm(a[i]);
    eb += std::norm(b[i]);
  }
  return std::abs(acc) / std::sqrt(ea * eb + 1e-12);
}

} // namespace

int main()
{
  int fails = 0;
  const double kSrate = 23.04e6;
  const double kPi = 3.14159265358979323846;
  const unsigned kPrbs = 51;
  const unsigned kLra = 139;       // B4 sequence length
  const unsigned kSymLen = 768;    // srate / 30 kHz
  const unsigned kCpLen = 351;     // B4 CP @ 23.04 MS/s (468 kappa)

  std::printf("== ZC root sequence ==\n");
  {
    auto zc = gone::nr::zc_root_sequence(kLra, 138);
    double max_dev = 0.0;
    for (std::size_t n = 0; n < zc.size() && n < kLra; ++n) {
      const double phase = -kPi * 138.0 * static_cast<double>(n) *
                           static_cast<double>(n + 1) / static_cast<double>(kLra);
      const std::complex<double> ref(std::cos(phase), std::sin(phase));
      max_dev = std::max(max_dev,
          std::abs(static_cast<std::complex<double>>(zc[n]) - ref));
    }
    // u' = L - u roots are complex conjugates of each other.
    auto zc_mirror = gone::nr::zc_root_sequence(kLra, 1);
    double mirror_dev = 0.0;
    for (std::size_t n = 0; n < zc.size() && n < kLra; ++n)
      mirror_dev = std::max(mirror_dev,
          std::abs(static_cast<std::complex<double>>(zc[n]) -
                   std::conj(static_cast<std::complex<double>>(zc_mirror[n]))));
    fails += !check(zc.size() == kLra && max_dev < 1e-6 && mirror_dev < 1e-6,
                    "zc_root_sequence matches closed form (u vs L-u conj)");
  }

  std::printf("== Short root LUT (L=139) ==\n");
  {
    // srsRAN get_sequence_number_short: {1,138,2,137,3,136,4,135,...}
    const bool ok = gone::nr::sequence_number_short(0) == 1 &&
                    gone::nr::sequence_number_short(1) == 138 &&
                    gone::nr::sequence_number_short(2) == 2 &&
                    gone::nr::sequence_number_short(3) == 137 &&
                    gone::nr::sequence_number_short(10) == 6 &&
                    gone::nr::sequence_number_short(11) == 133 &&
                    gone::nr::sequence_number_short(137) == 70;
    fails += !check(ok, "sequence_number_short matches srsRAN LUT rows");
  }

  std::printf("== Ncs table (L=139, unrestricted) ==\n");
  {
    const bool ok = gone::nr::prach_ncs(0) == 0 && gone::nr::prach_ncs(1) == 2 &&
                    gone::nr::prach_ncs(7) == 13 && gone::nr::prach_ncs(15) == 59;
    fails += !check(ok, "prach_ncs matches TS 38.211 Tab 6.3.3.1-5 rows");
  }

  std::printf("== Burst geometry @ 23.04 MHz ==\n");
  {
    auto bp = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6);
    const bool geom = bp.samples.size() == kCpLen + 12 * kSymLen;
    const double dur_us = bp.duration_sec * 1e6;
    fails += !check(geom && std::abs(dur_us - 415.2) < 0.5,
                    "burst = 351 CP + 12x768 seq = 9567 samples, 415.2 us");

    double rms = 0.0, peak = 0.0;
    for (const auto& v : bp.samples) {
      rms += std::norm(v);
      peak = std::max(peak, (double)std::abs(v));
    }
    rms = std::sqrt(rms / bp.samples.size());
    // B4's 139 co-phased subcarriers are inherently high-PAPR (the sample-0
    // pilot peak = 139): peak-clamping at 0.92 leaves rms ~0.08.
    fails += !check(rms >= 0.05 && rms <= 1.05 && peak <= 0.92f,
                    "burst power: peak capped 0.92 (inherent ~139 pilot peak)");

    std::size_t mism = 0;
    // The CP repeats the tail of the symbol, and each symbol is identical.
    for (std::size_t i = 0; i < kCpLen; ++i) {
      const auto a = bp.samples[kSymLen + i];          // first CP is an extension of sym
      const auto b = bp.samples[2 * kSymLen + i];      // identical 2nd symbol
      if (std::abs(static_cast<std::complex<double>>(a - b)) > 1e-6) ++mism;
    }
    fails += !check(mism == 0, "CP is a cyclic extension of the repeated symbol");
  }

  std::printf("== Frequency placement (+ fractional CFO is real) ==\n");
  {
    auto bp0 = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6, 0.0);
    // PRACH SC0 = -51*12*30k/2 + 6*12*30k + 2*30k = -6.96 MHz -> 30 kHz bins -232.
    fails += !check(bp0.f0_bin == -232, "f0_bin == -232 at 6 PRBs (+kbar2) / 51-PRB BWP");
    fails += !check(bp0.cfo_residual_hz == 0.0, "cfo 0 -> zero residual");
    auto bp1 = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6, 13500.0);
    fails += !check(bp1.f0_bin == bp0.f0_bin + 0,
                    "cfo 13500 Hz keeps f0_bin unchanged at 30 kHz granularity");
    fails += !check(std::abs(bp1.cfo_residual_hz - 13500.0) < 1e-9,
                    "cfo 13500 Hz recorded as +13500 residual (continuous NCO)");
    // Sub-bin comps must actually move the waveform (the old code emitted
    // bit-identical bursts across the whole +-15 kHz dither span).
    const double c01 = dot_norm(bp0.samples, bp1.samples);
    fails += !check(c01 < 0.99, "cfo 0 vs 13500 bursts differ (NCO is real)");
    auto bp2 = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6, 60000.0);
    fails += !check(bp2.f0_bin == bp0.f0_bin + 2,
                    "cfo 60000 Hz lifts f0_bin by round(60k/30k) = 2 bins");
    fails += !check(std::abs(bp2.cfo_residual_hz) < 1e-9,
                    "cfo 60000 Hz leaves zero residual (exact bins)");
    // Same comp twice -> bit-identical (deterministic synthesis).
    auto bp1b = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6, 13500.0);
    fails += !check(dot_norm(bp1.samples, bp1b.samples) > 0.999999,
                    "same cfo twice -> identical burst");
  }

  std::printf("== FB projection recovers DFT ZC ==\n");
  {
    auto bp = gone::nr::synth_prach_b4(1, 0, kPrbs, 30000.0, kSrate, 6, 0.0);

    // Expected frequency-domain sequence: y(k) = DFT of x_u, u = 138 (root 1).
    auto x = gone::nr::zc_root_sequence(kLra, gone::nr::sequence_number_short(1));
    std::vector<std::complex<float>> y(kLra, std::complex<float>(0.0f, 0.0f));
    const double w = -2.0 * kPi / static_cast<double>(kLra);
    for (unsigned k = 0; k < kLra; ++k) {
      std::complex<double> acc(0.0, 0.0);
      for (unsigned n = 0; n < kLra; ++n) {
        const double ph = w * static_cast<double>(k) * static_cast<double>(n);
        acc += static_cast<std::complex<double>>(x[n]) *
               std::complex<double>(std::cos(ph), std::sin(ph));
      }
      y[k] = static_cast<std::complex<float>>(acc);
    }

    // First symbol of the burst (skip CP), projected onto the 139 PRACH bins:
    // X[k] = sum_n sym[n] * exp(-j2pi (f0+k) n / 768)  ==  768 * y(k).
    const SampleBuffer sym(bp.samples.begin() + kCpLen,
                           bp.samples.begin() + kCpLen + kSymLen);
    std::vector<std::complex<float>> X(kLra, std::complex<float>(0.0f, 0.0f));
    for (unsigned k = 0; k < kLra; ++k) {
      const double step_ph = -2.0 * kPi * (static_cast<double>(bp.f0_bin) + k) / kSymLen;
      const std::complex<double> step(std::cos(step_ph), std::sin(step_ph));
      std::complex<double> ph(1.0, 0.0);
      for (std::size_t n = 0; n < kSymLen; ++n) {
        X[k] += static_cast<std::complex<double>>(sym[n]) * ph;
        ph *= step;
      }
    }
    double px = 0.0, pe = 0.0;
    for (unsigned k = 0; k < kLra; ++k) { px += std::norm(X[k]); pe += std::norm(y[k]); }
    px = std::sqrt(px); pe = std::sqrt(pe);
    double best = -1.0;
    unsigned best_shift = 0;
    for (unsigned s = 0; s < kLra; ++s) {
      std::complex<double> acc(0.0, 0.0);
      for (unsigned k = 0; k < kLra; ++k)
        acc += static_cast<std::complex<double>>(X[k]) *
               std::conj(static_cast<std::complex<double>>(y[(k + s) % kLra]));
      const double c = std::abs(acc) / (px * pe);
      if (c > best) { best = c; best_shift = s; }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "projection of symbol -> DFT ZC at shift 0 (best=%.4f, got %u)",
                  best, best_shift);
    fails += !check(best_shift == 0 && best > 0.99, buf);
  }

  std::printf("== Occasion math (every frame, subframe 9 = slot 19) ==\n");
  {
    // Slot grid truth: 0.5 ms at 23.04 MS/s is exactly 11520 samples
    // (TS 38.211 long-CP term 16*K is unscaled; an earlier revision divided
    // it and produced 11514, walking TX occasions 0.26 us/slot).
    {
        gone::nr::Ofdm ofdm(kSrate, 30000, kPrbs);
        fails += !check(ofdm.samples_per_slot() == 11520,
                        "Ofdm slot = 11520 samples (matches 0.5 ms air time)");
        fails += !check(ofdm.sym_len(0) == 768 + 66 && ofdm.cp_len(0) == 66,
                        "symbol 0 carries the long CP (66 samples)");
        fails += !check(ofdm.sym_len(1) == 768 + 54 && ofdm.cp_len(1) == 54,
                        "normal symbols carry CP 54");
    }
    using gone::nr::is_prach_occasion_slot;
    bool ok = is_prach_occasion_slot(19) && is_prach_occasion_slot(39) &&
              is_prach_occasion_slot(59) && !is_prach_occasion_slot(18) &&
              !is_prach_occasion_slot(20) && !is_prach_occasion_slot(38) &&
              !is_prach_occasion_slot(0);
    fails += !check(ok, "slot 19 of every 20-slot frame is the only occasion");

    const double sps = 11520.0;   // 30 kHz slot at 23.04 MS/s (0.5 ms exact)
    const uint64_t occ = gone::nr::next_prach_occasion_start(0, sps, 0);
    const uint64_t occ39 = gone::nr::next_prach_occasion_start(0, sps, 19 * sps + 1);
    const uint64_t occ40 = gone::nr::next_prach_occasion_start(0, sps, 40 * sps);
    const bool at = occ == 19 * sps;
    const bool next = occ39 == 39 * sps;
    const bool wrap = occ40 == 59 * sps;
    fails += !check(at && next && wrap,
                    "next start: 0->slot19; mid-occasion->next frame; >slot40->59");

    // Frame-phase sweep knob: the CellSync frame clock is anchored on the SSB
    // slot, which need not be frame slot 0, so the occasion slot is overridable.
    const uint64_t s3 = gone::nr::next_prach_occasion_start(0, sps, 0, 3);
    const uint64_t ok3 = gone::nr::is_prach_occasion_slot(3, 3) &&
                         gone::nr::is_prach_occasion_slot(23, 3) &&
                         !gone::nr::is_prach_occasion_slot(19, 3);
    fails += !check(s3 == 3 * sps && ok3,
                    "occasion_slot override shifts the occasion to that slot");
  }

  std::printf("\n%s (%d failure%s)\n",
              fails == 0 ? "ALL PRACH TESTS PASSED" : "PRACH TESTS FAILED",
              fails, fails == 1 ? "" : "s");
  return fails == 0 ? 0 : 1;
}