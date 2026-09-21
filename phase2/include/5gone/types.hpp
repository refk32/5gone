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

// Step 5: PRACH (Msg1) preamble sender. The attacker broadcasts a B4 short ZC
// preamble (L_RA=139, 30 kHz) on the gNB's UL PRACH occasion (config 159) so
// the gNB hands us a real RAR.
struct PrachCfg {
  bool     enabled{false};               // send Msg1 on the PRACH occasion
  uint16_t rapid{0};                     // RAPID we claim (Ncs=0 -> logical root 1+rapid)
  bool     cycle_rapids{true};          // claim the next RAPID per send ((base+sent)%64):
                                        // same-rapid repeats look suppressed after the
                                        // first answers, and cycling maps gNB
                                        // preamble=k + MAC RAPID echoes 1:1 to sends
  uint16_t root_sequence_index{1};       // srsRAN default prach_root_seq_index (l139)
  uint8_t  zero_correlation_zone{0};     // srsRAN default (0 -> Ncs 0 for L=139)
  uint16_t msg1_frequency_start_prb{6};  // srsRAN default PRACH offset in PRBs
  uint8_t  occasion_slot{19};            // slot of the PRACH occasion in the frame
                                         // (config 159 -> slot 19); sweep 0..19 to
                                         // align when the SSB slot is not frame slot 0
  bool     sweep_occasion_slots{false};  // cycle occasion_slot 0..19 across sends
                                         // within ONE lock (kills the beam lottery:
                                         // exactly one slot maps to the gNB
                                         // occasion per lock; zero hits = bad lock)
  uint32_t occasion_count{0};            // 0 = keep sending until stop
  bool     auto_cfo{true};               // center TX band on the gNB using measured CFO
  double   cfo_comp_hz{0.0};             // manual override (auto_cfo=false)
  bool     dither_cfo{false};            // sweep the burst carrier one step per occasion
                                         // around (auto)center so at least one step per
                                         // pass lands inside the gNB PRACH detector's
                                         // frequency comb (observed hits ~0/+8000/+12000 Hz)
  double   dither_halfspan_hz{14000.0};  // sweep center +/- this (Hz)
  double   dither_step_hz{1000.0};       // step size (Hz, >0)
  double   tx_gain_db{-1.0};             // <0 = keep radio.tx_gain
  std::string rar_capture_dir;          // if set, dump one OTA RAR window (window
                                        // starts one slot before the occasion)
  uint32_t rar_capture_window_slots{0}; // window length in slots (0 -> 4)
  double   rar_capture_drain_sec{0.0};  // tail-recording length (s) after the
                                        // occasion_count is hit: drains the
                                        // dilated gNB RAR backlog to disk
                                        // (bounded wall-clock, then stop)
  uint32_t rar_capture_pre_slots{1};    // slots before the occasion where the
                                        // file starts (head-trim from an early
                                        // overflow jump ate the RARs twice;
                                        // raise to 60-100 for RAR safety)
  bool live_scan{true};                 // run the live RAR monitor on RX buffers.
                                        // Set false for pure capture runs: the
                                        // scan costs a full demod+decode per
                                        // buffer (the main RX-stall source) and
                                        // is slot-blind on unaligned buffers,
                                        // so it never fires there anyway.
};

struct RarEvent {
  uint8_t  rapid{0};
  uint16_t ta{0};
  uint16_t c_rnti{0};
  UlDci   ul_dci;
  UlGrant grant;
  // Sample offset (relative to the scanned buffer) where the slot carrying this
  // RAR begins — anchors the Step-4 Msg3 UL gate into the absolute frame clock.
  uint64_t rar_slot_offset{0};
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
  // SSB-center offset from the tuned carrier, in SCS (30 kHz) bins, positive =
  // above carrier. 0 == identity mapping (PSS centered at +119 bins) used by
  // the synthetic-packet tests; the lab gNB's SSB sits at -67. See nr_pss.hpp.
  int pss_bin_shift{0};
  double symbol_advance_us{8.0};   // loopback-calibrated TX->RX latency (+7.5..8 µs)
  double tx_power_scale{1.5};
  std::string iq_template_dir{"phase2/iq_templates"};
  std::string grant_file;
  std::string dataset_dir;
  std::string log_file{"/tmp/5gone_attacker.log"};
  std::string grant_bus{"/tmp/5gone_grants.jsonl"};
  bool dry_run{false};

  // Path-2 live firing: DMRS-level corr fence for scan_window hits to become a
  // constant-grant Msg3 (P2-1 detection has no DCI bits; the fence separates
  // confident RAR slots from the 0.5..0.9 noise floor).
  float live_fire_corr{0.9f};
  // TcRntiTracker seed: gNB assigns TC-RNTIs sequentially, so one observed seed
  // predicts every later RAR's TC-RNTI by count. 0 = leave tracker unseeded
  // (fires with tc_rnti 0 until the first decode or manual reseed).
  uint16_t live_tc_rnti_seed{0};

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

  // Step 4b software-victim RAR overshadow (--mode collide): we transmit the
  // synthetic gNB RAR slot plus a conflicting "attack" copy over the air and
  // score how a software victim (RarDecoder + message-2 correlation) sees them.
  uint32_t collide_iterations{3};              // windows per delay step
  double collide_legit_scale{2.0};             // TX amplitude of the legit slot
  double collide_attack_scale{2.0};            // attack copy amplitude relative to legit
  std::vector<double> collide_delay_symbols{1.0, 3.0, 6.0, 9.0};  // attack offset in OFDM symbols
  std::string collide_dump_path;               // optional prefix: dumps control + per-Δ RX windows

  // Step 5: PRACH Msg1 sender knobs.
  PrachCfg prach;
};

using Sample = std::complex<float>;
using SampleBuffer = std::vector<Sample>;

} // namespace gone
