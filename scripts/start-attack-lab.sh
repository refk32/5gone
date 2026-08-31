#!/usr/bin/env bash
# start-attack-lab.sh — Start full attack lab (gNB + sniffer + attacker)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/run"
mkdir -p "$RUN"

MODE="${1:-dry}"
ATTACKER_MODE="${ATTACKER_MODE:-bus}"

if [ "$(uname -s)" != "Linux" ]; then
  echo "RF lab requires Linux. Running offline demo instead:"
  bash "$ROOT/scripts/run-full-demo.sh"
  exit 0
fi

start_core() {
  bash "$ROOT/scripts/start-5g-native.sh" core
  sleep 3
  bash "$ROOT/scripts/patch-open5gs-plmn.sh" 2>/dev/null || true
  bash "$ROOT/scripts/add-test-subscriber.sh" 2>/dev/null || true
}

case "$MODE" in
  dry|demo)
    bash "$ROOT/scripts/run-full-demo.sh"
    ;;
  up)
    echo "==> Starting 5G core + gNB + sniffer + attacker"
    start_core
    bash "$ROOT/scripts/start-5g-native.sh" gnb
    sleep 5
    : > /tmp/5gone_grants.jsonl
    bash "$ROOT/scripts/start-sniffer.sh" tail &
    echo $! > "$RUN/sniffer.pid"
    sleep 1
    bash "$ROOT/scripts/run-attack-rar-dos.sh" "$ATTACKER_MODE" &
    echo $! > "$RUN/attacker.pid"
    echo "    sniffer PID: $(cat "$RUN/sniffer.pid")"
    echo "    attacker PID: $(cat "$RUN/attacker.pid")"
    echo "    grant bus: /tmp/5gone_grants.jsonl"
    echo "    logs: /tmp/gnb_5gone.log (MAC), /tmp/5gone_attacker.log, $RUN/gnb.log (startup)"
    ;;
  down)
    [ -f "$RUN/attacker.pid" ] && kill "$(cat "$RUN/attacker.pid")" 2>/dev/null || true
    [ -f "$RUN/sniffer.pid" ] && kill "$(cat "$RUN/sniffer.pid")" 2>/dev/null || true
    rm -f "$RUN/attacker.pid" "$RUN/sniffer.pid"
    bash "$ROOT/scripts/start-5g-native.sh" down
    echo "    attack lab stopped"
    ;;
  status)
    echo "Sniffer:"
    pgrep -af "rar_sniffer" || echo "  down"
    echo "Attacker:"
    pgrep -af "5gone-rar-dos|bus_attacker" || echo "  down"
    bash "$ROOT/scripts/start-5g-native.sh" status
    ;;
  *)
    echo "Usage: $0 {demo|up|down|status}"
    echo "  demo  — offline pipeline (Mac/Linux)"
    echo "  up    — live gNB + sniffer + attacker (Linux + 2x B210 for RF TX)"
    exit 1
    ;;
esac
