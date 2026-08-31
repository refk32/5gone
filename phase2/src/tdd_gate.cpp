#include "5gone/tdd_gate.hpp"

namespace gone {

TddGate::TddGate(uint8_t scs_khz)
{
  symbols_per_slot_ = (scs_khz == 120) ? 14 : 14; // normal CP @ 30/120 kHz
  const double scs_hz = scs_khz * 1000.0;
  slot_duration_sec_ = symbols_per_slot_ / (scs_hz * 14.0 / symbols_per_slot_);
  // 1 slot = 14 symbols @ 30 kHz → 0.5 ms
  slot_duration_sec_ = 1.0 / (scs_hz * 14.0 / 1000.0); // 0.5 ms for 30 kHz
  symbol_duration_sec_ = slot_duration_sec_ / symbols_per_slot_;
}

bool TddGate::is_dl_slot(uint64_t slot_idx) const
{
  // DDDSU pattern — slots 0,1,2 DL; slot 3 UL-ish guard; slot 4 UL (simplified lab)
  const uint64_t pos = slot_idx % 5;
  return pos <= 2;
}

bool TddGate::is_ul_slot(uint64_t slot_idx) const
{
  const uint64_t pos = slot_idx % 5;
  return pos >= 3;
}

} // namespace gone
