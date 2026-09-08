#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace gone {

struct UlDci {
  bool     freq_hopping{false};
  uint16_t pusch_freq_res{0};
  uint8_t  pusch_time_res{1};
  uint8_t  mcs{4};
  uint8_t  tpc_for_pusch{3};
  bool     csi_request{false};
};

struct UlGrant {
  uint16_t rnti{0};
  uint8_t  k{6};
  uint8_t  mcs{4};
  uint16_t tbs_bits{264};
  uint16_t pusch_freq_res{0};
  uint8_t  pusch_time_res{1};
  std::string mapping{"A"};
};

struct RarEvent {
  uint8_t  rapid{0};
  uint16_t ta{0};
  uint16_t c_rnti{0};
  UlDci   ul_dci;
  UlGrant grant;
};

struct AttackConfig {
  std::string mode{"sim"};
  double sample_rate{23.04e6};
  double center_freq_hz{3.5e9};
  double tx_gain{80.0};
  double rx_gain{40.0};
  std::string device_args{"type=b200,master_clock_rate=23.04e6,num_recv_frames=512,num_send_frames=512"};
  uint8_t pci{1};
  uint8_t scs_khz{30};
  uint16_t bandwidth_mhz{20};
  uint8_t band{78};
  double symbol_advance_us{500.0};
  double tx_power_scale{1.5};
  std::string iq_template_dir{"phase2/iq_templates"};
  std::string grant_file;
  std::string dataset_dir;
  std::string log_file{"/tmp/5gone_attacker.log"};
  std::string grant_bus{"/tmp/5gone_grants.jsonl"};
  bool dry_run{false};

  // Step 4 loopback: single B210, cross-chain near-field (TX/RX A -> TX/RX B).
  std::string tx_subdev{"A:A"};
  std::string rx_subdev{"A:B"};
  std::string tx_antenna{"TX/RX"};
  std::string rx_antenna{"TX/RX"};
  uint32_t loopback_iterations{3};
  double loopback_window_ms{200.0};
  std::string loopback_dump_path;      // optional: write RX window to cf32 for debugging
  double loopback_tx_scale{5.0};       // scale applied to the TX marker burst
  bool loopback_probe{true};           // run the TX RF / RSSI probe stages
};

using Sample = std::complex<float>;
using SampleBuffer = std::vector<Sample>;

} // namespace gone
