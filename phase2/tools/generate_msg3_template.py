#!/usr/bin/env python3
"""
generate_msg3_template.py — Precompute Msg3 overshadow IQ for fixed lab grant.

Usage:
  python3 phase2/tools/generate_msg3_template.py --output phase2/iq_templates
"""
from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

DEFAULT_GRANTS = [
    {"mcs": 4, "pusch_freq_res": 2, "k": 6, "tbs_bits": 264},
    {"mcs": 4, "pusch_freq_res": 0, "k": 6, "tbs_bits": 264},
    {"mcs": 4, "pusch_freq_res": 270, "k": 6, "tbs_bits": 264},
    {"mcs": 4, "pusch_freq_res": 272, "k": 6, "tbs_bits": 264},
]


def build_empty_mac_pdu(tb_bytes: int) -> bytes:
    if tb_bytes <= 0:
        return b""
    return bytes([0x3F]) + b"\x00" * (tb_bytes - 1)


def bits_from_tb(tb: bytes) -> list[int]:
    bits: list[int] = []
    for byte in tb:
        for b in range(7, -1, -1):
            bits.append((byte >> b) & 1)
    return bits


def bits_to_qpsk_cf32(tb: bytes, reps: int = 64, scale: float = 1.0) -> bytes:
    bits = bits_from_tb(tb)
    if len(bits) % 2:
        bits.append(0)
    norm = scale / math.sqrt(2)
    syms: list[tuple[float, float]] = []
    for i in range(0, len(bits), 2):
        re = (2 * bits[i] - 1) * norm
        im = (2 * bits[i + 1] - 1) * norm
        syms.append((re, im))
    target_len = max(len(syms) * reps, 4096)
    out = bytearray()
    idx = 0
    while len(out) // 8 < target_len:
        re, im = syms[idx % len(syms)]
        out.extend(struct.pack("<ff", re, im))
        idx += 1
    return bytes(out[: target_len * 8])


def template_name(grant: dict) -> str:
    return f"mcs{grant['mcs']}_freq{grant['pusch_freq_res']}_k{grant['k']}.cf32"


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate Msg3 overshadow IQ templates")
    ap.add_argument("--output", type=Path, default=Path("phase2/iq_templates"))
    ap.add_argument("--scale", type=float, default=1.5)
    args = ap.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    for g in DEFAULT_GRANTS:
        tb = build_empty_mac_pdu(g["tbs_bits"] // 8)
        raw = bits_to_qpsk_cf32(tb, scale=args.scale)
        out = args.output / template_name(g)
        out.write_bytes(raw)
        print(f"  wrote {out} ({len(raw) // 8} samples, {len(raw)} bytes)")
    print(f"\nDone — {len(DEFAULT_GRANTS)} templates in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
