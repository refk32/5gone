#pragma once

#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * mac_rar.hpp
 * ===========
 * MAC-layer decoding: takes the recovered DL-SCH transport block (the bytes
 * the gNB put on the RAR PDSCH) and pulls out the MAC RAR field(s):
 *   - RAPID (preamble the UE used, from the MAC subheader)
 *   - Timing Advance command
 *   - Temporary C-RNTI
 *   - RAR UL grant (27 bits, TS 38.321 Sec 6.2.3 / TS 38.213 Tab 8.2-1)
 *
 * Wraps srsRAN_4G's mac_rar_pdu_nr parser (gated behind GONE_HAVE_SRSRAN_OLD)
 * and adds a portable decoder that turns those 27 grant bits into a Msg3
 * grant (frequency hop / RIV / TDRA / MCS / TPC / CSI request).
 */

// The useful content of one decoded RAR.
struct MacRar {
    bool  valid = false;      // false if no RAR subPDU could be parsed
    uint8_t rapid = 0;        // preamble ID (0-63) the UE sent
    uint32_t timing_advance = 0;   // 12-bit TA command
    uint16_t t_c_rnti = 0;    // Temporary C-RNTI
    std::vector<uint8_t> ul_grant;  // 27-bit RAR UL grant (bit values, MSB first)
};

// Decoded RAR UL grant — the grant the gNB hands a UE for Msg3 (TS 38.213
// Table 8.2-1 "Random Access Response Grant Content"). Field order on the wire
// is MSB-first, identical to srsRAN_4G's dci_nr_rar_unpack(): hop(1) + RIV(14)
// + TDRA(4) + MCS(4) + TPC(3) + CSI(1) = 27 bits.
struct RarUlGrant {
    bool     valid = false;                   // false when the 27 bits aren't there
    bool     freq_hopping = false;            // 1 bit
    uint32_t freq_domain_assignment = 0;      // 14-bit Type-1 RIV
    uint32_t rb_start = 0;                    // RIV decoded for the UL BWP
    uint32_t rb_len = 0;
    uint8_t  time_domain_assignment = 0;      // 4-bit row into the PUSCH TDRA table
    uint32_t k2_slots = 0;                    // Msg3 slot offset for that row (default table)
    uint8_t  mcs = 0;                         // 4 bits
    uint8_t  tpc_for_pusch = 0;               // 3 bits
    bool     csi_request = false;             // 1 bit
};

// Decode the 27-bit RAR UL grant (as parse_mac_rar / srsRAN_4G extract it) into
// its per-field contents for a UL BWP of n_ul_prb PRBs. Returns valid==false
// when grant_bits has fewer than 27 bits.
RarUlGrant decode_rar_ul_grant(const std::vector<uint8_t>& grant_bits, uint32_t n_ul_prb);

// Msg3 slot offset K2 for a TDRA row in the default PUSCH time-domain resource
// allocation table (TS 38.214 Table 6.1.2.1.1-1), which RAR grants use when no
// higher-layer pusch-TimeDomainAllocationList overrides it.
uint32_t rar_k2_slots(uint8_t tdra_row);

// Parse the DL-SCH transport block bytes into a MacRar. Returns a MacRar with
// valid==false when nothing usable was parsed.
MacRar parse_mac_rar(const std::vector<uint8_t>& dlsch_tb);

} // namespace gone::nr
