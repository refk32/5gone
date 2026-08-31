#!/usr/bin/env bash
# run-full-demo.sh — End-to-end pipeline demo (sniffer → bus → attacker)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUS="/tmp/5gone_grants.jsonl"
LOG="/tmp/5gone_attacker.log"
DATASET="${1:-$ROOT/data/5gone-dataset/cell-wide-dos/enb-export-cell-wide-dos.log}"
LIMIT="${DEMO_LIMIT:-5}"

echo "============================================"
echo " 5Gone Full Pipeline Demo"
echo " Sniffer → Grant Bus → Attacker"
echo "============================================"
echo ""

: > "$BUS"
: > "$LOG"

echo "==> [1/3] Sniffer replay (first $LIMIT RAR from enb log)"
python3 phase1/sniffer/rar_sniffer.py replay \
  --log "$DATASET" \
  --bus "$BUS" \
  --delay-ms 0

GRANTS=$(wc -l < "$BUS" | tr -d ' ')
echo "    published $GRANTS grants to $BUS"
head -n "$LIMIT" "$BUS" > "${BUS}.demo"
mv "${BUS}.demo" "$BUS"

echo ""
echo "==> [2/3] Parse sniffer output"
python3 -c "
import json
from pathlib import Path
for i, line in enumerate(Path('$BUS').read_text().splitlines(), 1):
    g = json.loads(line)
    print(f'  [{i}] rapid={g[\"rapid\"]} TC-RNTI=0x{g[\"c_rnti\"]:04x} freq={g[\"grant\"][\"pusch_freq_res\"]}')
"

echo ""
echo "==> [3/3] Bus attacker (dry-run)"
python3 phase2/python/bus_attacker.py --bus "$BUS" --dry-run --batch

echo ""
echo "==> Attacker log ($LOG):"
tail -5 "$LOG" 2>/dev/null || echo "  (empty)"
echo ""
echo "============================================"
echo " PASS — full pipeline demo complete"
echo ""
echo " Live lab:"
echo "   bash scripts/start-5g-native.sh up"
echo "   bash scripts/start-sniffer.sh tail &"
echo "   bash scripts/run-attack-rar-dos.sh bus"
echo "============================================"
