// analyze_collide: offline dissection of a dumped collide-mode RX window.
//
// Reads a cf32 dump (collide.dump_path), regenerates the exact RAR slot grid
// the harness transmits, locates the PSS with the same full-window scan, slices
// the slot, and prints per-symbol message-2 clarity under several CFO/phase
// treatments so we can see exactly what corrupts the live constellation:
//    (a) no derotation
//    (b) CP-based CFO derotation only
//    (c) CP CFO + absolute phase reference from slot symbol 0 (RAR PDCCH DM-RS)
//    (d) fine CFO sweep maximizing total |clarity|
// It also prints each case's RarDecoder rar_obs and the per-symbol correlation
// phase, so a residual CFO ramp vs. a constant phase offset vs. a demod
// misalignment is immediately visible.
//
// Usage: analyze_collide <dump.cf32> [pci=1] [scs_khz=30] [srate=23040000]

#include "5gone/nr_capture.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_rar_tx.hpp"
#include "5gone/types.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gone;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Result {
  double cfo_hz = 0.0;
  bool   aligned = false;
  size_t rar_obs = 0;
  std::vector<float> rho;       // clarity sym 2..13
  std::vector<double> phase;    // arg of each symbol's complex correlation
  double rho_sum = 0.0;         // sum |rho|
  double rho_cross = 0.0;       // sum rho (signed; high iff clean +QPSK match)
  std::vector<float> rho_mag;   // flip-signed magnitude: sign(re dot)*|rho|
  double mag_sum = 0.0;
  double mag_cross = 0.0;
};

Result score(const SampleBuffer& win, const nr::RarSlotTx& B, size_t sym4,
             const std::vector<std::complex<float>>& pss_ref, double srate,
             nr::RarDecoder& dec, nr::Ofdm& ofdm, double cfo,
             bool use_sym0_phase, double sweep_cfo_offset)
{
  auto peaks = nr::pss_scan(win, pss_ref, 0.30f, srate);
  Result r; r.cfo_hz = cfo + sweep_cfo_offset;
  if (peaks.empty() || peaks.front().offset < sym4) return r;
  const size_t slot_start = peaks.front().offset - sym4;
  if (slot_start + B.slot_samples > win.size()) return r;
  r.aligned = true;
  SampleBuffer aligned(win.begin() + static_cast<ptrdiff_t>(slot_start),
                       win.begin() + static_cast<ptrdiff_t>(slot_start + B.slot_samples));
  const double w = 2.0 * kPi * r.cfo_hz / srate;
  if (std::abs(w) > 1e-12)
    for (size_t i = 0; i < aligned.size(); ++i)
      aligned[i] *= std::exp(std::complex<double>(0.0, -w * static_cast<double>(i)));

  // Optional absolute phase reference from slot symbol 0 (unflipped DM-RS).
  double ph0 = 0.0;
  if (use_sym0_phase) {
    auto syms0 = ofdm.demodulate(aligned);
    if (!syms0.empty()) {
      std::complex<double> dot(0.0, 0.0);
      const auto& rx = syms0[0].samples;
      const auto& rg = B.grid[0].samples;
      size_t L = std::min(rx.size(), rg.size());
      for (size_t k = 0; k < L; ++k) dot += rx[k] * std::conj(rg[k]);
      ph0 = std::arg(dot);
      if (std::abs(ph0) > 1e-9)
        for (auto& v : aligned) v *= std::exp(std::complex<double>(0.0, -ph0));
    }
  }

  r.rar_obs = dec.decode(aligned).size();

  auto syms = ofdm.demodulate(aligned);
  for (uint16_t sy = 2; sy <= 13 && sy < syms.size(); ++sy) {
    const auto& rx = syms[sy].samples;
    const auto& rg = B.grid[sy].samples;
    std::complex<double> dot(0.0, 0.0);
    double ea = 0.0, eg = 0.0;
    size_t L = std::min(rx.size(), rg.size());
    for (size_t k = 0; k < L; ++k) {
      dot += rx[k] * std::conj(rg[k]);
      ea += std::norm(rx[k]);
      eg += std::norm(rg[k]);
    }
    r.phase.push_back(std::arg(dot));
    const float rho01 = ea > 1e-12 && eg > 1e-12
        ? static_cast<float>(dot.real() / std::sqrt(ea * eg)) : 0.0f;
    const float rho_m = ea > 1e-12 && eg > 1e-12
        ? static_cast<float>(std::abs(dot) / std::sqrt(ea * eg)) : 0.0f;
    r.rho.push_back(rho01);
    r.rho_mag.push_back(rho_m >= 0.0f && rho01 < 0.0f ? -rho_m : rho_m);
    r.rho_sum += std::abs(rho01);
    r.rho_cross += rho01;
    r.mag_sum += r.rho_mag.back();
    r.mag_cross += r.rho_mag.back();
  }
  return r;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dump.cf32> [pci=1] [scs_khz=30] [srate=23040000]\n", argv[0]);
    return 2;
  }
  const uint32_t pci = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 1;
  const double scs_khz = argc > 3 ? std::atof(argv[3]) : 30.0;
  const double srate = argc > 4 ? std::atof(argv[4]) : 23.04e6;
  const uint32_t scs = static_cast<uint32_t>(std::lround(scs_khz * 1000.0));
  const uint16_t prbs = 51;

  SampleBuffer win = nr::read_cf32(argv[1]);
  std::printf("[anac] %s: %zu samples = %.3f ms @ %.1f MSPS (pci=%u scs=%.0f kHz)\n",
              argv[1], win.size(), win.size() / srate * 1e3, srate / 1e6, pci, scs_khz);

  nr::Ofdm ofdm(srate, scs, prbs);
  const nr::RarSlotTx B = nr::build_rar_slot(ofdm, pci, prbs, 0.5f);
  const uint32_t fft = static_cast<uint32_t>(std::llround(srate / scs));
  const auto pss_ref = nr::pss_time_reference(pci % 3, fft, srate, scs);
  const size_t sym4 = B.symbol_offsets[4] + ofdm.cp_len(4);
  nr::RarDecoder dec(srate, scs, pci, prbs, false);

  auto peaks = nr::pss_scan(win, pss_ref, 0.30f, srate);
  std::printf("[anac] PSS peaks: %zu\n", peaks.size());
  for (const auto& p : peaks)
    std::printf("  corr=%.3f @ off=%zu (+%.1f us) cfo=%+.0f Hz\n",
                p.corr, p.offset, p.offset / srate * 1e6, p.cfo_hz);
  if (peaks.empty()) {
    std::printf("[anac] NO PSS -> burst absent in this dump\n");
    return 1;
  }

  // (a) no derotation
  auto ra = score(win, B, sym4, pss_ref, srate, dec, ofdm, 0.0, false, 0.0);
  // (b) CP-CFO derotation only
  nr::PssPeak best = peaks.front();
  const size_t slot_start = best.offset - sym4;
  SampleBuffer aligned(win.begin() + static_cast<ptrdiff_t>(slot_start),
                       win.begin() + static_cast<ptrdiff_t>(slot_start + B.slot_samples));
  {
    std::complex<double> cp_acc(0.0, 0.0);
    for (uint16_t sy = 0; sy < B.grid.size() && sy < 14; ++sy) {
      const size_t off = B.symbol_offsets[sy];
      const size_t cp = ofdm.cp_len(sy);
      if (off + cp + ofdm.fft_size() > aligned.size()) break;
      for (size_t i = 0; i < cp; ++i)
        cp_acc += aligned[off + i] * std::conj(aligned[off + ofdm.fft_size() + i]);
    }
    const double cfo_cp = cp_acc == std::complex<double>(0.0, 0.0)
        ? 0.0 : -std::arg(cp_acc) / (ofdm.fft_size() / srate) / (2.0 * kPi);
    auto rb = score(win, B, sym4, pss_ref, srate, dec, ofdm, cfo_cp, false, 0.0);
    // (c) + absolute phase from symbol 0 DM-RS
    auto rc = score(win, B, sym4, pss_ref, srate, dec, ofdm, cfo_cp, true, 0.0);

    // (d) fine CFO sweep on top of the CP estimate, maximizing |rho| sum
    std::printf("[anac] CP-based CFO = %+.1f Hz (two-half PSS said %+.0f Hz)\n",
                cfo_cp, best.cfo_hz);
    double best_sum = -1.0, best_off = 0.0;
    for (double off = -400.0; off <= 400.0; off += 25.0) {
      auto rd = score(win, B, sym4, pss_ref, srate, dec, ofdm, cfo_cp, true, off);
      if (rd.rho_sum > best_sum) { best_sum = rd.rho_sum; best_off = off; }
    }
    auto rd = score(win, B, sym4, pss_ref, srate, dec, ofdm, cfo_cp, true, best_off);

    // (e) flip-signed magnitude clarity, per-symbol phase-agnostic (the
    //     recommended victim metric): immune to the mid-slot phase step but
    //     still flips sign when the attacker rotates the payload 180 deg.
    auto re = score(win, B, sym4, pss_ref, srate, dec, ofdm, cfo_cp, false, 0.0);

    auto dump = [&](const char* tag, const Result& r) {
      std::printf("\n[anac] %s: cfo=%+.1f Hz rar_obs=%zu corr_sum=|%.2f| signed=%.2f\n",
                  tag, r.cfo_hz, r.rar_obs, r.rho_sum, r.rho_cross);
      if (!r.aligned) { std::printf("  (slot not aligned)\n"); return; }
      for (size_t i = 0; i < r.rho.size(); ++i)
        std::printf("  sym%zu: rho=%+.3f  arg=%+.1f deg\n", i + 2, r.rho[i],
                    r.phase[i] * 180.0 / kPi);
    };
    dump("(a) no derotation", ra);
    dump("(b) CP-CFO only", rb);
    dump("(c) CP-CFO + sym0 phase", rc);
    dump("(d) CP-CFO + sym0 phase + fine cfo sweep", rd);
    {
      std::printf("\n[anac] (e) CP-CFO only + flip-signed magnitude: "
                  "cfo=%+.1f Hz rar_obs=%zu mag_sum=|%.2f| signed=%.2f\n",
                  re.cfo_hz, re.rar_obs, re.mag_sum, re.mag_cross);
      if (re.aligned)
        for (size_t i = 0; i < re.rho_mag.size(); ++i)
          std::printf("  sym%zu: rho_mag=%+.3f (arg=%+.1f deg)\n", i + 2,
                      re.rho_mag[i], re.phase[i] * 180.0 / kPi);
    }
    std::printf("\n[anac] fine sweep picked extra %+.0f Hz -> total %+.1f Hz\n",
                best_off, cfo_cp + best_off);
  }
  return 0;
}