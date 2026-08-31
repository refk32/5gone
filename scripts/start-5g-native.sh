#!/usr/bin/env bash
# start-5g-native.sh — Start Open5GS + srsRAN natively (systemd / direct)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/run"
GNB_BIN="${SRSRAN_GNB:-$ROOT/build/srsran_build/apps/gnb/gnb}"
GNB_CFG="$ROOT/config/srsran/gnb_20mhz.yml"
OPEN5GS_PREFIX="${OPEN5GS_PREFIX:-$ROOT/build/open5gs}"

mkdir -p "$RUN"
export PATH="$OPEN5GS_PREFIX/bin:$PATH"

if [ "$(uname -s)" != "Linux" ]; then
  echo "Requires Linux + B210 direct USB."
  exit 1
fi

start_mongodb() {
  if pgrep -x mongod >/dev/null 2>&1; then
    echo "    mongod running"
    return
  fi
  if systemctl is-active mongod >/dev/null 2>&1; then
    echo "    mongod running (systemd)"
  elif command -v mongod >/dev/null 2>&1; then
    sudo systemctl start mongod
  else
    echo "MongoDB not installed"
    exit 1
  fi
}

start_open5gs() {
  if systemctl list-unit-files open5gs-amfd.service >/dev/null 2>&1; then
    echo "    starting Open5GS via systemd..."
    sudo systemctl start mongod
    sudo systemctl start open5gs-nrfd open5gs-scpd open5gs-amfd open5gs-smfd open5gs-upfd
    sudo systemctl start open5gs-ausfd open5gs-udmd open5gs-pcfd open5gs-nssfd open5gs-bsfd open5gs-udrd
    return
  fi

  if [ -x "$OPEN5GS_PREFIX/bin/open5gs-amfd" ]; then
    echo "    starting Open5GS from $OPEN5GS_PREFIX ..."
    for svc in nrfd scpd amfd smfd upfd ausfd udmd pcfd nssfd bsfd udrd; do
      bin="$OPEN5GS_PREFIX/bin/open5gs-${svc}"
      cfg="$OPEN5GS_PREFIX/etc/open5gs/${svc}.yaml"
      if [ -x "$bin" ] && [ -f "$cfg" ]; then
        "$bin" -c "$cfg" >> "$RUN/open5gs.log" 2>&1 &
      fi
    done
    return
  fi

  echo "Open5GS not found. Run: bash scripts/install-open5gs.sh"
  exit 1
}

start_gnb() {
  if [ ! -x "$GNB_BIN" ]; then
    echo "gNB missing: bash scripts/install-srsran.sh"
    exit 1
  fi
  if pgrep -f "$GNB_BIN" >/dev/null 2>&1; then
    echo "    gNB already running"
    return
  fi
  echo "    starting gNB: $GNB_BIN"
  "$GNB_BIN" -c "$GNB_CFG" >> "$RUN/gnb.log" 2>&1 &
  echo $! > "$RUN/gnb.pid"
}

stop_all() {
  if [ -f "$RUN/gnb.pid" ]; then
    kill "$(cat "$RUN/gnb.pid")" 2>/dev/null || true
  fi
  rm -f "$RUN/gnb.pid"
  pkill -f "$GNB_BIN" 2>/dev/null || true
  if systemctl list-unit-files open5gs-amfd.service >/dev/null 2>&1; then
    sudo systemctl stop open5gs-nrfd open5gs-scpd open5gs-amfd open5gs-smfd open5gs-upfd 2>/dev/null || true
    sudo systemctl stop open5gs-ausfd open5gs-udmd open5gs-pcfd open5gs-nssfd open5gs-bsfd open5gs-udrd 2>/dev/null || true
  else
    pkill -f open5gs- 2>/dev/null || true
  fi
  echo "    stopped"
}

status_all() {
  echo "MongoDB:"
  pgrep -a mongod || echo "  down"
  echo "Open5GS:"
  pgrep -af open5gs || echo "  down"
  echo "gNB:"
  pgrep -af gnb || echo "  down"
  echo "B210:"
  uhd_find_devices 2>/dev/null || echo "  not found"
}

cmd="${1:-status}"
case "$cmd" in
  up)
    start_mongodb
    start_open5gs
    sleep 5
    bash "$ROOT/scripts/patch-open5gs-plmn.sh" 2>/dev/null || true
    start_gnb
    ;;
  down)
    stop_all
    ;;
  core)
    start_mongodb
    start_open5gs
    ;;
  gnb)
    start_gnb
    ;;
  status)
    status_all
    ;;
  logs)
    tail -f "$RUN/gnb.log" "$RUN/open5gs.log" 2>/dev/null || true
    ;;
  *)
    echo "Usage: $0 {up|down|core|gnb|status|logs}"
    exit 2
    ;;
esac
