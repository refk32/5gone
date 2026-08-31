#!/usr/bin/env bash
# install-all.sh — Ordered office install (run ONCE on Ubuntu 22.04 Linux)
# Does NOT clone dataset — use setup-lab.sh --phase 0 separately if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "============================================"
echo " 5Gone lab — ordered native install"
echo " Ubuntu 22.04 + USRP B210 direct USB"
echo "============================================"
echo ""

bash scripts/validate-repo.sh
bash scripts/preflight.sh

echo ""
echo "==> Step 1/6: base dependencies"
bash scripts/install-deps.sh

echo ""
echo "==> Step 2/6: USB tuning (B210 must be plugged in)"
bash scripts/tune-usb-b210.sh

echo ""
echo "==> Step 3/6: B210 benchmark"
bash scripts/benchmark-b210.sh

echo ""
echo "==> Step 4/6: Open5GS"
bash scripts/install-open5gs.sh

echo ""
echo "==> Step 5/6: srsRAN gNB"
bash scripts/install-srsran.sh

echo ""
echo "==> Step 6/7: verify"
bash scripts/verify-lab.sh

echo ""
echo "==> Step 7/8: Phase 2 attacker"
bash scripts/install-phase2.sh

echo ""
echo "==> Step 8/8: Phase 1 sniffer verify"
bash scripts/verify-phase1.sh

echo ""
echo "============================================"
echo " Install complete."
echo " Start lab:  bash scripts/start-5g-native.sh up"
echo " Status:     bash scripts/start-5g-native.sh status"
echo " Subscriber: bash scripts/add-test-subscriber.sh"
echo " Attack demo: bash scripts/run-attack-rar-dos.sh demo"
echo " Phase 2 verify: bash scripts/verify-phase2.sh"
echo "============================================"
