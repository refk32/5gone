// tx_probe — emit recognizable bursts from the configured radio (no srsRAN).
//
// Answers "does this B210 chain actually radiate?" in one command: sends N
// PSS-body bursts/tone-windows via the device/freq/gain from rar_dos.yaml,
// and prints the TX front-end state (freq/gain/antenna/LO-locked/temp).
// Peer the RX side with rx_probe; check with rx_probe_psd.py/ssb.py.
//
// Usage:
//   tx_probe <rar_dos.yaml> [n=20] [period_sec=0.2] [scale=0.35] [mode=burst|cw]
//            [on_sec=0.40] [off_sec=0.10] [--device...]
//   burst = one PSS body (fft_size samples) per period
//   cw    = repeated PSS body filling on_sec per period (clearly visible on PSD)
// exit codes: 0 sent ok, 1 radio/config error

#include "5gone/config.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/radio_uhd.hpp"
#include "5gone/types.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace gone;

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <rar_dos.yaml> [n_bursts=20] [period_sec=0.2] [scale=0.35]\n",
                 argv[0]);
    return 1;
  }
  const std::string yaml_path = argv[1];
  const int n_bursts = argc > 2 ? std::atoi(argv[2]) : 20;
  const double period_sec = argc > 3 ? std::atof(argv[3]) : 0.2;
  const float scale = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 0.35f;
  const std::string mode = argc > 5 ? argv[5] : "cw";
  const double on_sec = argc > 6 ? std::atof(argv[6]) : 0.40;
  const double off_sec = argc > 7 ? std::atof(argv[7]) : 0.10;
  const bool cw_mode = (mode == "cw");

  AttackConfig cfg;
  try {
    cfg = gone::load_config_with_overrides(yaml_path, argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "tx_probe: config error: %s\n", e.what());
    return 1;
  }

  gone::RadioUhd radio(cfg);
  radio.sync_time(0.0);

  std::printf("[tx] device=%s freq=%.3f MHz tx_gain=%.1f antenna=%s "
              "lo_locked=%s temp=%.1f C\n",
              cfg.device_args.c_str(), radio.get_tx_freq_hz() / 1e6,
              radio.get_tx_gain(), radio.get_tx_antenna().c_str(),
              radio.get_tx_sensor("lo_locked").c_str(), radio.get_temp_c());

  const uint32_t fft_size = static_cast<uint32_t>(std::llround(
      cfg.sample_rate / (cfg.scs_khz * 1000.0)));
  auto body = nr::pss_time_reference(cfg.pci % 3, fft_size,
                                     cfg.sample_rate, cfg.scs_khz * 1000.0);
  for (auto& v : body) v *= scale;
  if (cw_mode) {
    // Fill on_sec with the PSS body repeated back-to-back: a robust CW-ish
    // multi-tone window that any sane PSD analysis sees.
    const std::size_t want = static_cast<std::size_t>(std::llround(
        on_sec * cfg.sample_rate));
    std::vector<std::complex<float>> tone;
    tone.reserve(want);
    while (tone.size() < want) {
      const std::size_t add = std::min(body.size(), want - tone.size());
      tone.insert(tone.end(), body.begin(), body.begin() + add);
    }
    body.swap(tone);
    std::printf("[tx] cw mode: %.0f ms on (%zu samples) every %.0f ms\n",
                on_sec * 1e3, body.size(), period_sec * 1e3);
  }

  const double t0 = radio.uhd_now_sec() + 0.5;
  const double step = cw_mode ? period_sec : period_sec;
  for (int k = 0; k < n_bursts; ++k) {
    const double t = t0 + static_cast<double>(k) * step;
    radio.transmit_timed(body, t);
    std::printf("[tx] %s %2d/%d @ t=%.6f s (samples=%zu)\n",
                cw_mode ? "window" : "burst",
                k + 1, n_bursts, t, body.size());
  }
  std::printf("[tx] sent %d bursts; tx underruns=%llu; done\n", n_bursts,
              (unsigned long long)radio.tx_underrun_count());
  return 0;
}