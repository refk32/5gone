#pragma once

#include <cstdint>

namespace gone {

/*
 * sample_clock.hpp
 * ================
 * Pure conversion between the UHD device clock (seconds, epoch 0 = the moment
 * RadioUhd::sync_time(0.0) ran) and the absolute receive-sample index. The
 * device timer is sample-exact, so with one shared epoch every RX packet's
 * time-stamp maps deterministically to a global sample number — the same clock
 * CellSync and the TDD slot gate reason in.
 *
 * Kept dependency-free (no UHD) so the math is unit-testable on any host.
 */

// Global receive-sample index whose UHD time is t_sec (epoch 0). With a
// continuous stream of packets the mapped indices stay contiguous and drift
// free: packet N starts at rx_sample_from_time(t_first) + N * packet_len.
uint64_t rx_sample_from_time(double t_sec, double sample_rate);

// Inverse map (diagnostics / TX scheduling in seconds).
double rx_time_from_sample(uint64_t sample_index, double sample_rate);

} // namespace gone