#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ARGS=("${@}")

echo "============================================"
echo " RPM Full Build"
echo "============================================"
echo ""

RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "[Step 1/5] Updating submodules..."
git -C "$RPM_ROOT" submodule update --init --recursive
echo ""

echo "[Step 2/5] Checking environment..."
"$SCRIPT_DIR/setup_env.sh"
echo ""

echo "[Step 3/5] Building dependencies..."
"$SCRIPT_DIR/build_deps.sh" ${ARGS[@]+"${ARGS[@]}"}
echo ""

echo "[Step 4/5] Building all models..."
"$SCRIPT_DIR/build_model.sh" --all ${ARGS[@]+"${ARGS[@]}"}
echo ""

echo "[Step 5/5] Building test programs..."
make -C "$RPM_ROOT/tests/programs" generated
echo ""

echo "============================================"
echo " Full build complete"
echo "============================================"
