#!/usr/bin/env bash
# start-sniffer.sh — RAR sniffer → grant bus
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-tail}"
LOG="${SNIFFER_LOG:-/tmp/gnb_5gone.log}"
BUS="${GRANT_BUS:-/tmp/5gone_grants.jsonl}"
DATASET="${SNIFFER_DATASET:-$ROOT/data/5gone-dataset/cell-wide-dos/enb-export-cell-wide-dos.log}"

cd "$ROOT"

case "$MODE" in
  tail)
    echo "==> Sniffer tail: $LOG → $BUS"
    python3 phase1/sniffer/rar_sniffer.py tail --log "$LOG" --bus "$BUS"
    ;;
  replay)
    SRC="${2:-$DATASET}"
    echo "==> Sniffer replay: $SRC → $BUS"
    python3 phase1/sniffer/rar_sniffer.py replay --log "$SRC" --bus "$BUS" --delay-ms "${SNIFFER_DELAY_MS:-80}"
    ;;
  once)
    SRC="${2:-$DATASET}"
    python3 phase1/sniffer/rar_sniffer.py once --log "$SRC" --bus "$BUS"
    ;;
  *)
    echo "Usage: $0 {tail|replay|once} [logfile]"
    exit 1
    ;;
esac
