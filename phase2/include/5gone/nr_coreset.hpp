#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gone::nr {

/*
 * nr_coreset.hpp
 * ==============
 * Describes where/how PDCCH is transmitted: the CORESET (Control REsource SET)
 * and the search space. TS 38.331 (RRC) defines these.
 *
 * CORESET = a "box" of time x frequency resources where PDCCH lives. It is
 * made of REGs (Resource Element Groups = one PRB x one OFDM symbol). These
 * group into CCEs (Control Channel Elements = 6 REGs = 72 REs), which are the
 * quantum of PDCCH. The gNB "blinds" us with several possible CCE sizes
 * (aggregation levels 1/2/4/8/16) at which it might have sent the DCI.
 *
 * For RAR on a Common Search Space the key simplifications are:
 *   - duration           = 1 OFDM symbol
 *   - non-interleaved    = CCEs fill REGs in order (easy to compute)
 *   - cell_id (PCI)      = the DM-RS scrambling id
 *   - candidate per AL   = how many possible spots to try at each AL
 *                          (CSS for RAR: 1 each for AL 1/2, 2 for AL 4/8,
 *                          4 for AL 16).
 */

// CORESET #0 used for the RAR common search space.
struct Coreset {
    uint8_t  control_resourceset_id = 1;
    // Which PRBs the CORESET spans (bitmap -> PRBs). For a full-BWP CORESET
    // this is `(1 << bwp_num_prbs) - 1`.
    uint16_t frequency_domain_resources = 0;
    uint8_t  duration = 1;            // OFDM symbols (1..3)
    std::string cce_reg_mapping_type = "non-interleaved";
    uint8_t  reg_bundlesize = 6;
    uint8_t  interleaver_size = 2;
    uint16_t shift_index = 0;
    uint16_t cell_id = 0;             // = PCI (DM-RS scrambling id for CSS)
    uint8_t  starting_ofdm_symbol_within_slot = 0;
    uint8_t  num_symbols_per_slot = 14;
    uint8_t  num_slots_per_frame = 20;
    // Number of candidates to try at each aggregation level.
    std::vector<uint8_t> candidates_search_space = {1, 2, 4, 8, 16};
};

} // namespace gone::nr
