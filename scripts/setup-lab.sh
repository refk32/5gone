#!/usr/bin/env bash
# setup-lab.sh — Bootstrap 5Gone PoC phases
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PHASE="all"

usage() {
  echo "Usage: bash scripts/setup-lab.sh [--phase 0|1|2|all]"
  echo "  --phase 0   Clone 5gone dataset + analyze (no RF)"
  echo "  --phase 1   Phase 0 + verify UHD/B210"
  echo "  --phase 2   Phase 0 + verify sniffer/attacker + full demo"
  echo "  --phase all Install deps + phase 0 + benchmark prompt"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --phase) PHASE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown: $1"; usage; exit 2 ;;
  esac
done

phase0() {
  echo "==> Phase 0: 5gone dataset"
  DATASET_DIR="$ROOT/data/5gone-dataset"
  if [ ! -d "$DATASET_DIR/.git" ]; then
    git clone --depth 1 https://github.com/5gone/dataset.git "$DATASET_DIR"
  else
    echo "    dataset already cloned"
  fi

  if [ -d "$ROOT/.venv" ]; then
    # shellcheck disable=SC1091
    source "$ROOT/.venv/bin/activate"
  fi
  python3 "$ROOT/phase0/analyze_traces.py" \
    --dataset "$DATASET_DIR" \
    --out "$ROOT/logs/phase0-report.txt"
  python3 "$ROOT/phase0/parse_attacker_logs.py" \
    --dataset "$DATASET_DIR" \
    --out "$ROOT/logs/phase0-attacker-timing.txt" \
    --json "$ROOT/logs/phase0-attacker-timing.json"
  [ -f "$ROOT/.venv/bin/activate" ] && deactivate || true
  echo "    reports: $ROOT/logs/phase0-report.txt"
  echo "             $ROOT/logs/phase0-attacker-timing.txt"
}

phase1() {
  echo "==> Phase 1: B210 check"
  bash "$ROOT/scripts/benchmark-b210.sh"
}

case "$PHASE" in
  0) phase0 ;;
  1) phase0; phase1 ;;
  2) phase0; bash "$ROOT/scripts/verify-phase1.sh"; bash "$ROOT/scripts/verify-phase2.sh"; bash "$ROOT/scripts/run-full-demo.sh" ;;
  all)
    bash "$ROOT/scripts/install-deps.sh"
    phase0
    echo ""
    echo "Linux + B210:"
    echo "  bash scripts/tune-usb-b210.sh"
    echo "  bash scripts/benchmark-b210.sh"
    echo "  bash scripts/install-open5gs.sh && bash scripts/install-srsran.sh"
    echo "  bash scripts/start-5g-native.sh up"
    ;;
  *) echo "Invalid phase: $PHASE"; usage; exit 2 ;;
esac

echo "==> Setup phase '$PHASE' complete"
