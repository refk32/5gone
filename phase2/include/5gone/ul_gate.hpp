#pragma once

#include "5gone/cell_sync.hpp"
#include "5gone/tdd_gate.hpp"
#include "5gone/types.hpp"
#include <cstdint>

namespace gone {

/*
 * ul_gate.hpp
 * ===========
 * Step 4: turn a decoded RAR event into the absolute sample window of the Msg3
 * the RAR's UL grant schedules, and the shared-epoch UHD time to transmit the
 * paper-faithful false Msg3 so it arrives on air as the real UE's would.
 *
 * Everything here is pure math over the CellSync frame clock (locked to the
 * gNB SSB) plus the DDDSU TDD pattern — no UHD — so it unit-tests on any host.
 *
 * Timeline model:
 *   RAR decoded from a buffer that began at absolute RX-sample `buf_start`
 *   -> RAR slot start = buf_start + ev.rar_slot_offset
 *   -> Msg3 slot      = that slot + K2 (ev.grant.k), offset by the frame clock
 *   -> TX time        = Msg3 slot start (in the shared UHD epoch) - advance_us
 *                       the advance is the loopback-measured TX->RX latency so
 *                       the kicker lands exactly on the slot (Step 4 semantics:
 *                       TX at slot_start - symbol_advance_us).
 */

// The computed Msg3 window for one RAR.
struct UlGrantWindow {
  bool     valid = false;         // set when a window could be computed at all
  bool     ul_ok = false;         // Msg3 slot is a TDD UL slot (else skip)
  uint64_t rar_abs_slot = 0;      // absolute gNB slot index the RAR was in
  uint64_t msg3_abs_slot = 0;     // absolute gNB slot index the RAR schedules
  uint64_t start_sample = 0;      // absolute RX-sample of Msg3 slot start
  uint64_t end_sample = 0;        // one slot later (window span)
  double   tx_abs_time_sec = 0.0; // UHD epoch time to begin TX (start - advance)
  double   tx_ahead_sec = 0.0;    // tx_abs_time_sec - now_sec (negative = past)
};

// `ev` was decoded from the buffer whose first sample sits at absolute RX-sample
// `buf_start_sample` in the CellSync global clock. `now_sec` is the current UHD
// device time in the same epoch (only feeds tx_ahead_sec; the math is pure).
UlGrantWindow compute_ul_grant_window(const CellSync& sync,
                                      const TddGate& tdd,
                                      uint64_t buf_start_sample,
                                      const RarEvent& ev,
                                      const AttackConfig& cfg,
                                      double now_sec);

} // namespace gone