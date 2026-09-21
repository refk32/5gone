#include "5gone/rar_monitor.hpp"
#include "5gone/mac_rar.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pdcch.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gone {

RarMonitor::RarMonitor(const AttackConfig& cfg) : cfg_(cfg) {}

void RarMonitor::emit(const RarEvent& ev) const
{
  if (callback_) callback_(ev);
}

RarEvent RarMonitor::rar_event_from_slot_hit(const SlotHit& hit,
                                             const RarGrant& hit_grant,
                                             uint16_t rapid,
                                             uint64_t buf_start_sample,
                                             uint64_t frame_start_sample,
                                             double samples_per_slot,
                                             uint16_t n_ul_prb)
{
  RarEvent ev;
  ev.rapid = static_cast<uint8_t>(rapid);
  ev.ta = hit_grant.ta;
  ev.c_rnti = hit_grant.tc_rnti;

  const uint32_t riv = nr::riv_encode(hit_grant.rb_start, hit_grant.rb_len, n_ul_prb);
  ev.ul_dci.freq_hopping   = false;
  ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(riv);
  ev.ul_dci.pusch_time_res = 1;
  ev.ul_dci.mcs            = hit_grant.mcs;
  ev.ul_dci.tpc_for_pusch  = hit_grant.tpc;
  ev.ul_dci.csi_request    = false;

  ev.grant.rnti = hit_grant.tc_rnti;
  ev.grant.k = hit_grant.k2_slots;
  ev.grant.mcs = hit_grant.mcs;
  ev.grant.tbs_bits = 264;   // paper cell-wide-dos default
  ev.grant.pusch_freq_res = static_cast<uint16_t>(riv);
  ev.grant.pusch_time_res = 1;

  // Abs slot -> sample offset within the scanned buffer, in the shared clock.
  const uint64_t slot_abs = frame_start_sample +
      static_cast<uint64_t>(std::llround(static_cast<double>(hit.abs_slot) * samples_per_slot));
  ev.rar_slot_offset = slot_abs > buf_start_sample ? slot_abs - buf_start_sample : 0;
  return ev;
}

static UlGrant grant_from_dci(const UlDci& dci, uint16_t c_rnti, uint8_t k = 6)
{
  UlGrant g;
  g.rnti = c_rnti;
  g.k = k;
  g.mcs = dci.mcs;
  g.pusch_freq_res = dci.pusch_freq_res;
  g.pusch_time_res = dci.pusch_time_res;
  g.tbs_bits = 264; // paper cell-wide-dos default
  return g;
}

static RarEvent parse_attacker_log_line(const std::string& line)
{
  RarEvent ev;
  static const std::regex rapid_re(R"("rapid":(\d+))");
  static const std::regex ta_re(R"("ta":(\d+))");
  static const std::regex crnti_re(R"("c_rnti":(\d+))");
  static const std::regex freq_re(R"("pusch_freq_res":(\d+))");
  static const std::regex mcs_re(R"("mcs":(\d+))");
  static const std::regex k_re(R"(\bk=(\d+)\b)");

  std::smatch m;
  if (std::regex_search(line, m, rapid_re)) ev.rapid = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, ta_re)) ev.ta = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, crnti_re)) ev.c_rnti = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, freq_re)) ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, mcs_re)) ev.ul_dci.mcs = static_cast<uint8_t>(std::stoi(m[1].str()));
  ev.ul_dci.pusch_time_res = 1;
  ev.grant = grant_from_dci(ev.ul_dci, ev.c_rnti, 6);
  if (std::regex_search(line, m, k_re)) ev.grant.k = static_cast<uint8_t>(std::stoi(m[1].str()));
  ev.grant.rnti = ev.c_rnti;
  return ev;
}

std::vector<RarEvent> RarMonitor::load_grants_from_file(const std::string& path) const
{
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open grant file: " + path);

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  std::vector<RarEvent> events;

  // JSON grant file (sample_grant.json)
  if (content.find("\"events\"") != std::string::npos) {
    static const std::regex ev_block(R"(\{[^{}]*"rapid"[^{}]*\})");
    auto begin = std::sregex_iterator(content.begin(), content.end(), ev_block);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      events.push_back(parse_attacker_log_line(it->str()));
    }
    if (!events.empty()) return events;
  }

  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("Attacking RAR") != std::string::npos || line.find("\"rapid\"") != std::string::npos) {
      events.push_back(parse_attacker_log_line(line));
    }
  }
  return events;
}

std::vector<RarEvent> RarMonitor::load_grants_from_dataset(const std::string& dataset_dir) const
{
  const std::string log_path = dataset_dir + "/cell-wide-dos/attacker.log";
  return load_grants_from_file(log_path);
}

// Maps the configured bandwidth/SCS to the number of active PRBs. The naive
// bw/(12*scs) over-estimates because it ignores the guard band: the lab cell
// (20 MHz, 30 kHz) has 51 PRBs, matching the gNB config and nr::bwp_num_prbs.
static uint16_t bwp_prbs_from_cfg(const AttackConfig& cfg)
{
  if (cfg.bandwidth_mhz == 20 && cfg.scs_khz == 30) {
    return nr::bwp_num_prbs;   // 51
  }
  return nr::bwp_num_prbs;
}

std::vector<RarEvent> RarMonitor::scan_buffer(const SampleBuffer& iq) const
{
  // Live DL RAR decode + log (receive-only, single B210).
  // This is the ported 5GSniffer PDCCH chain: OFDM -> DM-RS correlation ->
  // (polar decode when srsRAN-4G is linked) -> DCI 1_0 parse -> PDSCH -> MAC RAR.
  //
  // KNOWN LIMIT (live-mode work item, not fixed here): decode() runs with the
  // default starting slot 0 on a stream buffer that starts at an arbitrary
  // sample, so DM-RS scrambling is wrong ~19/20 of the time AND the buffer is
  // not slot-aligned (demod assumes sample 0 = symbol 0). Live firing needs
  // slot-aware, boundary-aligned scanning driven by CellSync — until then this
  // path is kept only for the integer-slot-lucky case, and capture runs skip
  // it entirely (PrachCfg.live_scan) because its per-buffer demod+decode cost
  // is the main RX-stall/overshoot source.
  //
  // Step 1 of live mode: every decoded MAC RAR (RAPID / TA / TC-RNTI + the
  // 27-bit RAR UL grant) is turned into a real RarEvent so the returned list
  // can feed execute_attack() (Msg3 overshadow toward the gNB).
  const uint32_t scs = cfg_.scs_khz ? cfg_.scs_khz * 1000u : nr::scs_hz;
  const uint16_t prbs = bwp_prbs_from_cfg(cfg_);

  if (!decoder_) {
    decoder_ = std::make_unique<nr::RarDecoder>(cfg_.sample_rate, scs, cfg_.pci, prbs);
  }
  std::vector<RarEvent> events;

  for (const nr::RarDciObs& obs : decoder_->decode(iq)) {
    if (!obs.rar_parsed) {
      // R2 honesty fix: a PDCCH we detected but could not turn into a MAC RAR
      // used to be silently dropped, so a blind CORESET / failed PDSCH decode
      // looked like "the gNB never sent a RAR". Surface it (it is not an
      // attackable grant, but it IS evidence the scheduler responded).
      std::printf("[rar-scan] RAR PDCCH seen (no MAC RAR): %s AL=%u slot=%u sym=%u "
                  "cand=%u corr=%.3f\n",
                  obs.decoded_bits ? "DCI bits decoded, PDSCH/RAR failed"
                                   : "DM-RS only (decode disabled or CORESET mismatch)",
                  (unsigned)obs.aggregation_level, (unsigned)obs.slot, (unsigned)obs.symbol,
                  (unsigned)obs.candidate, obs.correlation);
      continue;   // no parsable grant -> cannot drive the Msg3 overshadow
    }

    RarEvent ev;
    ev.rapid  = obs.rapid;
    ev.ta     = static_cast<uint16_t>(obs.timing_advance);
    ev.c_rnti = obs.t_c_rnti;

    const nr::RarUlGrant g = nr::decode_rar_ul_grant(obs.ul_grant, prbs);
    if (g.valid) {
      ev.ul_dci.freq_hopping   = g.freq_hopping;
      ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(g.freq_domain_assignment);
      ev.ul_dci.pusch_time_res = g.time_domain_assignment;
      ev.ul_dci.mcs            = g.mcs;
      ev.ul_dci.tpc_for_pusch  = g.tpc_for_pusch;
      ev.ul_dci.csi_request    = g.csi_request;
    }
    ev.grant = grant_from_dci(ev.ul_dci, ev.c_rnti, static_cast<uint8_t>(g.k2_slots));
    ev.rar_slot_offset = obs.slot_start_sample;

    std::printf("[rar-scan] rapid=%u ta=%u c_rnti=%u hop=%d freq_riv=%u tdra=%u mcs=%u tpc=%u csi=%d k2=%u\n",
                ev.rapid, ev.ta, ev.c_rnti, ev.ul_dci.freq_hopping ? 1 : 0,
                ev.ul_dci.pusch_freq_res, ev.ul_dci.pusch_time_res, ev.ul_dci.mcs,
                ev.ul_dci.tpc_for_pusch, ev.ul_dci.csi_request ? 1 : 0, ev.grant.k);
    events.push_back(ev);
  }

  return events;
}

// ---- Path-2 live detection (P2-1) ----

void RarMonitor::ensure_grid()
{
  if (grid_ready_) return;
  grid_ready_ = true;
  // Empirical hot set: every offset that ever won a real capture, both
  // durations the cell is known to use, both mappings, all shifts.
  // Width per offset: 48 PRBs where it fits inside the BWP (a 48-wide
  // candidate's first 24 RBs share DMRS refs with a 24-wide one at the same
  // start, so wide covers narrow; narrow covers the offsets wide can't
  // reach). 2 x 2 x 2 x 3 + 8 x 2 x 2 x 3 = 120 correlation-only configs
  // (ms per window).
  const uint16_t offs[] = {0, 2, 4, 6, 9, 11, 14, 19, 24, 26};
  const uint8_t durs[] = {2, 3};
  for (uint16_t off : offs) {
    const uint16_t freq = (off + 48 <= nr::bwp_num_prbs) ? 48 : 24;
    for (uint8_t dur : durs) {
      for (int im = 0; im < 2; ++im) {
        for (uint16_t sh = 0; sh < 3; ++sh) {
          nr::Coreset cs;
          cs.control_resourceset_id = 1;
          cs.frequency_domain_resources = freq;
          cs.start_prb = off;
          cs.duration = dur;
          cs.cce_reg_mapping_type = im ? "interleaved" : "non-interleaved";
          cs.reg_bundlesize = 6;
          cs.interleaver_size = 2;
          cs.shift_index = sh;
          cs.cell_id = cfg_.pci;
          cs.starting_ofdm_symbol_within_slot = 0;
          cs.num_symbols_per_slot = 14;
          cs.num_slots_per_frame = 20;
          cs.candidates_search_space = {1, 2, 4, 8, 16};
          auto p = std::make_unique<nr::Pdcch>();
          p->set_coreset_info(cs);
          p->scrambling_id_start = cfg_.pci;
          p->scrambling_id_end = cfg_.pci;
          p->set_corr_thresholds(0.0f);   // report every score; floor applied here
          p->set_decode_enabled(false);   // correlation-only, no srsRAN needed
          p->initialize_dmrs_seq();
          GridEntry e;
          e.pdcch = std::move(p);
          e.freq_prbs = freq;
          e.start_prb = off;
          e.duration = dur;
          e.interleaved = im != 0;
          e.shift = sh;
          grid_.push_back(std::move(e));
        }
      }
    }
  }
}

std::vector<RarMonitor::SlotHit> RarMonitor::scan_window(
    const SampleBuffer& iq, uint64_t abs_start, uint64_t frame_start,
    double sps, double cfo_hz)
{
  std::vector<SlotHit> out;
  if (iq.empty() || sps <= 0.0 || abs_start < frame_start) return {};
  ensure_grid();
  if (grid_.empty()) return {};

  // Next slot boundary at/after the buffer start (sample-exact: sps is
  // integral and all counters stay below 2^53).
  const double rel = static_cast<double>(abs_start - frame_start);
  const uint64_t slot_idx = static_cast<uint64_t>(rel / sps);
  const double rem = rel - static_cast<double>(slot_idx) * sps;
  const uint64_t first_slot = slot_idx + (rem > 0.5 ? 1u : 0u);
  double off_d = static_cast<double>(first_slot) * sps - rel;
  if (off_d < 0.0) off_d = 0.0;
  const size_t off = static_cast<size_t>(off_d + 0.5);
  // Need a whole CORESET past the boundary (3 longest symbols + margin);
  // otherwise this buffer can't hold a PDCCH and the next one covers it.
  const size_t kMinSpan = 3 * 840;
  if (off + kMinSpan > iq.size()) return {};
  const uint8_t slot_in_frame = static_cast<uint8_t>(first_slot % 20);

  // CFO-correct the aligned subspan (same convention as RarDecoder::decode).
  SampleBuffer sub(iq.begin() + static_cast<std::ptrdiff_t>(off), iq.end());
  if (cfo_hz != 0.0) {
    const double twopi_f = 2.0 * 8.0 * std::atan(1.0) * cfo_hz /
                           static_cast<double>(cfg_.sample_rate);
    const std::complex<double> step(std::cos(twopi_f), -std::sin(twopi_f));
    std::complex<double> phasor(1.0, 0.0);
    for (auto& s : sub) {
      const std::complex<float> c(s);
      s = std::complex<float>(
          static_cast<float>(c.real() * phasor.real() - c.imag() * phasor.imag()),
          static_cast<float>(c.real() * phasor.imag() + c.imag() * phasor.real()));
      phasor *= step;
    }
  }

  uint16_t bwp = bwp_prbs_from_cfg(cfg_);
  nr::Ofdm ofdm(cfg_.sample_rate, static_cast<double>(cfg_.scs_khz) * 1000.0, bwp);
  auto symbols = ofdm.demodulate(sub, slot_in_frame);

  const uint8_t base_mod = static_cast<uint8_t>(first_slot % 20);
  for (const auto& g : grid_) {
    auto found = g.pdcch->process(symbols, 0);
    for (const auto& d : found) {
      if (d.correlation < 0.5f) continue;
      SlotHit h;
      // Unwrap the slot label to absolute (window spans <= 2 slots).
      if (d.n_slot == base_mod) h.abs_slot = first_slot;
      else if (d.n_slot == static_cast<uint8_t>((base_mod + 1) % 20))
        h.abs_slot = first_slot + 1;
      else
        h.abs_slot = first_slot + ((d.n_slot + 20u - base_mod) % 20u);
      h.slot = d.n_slot;
      h.symbol = d.n_ofdm;
      h.al = d.found_aggregation_level;
      h.candidate = d.found_candidate;
      h.corr = d.correlation;
      h.freq_prbs = g.freq_prbs;
      h.start_prb = g.start_prb;
      h.duration = g.duration;
      h.interleaved = g.interleaved;
      h.shift = g.shift;
      out.push_back(h);
    }
  }
  return out;
}

void RarMonitor::note_hits_for_sib(const std::vector<SlotHit>& hits)
{
  // Confident hits only (>= 0.9): learning weak hits would pollute buckets
  // with noise. Note this is REPORTING, not a firing veto — a veto here
  // would self-blind on our own periodic occasions landing on a learned
  // bucket. P2-5 gates firing on RAR-window membership, never on SIB veto.
  for (const auto& h : hits) {
    if (h.corr < 0.9f) continue;
    sib_slots_[h.abs_slot % 40u].insert(h.abs_slot);
  }
}

size_t RarMonitor::sib_distinct(uint8_t bucket40) const
{
  return bucket40 < 40 ? sib_slots_[bucket40].size() : 0;
}

void RarMonitor::report_sib_clusters() const
{
  for (unsigned m = 0; m < 40; ++m) {
    if (sib_slots_[m].size() >= 2)
      std::printf("[rar-scan] slot%%%2u: %zu distinct abs slots with hits\n",
                  m, sib_slots_[m].size());
  }
}

void RarMonitor::reset_sib_stats()
{
  for (auto& s : sib_slots_) s.clear();
}

} // namespace gone
