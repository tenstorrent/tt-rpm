#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

JOBS=$(nproc 2>/dev/null || echo 4)
# Cap parallelism by available memory.
# Default to at most one job per 2 GB of total RAM.
if MEM_KB=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null) && [ -n "${MEM_KB:-}" ]; then
    MEM_JOBS=$(( MEM_KB / 1024 / 1024 / 2 ))
    [ "$MEM_JOBS" -lt 1 ] && MEM_JOBS=1
    [ "$JOBS" -gt "$MEM_JOBS" ] && JOBS="$MEM_JOBS"
fi
CLEAN=0
BUILD_TYPE="Release"

usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Build all external dependencies (Sparta, Whisper, etc.)"
    echo ""
    echo "Options:"
    echo "  -j, --jobs N          Parallel build jobs (default: $JOBS)"
    echo "  -c, --clean           Clean build (remove existing build artifacts first)"
    echo "  -t, --type TYPE       CMake build type for Sparta (default: Release)"
    echo "  -h, --help            Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j|--jobs)  JOBS="$2"; shift 2 ;;
        -c|--clean) CLEAN=1; shift ;;
        -t|--type)  BUILD_TYPE="$2"; shift 2 ;;
        -h|--help)  usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

SPARTA_ROOT="$RPM_ROOT/ext/map/sparta"
SPARTA_BUILD="$SPARTA_ROOT/release"
WHISPER_ROOT="$RPM_ROOT/ext/whisper"

echo "===================================="
echo " Building External Dependencies"
echo "===================================="
echo "  RPM_ROOT:   $RPM_ROOT"
echo "  Jobs:       $JOBS"
echo "  Build type: $BUILD_TYPE"
echo ""

# --- Sparta ---
echo "--- [1/3] Sparta ---"
if [ "$CLEAN" -eq 1 ] && [ -d "$SPARTA_BUILD" ]; then
    echo "  Cleaning $SPARTA_BUILD"
    rm -rf "$SPARTA_BUILD"
fi

if [ ! -f "$SPARTA_ROOT/CMakeLists.txt" ]; then
    echo "  ERROR: Sparta source not found at $SPARTA_ROOT"
    echo "  Run: git submodule update --init --recursive"
    exit 1
fi
mkdir -p "$SPARTA_BUILD"
YAML_SHIM="$RPM_ROOT/models/cmake/yaml-cpp-shim.cmake"
cmake -S "$SPARTA_ROOT" -B "$SPARTA_BUILD" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PROJECT_INCLUDE="$YAML_SHIM"
cmake --build "$SPARTA_BUILD" --target sparta simdb -j "$JOBS"
echo "  Sparta build complete."
echo ""

# --- Whisper ---
echo "--- [2/3] Whisper (librvcore) ---"
WHISPER_BUILD="$WHISPER_ROOT/build-Linux"
if [ "$CLEAN" -eq 1 ]; then
    echo "  Cleaning whisper build artifacts"
    make -C "$WHISPER_ROOT" clean 2>/dev/null || true
fi

if [ ! -f "$WHISPER_ROOT/Makefile" ] && [ ! -f "$WHISPER_ROOT/GNUmakefile" ]; then
    echo "  ERROR: Whisper source not found at $WHISPER_ROOT"
    echo "  Run: git submodule update --init --recursive"
    exit 1
fi
make -C "$WHISPER_ROOT" -j "$JOBS"
echo "  Whisper build complete."
echo ""

# --- Whisper trace-reader ---
echo "--- [3/3] Whisper trace-reader ---"
make -C "$WHISPER_ROOT/trace-reader" TraceReader.a -j "$JOBS"
echo "  Trace-reader build complete."
echo ""

echo "===================================="
echo " All dependencies built successfully"
echo "===================================="
