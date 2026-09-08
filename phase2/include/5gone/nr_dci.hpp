#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace gone::nr {

/*
 * A detected / decoded PDCCH DCI candidate. One of these is created for every
 * candidate (AL x slot x DMRS-scrambling-id) whose DM-RS correlation exceeds
 * the threshold in Step 7. Once the polar decoder (Step 8) recovers the bits,
 * `payload` and `crc_ok` are filled in.
 */
struct Dci {
    bool found_possible_dci = false;
    uint8_t found_aggregation_level = 1;   // 1, 2, 4, 8, or 16
    uint8_t found_candidate = 0;           // which candidate index within the AL
    uint8_t max_num_candidate = 0;         // how many candidates for that AL
    uint16_t rnti = 0;                     // RA-RNTI that passed CRC
    std::string rnti_type = "ra-rnti";
    std::vector<uint8_t> payload;          // decoded DCI bits, MSB first
    uint16_t nof_bits = 0;                 // payload bit count (without CRC)
    uint16_t pdcch_scrambling_id = 0;      // DM-RS scrambling id (= PCI for CSS)
    uint8_t n_slot = 0;                    // slot within frame
    uint8_t n_ofdm = 0;                    // OFDM symbol within slot
    float correlation = 0.0f;              // DM-RS correlation that found it
    bool crc_ok = false;                   // polar/CRC decode succeeded
};



// Parsed DCI Format 1_0 (schedules the RAR PDSCH).
struct DciFormat10 {
    bool     valid = false;
    uint32_t identifier_dci_formats = 0;   // 1 bit, must be 1 (DL assignment)
    // Frequency-domain resource assignment (Type-1 RIV) of ceil(log2(N(N+1)/2)) bits.
    uint32_t freq_domain_riv = 0;          // packed RIV value
    uint32_t n_start_prb = 0;              // first allocated PRB (decoded from RIV)
    uint32_t n_length_prb = 0;             // number of contiguous PRBs
    uint8_t  time_domain_assignment = 0;   // 4 bits -> TDRA table row (PDSCH timing)
    uint8_t  vrb_to_prb_mapping = 0;       // 1 bit
    uint8_t  mcs = 0;                      // 5 bits -> modulation + code rate
    uint8_t  new_data_indicator = 0;       // 1 bit
    uint8_t  redundancy_version = 0;       // 2 bits
    uint8_t  harq_process_number = 0;      // 4 bits
    uint8_t  dl_assignment_index = 0;      // 2 bits
    uint8_t  tpc_for_pucch = 0;            // 2 bits
    uint8_t  pucch_resource_indicator = 0; // 3 bits
    uint8_t  pdsch_harq_timing = 0;        // 3 bits

    // Parse DCI 1_0 from `bits` (MSB first, payload only, no CRC) for a BWP of
    // n_rb_bwp PRBs. Sets `valid` only if it's a proper DL 1_0 assignment.
    static DciFormat10 parse(const std::vector<uint8_t>& bits, uint32_t n_rb_bwp);
};

// Decode a Type-1 frequency assignment RIV (TS 38.214 5.1.2.2.1) into
// (n_start_prb, n_length_prb) for a BWP of n_rb_bwp PRBs.
void riv_decode(uint32_t riv, uint32_t n_rb_bwp, uint32_t& n_start_prb, uint32_t& n_length_prb);

// Number of DCI 1_0 payload bits for the given BWP size.
uint32_t dci_format10_bits(uint32_t n_rb_bwp);

} // namespace gone::nr
