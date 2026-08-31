#!/usr/bin/env bash
# verify-phase1.sh — Phase 1 sniffer verification
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

ok()  { echo "  ✓ $1"; }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "==> Phase 1 sniffer verification"
echo ""

for f in phase1/sniffer/rar_sniffer.py phase1/sniffer/log_parser.py; do
  [ -f "$ROOT/$f" ] && ok "$f" || bad "missing $f"
done

echo ""
echo "-- Parse enb-export RAR"
python3 -c "
from pathlib import Path
import sys
sys.path.insert(0, '$ROOT')
from phase1.sniffer.log_parser import parse_log_file
p = Path('$ROOT/data/5gone-dataset/cell-wide-dos/enb-export-cell-wide-dos.log')
grants = parse_log_file(p)
assert len(grants) >= 10, f'expected >=10 grants, got {len(grants)}'
g = grants[0]
assert g.c_rnti > 0, 'missing tc-rnti'
print(f'  parsed {len(grants)} RAR grants, sample rapid={g.rapid} crnti=0x{g.c_rnti:04x}')
" && ok "enb-export parser" || bad "enb-export parser"

echo ""
echo "-- Sniffer replay → bus"
BUS="/tmp/5gone_verify_grants.jsonl"
python3 "$ROOT/phase1/sniffer/rar_sniffer.py" replay \
  --log "$ROOT/data/5gone-dataset/cell-wide-dos/enb-export-cell-wide-dos.log" \
  --bus "$BUS" --delay-ms 0 >/dev/null
LINES=$(wc -l < "$BUS" | tr -d ' ')
[ "$LINES" -ge 10 ] && ok "replay $LINES grants" || bad "replay failed"

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "==> PASS — Phase 1 sniffer ready"
  exit 0
else
  echo "==> FAIL — $FAIL issue(s)"
  exit 1
fi
