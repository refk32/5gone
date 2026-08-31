#pragma once

#include "5gone/types.hpp"
#include <functional>
#include <string>
#include <vector>

namespace gone {

using RarCallback = std::function<void(const RarEvent&)>;

class RarMonitor {
public:
  explicit RarMonitor(const AttackConfig& cfg);

  // inject: read grants from JSON file (lab / debug)
  std::vector<RarEvent> load_grants_from_file(const std::string& path) const;

  // sim: parse 5gone dataset attacker.log
  std::vector<RarEvent> load_grants_from_dataset(const std::string& dataset_dir) const;

  // live: placeholder — scans IQ buffer; full PDCCH decode via srsRAN when linked
  std::vector<RarEvent> scan_buffer(const SampleBuffer& iq) const;

  void on_rar(RarCallback cb) { callback_ = std::move(cb); }
  void emit(const RarEvent& ev) const;

private:
  AttackConfig cfg_;
  RarCallback callback_;
};

} // namespace gone
