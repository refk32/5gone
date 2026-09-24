#pragma once

#include "5gone/types.hpp"
#include "5gone/nr_pdcch.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gone {

using RarCallback = std::function<void(const RarEvent&)>;

class RarMonitor {
public:
  explicit RarMonitor(const AttackConfig& cfg);

  // inject: read grants from JSON file (lab / debug)
  std::vector<RarEvent> load_grants_from_file(const std::string& path) const;

  // sim: parse 5gone dataset attacker.log
  std::vector<RarEvent> load_grants_from_dataset(const std::string& dataset_dir) const;

  // live: scan an IQ buffer through the full DL decode chain and return the
  // decoded RAR events (RAPID / TA / TC-RNTI / UL grant) for execute_attack()
  std::vector<RarEvent> scan_buffer(const SampleBuffer& iq) const;

  // ---- Path-2 live detection (P2-1): slot-aware window scan ----
  // One DMRS-level candidate observation. abs_slot is unwrapped (frame 0
  // origin); slot is abs_slot % 20.
  struct SlotHit {
    uint64_t abs_slot = 0;
    uint8_t  slot = 0;
    uint8_t  symbol = 0;
    uint8_t  al = 0;
    uint8_t  candidate = 0;
    float    corr = 0.0f;
    uint16_t freq_prbs = 0;
    uint16_t start_prb = 0;
    uint8_t  duration = 0;
    bool     interleaved = false;
    uint16_t shift = 0;
  };

  // Scan one RX buffer with correct slot labels: slices [abs_start, end) at
  // the next slot boundary of the (frame_start, samples_per_slot) clock,
  // demodulates the aligned subspan (needs ~3 symbols past the boundary),
  // and correlates the empirical CORESET mini-grid (hot offsets x dur 2..3 x
  // maps x shifts, currently 120 configs). Reports every candidate >= 0.5
  // (no hidden thresholds); empty when the buffer holds no boundary+span.
  // abs_start_sample/frame_start_sample share the UHD device epoch.
  std::vector<SlotHit> scan_window(const SampleBuffer& iq,
                                   uint64_t abs_start_sample,
                                   uint64_t frame_start_sample,
                                   double samples_per_slot,
                                   double cfo_hz);

  // SIB1 learning: broadcast PDCCH repeats every 40 slots identically, RARs
  // are one-shots. Feed every window's hits; sib_distinct(m) counts DISTINCT
  // abs slots seen in bucket m (dedupe across candidates/configs).
  void note_hits_for_sib(const std::vector<SlotHit>& hits);
  size_t sib_distinct(uint8_t bucket40) const;
  void report_sib_clusters() const;
  void reset_sib_stats();

  // ---- Null calibration (Path-2, P2-1) ----
  // The scan_window() grid (120 configs) reports every candidate >= 0.5, and
  // the live-fire / SIB-learning fences used absolute thresholds (0.9). On
  // "empty" captures the max-of-extreme correlation over that grid climbs to
  // 0.9+ without any PDCCH being present, so absolute fences spam/fire on
  // noise. `ensure_null_floor()` measures the same grid's largest correlation
  // on a synthetic noise window of the same span; `fire_floor()` is the
  // null-calibrated fence (absolute 0.9 vs null floor + margin, whichever is
  // higher).
  float null_floor() const;   // grid's measured noise-only max (0 if unmeasured)
  float fire_floor() const;   // fence scan_window hits must clear to be acted on
  void  ensure_null_floor();

  // ---- Path-2 grant provider seam (P2-2) ----
  // The firing path consumes RarGrant without caring which backend produced
  // it: STATIC/TRACKED now, DECODED later (polar backend drops in behind the
  // same struct once the link yields bits).
  struct RarGrant {
    uint8_t  k2_slots = 4;    // RAR -> Msg3 delay (TDRA row; CALIBRATE on first firing)
    uint16_t rb_start = 0;    // observed rb=[0..3) on every scheduled RAR
    uint16_t rb_len = 3;      // [0..3) = RBs 0,1,2 (CALIBRATE: riv semantics)
    uint8_t  mcs = 0;         // CALIBRATE from first decoded grant or gNB-log tbs
    int8_t   tpc = 0;         // CALIBRATE
    uint16_t tc_rnti = 0;     // tracked (TcRntiTracker) until decoded once
    uint16_t ta = 0;          // ~0 co-located (validate on first firing)
    bool     from_decode = false;
  };
  static RarGrant static_grant(uint16_t tc_rnti)
  {
    RarGrant g;
    g.tc_rnti = tc_rnti;
    return g;
  }
  // TC-RNTI tracker: the gNB assigns them sequentially (observed 0x4601,
  // 0x4602, ...), so after one seed every later RAR's TC-RNTI is known by
  // count. Resync (reseed) on any tripwire: unexpected gNB-log outcome.
  class TcRntiTracker {
   public:
    void reseed(uint16_t base) { base_ = base; seen_ = 0; seeded_ = true; }
    uint16_t next()
    {
      if (!seeded_) return 0;
      return static_cast<uint16_t>(base_ + (seen_++ & 0xffffu));
    }
    bool seeded() const { return seeded_; }
    uint32_t seen() const { return seen_; }
   private:
    uint16_t base_ = 0;
    bool seeded_ = false;
    uint32_t seen_ = 0;
  };

  void on_rar(RarCallback cb) { callback_ = std::move(cb); }
  void emit(const RarEvent& ev) const;

  // Path-2 (P2-3): map a DMRS-level SlotHit + the site's constant grant into
  // the RarEvent the Step-4 UL gate / Msg3 TX consume. Purely additive to the
  // P2-1 detection: no DCI bits required, so it fires even when the polar
  // backend yields nothing (the null-calibrated link). abs_slot -> rar
  // slot_offset via the (frame_start, samples_per_slot) clock so the window
  // math below stays in absolute samples.
  // Conventions (mirror scan_buffer's grant_from_dci so the burst builder and
  // gating behave identically): k = hit_grant.k2_slots, rb start/length pumped
  // through riv_encode -> grant.pusch_freq_res, mcs/tpc from the constant
  // grant, tbs 264 (paper cell-wide default).
  static RarEvent rar_event_from_slot_hit(const SlotHit& hit,
                                          const RarGrant& hit_grant,
                                          uint16_t rapid,
                                          uint64_t buf_start_sample,
                                          uint64_t frame_start_sample,
                                          double samples_per_slot,
                                          uint16_t n_ul_prb);

 private:
  AttackConfig cfg_;
  RarCallback callback_;
  // The RarDecoder is expensive to construct (Pdcch/Pdsch allocations + init
  // logs). Build it lazily once and reuse across scan_buffer() calls.
  mutable std::unique_ptr<nr::RarDecoder> decoder_;
  // Correlation-only PDCCH grid for scan_window(), built lazily once
  // (120 configs x DMRS tables; seconds at most, then reused).
  struct GridEntry {
    std::unique_ptr<nr::Pdcch> pdcch;
    uint16_t freq_prbs = 0;
    uint16_t start_prb = 0;
    uint8_t duration = 0;
    bool interleaved = false;
    uint16_t shift = 0;
  };
  std::vector<GridEntry> grid_;
  bool grid_ready_ = false;
  void ensure_grid();
  float null_floor_ = 0.0f;
  bool  null_ready_ = false;
  // SIB1 buckets: distinct abs slots with hits, keyed by abs % 40.
  std::set<uint64_t> sib_slots_[40];
};

} // namespace gone
