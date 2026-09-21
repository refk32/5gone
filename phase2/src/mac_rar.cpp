#include "5gone/mac_rar.hpp"

#include "5gone/nr_dci.hpp" // riv_decode

#include <cstdio>
#include <exception>

#ifdef GONE_HAVE_SRSRAN_OLD
// Old srsRAN 4G MAC RAR PDU parser. This header is C++ (srsran::mac_rar_pdu_nr
// classes), so unlike the C PDSCH headers it does NOT need an extern "C" wrap.
#include <srsran/mac/mac_rar_pdu_nr.h>
#endif

namespace gone::nr {

MacRar parse_mac_rar(const std::vector<uint8_t>& dlsch_tb)
{
    MacRar out;

    if (dlsch_tb.empty()) {
        return out;
    }

#ifdef GONE_HAVE_SRSRAN_OLD
    // srsRAN_4G MAC RAR PDU parser (C++ classes). It walks the DL-SCH TB and
    // splits it into MAC RAR subPDUs; each one exposes RAPID / TA / temp
    // C-RNTI / UL grant.
    try {
        srsran::mac_rar_pdu_nr pdu;
        if (!pdu.unpack(dlsch_tb.data(), (uint32_t)dlsch_tb.size())) {
            return out;
        }
        uint32_t n = pdu.get_num_subpdus();
        if (n == 0) {
            return out;
        }
        // We take the first RAR subPDU. (A RAR can carry several UEs' grants
        // back-to-back; a real sniffer would iterate over all of them.)
        const srsran::mac_rar_subpdu_nr& sp = pdu.get_subpdu(0);
        if (!sp.has_rapid()) {
            return out;
        }
        out.valid          = true;
        out.rapid          = sp.get_rapid();
        out.timing_advance = sp.get_ta();
        out.t_c_rnti       = sp.get_temp_crnti();
        auto ug            = sp.get_ul_grant();
        out.ul_grant.assign(ug.begin(), ug.end());
    } catch (const std::exception& e) {
        if (out.valid == false) {
            std::fprintf(stderr, "[mac-rar] parse error: %s\n", e.what());
        }
        return out;
    }
#endif

    return out;
}

uint32_t rar_k2_slots(uint8_t tdra_row)
{
    // TS 38.214 Table 6.1.2.1.1-1 (default PUSCH time-domain resource
    // allocation), K2 column. RAR-scheduled PUSCH (Msg3) uses this table when
    // the cell does not configure its own pusch-TimeDomainAllocationList.
    static const uint32_t k2[16] = {1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 8, 8};
    return k2[tdra_row & 0x0F];
}

RarUlGrant decode_rar_ul_grant(const std::vector<uint8_t>& grant_bits, uint32_t n_ul_prb)
{
    RarUlGrant out;
    if (grant_bits.size() < 27) {
        return out; // truncated / corrupt decode — nothing usable
    }

    // TS 38.213 Table 8.2-1 field order, MSB first (matches srsRAN_4G's
    // dci_nr_rar_unpack()): hop(1) + RIV(14) + TDRA(4) + MCS(4) + TPC(3) + CSI(1).
    unsigned     bit = 0;
    auto         next = [&](unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v = (v << 1) | (grant_bits.at(bit++) & 1u);
        }
        return v;
    };

    out.freq_hopping           = next(1) != 0;
    out.freq_domain_assignment = next(14);
    out.time_domain_assignment = static_cast<uint8_t>(next(4));
    out.mcs                    = static_cast<uint8_t>(next(4));
    out.tpc_for_pusch          = static_cast<uint8_t>(next(3));
    out.csi_request            = next(1) != 0;

    riv_decode(out.freq_domain_assignment, n_ul_prb, out.rb_start, out.rb_len);
    out.k2_slots = rar_k2_slots(out.time_domain_assignment);
    out.valid    = true;
    return out;
}

} // namespace gone::nr
