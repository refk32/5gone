#include "5gone/rar_monitor.hpp"
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace gone {

RarMonitor::RarMonitor(const AttackConfig& cfg) : cfg_(cfg) {}

void RarMonitor::emit(const RarEvent& ev) const
{
  if (callback_) callback_(ev);
}

static UlGrant grant_from_dci(const UlDci& dci, uint16_t c_rnti, uint8_t k = 6)
{
  UlGrant g;
  g.rnti = c_rnti;
  g.k = k;
  g.mcs = dci.mcs;
  g.pusch_freq_res = dci.pusch_freq_res;
  g.pusch_time_res = dci.pusch_time_res;
  g.tbs_bits = 264; // paper cell-wide-dos default
  return g;
}

static RarEvent parse_attacker_log_line(const std::string& line)
{
  RarEvent ev;
  static const std::regex rapid_re(R"("rapid":(\d+))");
  static const std::regex ta_re(R"("ta":(\d+))");
  static const std::regex crnti_re(R"("c_rnti":(\d+))");
  static const std::regex freq_re(R"("pusch_freq_res":(\d+))");
  static const std::regex mcs_re(R"("mcs":(\d+))");
  static const std::regex k_re(R"(\bk=(\d+)\b)");

  std::smatch m;
  if (std::regex_search(line, m, rapid_re)) ev.rapid = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, ta_re)) ev.ta = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, crnti_re)) ev.c_rnti = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, freq_re)) ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, mcs_re)) ev.ul_dci.mcs = static_cast<uint8_t>(std::stoi(m[1].str()));
  ev.ul_dci.pusch_time_res = 1;
  ev.grant = grant_from_dci(ev.ul_dci, ev.c_rnti, 6);
  if (std::regex_search(line, m, k_re)) ev.grant.k = static_cast<uint8_t>(std::stoi(m[1].str()));
  ev.grant.rnti = ev.c_rnti;
  return ev;
}

std::vector<RarEvent> RarMonitor::load_grants_from_file(const std::string& path) const
{
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open grant file: " + path);

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  std::vector<RarEvent> events;

  // JSON grant file (sample_grant.json)
  if (content.find("\"events\"") != std::string::npos) {
    static const std::regex ev_block(R"(\{[^{}]*"rapid"[^{}]*\})");
    auto begin = std::sregex_iterator(content.begin(), content.end(), ev_block);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      events.push_back(parse_attacker_log_line(it->str()));
    }
    if (!events.empty()) return events;
  }

  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("Attacking RAR") != std::string::npos || line.find("\"rapid\"") != std::string::npos) {
      events.push_back(parse_attacker_log_line(line));
    }
  }
  return events;
}

std::vector<RarEvent> RarMonitor::load_grants_from_dataset(const std::string& dataset_dir) const
{
  const std::string log_path = dataset_dir + "/cell-wide-dos/attacker.log";
  return load_grants_from_file(log_path);
}

std::vector<RarEvent> RarMonitor::scan_buffer(const SampleBuffer& iq) const
{
  (void)iq;
  // Live PDCCH decode: requires srsRAN polar decoder + RA-RNTI blind search.
  // Returns empty until DL sync + PDCCH pipeline completes (see docs/phase2-attack.md).
  return {};
}

} // namespace gone
