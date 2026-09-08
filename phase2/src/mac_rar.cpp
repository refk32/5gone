#include "5gone/mac_rar.hpp"

#include <cstdio>
#include <exception>

#ifdef GONE_HAVE_SRSRAN_OLD
#include <srsran/mac/mac_rar_pdu_nr.h>
#endif

namespace gone::nr {

MacRar parse_mac_rar(const std::vector<uint8_t>& dlsch_tb)
{
    MacRar out;
    if (dlsch_tb.empty()) return out;

#ifdef GONE_HAVE_SRSRAN_OLD
    try {
        srsran::mac_rar_pdu_nr pdu;
        if (!pdu.unpack(dlsch_tb.data(), (uint32_t)dlsch_tb.size())) return out;
        uint32_t n = pdu.get_num_subpdus();
        if (n == 0) return out;
        const srsran::mac_rar_subpdu_nr& sp = pdu.get_subpdu(0);
        if (!sp.has_rapid()) return out;
        out.valid          = true;
        out.rapid          = sp.get_rapid();
        out.timing_advance = sp.get_ta();
        out.t_c_rnti       = sp.get_temp_crnti();
        auto ug            = sp.get_ul_grant();
        out.ul_grant.assign(ug.begin(), ug.end());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[mac-rar] parse error: %s\n", e.what());
        return out;
    }
#endif
    return out;
}

} // namespace gone::nr