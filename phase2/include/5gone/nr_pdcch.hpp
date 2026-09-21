#pragma once

#include "5gone/nr_coreset.hpp"
#include "5gone/nr_constants.hpp"
#include "5gone/nr_dci.hpp"
#include "5gone/nr_symbol.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gone::nr {

// PDCCH blind decoder for RAR scheduling DCIs (DCI Format 1_0, RA-RNTI).
// Faithful port of 5GSniffer's nr::pdcch (DMRS correlation + candidate detection),
// with the final polar CRC decode gated behind GONE_HAVE_SRSRAN_OLD (srsRAN_4G C API).
// NOTE: the macro name must NOT start with a digit ('5GONE...' would be an
// invalid preprocessor identifier and silently break the gate).
class Pdcch {
public:
  Pdcch();
  ~Pdcch();

  // Configuration
  uint16_t scrambling_id_start = 0;
  uint16_t scrambling_id_end = 0xffff;
  uint16_t rnti_start = 1;    // RA-RNTI range (TS 38.321; see ra_rnti_max:
  uint16_t rnti_end = 512;    // the lab cell uses 267 = 0x10b, ctor overwrites
                              // both from nr_constants anyway)
  std::vector<uint8_t> dci_sizes_list;          // e.g. {39} for DCI 1_0 @ 51 RB
  std::vector<float> AL_corr_thresholds;        // per AL {1,2,4,8,16}
  int rnti_list_length = 0xffff;
  uint64_t sample_rate_time = 23040000;
  bool decode_enabled = false;  // set true when srsRAN_4G polar decode is available

  Coreset coreset_info;

  void set_coreset_info(const Coreset& c) { coreset_info = c; }

  // Correlation-only sweep helpers: floor every per-AL threshold to the same
  // value (0 = report every candidate with its score), and force the polar/CRC
  // decode path off (or back on). scan_pdcch() / 5gone-decode --coreset-sweep
  // use these so a sweep never needs srsRAN-4G and never throws away a weak
  // but real DM-RS hit just because an AL threshold was tuned for decodes.
  void set_corr_thresholds(float t) {
    AL_corr_thresholds.assign(NUM_ALs, t);
  }
  void set_decode_enabled(bool on) { decode_enabled = on; }

  // Precompute DMRS reference sequences/indices for all scrambling ids / ALs /
  // slots / candidates.
  void initialize_dmrs_seq();

  // Decode all candidate DCIs in `symbols` (already aggregated over CORESET
  // duration by the caller). Returns decoded DCIs with crc_ok == true when the
  // polar/CRC decode succeeded, otherwise the detected candidates.
  std::vector<Dci> process(std::vector<Symbol>& symbols, int64_t metadata);

  // Index/candidate helpers (exposed for testing & PDSCH stage)
  std::vector<uint16_t> get_candidates(uint8_t aggregation_level, uint8_t candidate_idx,
                                       uint8_t num_candidates, uint8_t slotNum, bool user_search_space);
  int compute_Yp(uint8_t slotNum, bool user_search_space);
  uint32_t pdcch_nr_c_init_scrambler(uint16_t rnti, uint16_t pdcch_scrambling_id);

  // The DM-RS reference symbols and their subcarrier positions for a candidate.
  // Useful for tests (to synthesis a transmission) and for the PDSCH stage.
  std::vector<uint64_t> get_dmrs_sc_indices(uint8_t aggregation_level, uint8_t candidate_idx,
                                            uint8_t num_candidates, uint8_t slotNum, bool user_search_space);
  std::vector<uint16_t> get_data_sc_indices(uint8_t aggregation_level, uint8_t candidate_idx,
                                            uint8_t num_candidates, uint8_t slotNum, bool user_search_space);

  // DM-RS reference symbols (in the same order as get_dmrs_sc_indices) for the
  // candidate at `slotNum`, `n_ofdm`. Used to synthesize a transmitted PDCCH.
  std::vector<std::complex<float>> get_dmrs_symbols(uint8_t aggregation_level, uint8_t candidate_idx,
                                                    uint8_t num_candidates, uint8_t slotNum, uint8_t n_ofdm);

private:
  bool correlate_DMRS(Symbol& symbol, std::vector<Dci>& found_dci_list);
  std::vector<std::complex<float>> estimate_channel_dci(Symbol& symbol, const Dci& dci_);
  int  decode_pdcch(Symbol& symbol, std::vector<std::complex<float>>& pdcch_symbols,
                    Dci& dci_, bool rep_opt, int64_t metadata, int symbol_in_chunk);
  std::vector<Dci> get_found_dci_list_per_AL(uint8_t AL, const std::vector<Dci>& found_dci_list);
  int  delete_lower_AL_dcis(uint16_t scrambling_id, uint8_t n_slot, uint8_t n_ofdm,
                            uint8_t candidate_idx, uint8_t AL, std::vector<Dci>& found_dci_list);

  std::vector<uint16_t> cce_reg_interleaving();
  std::vector<uint16_t> get_rb_interleaved(uint8_t aggregation_level);
  std::vector<uint16_t> get_rb_candidates(uint8_t aggregation_level, uint8_t candidate_idx,
                                          uint8_t num_candidates, uint8_t slotNum, bool user_search_space);

  std::unordered_map<std::string, std::vector<std::complex<float>>> dmrs_seq_table_;
  std::unordered_map<std::string, std::vector<uint64_t>> dmrs_sc_indices_table_;
  std::unordered_map<std::string, std::vector<uint16_t>> data_sc_indices_table_;
  std::vector<uint16_t> found_rnti_list_;
};

} // namespace gone::nr
