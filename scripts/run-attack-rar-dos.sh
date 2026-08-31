#!/usr/bin/env bash
# run-attack-rar-dos.sh — Run Phase 2 RAR DoS attack demo
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-demo}"
ATTACKER="${PHASE2_BIN:-$ROOT/build/phase2/5gone-rar-dos}"
CONFIG="$ROOT/phase2/config/rar_dos.yaml"

cd "$ROOT"

case "$MODE" in
  demo|sim)
    echo "==> Phase 2 sim demo (offline RAR DoS replay)"
    if [ -x "$ATTACKER" ]; then
      "$ATTACKER" "$CONFIG" --mode sim
    else
      python3 phase2/python/orchestrator.py demo
    fi
    ;;
  inject)
    GRANT="${2:-$ROOT/phase2/config/sample_grant.json}"
    echo "==> Phase 2 inject — grant from $GRANT"
    echo "    WARNING: RF TX inside shield tent only"
    if [ ! -x "$ATTACKER" ]; then
      echo "Build first: bash scripts/install-phase2.sh"
      exit 1
    fi
    "$ATTACKER" "$CONFIG" --mode inject --grant "$GRANT"
    ;;
  live)
    echo "==> Phase 2 live — DL monitor + UL overshadow"
    echo "    Requires: gNB running + 2nd USRP B210 for attacker"
    echo "    Set device: export ATTACKER_DEVICE='serial=XXXXX'"
    EXTRA=()
    if [ -n "${ATTACKER_DEVICE:-}" ]; then
      EXTRA+=(--device "$ATTACKER_DEVICE")
    fi
    if [ ! -x "$ATTACKER" ]; then
      echo "Build first: bash scripts/install-phase2.sh"
      exit 1
    fi
    "$ATTACKER" "$CONFIG" --mode live "${EXTRA[@]}"
    ;;
  bus)
    echo "==> Phase 2 bus — sniffer → grant bus → attacker"
    echo "    Start sniffer: bash scripts/start-sniffer.sh tail"
    if [ -x "$ATTACKER" ]; then
      EXTRA=()
      [ -n "${ATTACKER_DEVICE:-}" ] && EXTRA+=(--device "$ATTACKER_DEVICE")
      "$ATTACKER" "$CONFIG" --mode bus "${EXTRA[@]}"
    else
      python3 phase2/python/bus_attacker.py --bus /tmp/5gone_grants.jsonl --dry-run "${@:2}"
    fi
    ;;
  templates)
    python3 phase2/tools/generate_msg3_template.py --output phase2/iq_templates
    ;;
  *)
    echo "Usage: $0 {demo|sim|inject|live|bus|templates} [grant.json]"
    exit 1
    ;;
esac
