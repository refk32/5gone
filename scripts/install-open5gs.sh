#!/usr/bin/env bash
# install-open5gs.sh — Native Open5GS per official docs (Ubuntu 22.04)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${OPEN5GS_SRC:-$ROOT/build/open5gs-src}"
PREFIX="${OPEN5GS_PREFIX:-$ROOT/build/open5gs}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ "$(uname -s)" != "Linux" ]; then
  echo "Open5GS requires Linux."
  exit 1
fi

echo "==> Open5GS install (native)"
echo "    Docs: https://open5gs.org/open5gs/docs/guide/02-building-open5gs-from-sources/"

if command -v open5gs-amfd >/dev/null 2>&1; then
  echo "    open5gs-amfd already in PATH — skip"
  exit 0
fi

echo "==> MongoDB 8.0 (required)"
if ! command -v mongod >/dev/null 2>&1; then
  curl -fsSL https://www.mongodb.org/static/pgp/server-8.0.asc | \
    sudo gpg -o /usr/share/keyrings/mongodb-server-8.0.gpg --dearmor
  echo "deb [ arch=amd64,arm64 signed-by=/usr/share/keyrings/mongodb-server-8.0.gpg ] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/8.0 multiverse" | \
    sudo tee /etc/apt/sources.list.d/mongodb-org-8.0.list
  sudo apt-get update
  sudo apt-get install -y mongodb-org
fi
sudo systemctl enable --now mongod

echo "==> Build dependencies"
sudo apt-get install -y \
  meson ninja-build git build-essential \
  libtalloc-dev libpcsclite-dev libsctp-dev lksctp-tools \
  libyaml-dev libmicrohttpd-dev libcurl4-openssl-dev \
  libnghttp2-dev libidn11-dev libmongoc-dev libbson-dev \
  libssl-dev libgcrypt20-dev libgnutls28-dev flex bison pkg-config

if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch v2.7.2 https://github.com/open5gs/open5gs.git "$SRC"
fi

cd "$SRC"
if [ ! -d build ]; then
  meson setup build --prefix="$PREFIX" -Ddb_server=mongodb://127.0.0.1/open5gs
fi
meson compile -C build -j"$JOBS"
meson install -C build

echo ""
echo "==> Open5GS installed: $PREFIX"
echo "    export PATH=\"$PREFIX/bin:\$PATH\""
echo "    export OPEN5GS_PREFIX=\"$PREFIX\""
echo ""
echo "Next:"
echo "  bash scripts/patch-open5gs-plmn.sh   # if using /etc/open5gs from package"
echo "  Or edit $PREFIX/etc/open5gs/amf.yaml for PLMN 001/01 TAC 1"
echo "  bash scripts/start-5g-native.sh core"
