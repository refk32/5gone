#!/usr/bin/env python3
"""
grant_bus.py — Write RAR grants to bus file for inject/live handoff.

Sniffer or manual decode writes JSON lines; attacker watches file.

Usage:
  python3 phase2/python/grant_bus.py publish --rapid 5 --crnti 17922 --freq 2
  python3 phase2/python/grant_bus.py tail
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

DEFAULT_BUS = Path("/tmp/5gone_grants.jsonl")


def cmd_publish(args: argparse.Namespace) -> int:
    ev = {
        "rapid": args.rapid,
        "ta": 0,
        "c_rnti": args.crnti,
        "ul_dci": {
            "pusch_freq_res": args.freq,
            "pusch_time_res": 1,
            "mcs": args.mcs,
        },
        "grant": {"k": args.k, "mcs": args.mcs, "tbs_bits": 264, "pusch_freq_res": args.freq},
    }
    line = json.dumps(ev) + "\n"
    with DEFAULT_BUS.open("a") as f:
        f.write(line)
    print(f"published → {DEFAULT_BUS}")
    return 0


def cmd_tail(args: argparse.Namespace) -> int:
    path = Path(args.file)
    print(f"watching {path}")
    last_size = 0
    while True:
        if path.is_file():
            data = path.read_text()
            if len(data) > last_size:
                for line in data[last_size:].splitlines():
                    if line.strip():
                        print(f"  grant: {line[:100]}")
                last_size = len(data)
        time.sleep(0.2)


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    pub = sub.add_parser("publish")
    pub.add_argument("--rapid", type=int, default=0)
    pub.add_argument("--crnti", type=int, required=True)
    pub.add_argument("--freq", type=int, default=2)
    pub.add_argument("--k", type=int, default=6)
    pub.add_argument("--mcs", type=int, default=4)

    tail = sub.add_parser("tail")
    tail.add_argument("--file", default=str(DEFAULT_BUS))

    args = ap.parse_args()
    if args.cmd == "publish":
        return cmd_publish(args)
    return cmd_tail(args)


if __name__ == "__main__":
    raise SystemExit(main())
