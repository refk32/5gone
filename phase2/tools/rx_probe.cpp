// rx_probe — pure-RX listen on the attacker radio and lock the real gNB SSB.
//
// Captures `duration` seconds of RX on the configured chain (no TX), runs the
// same full-window PSS scan the victim uses, and reports every SSB hit with its
// timing and CFO. This is the "attacker hears the real cell" step of the
// 2-SDR PoC, and the lock it produces is what the collision step needs.
//
// Usage: rx_probe <rar_dos.yaml> [dump.cf32] [duration_sec=2.0] [scan_sec=0.25]
//        [pss_bin_shift=0]
//   Reuses the radio/ device serial / gains / freq from the yaml. The full
//   capture (duration_sec) is kept for later decode/dump; only the LAST
//   scan_sec are cross-correlated (pss_scan is O(N*fft), so scanning the whole
//   multi-second capture would take forever).
//   pss_bin_shift: shift the PSS reference by N 30 kHz subcarriers before
//   correlating (srsRAN places the SSB off the channel center: with point-A 0
//   and k_SSB 0, PSS lands ~+198 bins from our identity-mapping ref).
//
// exit codes: 0 locked (>=1 PSS peak), 2 no PSS, 1 file/radio error

#include "5gone/config.hpp"
#include "5gone/nr_capture.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/radio_uhd.hpp"
#include "5gone/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gone;

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <rar_dos.yaml> [dump.cf32] [duration_sec=2.0] "
                 "[scan_sec=0.25] [pss_bin_shift=0]\n",
                 argv[0]);
    return 1;
  }
  const std::string yaml_path = argv[1];
  const std::string dump_path = argc > 2 ? argv[2] : "";
  const double duration_sec = argc > 3 ? std::atof(argv[3]) : 2.0;
  const double scan_sec = argc > 4 ? std::atof(argv[4]) : 0.25;
  const int pss_bin_shift = argc > 5 ? std::atoi(argv[5]) : 0;

  gone::AttackConfig cfg;
  try {
    cfg = gone::load_config_with_overrides(yaml_path, argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rx_probe: config error: %s\n", e.what());
    return 1;
  }

  std::printf("[probe] RX on device (args=%s) freq=%.3f MHz rx_gain=%.1f "
              "chain=%s/%s srate=%.2f MSPS\n",
              cfg.device_args.c_str(), cfg.center_freq_hz / 1e6, cfg.rx_gain,
              cfg.rx_subdev.c_str(), cfg.rx_antenna.c_str(),
              cfg.sample_rate / 1e6);

  gone::RadioUhd radio(cfg);
  radio.sync_time(0.0);
  radio.start_streaming();

  gone::SampleBuffer acc, pkt(65536);
  const double t_budget = radio.uhd_now_sec() + duration_sec;
  while (radio.uhd_now_sec() < t_budget) {
    bool got = false; double t_pkt = 0.0;
    std::size_t n = radio.recv_timed(pkt, 0.1, got, t_pkt);
    if (!got || n == 0) continue;
    acc.insert(acc.end(), pkt.begin(), pkt.begin() + static_cast<ptrdiff_t>(n));
  }
  radio.stop_streaming();

  if (acc.empty()) {
    std::fprintf(stderr, "rx_probe: captured 0 samples (radio stream empty)\n");
    return 1;
  }

  if (!dump_path.empty())
    nr::write_cf32(dump_path, acc);
  std::printf("[probe] captured %zu samples = %.3f ms (rx ovf=%llu lost=%llu)%s\n",
              acc.size(), acc.size() / cfg.sample_rate * 1e3,
              (unsigned long long)radio.rx_overflow_count(),
              (unsigned long long)radio.rx_lost_count(),
              dump_path.empty() ? "" : (" dump=" + dump_path).c_str());

  const uint32_t fft_size = static_cast<uint32_t>(std::llround(
      cfg.sample_rate / (cfg.scs_khz * 1000.0)));
  const auto ref = nr::pss_time_reference(cfg.pci % 3, fft_size,
                                          cfg.sample_rate,
                                          cfg.scs_khz * 1000.0);
  std::vector<std::complex<float>> ref_shifted = ref;
  if (pss_bin_shift != 0) {
    // Move the PSS reference's subcarriers by pss_bin_shift bins: multiply by
    // exp(+j2pi*s*k/N) shifts the spectrum UP by s bins in our bin indexing.
    for (size_t k = 0; k < ref_shifted.size(); ++k) {
      const double ph = 2.0 * 3.14159265358979323846 * pss_bin_shift *
                        static_cast<double>(k) / static_cast<double>(fft_size);
      ref_shifted[k] *= std::exp(std::complex<float>(0.0f, static_cast<float>(ph)));
    }
    std::printf("[probe] PSS ref subcarrier shift %+d bins (%+.2f MHz)\n",
                pss_bin_shift, pss_bin_shift * cfg.scs_khz * 0.001);
  }

  const size_t scan_cap = static_cast<size_t>(scan_sec * cfg.sample_rate);
  const size_t scan_n = std::min(acc.size(), scan_cap);
  const size_t scan_from = acc.size() - scan_n;
  std::printf("[probe] scanning last %.1f ms (from off=%zu) of the capture\n",
              scan_n / cfg.sample_rate * 1e3, scan_from);

  auto peaks = nr::pss_scan(std::vector<std::complex<float>>(
                                acc.begin() + static_cast<ptrdiff_t>(scan_from),
                                acc.end()),
                            ref_shifted, 0.4f, cfg.sample_rate);
  for (auto& p : peaks)
    p.offset += scan_from;
  std::printf("[probe] PSS peaks: %zu\n", peaks.size());
  for (const auto& p : peaks)
    std::printf("  corr=%.3f @ off=%zu (+%.1f us) cfo=%+.1f Hz\n",
                p.corr, p.offset, p.offset / cfg.sample_rate * 1e6, p.cfo_hz);

  if (peaks.empty()) {
    std::printf("[probe] NO PSS -> real gNB SSB not seen\n");
    return 2;
  }
  std::printf("[probe] LOCKED: real SSB on air, offset=%.3f ms from capture start\n",
              peaks.front().offset / cfg.sample_rate * 1e3);
  return 0;
}