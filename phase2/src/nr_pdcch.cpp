#include "5gone/nr_pdcch.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_dmrs.hpp"
#include "5gone/nr_dsp.hpp"

// Old srsRAN 4G C API used by the polar/CRC decode path (nr_pdcch_decode_srsran.ipp).
// Must be included at file scope (srsran.h opens with `extern "C" {`), not inside
// the function where the .ipp is textually expanded — that is why it lives here.
#ifdef GONE_HAVE_SRSRAN_OLD
#include <srsran/srsran.h>
// The umbrella srsran.h (via pdcch_nr.h) declares the polar code/decoder/rm
// routines but NOT chanalloc/interleaver, which the decode chain also uses.
// Those two headers are NOT wrapped in extern "C" by srsran.h, so we must wrap
// them ourselves: the definitions in polar_chanalloc.c / polar_interleaver.c are
// plain C symbols, and calls from this C++ TU will otherwise be C++-mangled and
// fail to link.
extern "C" {
#include <srsran/phy/fec/polar/polar_chanalloc.h>
#include <srsran/phy/fec/polar/polar_interleaver.h>
}
#endif

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace gone::nr {

Pdcch::Pdcch()
{
  scrambling_id_start = 0;
  scrambling_id_end = 0xffff;
  rnti_start = ra_rnti_min;
  rnti_end = ra_rnti_max;
  dci_sizes_list = {};
  // Per-AL DM-RS correlation thresholds. A correctly-aligned DM-RS gives ~1.0;
  // uncorrelated noise gives ~0.15-0.20. 0.5 for the higher ALs keeps false
  // positives out of the correlation-only path (was 0.15, far too low).
  AL_corr_thresholds = {0.9f, 0.8f, 0.7f, 0.5f, 0.5f};
  rnti_list_length = 0xffff;
  decode_enabled = false;
}

Pdcch::~Pdcch() = default;

void Pdcch::initialize_dmrs_seq()
{
  Dmrs dmrs_pdcch;
  const uint8_t symbol_index = coreset_info.starting_ofdm_symbol_within_slot;
  const uint8_t coreset_duration = coreset_info.duration;
  const bool user_search_space = false;

  for (uint8_t slot_index = 0; slot_index < coreset_info.num_slots_per_frame; ++slot_index) {
    for (int agg_level = 0; agg_level < NUM_ALs; ++agg_level) {
      const uint8_t max_num_candidate = coreset_info.candidates_search_space[agg_level];
      for (int candidate_idx = 0; candidate_idx < max_num_candidate; ++candidate_idx) {
        const uint8_t al = static_cast<uint8_t>(1u << agg_level);

        const auto dmrs_rb = get_rb_candidates(al, (uint8_t)candidate_idx, max_num_candidate, slot_index, user_search_space);
        // DMRS SC indices within the BWP. `rb` is CORESET-relative (0..freq-1);
        // coreset_info.start_prb puts the CORESET at its real position inside
        // the BWP. dmrs_gold_idx below stays CORESET-relative (it only indexes
        // the gold sequence, whose length is independent of position).
        std::vector<uint64_t> dmrs_sc;
        std::vector<uint16_t> data_sc;
        const std::vector<uint16_t> dmrs_per_rb = {1, 5, 9};
        const std::vector<uint16_t> data_per_rb = {0, 2, 3, 4, 6, 7, 8, 10, 11};
        for (uint16_t rb : dmrs_rb) {
          const uint16_t rb_abs = static_cast<uint16_t>(rb + coreset_info.start_prb);
          for (uint16_t d : dmrs_per_rb) dmrs_sc.push_back(12u * rb_abs + d);
          for (uint16_t d : data_per_rb) data_sc.push_back(12u * rb_abs + d);
        }

        // Gold-sequence DMRS symbol indices (length = per RB * 3)
        std::vector<uint16_t> dmrs_gold_idx;
        for (uint16_t rb : dmrs_rb) {
          for (int k = 0; k < DMRS_RE_PRB; ++k) dmrs_gold_idx.push_back(3u * rb + (uint16_t)k);
        }

        // Generate DMRS symbols, aggregated over CORESET duration
        std::vector<std::complex<float>> pdcch_dmrs_symbols;
        pdcch_dmrs_symbols.reserve(dmrs_gold_idx.size() * coreset_duration);
        for (uint8_t dur_idx = 0; dur_idx < coreset_duration; ++dur_idx) {
          const auto symbols_al_max = Dmrs::generate_pdcch_dmrs_symb(
              scrambling_id_start, slot_index, (uint8_t)(symbol_index + dur_idx),
              coreset_info.num_symbols_per_slot, 2 * AL_16 * DMRS_SC_CCE);
          for (uint16_t gi : dmrs_gold_idx) {
            pdcch_dmrs_symbols.push_back(symbols_al_max[gi]);
          }
        }

        const std::string key = std::to_string(scrambling_id_start) + std::to_string(agg_level) +
                                std::to_string(slot_index) + std::to_string(candidate_idx);
        dmrs_seq_table_[key] = pdcch_dmrs_symbols;
        dmrs_sc_indices_table_[key] = dmrs_sc;
        data_sc_indices_table_[key] = data_sc;
      }
    }
  }

  // RNTI list over the RA-RNTI candidate range (small, unlike the full brute
  // force used for C-RNTI search).
  found_rnti_list_.clear();
  for (int i = rnti_start; i <= rnti_end; ++i) found_rnti_list_.push_back((uint16_t)i);
}

std::vector<Dci> Pdcch::process(std::vector<Symbol>& symbols, int64_t metadata)
{
  (void)metadata;
  coreset_info.num_symbols_per_slot = 14;

  std::vector<Dci> decoded;
  bool user_search_space = false;
  int symbol_in_chunk = 0;

  for (auto& symbol : symbols) {
    symbol_in_chunk++;
    std::vector<Dci> found_dci_list;

    if (!correlate_DMRS(symbol, found_dci_list)) {
      continue;
    }

    // Without the srsRAN_4G polar decoder, we cannot retrieve the DCI payload
    // bits or check the CRC. What we CAN do is tell the caller WHERE a RAR
    // (PDCCH) is: the DM-RS correlation above already found the (AL, candidate,
    // slot, symbol) with a strong match. This is the POC deliverable — "I found
    // the RAR scheduling DCI, here it is" — even before decoding it.
    if (!decode_enabled) {
      decoded.insert(decoded.end(), found_dci_list.begin(), found_dci_list.end());
      continue;
    }

    // Decode in descending AL order, deleting lower-AL candidates once found.
    for (uint8_t al_idx = NUM_ALs; al_idx > 0; --al_idx) {
      const uint8_t al = static_cast<uint8_t>(1u << (al_idx - 1));
      auto found_dcis = get_found_dci_list_per_AL(al, found_dci_list);
      for (auto& aux : found_dcis) {
        for (uint8_t dci_size : dci_sizes_list) {
          aux.nof_bits = dci_size;

          auto equalized = estimate_channel_dci(symbol, aux);

          bool found_dci = false;
          if (al > AL_4) {
            // AL 8 / 16: SI/RA mode with RNTI=0 + repetition optimization.
            aux.rnti = 0;
            int outp = decode_pdcch(symbol, equalized, aux, true, metadata, symbol_in_chunk);
            // Keep EVERY successful decode, including AL 1: an earlier
            // `> 1` gate silently dropped all AL-1 successes (returning only
            // their correlation shells), which made every weak-but-decodable
            // real RAR — the level every OTA capture hits at — vanish.
            if (outp == 1 && aux.found_aggregation_level >= 1) {
              delete_lower_AL_dcis(aux.pdcch_scrambling_id, aux.n_slot, aux.n_ofdm,
                                   aux.found_candidate, aux.found_aggregation_level, found_dci_list);
              decoded.push_back(aux);
              found_dci = true;
            }
          } else {
            // Iterate candidate RNTIs (RA-RNTI), decoding each.
            const int limit = std::min(rnti_list_length, (int)found_rnti_list_.size());
            for (int rnti_i = 0; rnti_i < limit; ++rnti_i) {
              aux.rnti = found_rnti_list_[rnti_i];
              int outp = decode_pdcch(symbol, equalized, aux, false, metadata, symbol_in_chunk);
              if (outp == 1 && aux.found_aggregation_level >= 1) {
                found_dci = true;
                decoded.push_back(aux);
                break;
              }
            }
          }
          if (found_dci) break;
        }
      }
    }

    // Even with the srsRAN_4G polar decoder enabled, a strong DM-RS correlation
    // that fails polar/CRC is still a confident RAR observation ("where the RAR
    // is"). Keep every detected candidate that did not decode so callers that
    // count RAR PDCCH observations (e.g. the collide victim) see it regardless
    // of whether the DCI bits come back.
    for (const auto& d : found_dci_list) {
      bool dup = false;
      for (const auto& done : decoded) {
        if (done.pdcch_scrambling_id == d.pdcch_scrambling_id &&
            done.found_aggregation_level == d.found_aggregation_level &&
            done.found_candidate == d.found_candidate &&
            done.n_slot == d.n_slot && done.n_ofdm == d.n_ofdm) {
          dup = true;
          break;
        }
      }
      if (!dup) decoded.push_back(d);
    }
  }
  return decoded;
}

std::vector<Dci> Pdcch::get_found_dci_list_per_AL(uint8_t AL, const std::vector<Dci>& found_dci_list)
{
  std::vector<Dci> out;
  out.reserve(found_dci_list.size());
  for (const auto& d : found_dci_list) {
    if (d.found_aggregation_level == AL) out.push_back(d);
  }
  return out;
}

int Pdcch::delete_lower_AL_dcis(uint16_t scrambling_id, uint8_t n_slot, uint8_t n_ofdm,
                                uint8_t candidate_idx, uint8_t AL, std::vector<Dci>& found_dci_list)
{
  (void)scrambling_id;
  int counter = 0;
  bool user_search_space = false;
  const uint8_t al_log = static_cast<uint8_t>(std::log2((float)AL));
  if (al_log < coreset_info.candidates_search_space.size() &&
      coreset_info.candidates_search_space[al_log] > 0) {
    auto cce_indices = get_candidates(AL, candidate_idx, coreset_info.candidates_search_space[al_log],
                                      n_slot, user_search_space);
    std::sort(cce_indices.begin(), cce_indices.end());
    for (auto it = found_dci_list.begin(); it != found_dci_list.end();) {
      if (it->found_aggregation_level < AL && it->n_slot == n_slot && it->n_ofdm == n_ofdm) {
        const uint8_t lo_log = static_cast<uint8_t>(std::log2((float)it->found_aggregation_level));
        auto low = get_candidates(it->found_aggregation_level, it->found_candidate,
                                  coreset_info.candidates_search_space[lo_log], n_slot, user_search_space);
        std::sort(low.begin(), low.end());
        if (std::includes(cce_indices.begin(), cce_indices.end(), low.begin(), low.end())) {
          it = found_dci_list.erase(it);
          ++counter;
          continue;
        }
      }
      ++it;
    }
  }
  return counter;
}

bool Pdcch::correlate_DMRS(Symbol& symbol, std::vector<Dci>& found_dci_list)
{
  bool user_search_space = false;
  bool dci_found = false;

  for (uint32_t pdcch_scrambling_id = scrambling_id_start; pdcch_scrambling_id <= scrambling_id_end; ++pdcch_scrambling_id) {
    // DMRS sequence was generated only for scrambling_id_start; if the scan
    // covers a range, regenerate per ID (kept simple/linear for the RA-RNTI case
    // where the scrambling id == cell id).
    if (pdcch_scrambling_id != scrambling_id_start) {
      continue; // initialization only precomputed for scrambling_id_start
    }

    for (int agg_level = 0; agg_level < NUM_ALs; ++agg_level) {
      const uint8_t max_num_candidate = coreset_info.candidates_search_space[agg_level];
      for (int candidate_idx = 0; candidate_idx < max_num_candidate; ++candidate_idx) {
        // A symbol only carries DM-RS if it lies inside the CORESET span
        // [starting_ofdm_symbol_within_slot, +duration). Symbols outside the
        // span can't correlate against any candidate.
        const uint8_t dur_idx = symbol.symbol_index >= coreset_info.starting_ofdm_symbol_within_slot
                                    ? static_cast<uint8_t>(symbol.symbol_index - coreset_info.starting_ofdm_symbol_within_slot)
                                    : 0xFF;
        if (coreset_info.duration == 0 || dur_idx >= coreset_info.duration) {
          continue;
        }

        const std::string key = std::to_string(pdcch_scrambling_id) + std::to_string(agg_level) +
                                std::to_string(symbol.slot_index) + std::to_string(candidate_idx);

        auto it_sc = dmrs_sc_indices_table_.find(key);
        auto it_seq = dmrs_seq_table_.find(key);
        if (it_sc == dmrs_sc_indices_table_.end() || it_seq == dmrs_seq_table_.end()) {
          continue;
        }
        std::vector<std::complex<float>> rx_dmrs(it_sc->second.size());
        for (size_t i = 0; i < it_sc->second.size(); ++i) {
          const size_t idx = it_sc->second[i];
          rx_dmrs[i] = (idx < symbol.samples.size()) ? symbol.samples[idx] : std::complex<float>(0, 0);
        }

        // The reference sequence is stacked dur_idx 0..duration-1 (each block
        // one OFDM symbol's worth of DM-RS). Slice the current symbol's block
        // so rx_dmrs and the reference have the same length (duration>1).
        const size_t per_sym = coreset_info.duration
                                   ? it_seq->second.size() / coreset_info.duration
                                   : it_seq->second.size();
        if (per_sym == 0 || per_sym != it_sc->second.size()) {
          continue;
        }
        const size_t ref_begin = static_cast<size_t>(dur_idx) * per_sym;
        const std::vector<std::complex<float>> ref_slice(it_seq->second.begin() + ref_begin,
                                                        it_seq->second.begin() + ref_begin + per_sym);

        std::vector<float> corr_out;
        correlate_magnitude_normalized(corr_out, rx_dmrs, ref_slice);
        const float corr = corr_out.empty() ? 0.0f : corr_out[0];

        if (corr > AL_corr_thresholds[agg_level]) {
          Dci d;
          d.found_possible_dci = true;
          d.found_aggregation_level = static_cast<uint8_t>(1u << agg_level);
          d.found_candidate = (uint8_t)candidate_idx;
          d.max_num_candidate = max_num_candidate;
          d.pdcch_scrambling_id = (uint16_t)pdcch_scrambling_id;
          d.n_slot = symbol.slot_index;
          d.n_ofdm = symbol.symbol_index;
          d.correlation = corr;
          found_dci_list.push_back(d);
          dci_found = true;
        }
      }
    }
  }
  return dci_found && !found_dci_list.empty();
}

std::vector<std::complex<float>> Pdcch::estimate_channel_dci(Symbol& symbol, const Dci& dci_)
{
  const bool user_search_space = false;
  const uint8_t al_log = static_cast<uint8_t>(std::log2((float)dci_.found_aggregation_level));
  const std::string key = std::to_string(dci_.pdcch_scrambling_id) + std::to_string(al_log) +
                          std::to_string(dci_.n_slot) + std::to_string(dci_.found_candidate);

  auto it_sc = dmrs_sc_indices_table_.find(key);
  auto it_seq = dmrs_seq_table_.find(key);
  auto it_data = data_sc_indices_table_.find(key);
  if (it_sc == dmrs_sc_indices_table_.end() || it_seq == dmrs_seq_table_.end() || it_data == data_sc_indices_table_.end()) {
    return {};
  }

  const uint64_t sc_start = it_data->second.empty() ? 0 : it_data->second.front();
  const uint64_t sc_end = it_data->second.empty() ? 0 : it_data->second.back();

  // Same per-symbol slicing as correlate_DMRS: the stacked reference covers the
  // whole CORESET duration; use only this DCI's symbol's block (duration>1).
  const uint8_t dur_idx = dci_.n_ofdm >= coreset_info.starting_ofdm_symbol_within_slot
                              ? static_cast<uint8_t>(dci_.n_ofdm - coreset_info.starting_ofdm_symbol_within_slot)
                              : 0xFF;
  if (coreset_info.duration == 0 || dur_idx >= coreset_info.duration) {
    return {};
  }
  const size_t per_sym = coreset_info.duration
                             ? it_seq->second.size() / coreset_info.duration
                             : it_seq->second.size();
  if (per_sym == 0 || per_sym != it_sc->second.size()) {
    return {};
  }
  const size_t ref_begin = static_cast<size_t>(dur_idx) * per_sym;
  const std::vector<std::complex<float>> ref_slice(it_seq->second.begin() + ref_begin,
                                                   it_seq->second.begin() + ref_begin + per_sym);
  symbol.channel_estimate(ref_slice, it_sc->second, sc_start, sc_end);

  std::vector<std::complex<float>> out;
  out.reserve(it_data->second.size());
  for (uint16_t si : it_data->second) {
    out.push_back(symbol.samples_eq[si]);
  }
  return out;
}

int Pdcch::decode_pdcch(Symbol& symbol, std::vector<std::complex<float>>& pdcch_symbols,
                        Dci& dci_, bool rep_opt, int64_t metadata, int symbol_in_chunk)
{
  (void)symbol;
  (void)metadata;
  (void)symbol_in_chunk;
  dci_.crc_ok = false;

#ifndef GONE_HAVE_SRSRAN_OLD
  // Polar decode requires srsRAN_4G (old C API: srsran_polar_code_get,
  // srsran_demod_soft_demodulate_b, srsran_crc24c, ...). Without it we can only
  // report the DMRS-correlation candidate, not the decoded DCI bits.
  (void)rep_opt;
  (void)pdcch_symbols;
  return 0;
#else
  // Included only when building against srsRAN_4G on the Linux/office machine.
  // The .ipp body contains its own return value (1 = CRC ok, 0 = failed).
  #include "5gone/nr_pdcch_decode_srsran.ipp"
#endif
}

std::vector<uint16_t> Pdcch::cce_reg_interleaving()
{
  const uint8_t L = coreset_info.reg_bundlesize;
  const uint8_t R = coreset_info.interleaver_size;
  const uint8_t M = coreset_info.duration;
  const uint16_t N_CORESET_RB = coreset_info.frequency_domain_resources;
  const uint16_t N_CORESET_REG = N_CORESET_RB * M;
  const uint16_t nshift = coreset_info.shift_index;
  const uint16_t C = N_CORESET_REG / (R * L);

  std::vector<uint16_t> interleaved_f;
  if (coreset_info.cce_reg_mapping_type == "interleaved") {
    interleaved_f.resize((C - 1) * R + (R - 1) + 1 <= 0 ? 0 : (C - 1) * R + (R - 1) + 1);
    for (int C_idx = 0; C_idx < C; ++C_idx) {
      for (int R_idx = 0; R_idx < R; ++R_idx) {
        interleaved_f[C_idx * R + R_idx] =
            (uint16_t)((R_idx * C + C_idx + nshift) % (N_CORESET_REG / L));
      }
    }
  } else {
    const size_t n = ((size_t)(C - 1) * R + (R - 1)) + 1;
    interleaved_f.resize(n);
    for (size_t i = 0; i < n; ++i) interleaved_f[i] = (uint16_t)i;
  }
  return interleaved_f;
}

std::vector<uint16_t> Pdcch::get_rb_interleaved(uint8_t aggregation_level)
{
  (void)aggregation_level;
  const auto interleaving_f = cce_reg_interleaving();
  const uint16_t bw = coreset_info.frequency_domain_resources;
  const uint16_t dur = coreset_info.duration;

  std::vector<uint16_t> rb_complete_list;
  rb_complete_list.reserve((size_t)bw * dur);
  for (uint16_t i = 0; i < bw; ++i)
    for (uint16_t j = 0; j < dur; ++j)
      rb_complete_list.push_back((uint16_t)(i + j * bw));

  std::vector<uint16_t> rb_interleaved;
  rb_interleaved.reserve(rb_complete_list.size());
  for (uint16_t f : interleaving_f) {
    const size_t start = (size_t)coreset_info.reg_bundlesize * f;
    const size_t end = (size_t)coreset_info.reg_bundlesize * (f + 1);
    rb_interleaved.insert(rb_interleaved.end(), rb_complete_list.begin() + start,
                          rb_complete_list.begin() + end);
  }
  return rb_interleaved;
}

std::vector<uint16_t> Pdcch::get_rb_candidates(uint8_t aggregation_level, uint8_t candidate_idx,
                                               uint8_t num_candidates, uint8_t slotNum, bool user_search_space)
{
  auto cce_indices = get_candidates(aggregation_level, candidate_idx, num_candidates, slotNum, user_search_space);
  auto rb_interleaved = get_rb_interleaved(aggregation_level);
  std::vector<uint16_t> rb_idx_candidates;
  rb_idx_candidates.reserve(CCE_REG * cce_indices.size());
  for (uint16_t cce : cce_indices) {
    for (int rb_idx = 0; rb_idx < CCE_REG; ++rb_idx) {
      const size_t pos = (size_t)cce * CCE_REG + rb_idx;
      if (pos < rb_interleaved.size()) rb_idx_candidates.push_back(rb_interleaved[pos]);
    }
  }
  // The flat REG index encodes (symbol-layer * bw) + CORESET-PRB (see
  // get_rb_interleaved). PDCCH DM-RS occupies the SAME PRBs in every symbol of
  // a duration>1 CORESET, so reduce to the unique CORESET-relative PRB set;
  // otherwise symbol-layer bytes masquerade as +bw BWP PRBs and address SCs
  // past the BWP edge (the R1/B duration-2 bug).
  for (uint16_t& rb : rb_idx_candidates) rb %= coreset_info.frequency_domain_resources;
  std::sort(rb_idx_candidates.begin(), rb_idx_candidates.end());
  rb_idx_candidates.erase(std::unique(rb_idx_candidates.begin(), rb_idx_candidates.end()),
                          rb_idx_candidates.end());
  return rb_idx_candidates;
}

std::vector<uint64_t> Pdcch::get_dmrs_sc_indices(uint8_t aggregation_level, uint8_t candidate_idx,
                                                 uint8_t num_candidates, uint8_t slotNum, bool user_search_space)
{
  const std::vector<uint16_t> dmrs_per_rb = {1, 5, 9};
  auto rb_dmrs_idx = get_rb_candidates(aggregation_level, candidate_idx, num_candidates, slotNum, user_search_space);
  std::vector<uint64_t> sc;
  sc.reserve(aggregation_level * DMRS_SC_CCE);
  for (uint16_t rb : rb_dmrs_idx)
    for (uint16_t d : dmrs_per_rb) sc.push_back(12u * (uint64_t)(rb + coreset_info.start_prb) + d);
  return sc;
}

std::vector<uint16_t> Pdcch::get_data_sc_indices(uint8_t aggregation_level, uint8_t candidate_idx,
                                                 uint8_t num_candidates, uint8_t slotNum, bool user_search_space)
{
  const std::vector<uint16_t> data_per_rb = {0, 2, 3, 4, 6, 7, 8, 10, 11};
  auto rb_data_idx = get_rb_candidates(aggregation_level, candidate_idx, num_candidates, slotNum, user_search_space);
  std::vector<uint16_t> sc;
  sc.reserve(aggregation_level * 3 * DMRS_SC_CCE);
  for (uint16_t rb : rb_data_idx)
    for (uint16_t d : data_per_rb) sc.push_back(12u * (uint16_t)(rb + coreset_info.start_prb) + d);
  return sc;
}

std::vector<std::complex<float>> Pdcch::get_dmrs_symbols(uint8_t aggregation_level, uint8_t candidate_idx,
                                                         uint8_t num_candidates, uint8_t slotNum, uint8_t n_ofdm)
{
  // Re-generate the reference DM-RS for this candidate, exactly as
  // initialize_dmrs_seq() does (kept consistent so tests can synthesize a tx).
  const uint8_t al_log = static_cast<uint8_t>(std::log2((float)aggregation_level));
  const bool user_search_space = false;
  const uint8_t symbol_index = coreset_info.starting_ofdm_symbol_within_slot;
  const uint8_t coreset_duration = coreset_info.duration;

  const auto dmrs_rb = get_rb_candidates(aggregation_level, candidate_idx, num_candidates, slotNum,
                                         user_search_space);
  std::vector<uint16_t> dmrs_gold_idx;
  for (uint16_t rb : dmrs_rb)
    for (int k = 0; k < DMRS_RE_PRB; ++k) dmrs_gold_idx.push_back(3u * rb + (uint16_t)k);

  std::vector<std::complex<float>> out;
  out.reserve(dmrs_gold_idx.size() * coreset_duration);
  for (uint8_t dur_idx = 0; dur_idx < coreset_duration; ++dur_idx) {
    const auto seq = Dmrs::generate_pdcch_dmrs_symb(scrambling_id_start, slotNum,
                                                    (uint8_t)(symbol_index + dur_idx),
                                                    coreset_info.num_symbols_per_slot,
                                                    2 * AL_16 * DMRS_SC_CCE);
    for (uint16_t gi : dmrs_gold_idx) out.push_back(seq[gi]);
  }
  (void)al_log;
  (void)n_ofdm;
  return out;
}

std::vector<uint16_t> Pdcch::get_candidates(uint8_t aggregation_level, uint8_t candidate_idx,
                                            uint8_t num_candidates, uint8_t slotNum, bool user_search_space)
{
  const uint8_t num_CCE = (coreset_info.frequency_domain_resources * coreset_info.duration) / CCE_REG;
  uint8_t nCI = 0;
  std::vector<uint16_t> cce_indices;
  cce_indices.reserve(aggregation_level);
  const int Yp = compute_Yp(slotNum, user_search_space);
  for (int cce_idx = 0; cce_idx < aggregation_level; ++cce_idx) {
    const int n_CCE_floor = (num_CCE == 0) ? 1 : (int)std::floor((float)num_CCE / aggregation_level);
    const int idx = Yp + (int)std::floor((candidate_idx * num_CCE) / (float)(aggregation_level * num_candidates)) + nCI;
    cce_indices.push_back((uint16_t)(aggregation_level * ((idx % (n_CCE_floor < 1 ? 1 : n_CCE_floor))) + cce_idx));
  }
  return cce_indices;
}

int Pdcch::compute_Yp(uint8_t slotNum, bool user_search_space)
{
  int Yp = 0;
  const int D = 65537;
  uint16_t Ap = 0;
  switch (coreset_info.control_resourceset_id % 3) {
    case 0: Ap = 39827; break;
    case 1: Ap = 39829; break;
    default: Ap = 39839; break;
  }
  if (user_search_space) {
    // Not used for RA-RNTI (common search space).
    Yp = 0;
  } else {
    Yp = 0; // CSS: Y_p = 0
  }
  (void)slotNum;
  (void)Ap;
  return Yp;
}

uint32_t Pdcch::pdcch_nr_c_init_scrambler(uint16_t rnti, uint16_t pdcch_scrambling_id)
{
  return ((rnti << 16U) + pdcch_scrambling_id) & 0x7fffffffU;
}

} // namespace gone::nr
