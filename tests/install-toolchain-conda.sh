#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

# Install 64-bit RISC-V toolchain via Conda (no sudo). Creates env 'riscv' and installs riscv-tools.
# Requires conda (miniconda or anaconda).

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

# --override-channels: riscv-tools (ucb-bar) and its deps (conda-forge) need nothing from the
# Anaconda defaults channels, whose Terms of Service prompt aborts a non-interactive create.
echo "Creating conda env '$ENV_NAME' with riscv-tools (channels: ucb-bar, conda-forge)..."
conda create -n "$ENV_NAME" --override-channels -c ucb-bar -c conda-forge -y riscv-tools || {
    echo "conda create failed. Check that https://conda.anaconda.org/ucb-bar and"
    echo "https://conda.anaconda.org/conda-forge are reachable, or remove a partial env first:"
    echo "  conda env remove -n $ENV_NAME"
    exit 1
}

echo ""
echo "Done. To build the test workloads:"
echo "  conda activate $ENV_NAME"
echo "  make -C $(dirname "$0")"
echo ""
echo "Then run them in the core model, e.g.: make -C $(dirname "$0") run_coremark"
