#!/usr/bin/env python3
"""Phase 0: inventory and summarize 5gone public dataset."""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path


def scan_dataset(root: Path) -> dict:
    stats = {
        "root": str(root),
        "exists": root.is_dir(),
        "files_total": 0,
        "by_extension": Counter(),
        "by_top_dir": Counter(),
        "attack_keywords": Counter(),
        "sample_files": [],
    }
    if not root.is_dir():
        return stats

    keywords = re.compile(
        r"RAR|Msg3|Registration|Identity|SUCI|overshadow|attach|NAS|RRC|PHY|MAC",
        re.I,
    )

    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            stats["files_total"] += 1
            p = Path(dirpath) / name
            rel = p.relative_to(root)
            stats["by_extension"][p.suffix.lower() or "(none)"] += 1
            top = rel.parts[0] if rel.parts else "."
            stats["by_top_dir"][top] += 1

            if len(stats["sample_files"]) < 30:
                stats["sample_files"].append(str(rel))

            try:
                if p.suffix.lower() in {".txt", ".log", ".json", ".md", ".xml", ".yaml", ".yml"}:
                    text = p.read_text(errors="replace")[:50000]
                    for m in keywords.findall(text):
                        stats["attack_keywords"][m.upper()] += 1
            except OSError:
                pass

    return stats


def format_report(stats: dict) -> str:
    lines = [
        "5Gone Dataset Analysis (Phase 0)",
        "=" * 40,
        f"Root:   {stats['root']}",
        f"Exists: {stats['exists']}",
        f"Files:  {stats['files_total']}",
        "",
        "By extension:",
    ]
    for ext, n in stats["by_extension"].most_common(15):
        lines.append(f"  {ext:12} {n}")

    lines += ["", "By top-level directory:"]
    for d, n in stats["by_top_dir"].most_common(20):
        lines.append(f"  {d:20} {n}")

    if stats["attack_keywords"]:
        lines += ["", "Keyword hits in text files:"]
        for k, n in stats["attack_keywords"].most_common(20):
            lines.append(f"  {k:20} {n}")

    lines += ["", "Sample files:"]
    for f in stats["sample_files"][:20]:
        lines.append(f"  {f}")

    lines += [
        "",
        "Next steps:",
        "  1. Open sample trace files — map RAR → Msg3 timing",
        "  2. Build detector rules (Section 8 countermeasures)",
        "  3. Phase 1: passive decode on 20 MHz lab cell",
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze 5gone dataset")
    ap.add_argument("--dataset", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--json", type=Path, default=None)
    args = ap.parse_args()

    stats = scan_dataset(args.dataset.resolve())
    report = format_report(stats)
    print(report)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        serializable = {
            **stats,
            "by_extension": dict(stats["by_extension"]),
            "by_top_dir": dict(stats["by_top_dir"]),
            "attack_keywords": dict(stats["attack_keywords"]),
        }
        args.json.write_text(json.dumps(serializable, indent=2))

    return 0 if stats["exists"] else 1


if __name__ == "__main__":
    sys.exit(main())
