#pragma once

#include <cstdint>
#include <vector>

namespace gone {

// 3GPP TS 38.321 — MAC PDU with padding-only subPDUs (Msg3 overshadow payload).
std::vector<uint8_t> build_empty_mac_pdu(std::size_t tb_bytes);

} // namespace gone
