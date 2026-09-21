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
  // recv_timed additionally reports the UHD time spec (in seconds) of the first
  // sample of the received packet when the radio stamped it.
  std::size_t recv_timed(SampleBuffer& out, double timeout_sec, bool& got_time,
                         double& rx_time_sec);
  void transmit(const SampleBuffer& iq, double delay_sec = 0.0);

  // Low-level burst packet: send one chunk of a burst with explicit start/end
  // flags (proper UHD framing for multi-packet continuous bursts).
  void transmit_seg(const SampleBuffer& iq, bool start_of_burst, bool end_of_burst);

  // Time-scheduled TX: the first sample of `iq` leaves the antenna exactly at
  // `abs_time_sec` (UHD time spec). This is the determinism the loopback
  // latency harness and the slot-aligned overshadow both need.
  void transmit_timed(const SampleBuffer& iq, double abs_time_sec);

  // Pin the device to a known time epoch so the TX schedule and RX timestamps
  // share one explicit clock. Call ONCE before any timed work.
  void sync_time(double t_sec = 0.0);

  // Stream-health counters (incremented inside recv/recv_timed/send).
  uint64_t rx_overflow_count() const;
  uint64_t rx_lost_count() const;
  uint64_t tx_underrun_count() const;

  // Armed UHD time clock.
  double uhd_now_sec() const;

  // Read back what the device actually applied (for diagnostics).
  double get_tx_gain() const;
  double get_rx_gain() const;
  // Override TX gain after construction (used by the PRACH sender to raise the
  // burst power without touching the shared `radio.tx_gain` default).
  void set_tx_gain(double db);
  double get_tx_freq_hz() const;
  double get_rx_freq_hz() const;
  std::string get_tx_antenna() const;
  std::string get_rx_antenna() const;

  // Frontend PLL/state sensors (e.g. "lo_locked", "temp"). Returns "n/a" when
  // the sensor does not exist on this device. -1.0 for temp-like numeric ones.
  std::string get_tx_sensor(const std::string& name) const;
  std::string get_rx_sensor(const std::string& name) const;
  double get_temp_c() const;

  double sample_rate() const { return cfg_.sample_rate; }

private:
  AttackConfig cfg_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gone
