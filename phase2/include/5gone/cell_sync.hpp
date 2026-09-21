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
  double   cfo_mag{0.0};      // |CP correlation| (confidence of the CFO estimate)
  int      subcarrier_offset{0}; // integer SSB offset in subcarriers, from FD search
  float    pss_corr{0.0f};    // FD PSS |corr| at pss_off (agreement check)
  int      pss_off{0};        // FD PSS best integer offset
  float    sss_corr{0.0f};    // FD SSS |corr| at sss_off (agreement check)
  int      sss_off{0};        // FD SSS best integer offset
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

  // find_ssb() reports SSB timing relative to the searched buffer
  // (out.slot_start). Once the absolute receive-sample index of that buffer's
  // first sample is known (UHD stamp via rx_sample_from_time), shift the lock
  // into the global sample clock here — afterwards set_rx_now()/current_slot()/
  // samples_to_next_ul_slot() all reason in absolute stream samples.
  void set_frame_start_global(uint64_t frame_start_sample) { frame_start_sample_ = frame_start_sample; }

  // Search the buffer for the lab SSB. On success locks the frame and fills
  // `out` with timing/CFO; returns false (no lock) if nothing matches.
  bool find_ssb(const SampleBuffer& iq, SsbResult& out);

  // Once locked, feed one buffer per frame-slot-0 to accumulate the CP-phase
  // CFO across SSB frames. Averaging turns the marginal single-shot estimate
  // (which swings +-10 kHz on the near-field SSB) into a stable center for the
  // PRACH carrier. Returns false if the buffer held no SSB (don't count it).
  bool refine_cfo(const SampleBuffer& iq, SsbResult& out);

  // Averaged fractional CFO over all refined SSB frames (0 Hz pre-lock/none).
  double cfo_avg_hz() const;
  uint32_t cfo_frames() const { return cfo_frames_; }

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

  // ---- Two-phase frame lock (live TX safety) ------------------------------
  // A fresh find_ssb() lock is a CANDIDATE until the SSB reproduces on the
  // 20 ms frame grid over `kVerifyHitsNeeded` frames. A dead / absent cell can
  // still hand a spurious single-buffer lock (observed: strength 0.277 vs an
  // off-air gNB), but never reproduces at the same absolute position frame
  // after frame. launch path: no TX while verify_in_progress().
  enum class VerifyEvent { Idle, Pending, Confirmed, Broken };

  static constexpr unsigned kVerifyHitsNeeded = 3;   // consecutive grid hits
  static constexpr unsigned kVerifyMissLimit  = 3;   // drop the lock after these

  // Called automatically by find_ssb() when a lock is established. Idempotent.
  void start_lock_verify()
  {
    verify_anchor_ = frame_start_sample_;
    verify_hits_   = 0;
    verify_misses_ = 0;
  }

  // True while a freshly locked frame is still unconfirmed (TX must wait).
  bool verify_in_progress() const { return locked_ && verify_hits_ < kVerifyHitsNeeded; }

  // Feed one buffer (usually at a frame boundary) to judge whether the locked
  // SSB position reproduces. Returns the state transition: Confirmed (commit
  // TX) or Broken (cell not present — lock dropped, rescan).
  VerifyEvent verify_frame(const SampleBuffer& iq, uint64_t buf_global_start,
                           uint64_t tol_samples);

  // Frame period in samples (20 slots), as the SSB repeats once per frame.
  uint64_t frame_samples() const { return static_cast<uint64_t>(20.0 * samples_per_slot_); }

  // Whether an absolute sample position falls on the locked SSB grid position
  // (frame_start_sample() + k * frame_period, tolerance `tol_samples`).
  bool ssb_reproduced_here(uint64_t cand_abs, uint64_t tol_samples) const;

private:
  AttackConfig cfg_;
  nr::Ofdm ofdm_;
  uint64_t frame_start_sample_{0};
  bool locked_{false};
  double samples_per_slot_;
  uint64_t rx_now_{0};
  // Accumulated CP-phase CFO parts over refined SSB frames (find_ssb seeds it).
  std::complex<double> cfo_acc_{0.0, 0.0};
  uint32_t cfo_frames_{0};
  // Two-phase lock verification state.
  uint64_t verify_anchor_{0};  // SSB absolute position the lock is pinned to
  unsigned verify_hits_{0};    // consecutive confirmed frame reproductions
  unsigned verify_misses_{0};  // consecutive broken frames (limit flips Broken)
  void drop_verify() { locked_ = false; verify_hits_ = 0; verify_misses_ = 0; }
  // locate_ssb() = the TD search + FD verify + CP correlation shared by
  // find_ssb() (locks the frame, seeds the accumulator) and refine_cfo().
  bool locate_ssb(const SampleBuffer& iq, SsbResult& out, std::complex<double>& cp_corr);
};

} // namespace gone