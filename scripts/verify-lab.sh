#!/usr/bin/env bash
# verify-lab.sh — Post-install verification (run AFTER install on office Linux)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GNB_BIN="${SRSRAN_GNB:-$ROOT/build/srsran_build/apps/gnb/gnb}"
FAIL=0

ok()  { echo "  ✓ $1"; }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "==> 5Gone lab verification"
echo ""

echo "-- Binaries"
command -v uhd_find_devices >/dev/null && ok "UHD CLI" || bad "UHD missing"
[ -x "$GNB_BIN" ] && ok "srsRAN gNB: $GNB_BIN" || bad "gNB not built — run install-srsran.sh"
command -v open5gs-amfd >/dev/null && ok "Open5GS AMF" || \
  [ -x "$ROOT/build/open5gs/bin/open5gs-amfd" ] && ok "Open5GS AMF (local prefix)" || \
  bad "Open5GS AMF missing"

echo ""
echo "-- B210 USB"
if uhd_find_devices 2>/dev/null | grep -qi "b210\|b200"; then
  ok "USRP detected"
else
  bad "USRP not found"
fi

echo ""
echo "-- Benchmark (quick 5s)"
if command -v uhd_benchmark_rate >/dev/null; then
  ARGS=""
  [ -f "$ROOT/config/b210.uhd.args" ] && ARGS="$(tr -d '\n' < "$ROOT/config/b210.uhd.args")"
  OUT=$(mktemp)
  if uhd_benchmark_rate --args "$ARGS" --rx_rate 23.04e6 --duration 5 >"$OUT" 2>&1; then
    if grep -qi "overrun" "$OUT" && ! grep -q "Num overruns: 0" "$OUT"; then
      bad "UHD overruns detected — run tune-usb-b210.sh"
    else
      ok "UHD benchmark 23.04 MSPS (5s)"
    fi
  else
    bad "UHD benchmark failed"
  fi
  rm -f "$OUT"
fi

echo ""
echo "-- Services"
pgrep -x mongod >/dev/null && ok "MongoDB running" || echo "  ⚠ MongoDB not running (needed for Open5GS)"
pgrep -f open5gs-amfd >/dev/null && ok "Open5GS AMF running" || echo "  ⚠ AMF not running — start-5g-native.sh core"

echo ""
echo "-- Config alignment"
grep -q '127.0.0.5' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gNB → AMF 127.0.0.5" || bad "gNB AMF addr"
grep -q 'tac: 1' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gNB TAC 1" || bad "gNB TAC"

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "==> VERIFY PASS"
  exit 0
else
  echo "==> VERIFY FAIL — $FAIL issue(s)"
  exit 1
fi
