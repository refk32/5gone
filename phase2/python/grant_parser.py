#!/usr/bin/env python3
"""Parse RAR grants from 5gone attacker.log or JSON grant files."""
from __future__ import annotations

import json
import re
from pathlib import Path

ATTACK_RAR = re.compile(r"Attacking RAR ")
RAPID = re.compile(r'"rapid":(\d+)')
TA = re.compile(r'"ta":(\d+)')
CRNTI = re.compile(r'"c_rnti":(\d+)')
FREQ = re.compile(r'"pusch_freq_res":(\d+)')
MCS = re.compile(r'"mcs":(\d+)')
K = re.compile(r"\bk=(\d+)\b")


def parse_log_line(line: str) -> dict | None:
    if "Attacking RAR" not in line and '"rapid"' not in line:
        return None
    ev: dict = {"ul_dci": {}, "grant": {}}
    for pat, key in [(RAPID, "rapid"), (TA, "ta"), (CRNTI, "c_rnti")]:
        m = pat.search(line)
        if m:
            ev[key] = int(m.group(1))
    m = FREQ.search(line)
    if m:
        ev["ul_dci"]["pusch_freq_res"] = int(m.group(1))
    m = MCS.search(line)
    if m:
        ev["ul_dci"]["mcs"] = int(m.group(1))
    m = K.search(line)
    k = int(m.group(1)) if m else 6
    ev["grant"] = {
        "k": k,
        "mcs": ev["ul_dci"].get("mcs", 4),
        "tbs_bits": 264,
        "pusch_freq_res": ev["ul_dci"].get("pusch_freq_res", 0),
        "rnti": ev.get("c_rnti", 0),
    }
    return ev


def parse_json_file(path: Path) -> list[dict]:
    data = json.loads(path.read_text())
    if "events" in data:
        return data["events"]
    if isinstance(data, list):
        return data
    return [data]


def parse_grant_file(path: Path) -> list[dict]:
    text = path.read_text(errors="replace")
    if path.suffix == ".json" or text.lstrip().startswith("{"):
        return parse_json_file(path)
    return [ev for line in text.splitlines() if (ev := parse_log_line(line))]
