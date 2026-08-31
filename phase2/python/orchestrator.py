#!/usr/bin/env python3
"""
orchestrator.py — Phase 2 attack demo orchestrator (sim / inject / live prep).

Usage:
  python3 phase2/python/orchestrator.py sim
  python3 phase2/python/orchestrator.py inject --grant phase2/config/sample_grant.json
  python3 phase2/python/orchestrator.py demo
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ATTACKER = ROOT / "build" / "phase2" / "5gone-rar-dos"
CONFIG = ROOT / "phase2" / "config" / "rar_dos.yaml"


def run_attacker(mode: str, extra: list[str] | None = None) -> int:
    extra = extra or []
    if ATTACKER.is_file():
        cmd = [str(ATTACKER), str(CONFIG), "--mode", mode] + extra
    else:
        print("[orchestrator] binary not built — using Python sim fallback")
        return run_python_sim()
    print(f"[orchestrator] {' '.join(cmd)}")
    return subprocess.call(cmd, cwd=ROOT)


def run_python_sim() -> int:
    from phase2.python.sim_replay import run_sim

    return run_sim(ROOT / "data" / "5gone-dataset", limit=10)


def cmd_demo() -> int:
    print("=" * 50)
    print(" 5Gone Phase 2 — Attack Demo (RAR DoS)")
    print(" Paper Section 4.1 — Cell-Wide DoS")
    print("=" * 50)
    print()
    print("Flow:")
    print("  1. gNB sends RAR on DL (RA-RNTI PDCCH)")
    print("  2. Attacker extracts UL grant (k, freq_alloc, TC-RNTI)")
    print("  3. Encode empty MAC PDU (padding only)")
    print("  4. TX PUSCH Msg3 with Symbol0 advance > UE")
    print()

    ds = ROOT / "data" / "5gone-dataset" / "cell-wide-dos" / "attacker.log"
    if ds.is_file():
        print(f"[demo] dataset reference: {ds}")
        lines = [l for l in ds.read_text(errors="replace").splitlines() if "Attacking RAR" in l]
        print(f"[demo] {len(lines)} RAR attacks in paper trace")
        if lines:
            print(f"[demo] sample: {lines[0][:120]}...")
    print()
    return run_attacker("sim")


def main() -> int:
    ap = argparse.ArgumentParser(description="5Gone Phase 2 orchestrator")
    ap.add_argument("command", choices=["sim", "inject", "live", "demo"])
    ap.add_argument("--grant", type=Path, default=ROOT / "phase2" / "config" / "sample_grant.json")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.command == "demo":
        return cmd_demo()

    extra: list[str] = []
    if args.command == "inject":
        extra += ["--grant", str(args.grant)]
    if args.dry_run:
        extra += ["--dry-run"]

    return run_attacker(args.command, extra)


if __name__ == "__main__":
    sys.path.insert(0, str(ROOT))
    raise SystemExit(main())
