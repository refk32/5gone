#!/usr/bin/env bash
# install-srsran-4g.sh — Build the OLD srsRAN 4G C API (srsRANRF), which our
# PDCCH polar/CRC decoder needs (`#include <srsran/srsran.h>` behind the
# GONE_HAVE_SRSRAN_OLD gate). This is NOT srsRAN Project (that's the gNB).
#
# We build ONLY the static libraries + headers we need (srsran_phy,
# srsran_common, support, srslog ...) into a LOCAL prefix, and disable the
# srsUE/srsENB/srsEPC apps and all RF backends so the build is fast and has the
# fewest dependencies. Nothing touches the system outside the prefix, and we do
# NOT need sudo.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/5GSniffer/5gsniffer/lib/srsRANRF"
BUILD="${SRSRAN4G_BUILD:-$ROOT/build/srsran4g}"
PREFIX="${SRSRAN4G_PREFIX:-$ROOT/build/srsran4g-install}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ "$(uname -s)" != "Linux" ]; then
  echo "srsRAN 4G build requires Linux."
  exit 1
fi

if [ ! -f "$SRC/CMakeLists.txt" ]; then
  echo "ERROR: submodule not present. Run:"
  echo "  cd $ROOT/5GSniffer && git submodule update --init --depth 1 5gsniffer/lib/srsRANRF"
  exit 1
fi

echo "==> srsRAN 4G (old C API) native build"
echo "    source: $SRC"
echo "    build:  $BUILD"
echo "    prefix: $PREFIX"
echo "    jobs:   $JOBS"
echo ""
echo "Required apt packages (Debian/Ubuntu) if not already installed:"
echo "  sudo apt-get install -y build-essential cmake libfftw3-dev libmbedtls-dev"
echo "                         libboost-system-dev libboost-program-options-dev librjson-dev"
echo ""

mkdir -p "$BUILD" "$PREFIX"
cd "$BUILD"

cmake "$SRC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_CXX_FLAGS="-w" \
  -DENABLE_SRSUE=OFF \
  -DENABLE_SRSENB=OFF \
  -DENABLE_SRSEPC=OFF \
  -DENABLE_TTCN3=OFF \
  -DENABLE_GUI=OFF \
  -DENABLE_UHD=OFF \
  -DENABLE_BLADERF=OFF \
  -DENABLE_SOAPYSDR=OFF \
  -DENABLE_SKIQ=OFF \
  -DENABLE_ZEROMQ=OFF

# Build the whole library tree (apps + RF backends are disabled above, so this
# is just the static libs: srsran_phy, srsran_common, srsran_asn1, support,
# srslog, ...) then install headers + all libs. Building everything first is
# required because `make install` copies EVERY lib that CMake registered, even
# ones we don't individually use (e.g. libasn1_utils.a).
cmake --build . -j"$JOBS"
cmake --build . --target install -j"$JOBS"

echo ""
echo "==> Done. Headers installed to: $PREFIX/include/srsran/srsran.h"
echo "    Static libs installed to:   $PREFIX/lib (srsran_phy, srsran_common, support, srslog ...)"
echo ""
echo "Next: point phase2 at this prefix when building the decoded build:"
echo "  SRSRAN4G_PREFIX=$PREFIX bash scripts/install-phase2.sh"
