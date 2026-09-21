#include "5gone/ul_gate.hpp"
#include "5gone/sample_clock.hpp"

#include <cmath>

namespace gone {

UlGrantWindow compute_ul_grant_window(const CellSync& sync, const TddGate& tdd,
                                      uint64_t buf_start_sample, const RarEvent& ev,
                                      const AttackConfig& cfg, double now_sec)
{
  UlGrantWindow w;
  if (!sync.locked()) return w;   // no frame clock -> no slot math at all

  const uint64_t rar_abs_sample = buf_start_sample + ev.rar_slot_offset;
  const uint64_t frame_start = sync.frame_start_sample();
  if (rar_abs_sample < frame_start) return w;   // RAR precedes the SSB lock

  const double sps = sync.samples_per_slot();

  // Absolute slot indices are unwrapped across frames (frame_start = slot 0 of
  // frame 0), so K2 landing past a frame boundary works with no frame math.
  w.rar_abs_slot = static_cast<uint64_t>(
      std::floor(static_cast<double>(rar_abs_sample - frame_start) / sps));
  w.msg3_abs_slot = w.rar_abs_slot + ev.grant.k;

  w.start_sample = frame_start +
      static_cast<uint64_t>(std::llround(w.msg3_abs_slot * sps));
  w.end_sample = w.start_sample + static_cast<uint64_t>(sps);
  w.ul_ok = tdd.is_ul_slot(w.msg3_abs_slot);

  // The kicker goes out advance_us before the slot start so that, after the
  // loopback-measured TX->RX latency, its first symbol hits the UL slot edge.
  const double advance_sec = cfg.symbol_advance_us / 1e6;
  w.tx_abs_time_sec = rx_time_from_sample(w.start_sample, cfg.sample_rate) - advance_sec;
  w.tx_ahead_sec = w.tx_abs_time_sec - now_sec;

  w.valid = true;
  return w;
}

} // namespace gone