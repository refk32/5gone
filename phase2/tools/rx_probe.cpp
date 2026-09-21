// rx_probe — pure-RX listen on the attacker radio and lock the real gNB SSB.
//
// Captures `duration` seconds of RX on the configured chain (no TX), runs the
// same full-window PSS scan the victim uses, and reports every SSB hit with its
// timing and CFO. This is the "attacker hears the real cell" step of the
// 2-SDR PoC, and the lock it produces is what the collision step needs.
//
// Usage: rx_probe <rar_dos.yaml> [dump.cf32] [duration_sec=2.0] [scan_sec=0.25]
//        [pss_bin_shift=0] [gate=0.4]
//   pss_bin_shift = SSB-center offset from the tuned carrier, in 30 kHz SC bins
//   (positive = above carrier). The scan uses pss_time_reference(), whose PSS
//   spectrum is placed at kPssFirstSub..+127 (centered ~+119 bins), NOT at DC,
//   so the runtime shift applied is pss_bin_shift - ref_center_bins. E.g. for
//   the lab gNB (SSB ARFCN 632256 = carrier - 5.58 MHz, PSS center = -67 bins)
//   pass -67.
//
//   Sweep mode: pss_bin_shift can be "from:to" (e.g. -240:-40, one bin steps)
//   to test every SSB-center offset in a range and report the global best
//   correlation even when it never clears `gate`. Useful to see a weak/off-grid
//   SSB that the binary gate would silently miss.
//
//   Only the LAST scan_sec are cross-correlated (pss_scan is O(N*fft), so
//   scanning the whole multi-second capture would take forever).
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
#include <fstream>
#include <iterator>
#include <string>

using namespace gone;

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <rar_dos.yaml> [dump.cf32] [duration_sec=2.0] "
                 "[scan_sec=0.25] [pss_bin_shift=0] [gate=0.4]\n"
                 "  dump.cf32: if the file already exists, analyze it offline "
                 "(no radio); otherwise capture live and save to it.\n"
                 "  pss_bin_shift: single bin (e.g. -67) or sweep 'lo:hi[:step]' "
                 "(e.g. -240:-40:2) to find the best correlation; step default 1.\n",
                 argv[0]);
    return 1;
  }
  const std::string yaml_path = argv[1];
  const std::string dump_path = argc > 2 ? argv[2] : "";
  const double duration_sec = argc > 3 ? std::atof(argv[3]) : 2.0;
  const double scan_sec = argc > 4 ? std::atof(argv[4]) : 0.25;
  const std::string shift_arg = argc > 5 ? argv[5] : "0";
  const float gate = argc > 6 ? static_cast<float>(std::atof(argv[6])) : 0.4f;

  int shift_from = 0, shift_to = 0, shift_step = 1;
  const auto colon = shift_arg.find(':');
  if (colon != std::string::npos) {
    shift_from = std::atoi(shift_arg.substr(0, colon).c_str());
    std::string rest = shift_arg.substr(colon + 1);
    const auto colon2 = rest.find(':');
    if (colon2 != std::string::npos) {
      shift_to = std::atoi(rest.substr(0, colon2).c_str());
      shift_step = std::max(1, std::atoi(rest.substr(colon2 + 1).c_str()));
    } else {
      shift_to = std::atoi(rest.c_str());
    }
  } else {
    shift_from = shift_to = std::atoi(shift_arg.c_str());
  }

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

  gone::SampleBuffer acc;
  {
    std::ifstream inf(dump_path, std::ios::binary | std::ios::ate);
    if (inf) {
      // Offline mode: dump_path already exists -> analyze it instead of recapturing.
      const auto sz = inf.tellg();
      if (sz % 8 != 0) {
        std::fprintf(stderr, "rx_probe: %s is not whole cf32 samples\n",
                     dump_path.c_str());
        return 1;
      }
      inf.seekg(0);
      std::vector<float> raw(static_cast<size_t>(sz) / 4);
      inf.read(reinterpret_cast<char*>(raw.data()),
               static_cast<std::streamsize>(sz));
      acc.reserve(raw.size() / 2);
      for (size_t i = 0; i + 1 < raw.size(); i += 2)
        acc.emplace_back(raw[i], raw[i + 1]);
      std::printf("[probe] OFFLINE file=%s (%.0f samples produced)\n",
                  dump_path.c_str(), static_cast<double>(acc.size()));
    }
  }

  if (acc.empty()) {
    gone::RadioUhd radio(cfg);
    radio.sync_time(0.0);
    radio.start_streaming();

    gone::SampleBuffer pkt(65536);
    const double t_budget = radio.uhd_now_sec() + duration_sec;
    while (radio.uhd_now_sec() < t_budget) {
      bool got = false; double t_pkt = 0.0;
      std::size_t n = radio.recv_timed(pkt, 0.1, got, t_pkt);
      if (!got || n == 0) continue;
      acc.insert(acc.end(), pkt.begin(), pkt.begin() + static_cast<ptrdiff_t>(n));
    }
    radio.stop_streaming();

    if (!dump_path.empty())
      nr::write_cf32(dump_path, acc);
    std::printf("[probe] captured %zu samples = %.3f ms (rx ovf=%llu lost=%llu)%s\n",
                acc.size(), acc.size() / cfg.sample_rate * 1e3,
                (unsigned long long)radio.rx_overflow_count(),
                (unsigned long long)radio.rx_lost_count(),
                dump_path.empty() ? "" : (" dump=" + dump_path).c_str());
  }

  if (acc.empty()) {
    std::fprintf(stderr, "rx_probe: no samples to scan\n");
    return 1;
  }

  const uint32_t fft_size = static_cast<uint32_t>(std::llround(
      cfg.sample_rate / (cfg.scs_khz * 1000.0)));

  const size_t scan_cap = static_cast<size_t>(scan_sec * cfg.sample_rate);
  const bool sweep = shift_from != shift_to;
  // Sweep is O(N * fft) per shift; cap the window to 40 ms (>=2 SSB periods
  // for the common 20 ms periodicity) so a wide sweep stays tractable.
  // Single-shift probes keep the full requested window.
  const size_t eff_scan_cap = sweep ? std::min(scan_cap, static_cast<size_t>(0.040 * cfg.sample_rate))
                                    : scan_cap;
  const size_t scan_n = std::min(acc.size(), eff_scan_cap);
  const size_t scan_from = acc.size() - scan_n;
  std::printf("[probe] scanning last %.1f ms (from off=%zu) of the capture%s\n",
              scan_n / cfg.sample_rate * 1e3, scan_from,
              sweep ? " (sweep-window capped at 40 ms)" : "");

  const std::vector<std::complex<float>> scan_rx(
      acc.begin() + static_cast<ptrdiff_t>(scan_from), acc.end());

  const int shift_lo = std::min(shift_from, shift_to);
  const int shift_hi = std::max(shift_from, shift_to);

  std::vector<nr::PssPeak> all_peaks;
  double best_corr = 0.0;
  nr::PssPeak best_peak;
  int best_shift = shift_lo;

  for (int sh = shift_lo; sh <= shift_hi; sh += shift_step) {
    auto ref_shifted = nr::pss_time_reference(cfg.pci % 3, fft_size,
                                              cfg.sample_rate,
                                              cfg.scs_khz * 1000.0, sh);
    if (!sweep) {
      std::printf("[probe] SSB offset %+d bins (%+.2f MHz) -> ref shift %+d bins\n",
                  sh, sh * cfg.scs_khz * 0.001,
                  sh - static_cast<int>(nr::kPssFirstSub + nr::kPssLen / 2));
    }

    auto peaks = nr::pss_scan(scan_rx, ref_shifted, sweep ? 0.0f : gate,
                              cfg.sample_rate);
    for (auto& p : peaks) {
      p.offset += scan_from;
      all_peaks.push_back(p);
      if (p.corr > best_corr) { best_corr = p.corr; best_peak = p; best_shift = sh; }
    }
  }

  if (sweep) {
    std::printf("[probe] sweep %+d..%+d (step %d) bins gate=%.2f: %zu raw peaks, "
                "global best corr=%.3f @ shift %+d bins off=%zu (+%.1f us) cfo=%+.1f Hz\n",
                shift_lo, shift_hi, shift_step, gate, all_peaks.size(),
                best_corr, best_shift, best_peak.offset,
                best_peak.offset / cfg.sample_rate * 1e6, best_peak.cfo_hz);
  }
  std::sort(all_peaks.begin(), all_peaks.end(),
            [](const nr::PssPeak& a, const nr::PssPeak& b) { return a.corr > b.corr; });

  std::vector<nr::PssPeak> visible;
  std::copy_if(all_peaks.begin(), all_peaks.end(), std::back_inserter(visible),
               [gate](const nr::PssPeak& p) { return p.corr >= gate; });

  std::printf("[probe] PSS peaks: %zu\n", visible.size());
  for (const auto& p : visible)
    std::printf("  corr=%.3f @ off=%zu (+%.1f us) cfo=%+.1f Hz\n",
                p.corr, p.offset, p.offset / cfg.sample_rate * 1e6, p.cfo_hz);

  if (visible.empty()) {
    std::printf("[probe] NO PSS -> real gNB SSB not seen (best corr=%.3f)\n",
                best_corr);
    return 2;
  }
  std::printf("[probe] LOCKED: real SSB on air, offset=%.3f ms from capture start\n",
              visible.front().offset / cfg.sample_rate * 1e3);
  return 0;
}