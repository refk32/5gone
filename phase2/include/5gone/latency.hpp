#pragma once

#include "5gone/types.hpp"
#include <cstddef>

namespace gone {

// Result of locating a known transmit burst inside a receive buffer.
struct LatencyResult {
  bool     found{false};      // burst located above the correlation gate
  size_t   arrival_sample{0}; // absolute sample index in `rx` where the burst starts
  double   delay_sec{0.0};    // arrival_sample / sample_rate
  float    corr{0.0f};        // peak normalized correlation in [0,1]
};

// Locate `marker` (a known TX burst, e.g. our PUSCH overshadow burst) inside
// `rx` by sliding normalized correlation over [search_from,
// search_from + search_len). Returns the best match when the peak clears the
// gate (0.5). Pure compute — no radio/UHD involvement, so it is unit-testable
// and works identically on a live RX stream and on saved captures.
LatencyResult measure_latency(const SampleBuffer& marker, const SampleBuffer& rx,
                              double sample_rate, size_t search_from, size_t search_len);

} // namespace gone