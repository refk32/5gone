#!/usr/bin/env bash
# patch-open5gs-plmn.sh — Set Open5GS to lab PLMN 001/01 TAC 1 (idempotent)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AMF="${OPEN5GS_AMF_YAML:-/etc/open5gs/amf.yaml}"
NRF="${OPEN5GS_NRF_YAML:-/etc/open5gs/nrf.yaml}"

if [ ! -f "$AMF" ]; then
  echo "AMF config not found: $AMF"
  echo "Install Open5GS first: bash scripts/install-open5gs.sh"
  exit 1
fi

echo "==> Patching Open5GS PLMN 001/01 TAC 1"
sudo cp "$AMF" "${AMF}.bak.$(date +%Y%m%d%H%M%S)"

# NGAP listen on Open5GS default loopback
sudo sed -i 's/mcc: 999/mcc: 001/g; s/mnc: 70/mnc: 01/g; s/mnc: "70"/mnc: "01"/g' "$AMF" "$NRF" 2>/dev/null || true

# Ensure tac 1 in amf (if still 7 from defaults)
sudo sed -i 's/tac: 7/tac: 1/g' "$AMF" 2>/dev/null || true

echo "    patched: $AMF"
[ -f "$NRF" ] && echo "    patched: $NRF"
echo ""
echo "Verify manually:"
echo "  grep -A2 'plmn_id' $AMF | head -20"
echo "  sudo systemctl restart open5gs-amfd open5gs-nrfd"
