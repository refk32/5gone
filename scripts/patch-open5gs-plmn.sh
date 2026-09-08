#!/usr/bin/env bash
# patch-open5gs-plmn.sh — Set Open5GS to lab PLMN 001/01 TAC 1 (idempotent)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX_AMF="$ROOT/build/open5gs/etc/open5gs/amf.yaml"
PREFIX_NRF="$ROOT/build/open5gs/etc/open5gs/nrf.yaml"

if [ -n "${OPEN5GS_AMF_YAML:-}" ]; then
  AMF="$OPEN5GS_AMF_YAML"
elif [ -f "$PREFIX_AMF" ]; then
  AMF="$PREFIX_AMF"
elif [ -f /etc/open5gs/amf.yaml ]; then
  AMF="/etc/open5gs/amf.yaml"
else
  echo "AMF config not found (tried $PREFIX_AMF and /etc/open5gs/amf.yaml)"
  echo "Install Open5GS first: bash scripts/install-open5gs.sh"
  exit 1
fi

if [ -n "${OPEN5GS_NRF_YAML:-}" ]; then
  NRF="$OPEN5GS_NRF_YAML"
elif [ -f "$PREFIX_NRF" ]; then
  NRF="$PREFIX_NRF"
else
  NRF="/etc/open5gs/nrf.yaml"
fi

echo "==> Patching Open5GS PLMN 001/01 TAC 1"
echo "    AMF: $AMF"

if [ -w "$AMF" ]; then
  cp "$AMF" "${AMF}.bak.$(date +%Y%m%d%H%M%S)"
  sed -i 's/mcc: 999/mcc: 001/g; s/mnc: 70/mnc: 01/g; s/mnc: "70"/mnc: "01"/g' "$AMF"
  [ -f "$NRF" ] && [ -w "$NRF" ] && sed -i 's/mcc: 999/mcc: 001/g; s/mnc: 70/mnc: 01/g; s/mnc: "70"/mnc: "01"/g' "$NRF" || true
  sed -i 's/tac: 7/tac: 1/g' "$AMF"
else
  sudo cp "$AMF" "${AMF}.bak.$(date +%Y%m%d%H%M%S)"
  sudo sed -i 's/mcc: 999/mcc: 001/g; s/mnc: 70/mnc: 01/g; s/mnc: "70"/mnc: "01"/g' "$AMF"
  [ -f "$NRF" ] && sudo sed -i 's/mcc: 999/mcc: 001/g; s/mnc: 70/mnc: 01/g; s/mnc: "70"/mnc: "01"/g' "$NRF" 2>/dev/null || true
  sudo sed -i 's/tac: 7/tac: 1/g' "$AMF"
fi

echo "    patched: $AMF"
[ -f "$NRF" ] && echo "    patched: $NRF"
echo "    restart core with: bash scripts/start-5g-native.sh down && bash scripts/start-5g-native.sh up"
