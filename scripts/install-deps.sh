#!/usr/bin/env bash
# install-deps.sh — 5Gone PoC base dependencies (native, no Docker)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OS="$(uname -s)"

echo "==> 5Gone PoC dependency installer (native)"
echo "    root: $ROOT"
echo "    os:   $OS"

install_linux() {
  echo "==> Installing Linux packages (Ubuntu 22.04 recommended)..."
  sudo apt-get update
  sudo apt-get install -y \
    build-essential cmake git python3 python3-pip python3-venv \
    libuhd-dev uhd-host \
    libfftw3-dev libmbedtls-dev libsctp-dev libyaml-cpp-dev \
    libgtest-dev libboost-all-dev libconfig-dev \
    libsctp-dev lksctp-tools pkg-config \
    meson ninja-build flex bison libtalloc-dev libpcsclite-dev

  echo "==> UHD FPGA images..."
  sudo uhd_images_downloader || true

  echo "==> USB tuning for B210 (optional, run after connect)..."
  echo "    bash scripts/tune-usb-b210.sh"
}

install_macos() {
  echo "==> macOS — Phase 0 + UHD only"
  if ! command -v brew >/dev/null 2>&1; then
    echo "Install Homebrew: https://brew.sh"
    exit 1
  fi
  brew install uhd python@3.12 git meson ninja
  uhd_images_downloader || true
  echo ""
  echo "5G stack: Ubuntu 22.04 bare metal (recommended) or VM with USB passthrough."
  echo "Avoid Docker for B210 — USB passthrough is unstable."
}

setup_python_venv() {
  if [ ! -d "$ROOT/.venv" ]; then
    python3 -m venv "$ROOT/.venv"
  fi
  # shellcheck disable=SC1091
  source "$ROOT/.venv/bin/activate"
  pip install -q --upgrade pip
  [ -f "$ROOT/phase0/requirements.txt" ] && pip install -q -r "$ROOT/phase0/requirements.txt"
  [ -f "$ROOT/phase2/requirements.txt" ] && pip install -q -r "$ROOT/phase2/requirements.txt"
  deactivate
}

mkdir -p "$ROOT/data" "$ROOT/logs" "$ROOT/build" "$ROOT/run"

case "$OS" in
  Linux) install_linux ;;
  Darwin) install_macos ;;
  *) echo "Use Ubuntu 22.04 for native 5G stack"; exit 1 ;;
esac

setup_python_venv

echo ""
echo "==> Base deps OK. Next (Linux):"
echo "    bash scripts/install-open5gs.sh"
echo "    bash scripts/install-srsran.sh"
echo "    bash scripts/setup-lab.sh --phase 0"
