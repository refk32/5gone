#include "5gone/empty_mac_pdu.hpp"

namespace gone {

std::vector<uint8_t> build_empty_mac_pdu(std::size_t tb_bytes)
{
  // MAC subPDU: single-byte subheader with F=0, L=111111 (padding to end of MAC PDU)
  // 38.321 Table 6.2.1-2 — Padding with 1-byte subheader when rest is padding.
  if (tb_bytes == 0) return {};
  if (tb_bytes == 1) return {0x3F}; // all padding, minimal

  std::vector<uint8_t> pdu;
  pdu.reserve(tb_bytes);
  // Subheader: R/F/LCID — LCID=63 (padding), F=0, R=0 → 0x3F
  pdu.push_back(0x3F);
  while (pdu.size() < tb_bytes) {
    pdu.push_back(0x00);
  }
  return pdu;
}

} // namespace gone
