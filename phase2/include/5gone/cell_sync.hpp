#pragma once

#include "5gone/types.hpp"
#include <cstdint>
#include <optional>

namespace gone {

// Frame/slot counter — manual PCI lock for lab cell (known gNB config).
class CellSync {
public:
  explicit CellSync(const AttackConfig& cfg);

  void lock_from_ssb_peak(std::size_t sample_offset);
  std::optional<uint64_t> current_slot() const;
  double samples_to_next_ul_slot(uint64_t target_slot) const;

  uint64_t slot_from_rar(uint64_t rar_slot, uint8_t k) const { return rar_slot + k; }

private:
  AttackConfig cfg_;
  uint64_t frame_start_sample_{0};
  bool locked_{false};
  double samples_per_slot_;
};

} // namespace gone
