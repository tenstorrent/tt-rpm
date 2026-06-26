#!/usr/bin/env bash

# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

JOBS=$(nproc 2>/dev/null || echo 4)
# Cap parallelism by available memory to avoid OOM-killing the compiler on
# low-RAM machines/containers: at most one job per 2 GB of total RAM.
if MEM_KB=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null) && [ -n "${MEM_KB:-}" ]; then
    MEM_JOBS=$(( MEM_KB / 1024 / 1024 / 2 ))
    [ "$MEM_JOBS" -lt 1 ] && MEM_JOBS=1
    [ "$JOBS" -gt "$MEM_JOBS" ] && JOBS="$MEM_JOBS"
fi
CLEAN=0
BUILD_TYPE="Release"
MODEL=""
BUILD_ALL=0

declare -A MODEL_PATHS=(
    [simple]="models/cpu/src/simple"
    [core]="models/cpu/src/core"
)

usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Build an RPM simulation model."
    echo ""
    echo "Options:"
    echo "  -m, --model NAME      Model to build: simple, core"
    echo "  -a, --all             Build all available models"
    echo "  -j, --jobs N          Parallel build jobs (default: $JOBS)"
    echo "  -c, --clean           Clean build directory before building"
    echo "  -t, --type TYPE       CMake build type (default: Release)"
    echo "  -h, --help            Show this help"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") --model core"
    echo "  $(basename "$0") --all --jobs 8"
    echo "  $(basename "$0") --model core --clean --type Debug"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--model) MODEL="$2"; shift 2 ;;
        -a|--all)   BUILD_ALL=1; shift ;;
        -j|--jobs)  JOBS="$2"; shift 2 ;;
        -c|--clean) CLEAN=1; shift ;;
        -t|--type)  BUILD_TYPE="$2"; shift 2 ;;
        -h|--help)  usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

if [ "$BUILD_ALL" -eq 0 ] && [ -z "$MODEL" ]; then
    echo "Error: specify --model NAME or --all"
    usage
fi

build_model() {
    local name="$1"
    local src_rel="${MODEL_PATHS[$name]}"
    local src_dir="$RPM_ROOT/$src_rel"
    local build_dir="$RPM_ROOT/build/$name"

    if [ ! -f "$src_dir/CMakeLists.txt" ]; then
        echo "  WARNING: No CMakeLists.txt found at $src_dir — skipping $name"
        return 0
    fi

    echo "--- Building: $name ---"
    echo "  Source:  $src_rel"
    echo "  Build:   build/$name"

    if [ "$CLEAN" -eq 1 ] && [ -d "$build_dir" ]; then
        echo "  Cleaning build directory..."
        rm -rf "$build_dir"
    fi

    mkdir -p "$build_dir"
    cmake -S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$build_dir" -j "$JOBS"

    # Symlink config.yaml so the binary always reads the source copy
    if [ -f "$src_dir/config.yaml" ]; then
        ln -sf "$src_dir/config.yaml" "$build_dir/config.yaml"
        echo "  Linked config.yaml -> $src_rel/config.yaml"
    fi

    echo "  $name build complete."
    echo ""
}

echo "===================================="
echo " Building RPM Model(s)"
echo "===================================="
echo "  Build type: $BUILD_TYPE"
echo "  Jobs:       $JOBS"
echo ""

if [ "$BUILD_ALL" -eq 1 ]; then
    for name in "${!MODEL_PATHS[@]}"; do
        build_model "$name"
    done
else
    if [ -z "${MODEL_PATHS[$MODEL]+x}" ]; then
        echo "Error: unknown model '$MODEL'"
        echo "Available models: ${!MODEL_PATHS[*]}"
        exit 1
    fi
    build_model "$MODEL"
fi

echo "===================================="
echo " Model build(s) complete"
echo "===================================="
