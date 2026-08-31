#pragma once

#include "5gone/types.hpp"
#include <memory>

namespace gone {

class RadioUhd {
public:
  explicit RadioUhd(const AttackConfig& cfg);
  ~RadioUhd();

  void start_streaming();
  void stop_streaming();

  std::size_t recv(SampleBuffer& out, double timeout_sec = 0.1);
  void transmit(const SampleBuffer& iq, double delay_sec = 0.0);

  double sample_rate() const { return cfg_.sample_rate; }

private:
  AttackConfig cfg_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gone
