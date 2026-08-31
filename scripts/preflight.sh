#!/usr/bin/env bash
# preflight.sh — Pre-install checks on office Linux (run BEFORE install-*)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

warn() { echo "  ⚠ $1"; }
ok()   { echo "  ✓ $1"; }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "==> 5Gone lab preflight"
echo ""

echo "-- OS"
if [ "$(uname -s)" = "Linux" ]; then ok "Linux"; else bad "Need Linux (Ubuntu 22.04)"; fi
if [ -f /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  echo "    $PRETTY_NAME"
  [[ "$VERSION_ID" == "22.04" ]] && ok "Ubuntu 22.04" || warn "Ubuntu 22.04 recommended (found $VERSION_ID)"
fi

echo ""
echo "-- CPU"
if grep -q avx2 /proc/cpuinfo 2>/dev/null; then ok "AVX2 supported"; else bad "No AVX2 — srsRAN may fail"; fi
CORES=$(nproc 2>/dev/null || echo 0)
[ "$CORES" -ge 4 ] && ok "$CORES CPU cores" || warn "Only $CORES cores — 8+ recommended"

echo ""
echo "-- RAM"
MEM_GB=$(free -g 2>/dev/null | awk '/Mem:/{print $2}' || echo 0)
[ "$MEM_GB" -ge 8 ] && ok "${MEM_GB} GB RAM" || warn "${MEM_GB} GB RAM — 16 GB recommended"

echo ""
echo "-- Disk"
AVAIL=$(df -BG "$ROOT" 2>/dev/null | awk 'NR==2{print $4}' | tr -d G)
[ "${AVAIL:-0}" -ge 20 ] 2>/dev/null && ok "${AVAIL}G free in $ROOT" || warn "Need 20G+ free for builds"

echo ""
echo "-- Network (office)"
command -v git >/dev/null && ok "git" || bad "git missing"
command -v curl >/dev/null && ok "curl" || bad "curl missing"
ping -c1 -W2 github.com >/dev/null 2>&1 && ok "github.com reachable" || warn "github.com unreachable — clone will fail offline"

echo ""
echo "-- USB / B210 (optional pre-install)"
if command -v uhd_find_devices >/dev/null 2>&1; then
  ok "UHD installed"
  if uhd_find_devices 2>/dev/null | grep -qi b210; then
    ok "B210 detected"
  else
    warn "B210 not detected — plug in before benchmark"
  fi
else
  warn "UHD not installed yet — run install-deps.sh"
fi

echo ""
echo "-- Repo"
bash "$ROOT/scripts/validate-repo.sh" || FAIL=$((FAIL + 1))

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "==> PREFLIGHT PASS — safe to run install scripts"
  exit 0
else
  echo "==> PREFLIGHT FAIL — fix $FAIL blocker(s) before install"
  exit 1
fi
