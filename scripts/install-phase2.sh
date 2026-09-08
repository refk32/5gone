#!/usr/bin/env bash
# install-phase2.sh — Build 5Gone RAR DoS attacker (Phase 2)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${PHASE2_BUILD:-$ROOT/build/phase2}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

echo "==> Phase 2 — 5Gone RAR DoS attacker build"
echo "    root:  $ROOT"
echo "    build: $BUILD"

if [ "$(uname -s)" != "Linux" ]; then
  echo "==> Non-Linux: generating IQ templates + Python sim only"
  python3 "$ROOT/phase2/tools/generate_msg3_template.py" --output "$ROOT/phase2/iq_templates"
  if [ ! -d "$ROOT/.venv" ]; then
    python3 -m venv "$ROOT/.venv"
  fi
  # shellcheck disable=SC1091
  source "$ROOT/.venv/bin/activate"
  pip install -q -r "$ROOT/phase2/requirements.txt" 2>/dev/null || pip install -q numpy pyyaml
  python3 "$ROOT/phase2/python/orchestrator.py" demo || true
  echo ""
  echo "C++ binary requires Linux + UHD. Run install-phase2.sh on office Ubuntu."
  exit 0
fi

# Python deps + IQ templates
if [ ! -d "$ROOT/.venv" ]; then
  python3 -m venv "$ROOT/.venv"
fi
# shellcheck disable=SC1091
source "$ROOT/.venv/bin/activate"
pip install -q --upgrade pip
pip install -q -r "$ROOT/phase2/requirements.txt"

python3 "$ROOT/phase2/tools/generate_msg3_template.py" --output "$ROOT/phase2/iq_templates"

mkdir -p "$BUILD"
cd "$BUILD"

export SRSRAN_BUILD="${SRSRAN_BUILD:-$ROOT/build/srsran_build}"
export SRSRAN4G_PREFIX="${SRSRAN4G_PREFIX:-$ROOT/build/srsran4g-install}"
export SRSRAN4G_BUILD="${SRSRAN4G_BUILD:-$ROOT/build/srsran4g}"
cmake "$ROOT/phase2" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSRSRAN_BUILD_DIR="$SRSRAN_BUILD" \
  -DSRSRAN4G_PREFIX="$SRSRAN4G_PREFIX" \
  -DSRSRAN4G_BUILD="$SRSRAN4G_BUILD"
cmake --build . -j"$JOBS"

echo ""
echo "==> Phase 2 binary: $BUILD/5gone-rar-dos"
echo "    Sim demo:  bash scripts/run-attack-rar-dos.sh sim"
echo "    Inject:    bash scripts/run-attack-rar-dos.sh inject"
echo "    Live RF:   bash scripts/run-attack-rar-dos.sh live  (2nd B210 required)"
