#pragma once

#include "5gone/types.hpp"
#include <atomic>
#include <memory>

namespace gone {

class RadioUhd;

class AttackEngine {
public:
  explicit AttackEngine(AttackConfig cfg);

  int run();

  void request_stop() { stop_.store(true); }

  void set_shared_radio(std::shared_ptr<RadioUhd> radio) { shared_radio_ = std::move(radio); }

private:
  int run_sim();
  int run_inject();
  int run_live();
  int run_bus();
  int run_loopback();

  void execute_attack(const RarEvent& ev);

  AttackConfig cfg_;
  std::atomic<bool> stop_{false};
  std::shared_ptr<RadioUhd> shared_radio_;
};

} // namespace gone
