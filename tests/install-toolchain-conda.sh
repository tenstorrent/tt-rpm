#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

# Install 64-bit RISC-V toolchain via Conda (no sudo). Creates env 'riscv' and installs riscv-tools.
# Run from this directory. Requires conda (miniconda or anaconda).

set -e
ENV_NAME="${RISCV_CONDA_ENV:-riscv}"

if ! command -v conda &>/dev/null; then
    echo "conda: command not found"
    echo ""
    echo "Install Miniconda without sudo:"
    echo "  1. Download: https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh"
    echo "  2. Run: bash Miniconda3-latest-Linux-x86_64.sh"
    echo "  3. Restart your shell, then run this script again."
    echo ""
    exit 1
fi

echo "Adding ucb-bar and conda-forge channels (RISC-V toolchain; conda-forge for libzlib)..."
conda config --add channels ucb-bar 2>/dev/null || true
conda config --append channels conda-forge 2>/dev/null || true
conda config --set channel_priority flexible 2>/dev/null || true

echo "Creating conda env '$ENV_NAME' with riscv-tools..."
conda create -n "$ENV_NAME" -c ucb-bar -c conda-forge -y riscv-tools || {
    echo "If riscv-tools failed, try: conda create -n $ENV_NAME -c ucb-bar -c conda-forge -y riscv-tools"
    exit 1
}

echo ""
echo "Done. To build the assembly tests:"
echo "  conda activate $ENV_NAME"
echo "  cd $(dirname "$0")"
echo "  make"
echo ""
echo "Then set target_command in config.yaml to the ELF path and run the core model."
