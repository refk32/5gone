#pragma once

#include "5gone/nr_ofdm.hpp"
#include "5gone/types.hpp"
#include <cstdint>
#include <optional>

namespace gone {

// Result of one SSB/PSS detection attempt on a raw IQ buffer.
struct SsbResult {
  bool     found{false};      // a lab-matched SSB was found and verified
  uint64_t pss_sample{0};     // sample index (within the buffer) of PSS body start
  uint64_t slot_start{0};     // sample index (within the buffer) of the SSB slot
  float    strength{0.0f};    // best FD PSS/SSS |corr| in [0,1]
  double   cfo_hz{0.0};       // fractional CFO estimate, from CP phase
  int      subcarrier_offset{0}; // integer SSB offset in subcarriers, from FD search
};

// Frame/slot clock for the lab cell.
//
// `find_ssb()` searches a raw IQ buffer for the gNB's SSB (lab PCI from the
// config, k_ssb=0, PSS in slot symbol 4), locks the frame on the containing
// slot and reports timing + CFO. Once locked, the clock converts a running
// receive-sample counter (`set_rx_now`) into slot indices and ul-slot delays.
//
// The old manual lock (`lock_from_ssb_peak`) is kept for the simulation path.
class CellSync {
public:
  explicit CellSync(const AttackConfig& cfg);

  // Manual PCI lock for lab cell (sim mode; no SSB search).
  void lock_from_ssb_peak(std::size_t sample_offset);

  // Search the buffer for the lab SSB. On success locks the frame and fills
  // `out` with timing/CFO; returns false (no lock) if nothing matches.
  bool find_ssb(const SampleBuffer& iq, SsbResult& out);

  // Feed the monotonic receive-sample counter (absolute across the stream).
  void set_rx_now(uint64_t rx_global_sample) { rx_now_ = rx_global_sample; }
  uint64_t rx_now() const { return rx_now_; }

  // Slot index at the current receive-sample counter.
  std::optional<uint64_t> current_slot() const;

  // Samples from the current receive-sample counter to the start of target_slot
  // (0 if target already passed). Before lock: target_slot scaled directly.
  double samples_to_next_ul_slot(uint64_t target_slot) const;

  // Slot where the RAR for a Msg1 at `rar_slot` is scheduled (k slots later).
  uint64_t slot_from_rar(uint64_t rar_slot, uint8_t k) const { return rar_slot + k; }

  bool locked() const { return locked_; }
  uint64_t frame_start_sample() const { return frame_start_sample_; }
  double samples_per_slot() const { return samples_per_slot_; }

private:
  AttackConfig cfg_;
  nr::Ofdm ofdm_;
  uint64_t frame_start_sample_{0};
  bool locked_{false};
  double samples_per_slot_;
  uint64_t rx_now_{0};
};

} // namespace gone