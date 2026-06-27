#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

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

echo "[Step 5/5] Building test workloads..."
# The workloads are bare-metal RISC-V and need the (optional) newlib cross
# toolchain. Build them only if it's on PATH; otherwise skip cleanly so a fresh
# clone without the toolchain still finishes build_all without a confusing error.
if command -v "${CC:-riscv64-unknown-elf-gcc}" >/dev/null 2>&1; then
    make -C "$RPM_ROOT/tests" all
else
    echo "  Skipped: bare-metal RISC-V toolchain (riscv64-unknown-elf-gcc) not on PATH."
    echo "  Deps and models built fine. To build the test workloads, install the"
    echo "  toolchain once and build the tests separately:"
    echo "    bash tests/install-toolchain-conda.sh   # creates conda env 'riscv'"
    echo "    conda activate riscv"
    echo "    make -C tests"
fi
echo ""

echo "============================================"
echo " Full build complete"
echo "============================================"
