#!/usr/bin/env bash
# verify-phase2.sh — Phase 2 readiness checks (no RF TX)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

ok()  { echo "  ✓ $1"; }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "==> Phase 2 verification"
echo "    $ROOT"
echo ""

echo "-- Phase 2 files"
FILES=(
  phase2/CMakeLists.txt
  phase2/config/rar_dos.yaml
  phase2/config/sample_grant.json
  phase2/tools/generate_msg3_template.py
  phase2/python/orchestrator.py
  scripts/install-phase2.sh
  scripts/run-attack-rar-dos.sh
)
for f in "${FILES[@]}"; do
  [ -f "$ROOT/$f" ] && ok "$f" || bad "missing: $f"
done

echo ""
echo "-- IQ templates"
if ls "$ROOT/phase2/iq_templates/"*.cf32 >/dev/null 2>&1; then
  ok "iq_templates present ($(ls "$ROOT/phase2/iq_templates/"*.cf32 | wc -l | tr -d ' ') files)"
else
  echo "  → generating templates..."
  python3 "$ROOT/phase2/tools/generate_msg3_template.py" --output "$ROOT/phase2/iq_templates"
  ok "iq_templates generated"
fi

echo ""
echo "-- Python sim demo"
if python3 "$ROOT/phase2/python/sim_replay.py" 2>/dev/null; then
  :
fi
python3 -c "
import sys
sys.path.insert(0, '$ROOT')
from pathlib import Path
from phase2.python.sim_replay import run_sim
rc = run_sim(Path('$ROOT/data/5gone-dataset'), limit=3)
sys.exit(rc)
" && ok "sim_replay (3 events)" || bad "sim_replay failed — run setup-lab.sh --phase 0"

echo ""
echo "-- C++ binary (Linux only)"
if [ -x "$ROOT/build/phase2/5gone-rar-dos" ]; then
  ok "5gone-rar-dos built"
  "$ROOT/build/phase2/5gone-rar-dos" "$ROOT/phase2/config/rar_dos.yaml" --mode sim --dry-run \
    >/dev/null 2>&1 && ok "binary sim run" || bad "binary sim failed"
else
  echo "  ○ 5gone-rar-dos not built (expected on Mac — build on Linux)"
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "==> PASS — Phase 2 ready ($FAIL failures)"
  exit 0
else
  echo "==> FAIL — $FAIL issue(s)"
  exit 1
fi
