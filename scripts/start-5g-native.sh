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

have_open5gs_systemd() {
  [ -f /lib/systemd/system/open5gs-amfd.service ] || [ -f /usr/lib/systemd/system/open5gs-amfd.service ]
}

# Daemon open5gs-amfd uses amf.yaml (not amfd.yaml).
open5gs_yaml() {
  local daemon="$1"
  echo "$OPEN5GS_PREFIX/etc/open5gs/${daemon%d}.yaml"
}

start_open5gs() {
  if have_open5gs_systemd; then
    echo "    starting Open5GS via systemd..."
    sudo systemctl start open5gs-nrfd open5gs-scpd open5gs-amfd open5gs-smfd open5gs-upfd
    sudo systemctl start open5gs-ausfd open5gs-udmd open5gs-pcfd open5gs-nssfd open5gs-bsfd open5gs-udrd
    return
  fi

  if [ -x "$OPEN5GS_PREFIX/bin/open5gs-amfd" ]; then
    echo "    starting Open5GS from $OPEN5GS_PREFIX ..."
    if pgrep -x open5gs-amfd >/dev/null 2>&1; then
      echo "    Open5GS already running"
      return
    fi
    # NRF/SCP first so other NFs can register.
    local started=0
    for svc in nrfd scpd amfd smfd upfd ausfd udmd pcfd nssfd bsfd udrd; do
      local bin="$OPEN5GS_PREFIX/bin/open5gs-${svc}"
      local cfg
      cfg="$(open5gs_yaml "$svc")"
      if [ -x "$bin" ] && [ -f "$cfg" ]; then
        "$bin" -c "$cfg" >> "$RUN/open5gs.log" 2>&1 &
        started=$((started + 1))
        sleep 0.2
      else
        echo "    skip ${svc}: missing $bin or $cfg"
      fi
    done
    if [ "$started" -eq 0 ]; then
      echo "Open5GS configs not found under $OPEN5GS_PREFIX/etc/open5gs/"
      exit 1
    fi
    echo "    started $started NF processes (log: $RUN/open5gs.log)"
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
  if have_open5gs_systemd; then
    sudo systemctl stop open5gs-nrfd open5gs-scpd open5gs-amfd open5gs-smfd open5gs-upfd 2>/dev/null || true
    sudo systemctl stop open5gs-ausfd open5gs-udmd open5gs-pcfd open5gs-nssfd open5gs-bsfd open5gs-udrd 2>/dev/null || true
  else
    for d in nrfd scpd amfd smfd upfd ausfd udmd pcfd nssfd bsfd udrd; do
      pkill -x "open5gs-${d}" 2>/dev/null || true
    done
  fi
  echo "    stopped"
}

wait_for_amf() {
  local i
  # NGAP is SCTP, not TCP — ss -ltn will miss 38412.
  for i in $(seq 1 25); do
    if pgrep -x open5gs-amfd >/dev/null 2>&1 && \
       ss -ln 2>/dev/null | grep -q ':38412'; then
      echo "    AMF listening on :38412 (SCTP)"
      return 0
    fi
    sleep 0.4
  done
  if pgrep -x open5gs-amfd >/dev/null 2>&1; then
    echo "    AMF process up (open5gs-amfd) — NGAP is SCTP :38412"
    return 0
  fi
  echo "    WARNING: AMF not running — see $RUN/open5gs.log"
  return 0
}

status_all() {
  echo "MongoDB:"
  pgrep -a mongod || echo "  down"
  echo "Open5GS:"
  pgrep -a open5gs-amfd || echo "  down"
  pgrep -a 'open5gs-' 2>/dev/null | grep -v 'start-5g-native' || true
  echo "gNB:"
  pgrep -af "$GNB_BIN" || echo "  down"
  echo "B210:"
  uhd_find_devices 2>/dev/null || echo "  not found"
  echo "AMF NGAP (SCTP :38412):"
  ss -ln 2>/dev/null | grep 38412 || echo "  not listening"
  echo "UPF:"
  pgrep -a open5gs-upfd || echo "  down (need ogstun — sudo ip tuntap add name ogstun mode tun)"
}

cmd="${1:-status}"
case "$cmd" in
  up)
    start_mongodb
    OPEN5GS_AMF_YAML="$OPEN5GS_PREFIX/etc/open5gs/amf.yaml" \
      OPEN5GS_NRF_YAML="$OPEN5GS_PREFIX/etc/open5gs/nrf.yaml" \
      bash "$ROOT/scripts/patch-open5gs-plmn.sh" || true
    start_open5gs
    wait_for_amf
    start_gnb
    ;;
  down)
    stop_all
    ;;
  core)
    start_mongodb
    OPEN5GS_AMF_YAML="$OPEN5GS_PREFIX/etc/open5gs/amf.yaml" \
      OPEN5GS_NRF_YAML="$OPEN5GS_PREFIX/etc/open5gs/nrf.yaml" \
      bash "$ROOT/scripts/patch-open5gs-plmn.sh" || true
    start_open5gs
    wait_for_amf
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
