#include "5gone/cell_sync.hpp"
#include <cmath>
#include <optional>

namespace gone {

CellSync::CellSync(const AttackConfig& cfg) : cfg_(cfg)
{
  const double scs_hz = cfg_.scs_khz * 1000.0;
  const double slot_dur = 1.0 / (scs_hz * 14.0 / 1000.0);
  samples_per_slot_ = cfg_.sample_rate * slot_dur;
}

void CellSync::lock_from_ssb_peak(std::size_t sample_offset)
{
  frame_start_sample_ = sample_offset;
  locked_ = true;
}

std::optional<uint64_t> CellSync::current_slot() const
{
  if (!locked_) return std::nullopt;
  return 0;
}

double CellSync::samples_to_next_ul_slot(uint64_t target_slot) const
{
  return target_slot * samples_per_slot_;
}

} // namespace gone
