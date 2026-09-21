#pragma once

#include "5gone/types.hpp"
#include <atomic>
#include <memory>

namespace gone {

class RadioUhd;
struct UlGrantWindow;

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
  int run_prach();
  int run_bus();
  int run_loopback();
  int run_collide();

  void execute_attack(const RarEvent& ev);
  // Step 4: schedule the false Msg3 at the absolute UHD time the UL gate computed.
  void execute_attack_timed(const RarEvent& ev, const UlGrantWindow& win,
                            RadioUhd& radio);
  // Encode the empty-MAC-PDU Msg3 burst for a RAR grant (shared by both paths).
  SampleBuffer build_msg3(const RarEvent& ev);

  AttackConfig cfg_;
  std::atomic<bool> stop_{false};
  std::shared_ptr<RadioUhd> shared_radio_;
};

} // namespace gone
