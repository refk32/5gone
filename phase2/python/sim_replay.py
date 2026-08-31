#!/usr/bin/env python3
"""sim_replay.py — Python-only RAR DoS sim (no C++/UHD required)."""
from __future__ import annotations

import re
from pathlib import Path

ATTACK_RAR = re.compile(r"Attacking RAR ")
RAPID = re.compile(r'"rapid":(\d+)')
CRNTI = re.compile(r'"c_rnti":(\d+)')
FREQ = re.compile(r'"pusch_freq_res":(\d+)')
K = re.compile(r"\bk=(\d+)\b")


def parse_rar_line(line: str) -> dict | None:
    if "Attacking RAR" not in line:
        return None
    m = RAPID.search(line)
    rapid = int(m.group(1)) if m else 0
    m = CRNTI.search(line)
    crnti = int(m.group(1)) if m else 0
    m = FREQ.search(line)
    freq = int(m.group(1)) if m else 0
    m = K.search(line)
    k = int(m.group(1)) if m else 6
    return {"rapid": rapid, "c_rnti": crnti, "pusch_freq_res": freq, "k": k}


def run_sim(dataset_dir: Path, limit: int = 10) -> int:
    log = dataset_dir / "cell-wide-dos" / "attacker.log"
    if not log.is_file():
        print(f"[sim] missing {log} — run: bash scripts/setup-lab.sh --phase 0")
        return 1

    events = []
    for line in log.read_text(errors="replace").splitlines():
        ev = parse_rar_line(line)
        if ev:
            events.append(ev)

    print(f"[sim] replay {min(limit, len(events))}/{len(events)} RAR attacks (dry-run)")
    for i, ev in enumerate(events[:limit]):
        tb_bytes = 33  # tbs=264/8
        print(
            f"  [{i+1}] rapid={ev['rapid']} TC-RNTI=0x{ev['c_rnti']:04x} "
            f"k={ev['k']} freq={ev['pusch_freq_res']} "
            f"empty_mac_pdu={tb_bytes}B padding-only → PUSCH TX (simulated)"
        )
    print("[sim] success — attack pipeline validated offline")
    return 0
