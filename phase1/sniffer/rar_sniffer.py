#!/usr/bin/env python3
"""
rar_sniffer.py — Passive RAR sniffer → grant bus (/tmp/5gone_grants.jsonl)

Modes:
  tail   — follow live gNB MAC log (/tmp/gnb_5gone.log)
  replay — replay enb-export or attacker.log into grant bus (demo)
  once   — parse entire log file and exit

Usage:
  python3 phase1/sniffer/rar_sniffer.py tail
  python3 phase1/sniffer/rar_sniffer.py replay --log data/5gone-dataset/cell-wide-dos/enb-export-cell-wide-dos.log
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase1.sniffer.log_parser import RAR_START, parse_rar_block, parse_attacker_json_line  # noqa: E402

DEFAULT_BUS = Path("/tmp/5gone_grants.jsonl")
DEFAULT_GNB_LOG = Path("/tmp/gnb_5gone.log")
FALLBACK_GNB_LOG = ROOT / "run" / "gnb.log"


def publish(grant: dict, bus: Path) -> None:
    line = json.dumps(grant, separators=(",", ":")) + "\n"
    with bus.open("a") as f:
        f.write(line)
    print(
        f"[sniffer] RAR rapid={grant['rapid']} TC-RNTI=0x{grant['c_rnti']:04x} "
        f"freq={grant['grant']['pusch_freq_res']} → {bus}"
    )


def cmd_replay(args: argparse.Namespace) -> int:
    log_path = Path(args.log)
    bus = Path(args.bus)
    bus.write_text("")  # clear bus for clean demo
    delay = args.delay_ms / 1000.0

    if "attacker.log" in log_path.name:
        for line in log_path.read_text(errors="replace").splitlines():
            g = parse_attacker_json_line(line)
            if not g:
                continue
            publish(g.to_bus_dict(), bus)
            if delay > 0:
                time.sleep(delay)
    else:
        from phase1.sniffer.log_parser import iter_rar_grants

        for g in iter_rar_grants(log_path):
            publish(g.to_bus_dict(), bus)
            if delay > 0:
                time.sleep(delay)

    print(f"[sniffer] replay done → {bus}")
    return 0


def cmd_tail(args: argparse.Namespace) -> int:
    log_path = Path(args.log)
    if not log_path.is_file() and args.log == str(DEFAULT_GNB_LOG):
        fb = FALLBACK_GNB_LOG
        if fb.is_file():
            print(f"[sniffer] {log_path} not found — falling back to {fb}")
            log_path = fb
    bus = Path(args.bus)
    print(f"[sniffer] tail {log_path} → {bus}")
    if not log_path.is_file():
        print(f"[sniffer] waiting for {log_path} ...")
        while not log_path.is_file():
            time.sleep(0.5)

    seen: set[str] = set()
    with log_path.open() as f:
        f.seek(0, 2)  # end
        block: list[str] = []
        in_rar = False
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.05)
                continue
            stripped = line.rstrip("\n")
            if RAR_START.search(stripped) or "Attacking RAR" in stripped:
                if "Attacking RAR" in stripped:
                    g = parse_attacker_json_line(stripped)
                    if g:
                        key = f"{g.rapid}:{g.c_rnti}"
                        if key not in seen:
                            seen.add(key)
                            publish(g.to_bus_dict(), bus)
                else:
                    in_rar = True
                    block = [stripped]
                continue
            if in_rar:
                block.append(stripped)
                if "tc-rnti=" in stripped or len(block) > 12:
                    g = parse_rar_block(block)
                    in_rar = False
                    block = []
                    if g and g.c_rnti:
                        key = f"{g.rapid}:{g.c_rnti}"
                        if key not in seen:
                            seen.add(key)
                            publish(g.to_bus_dict(), bus)


def cmd_once(args: argparse.Namespace) -> int:
    from phase1.sniffer.log_parser import parse_log_file

    grants = parse_log_file(Path(args.log))
    bus = Path(args.bus)
    for g in grants:
        publish(g.to_bus_dict(), bus)
    print(f"[sniffer] parsed {len(grants)} grants")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="5Gone RAR sniffer → grant bus")
    sub = ap.add_subparsers(dest="cmd", required=True)

    rep = sub.add_parser("replay", help="Replay log into grant bus")
    rep.add_argument("--log", required=True)
    rep.add_argument("--bus", default=str(DEFAULT_BUS))
    rep.add_argument("--delay-ms", type=float, default=80.0)

    tail = sub.add_parser("tail", help="Follow live gNB log")
    tail.add_argument("--log", default=str(DEFAULT_GNB_LOG))
    tail.add_argument("--bus", default=str(DEFAULT_BUS))

    once = sub.add_parser("once", help="Parse log file once")
    once.add_argument("--log", required=True)
    once.add_argument("--bus", default=str(DEFAULT_BUS))

    args = ap.parse_args()
    if args.cmd == "replay":
        return cmd_replay(args)
    if args.cmd == "tail":
        return cmd_tail(args)
    return cmd_once(args)


if __name__ == "__main__":
    raise SystemExit(main())
