#pragma once

#include "5gone/types.hpp"
#include <cstdint>

namespace gone {

// TDD DL/UL slot pattern for n78 (DDDSU, periodicity 5 slots @ 30 kHz).
class TddGate {
public:
  explicit TddGate(uint8_t scs_khz = 30);

  bool is_dl_slot(uint64_t slot_idx) const;
  bool is_ul_slot(uint64_t slot_idx) const;
  double slot_duration_sec() const { return slot_duration_sec_; }
  double symbol_duration_sec() const { return symbol_duration_sec_; }
  uint8_t symbols_per_slot() const { return symbols_per_slot_; }

private:
  double slot_duration_sec_;
  double symbol_duration_sec_;
  uint8_t symbols_per_slot_;
};

} // namespace gone
