#!/usr/bin/env python3
"""
parse_attacker_logs.py — Extract RAR attack timing from 5gone attacker.log files.

Usage:
  python3 phase0/parse_attacker_logs.py --dataset data/5gone-dataset
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

ATTACK_RAR = re.compile(r"Attacking RAR ")
UL_GRANT_K = re.compile(r"\bk=(\d+)\b")
SYMBOL_ADVANCE = re.compile(
    r"First Symbol 0 sent ([0-9.]+)us in advance"
)
OVERFLOW = re.compile(r"Overflow!")
RAR_DCI = re.compile(r"Looking for \d+ RA-RNTIs")


def parse_log(path: Path) -> dict:
    text = path.read_text(errors="replace")
    advances = [float(m.group(1)) for m in SYMBOL_ADVANCE.finditer(text)]
    attacks = len(ATTACK_RAR.findall(text))
    k_values = [int(m.group(1)) for m in UL_GRANT_K.finditer(text)]

    return {
        "file": str(path),
        "rar_attacks": attacks,
        "overflows": len(OVERFLOW.findall(text)),
        "rar_dci_lookups": len(RAR_DCI.findall(text)),
        "symbol0_advance_us": advances,
        "k_values": k_values,
    }


def summarize(stats: list[dict]) -> str:
    lines = ["5Gone Attacker Log Analysis", "=" * 44]
    all_advances: list[float] = []
    all_k: list[int] = []

    for s in stats:
        rel = Path(s["file"]).parent.name
        adv = s["symbol0_advance_us"]
        lines.append(f"\n[{rel}]")
        lines.append(f"  RAR attacks:     {s['rar_attacks']}")
        lines.append(f"  RAR DCI lookups: {s['rar_dci_lookups']}")
        lines.append(f"  Sync overflows:  {s['overflows']}")
        if adv:
            lines.append(f"  Symbol0 advance: min={min(adv):.1f} max={max(adv):.1f} "
                         f"mean={statistics.mean(adv):.1f} µs (n={len(adv)})")
            all_advances.extend(adv)
        if s["k_values"]:
            k_common = max(set(s["k_values"]), key=s["k_values"].count)
            lines.append(f"  UL grant k:      {k_common} (dominant)")

    lines.append("\n" + "=" * 44)
    lines.append("Cross-log summary")
    if all_advances:
        lines.append(f"  Symbol0 advance (all): mean={statistics.mean(all_advances):.1f} µs, "
                     f"median={statistics.median(all_advances):.1f} µs")
        lines.append(f"  Paper RAR DoS target:  ~672 µs advance (100 MHz X310)")
        lines.append(f"  B210 target (20 MHz):  measure yours — expect higher on USB")
    if all_k:
        lines.append(f"  k values seen: {sorted(set(all_k))}  (k=6 → k2 scheduling in log)")
    lines.append("\nAttack flow (cell-wide-dos):")
    lines.append("  1. Decode RAR on DL (RA-RNTI PDCCH)")
    lines.append("  2. Extract UL grant (k, f_alloc, TC-RNTI)")
    lines.append("  3. Encode empty MAC PDU PUSCH")
    lines.append("  4. TX with Symbol0 advance > 0 before Msg3 slot")
    lines.append("\nDetector hints (Section 8):")
    lines.append("  - Spike in failed RA / no Msg3 completion")
    lines.append("  - Symbol advance consistent ~2ms batch in logs")
    lines.append("  - gNB sees MAC PDU with padding-only (no CCCH SDU)")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--json", type=Path, default=None)
    args = ap.parse_args()

    logs = sorted(args.dataset.glob("*/attacker.log"))
    if not logs:
        print(f"No attacker.log under {args.dataset}", file=sys.stderr)
        return 1

    stats = [parse_log(p) for p in logs]
    report = summarize(stats)
    print(report)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(stats, indent=2))

    return 0


if __name__ == "__main__":
    sys.exit(main())
