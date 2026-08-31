#!/usr/bin/env bash
# validate-repo.sh — Static checks (no install, no hardware). Run anytime on Mac/Linux.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

ok()  { echo "  ✓ $1"; }
bad() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "==> 5Gone repo validation (static)"
echo "    $ROOT"
echo ""

echo "-- Required files"
REQUIRED=(
  config/srsran/gnb_20mhz.yml
  config/b210.uhd.args
  config/open5gs/amf.yaml.patch
  scripts/install-deps.sh
  scripts/install-open5gs.sh
  scripts/install-srsran.sh
  scripts/start-5g-native.sh
  scripts/preflight.sh
  scripts/verify-lab.sh
  scripts/tune-usb-b210.sh
  scripts/benchmark-b210.sh
  docs/office-install-runbook.md
  docs/readiness.md
  phase0/analyze_traces.py
  phase0/parse_attacker_logs.py
  phase2/CMakeLists.txt
  phase2/config/rar_dos.yaml
  phase1/sniffer/rar_sniffer.py
  phase1/sniffer/log_parser.py
  scripts/install-phase2.sh
  scripts/run-attack-rar-dos.sh
  scripts/verify-phase2.sh
  scripts/start-sniffer.sh
  scripts/run-full-demo.sh
  scripts/start-attack-lab.sh
  scripts/verify-phase1.sh
  docs/phase2-attack.md
)
for f in "${REQUIRED[@]}"; do
  [ -f "$ROOT/$f" ] && ok "$f" || bad "missing: $f"
done

echo ""
echo "-- Bash syntax"
for s in "$ROOT"/scripts/*.sh; do
  bash -n "$s" && ok "$(basename "$s")" || bad "syntax: $(basename "$s")"
done

echo ""
echo "-- Python syntax"
for p in "$ROOT"/phase0/*.py "$ROOT"/phase2/tools/*.py "$ROOT"/phase2/python/*.py "$ROOT"/phase1/sniffer/*.py; do
  [ -f "$p" ] || continue
  python3 -c "import ast; ast.parse(open('$p').read())" && ok "$(basename "$p")" || bad "python: $(basename "$p")"
done

echo ""
echo "-- Config sanity"
grep -q 'addr: 127.0.0.5' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb AMF addr = 127.0.0.5 (Open5GS default)" || bad "gnb AMF addr wrong"
grep -q 'plmn: "00101"' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb PLMN 00101" || bad "gnb PLMN mismatch"
grep -q 'channel_bandwidth_MHz: 20' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb 20 MHz" || bad "gnb bandwidth"
grep -q 'filename: /tmp/gnb_5gone.log' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb log = /tmp/gnb_5gone.log" || bad "gnb log path"
grep -q 'num_recv_frames' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb UHD frame buffers" || bad "gnb missing num_recv_frames"
grep -q 'master_clock_rate=23.04e6' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "gnb srate 23.04" || bad "gnb srate"
grep -q '23.04e6' "$ROOT/config/b210.uhd.args" && ok "b210 args 23.04 MSPS" || bad "b210 args srate"
! grep -q 'addr: open5gs' "$ROOT/config/srsran/gnb_20mhz.yml" && ok "no docker hostname in gnb" || bad "gnb still has 'open5gs' hostname"

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "==> PASS — repo structure OK ($FAIL failures)"
  exit 0
else
  echo "==> FAIL — $FAIL issue(s) — fix before office install"
  exit 1
fi
