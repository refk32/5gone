#!/usr/bin/env bash
# install-srsran.sh — Native srsRAN Project gNB build (UHD/B210)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${SRSRAN_SRC:-$ROOT/build/srsran_project}"
BUILD="${SRSRAN_BUILD:-$ROOT/build/srsran_build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ "$(uname -s)" != "Linux" ]; then
  echo "srsRAN gNB requires Linux with direct USB to B210."
  exit 1
fi

echo "==> srsRAN Project native build (gnb only)"
echo "    source: $SRC"
echo "    build:  $BUILD"

if [ -x "$BUILD/apps/gnb/gnb" ]; then
  echo "    Already built: $BUILD/apps/gnb/gnb"
  exit 0
fi

if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch release_24_10 https://github.com/srsran/srsran_project.git "$SRC"
fi

mkdir -p "$BUILD"
cd "$BUILD"
cmake "$SRC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_EXPORT=ON \
  -DENABLE_UHD=ON \
  -DENABLE_ZEROMQ=OFF
cmake --build . --target gnb -j"$JOBS"

echo ""
echo "==> srsRAN gNB binary: $BUILD/apps/gnb/gnb"
echo "    Config: $ROOT/config/srsran/gnb_20mhz.yml"
echo "    Run:    bash scripts/start-5g-native.sh gnb"
