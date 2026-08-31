#!/usr/bin/env bash
# add-test-subscriber.sh — Add test SIM to Open5GS (native MongoDB)
set -euo pipefail

IMSI="${IMSI:-001010000000001}"
K="${K:-465B5CE8B199B49FAA5F0A2EE238A6BC}"
OPC="${OPC:-E8ED289DEBA952E4283B54E88E6183CA}"

while [ $# -gt 0 ]; do
  case "$1" in
    --imsi) IMSI="$2"; shift 2 ;;
    --k) K="$2"; shift 2 ;;
    --opc) OPC="$2"; shift 2 ;;
    *) shift ;;
  esac
done

echo "==> Open5GS test subscriber (native MongoDB)"
echo "    IMSI: $IMSI"
echo ""
echo "Run after MongoDB + Open5GS core are up:"
echo ""
cat <<EOF
mongosh open5gs --eval '
db.subscribers.insertOne({
  imsi: "$IMSI",
  security: { k: "$K", opc: "$OPC", amf: "8000" },
  slice: [{
    sst: 1, sd: "ffffff",
    default_indicator: true,
    session: [{
      name: "internet", type: 3,
      qos: { index: 9, arp: { priority_level: 8, pre_emption_capability: 1, pre_emption_vulnerability: 1 }},
      ambr: { uplink: { value: 1, unit: 3 }, downlink: { value: 1, unit: 3 }}
    }]
  }]
})'
EOF
