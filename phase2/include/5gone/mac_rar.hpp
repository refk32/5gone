#pragma once

#include <cstdint>
#include <vector>

namespace gone::nr {

struct MacRar {
    bool  valid = false;
    uint8_t rapid = 0;
    uint32_t timing_advance = 0;
    uint16_t t_c_rnti = 0;
    std::vector<uint8_t> ul_grant;
};

MacRar parse_mac_rar(const std::vector<uint8_t>& dlsch_tb);

} // namespace gone::nr