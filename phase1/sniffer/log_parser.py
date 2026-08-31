#!/usr/bin/env python3
"""
log_parser.py — Parse RAR + UL grant from gNB / Amarisoft-style logs.

Supports:
  - enb-export / srsRAN MAC log blocks (multi-line RAR)
  - 5gone attacker.log single-line JSON
"""
from __future__ import annotations

import re
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterator

RAR_START = re.compile(r"RAR:\s*rapid=(\d+)")
RAPID_INLINE = re.compile(r"^\s*rapid=(\d+)\s*$")
TA = re.compile(r"^\s*ta=(\d+)\s*$")
TC_RNTI = re.compile(r"tc-rnti=0x([0-9a-fA-F]+)")
HOPPING = re.compile(r"hopping_flag=(\d+)")
RIV = re.compile(r"riv=(0x[0-9a-fA-F]+|\d+)")
TIME_DOM = re.compile(r"time_domain_rsc=(\d+)")
MCS = re.compile(r"mcs=(\d+)")
TPC = re.compile(r"tpc_command=(\d+)")
CSI = re.compile(r"csi_request=(\d+)")
PUSCH_PRB = re.compile(r"PUSCH:.*\bprb=(\d+):")
ATTACKER_LINE = re.compile(r"Attacking RAR ")
PUSCH_FREQ = re.compile(r'"pusch_freq_res":(\d+)')
CRNTI_JSON = re.compile(r'"c_rnti":(\d+)')


@dataclass
class RarGrant:
    rapid: int
    ta: int = 0
    c_rnti: int = 0
    hopping_flag: int = 0
    riv: int = 0
    time_domain_rsc: int = 1
    mcs: int = 4
    tpc_command: int = 3
    csi_request: int = 0
    pusch_freq_res: int | None = None
    k: int = 6
    tbs_bits: int = 264
    source_line: str = ""

    def to_bus_dict(self) -> dict:
        freq = self.pusch_freq_res if self.pusch_freq_res is not None else riv_to_freq_hint(self.riv)
        return {
            "rapid": self.rapid,
            "ta": self.ta,
            "c_rnti": self.c_rnti,
            "ul_dci": {
                "freq_hopping": bool(self.hopping_flag),
                "pusch_freq_res": freq,
                "pusch_time_res": self.time_domain_rsc,
                "mcs": self.mcs,
                "tpc_for_pusch": self.tpc_command,
                "csi_request": bool(self.csi_request),
                "riv": self.riv,
            },
            "grant": {
                "k": self.k,
                "mcs": self.mcs,
                "tbs_bits": self.tbs_bits,
                "pusch_freq_res": freq,
                "rnti": self.c_rnti,
            },
        }


# Lab mapping from 5gone enb-export riv → PUSCH PRB (approx freq_res for templates)
RIV_FREQ_HINT: dict[int, int] = {
    0x0: 0,
    0x10e: 270,
    0x110: 272,
    0x2: 2,
}


def riv_to_freq_hint(riv: int) -> int:
    if riv in RIV_FREQ_HINT:
        return RIV_FREQ_HINT[riv]
    return riv & 0xFF


def parse_attacker_json_line(line: str) -> RarGrant | None:
    if "Attacking RAR" not in line and '"rapid"' not in line:
        return None
    g = RarGrant(rapid=0, source_line=line.strip())
    m = re.search(r'"rapid":(\d+)', line)
    if m:
        g.rapid = int(m.group(1))
    m = re.search(r'"ta":(\d+)', line)
    if m:
        g.ta = int(m.group(1))
    m = CRNTI_JSON.search(line)
    if m:
        g.c_rnti = int(m.group(1))
    m = PUSCH_FREQ.search(line)
    if m:
        g.pusch_freq_res = int(m.group(1))
    m = re.search(r'"mcs":(\d+)', line)
    if m:
        g.mcs = int(m.group(1))
    m = re.search(r"\bk=(\d+)\b", line)
    if m:
        g.k = int(m.group(1))
    return g


def parse_rar_block(lines: list[str]) -> RarGrant | None:
    text = "\n".join(lines)
    m = RAR_START.search(text)
    if not m:
        return None
    g = RarGrant(rapid=int(m.group(1)), source_line=lines[0].strip())
    for line in lines:
        for pat, attr, conv in [
            (RAPID_INLINE, "rapid", int),
            (TA, "ta", int),
            (HOPPING, "hopping_flag", int),
            (TIME_DOM, "time_domain_rsc", int),
            (MCS, "mcs", int),
            (TPC, "tpc_command", int),
            (CSI, "csi_request", int),
        ]:
            mm = pat.search(line)
            if mm:
                setattr(g, attr, conv(mm.group(1)))
        mm = TC_RNTI.search(line)
        if mm:
            g.c_rnti = int(mm.group(1), 16)
        mm = RIV.search(line)
        if mm:
            raw = mm.group(1)
            g.riv = int(raw, 16) if raw.startswith("0x") else int(raw)
    return g


def iter_rar_grants(path: Path) -> Iterator[RarGrant]:
    if not path.is_file():
        return
    text = path.read_text(errors="replace")
    if "Attacking RAR" in text:
        for line in text.splitlines():
            g = parse_attacker_json_line(line)
            if g:
                yield g
        return

    lines = text.splitlines()
    i = 0
    pending_tc: dict[int, RarGrant] = {}
    while i < len(lines):
        line = lines[i]
        if RAR_START.search(line):
            block = [line]
            j = i + 1
            while j < len(lines) and not RAR_START.search(lines[j]):
                if lines[j].strip() == "" and j > i + 8:
                    break
                if "PDSCH:" in lines[j] or "PDCCH:" in lines[j]:
                    break
                block.append(lines[j])
                j += 1
            g = parse_rar_block(block)
            if g:
                pending_tc[g.c_rnti] = g
                yield g
            i = j
            continue
        mm = PUSCH_PRB.search(line)
        if mm:
            tc = re.search(r"UL\s+[0-9a-f]+\s+\S+\s+([0-9a-f]{4})", line)
            if tc:
                rnti = int(tc.group(1), 16)
                if rnti in pending_tc and pending_tc[rnti].pusch_freq_res is None:
                    pending_tc[rnti].pusch_freq_res = int(mm.group(1))
        i += 1


def parse_log_file(path: Path) -> list[RarGrant]:
    return list(iter_rar_grants(path))
