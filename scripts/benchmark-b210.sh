#!/usr/bin/env bash
# benchmark-b210.sh — Verify B210 can sustain 23.04 MSPS without overruns
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RATE="${RATE:-23.04e6}"
DURATION="${DURATION:-30}"
ARGS_FILE="$ROOT/config/b210.uhd.args"

echo "==> B210 benchmark @ ${RATE} MSPS for ${DURATION}s"
echo ""

if ! command -v uhd_find_devices >/dev/null 2>&1; then
  echo "UHD not installed. Run: bash scripts/install-deps.sh"
  exit 1
fi

echo "==> Detecting devices..."
uhd_find_devices || { echo "No USRP found. Check USB 3.0 connection."; exit 1; }

DEVICE_ARGS=""
if [ -f "$ARGS_FILE" ]; then
  DEVICE_ARGS="$(tr -d '\n' < "$ARGS_FILE")"
  echo "    device args: $DEVICE_ARGS"
fi

echo ""
echo "==> RX benchmark..."
if [ -n "$DEVICE_ARGS" ]; then
  uhd_benchmark_rate --args "$DEVICE_ARGS" --rx_rate "$RATE" --duration "$DURATION" || true
else
  uhd_benchmark_rate --rx_rate "$RATE" --duration "$DURATION" || true
fi

echo ""
echo "==> Full-duplex RX+TX benchmark..."
if [ -n "$DEVICE_ARGS" ]; then
  uhd_benchmark_rate --args "$DEVICE_ARGS" \
    --rx_rate "$RATE" --tx_rate "$RATE" \
    --duration "$DURATION" || true
else
  uhd_benchmark_rate --rx_rate "$RATE" --tx_rate "$RATE" \
    --duration "$DURATION" || true
fi

echo ""
echo "Pass criteria: Num overruns = 0, Num dropped samples = 0"
echo "If overruns: bash scripts/tune-usb-b210.sh, dedicated USB3, try RATE=15.36e6"
