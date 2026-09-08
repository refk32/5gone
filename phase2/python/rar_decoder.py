#!/usr/bin/env python3
"""
rar_decoder.py — 5G-NR MAC RAR & 27-Bit Uplink Grant Bitfield Dissector.

Compliant with:
  - 3GPP TS 38.321 §6.2.3 (MAC PDU for Random Access Response)
  - 3GPP TS 38.213 §8.2   (Random Access Procedure — RAR UL Grant Format)

Educational Purpose:
  Demonstrates how a 5G-NR receiver unpacks binary MAC RAR payloads into
  individual protocol fields (Timing Advance, TC-RNTI, Frequency Allocation, MCS, and Delay k).
"""

from __future__ import annotations

import dataclasses
from typing import Optional


@dataclasses.dataclass
class RarUlGrant:
    """Represents the 27-bit RAR UL Grant defined in 3GPP TS 38.213 Table 8.2-1."""
    freq_hopping_flag: int       # 1 bit
    pusch_freq_allocation: int   # 14 bits (RIV / PRB allocation)
    pusch_time_allocation: int   # 4 bits (Points to TDRA table entry, determines slot delay k)
    mcs: int                     # 4 bits (Modulation and Coding Scheme, 0-15)
    tpc_command: int             # 3 bits (Transmit Power Control, 0-7)
    csi_request: int             # 1 bit  (Channel State Information request)
    raw_grant_bits: int          # Full 27-bit integer

    @classmethod
    def from_uint27(cls, grant27: int) -> RarUlGrant:
        """Extracts individual bitfields from a 27-bit unsigned integer."""
        # Field unpacking according to 3GPP TS 38.213 §8.2:
        # [26]    : Frequency hopping flag (1 bit)
        # [25:12] : PUSCH frequency resource allocation (14 bits)
        # [11:8]  : PUSCH time resource allocation (4 bits)
        # [7:4]   : MCS (4 bits)
        # [3:1]   : TPC command (3 bits)
        # [0]     : CSI request (1 bit)
        return cls(
            freq_hopping_flag=(grant27 >> 26) & 0x1,
            pusch_freq_allocation=(grant27 >> 12) & 0x3FFF,
            pusch_time_allocation=(grant27 >> 8) & 0x0F,
            mcs=(grant27 >> 4) & 0x0F,
            tpc_command=(grant27 >> 1) & 0x07,
            csi_request=grant27 & 0x01,
            raw_grant_bits=grant27 & 0x07FFFFFF,
        )


@dataclasses.dataclass
class MacRarPayload:
    """Represents a decoded 5G MAC RAR payload (7 octets / 56 bits total)."""
    rapid: int                   # Random Access Preamble ID (0-63) from subheader
    timing_advance: int          # 12 bits (TA command: 0 - 3846)
    tc_rnti: int                 # 16 bits (Temporary Cell RNTI: 0x0001 - 0xFFFD)
    ul_grant: RarUlGrant         # 27 bits (Decoded Uplink Grant)
    raw_bytes: bytes

    @classmethod
    def from_bytes(cls, raw: bytes, rapid: int = 0) -> Optional[MacRarPayload]:
        """
        Decodes a standard 7-byte MAC RAR payload:
          Octet 1: [R (1 bit)] [Timing Advance Command (7 bits)]
          Octet 2: [Timing Advance Command (5 bits)] [UL Grant (3 bits)]
          Octet 3: [UL Grant (8 bits)]
          Octet 4: [UL Grant (8 bits)]
          Octet 5: [UL Grant (8 bits)]
          Octet 6: [TC-RNTI MSB (8 bits)]
          Octet 7: [TC-RNTI LSB (8 bits)]
        """
        if len(raw) < 7:
            raise ValueError(f"MAC RAR payload must be at least 7 bytes, got {len(raw)}")

        # 1. Timing Advance (12 bits): bits [6:0] of Octet 1 + bits [7:3] of Octet 2
        ta = ((raw[0] & 0x7F) << 5) | ((raw[1] >> 3) & 0x1F)

        # 2. 27-bit UL Grant: bits [2:0] of Octet 2 + Octets 3, 4, 5
        grant27 = (
            ((raw[1] & 0x07) << 24)
            | (raw[2] << 16)
            | (raw[3] << 8)
            | raw[4]
        )
        grant = RarUlGrant.from_uint27(grant27)

        # 3. Temporary C-RNTI (16 bits): Octet 6 (MSB) + Octet 7 (LSB)
        tc_rnti = (raw[5] << 8) | raw[6]

        return cls(
            rapid=rapid,
            timing_advance=ta,
            tc_rnti=tc_rnti,
            ul_grant=grant,
            raw_bytes=raw[:7],
        )

    def summary(self) -> str:
        """Returns a formatted academic summary of the decoded parameters."""
        return (
            f"=== 5G MAC RAR Dissection (TS 38.321 / TS 38.213) ===\n"
            f"  RAPID (Preamble ID)    : {self.rapid}\n"
            f"  Timing Advance (TA)    : {self.timing_advance} ($T_A$ units)\n"
            f"  Assigned TC-RNTI       : 0x{self.tc_rnti:04X} ({self.tc_rnti})\n"
            f"  27-bit UL Grant Details:\n"
            f"    - Freq Hopping Flag  : {self.ul_grant.freq_hopping_flag}\n"
            f"    - PUSCH Freq Alloc   : {self.ul_grant.pusch_freq_allocation} (PRB Resource Indication Value)\n"
            f"    - PUSCH Time Alloc   : {self.ul_grant.pusch_time_allocation} (TDRA index -> delay k)\n"
            f"    - Modulation & Coding: MCS {self.ul_grant.mcs}\n"
            f"    - TPC Power Command  : {self.ul_grant.tpc_command} (dB adjustment index)\n"
            f"    - CSI Request        : {self.ul_grant.csi_request}\n"
            f"====================================================="
        )


def self_test():
    """Self-test demonstrating parsing of a standard 5G MAC RAR binary sequence."""
    sample_bytes = bytes([
        0x00, 0xF8, 0x01, 0x0E, 0x44, 0x46, 0x01
    ])
    rar = MacRarPayload.from_bytes(sample_bytes, rapid=35)
    print(rar.summary())


if __name__ == "__main__":
    self_test()
