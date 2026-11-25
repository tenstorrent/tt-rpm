#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODEL=""
NUM_CYCLES=""
ELF=""
OUTPUT_DIR=""

declare -A MODEL_BINARIES=(
    [simple]="build/simple/simple"
    [core]="build/core/core"
)

usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Run an RPM simulation model (execution-driven, on an ELF)."
    echo ""
    echo "Options:"
    echo "  -m, --model NAME        Model to run: simple, core (required)"
    echo "  -n, --num-cycles N      Run limit in ticks (1 tick = 1 ps; ~333 ticks/cycle at 3 GHz) (required)"
    echo "  -e, --elf PATH          ELF binary to execute (required)"
    echo "  -o, --output DIR        Output directory (default: build/<model>/output/<timestamp>)"
    echo "  -h, --help              Show this help"
    echo ""
    echo "The binary reads config.yaml from its working directory (build/<model>/)."
    echo "build_model.sh links config.yaml there automatically."
    echo ""
    echo "Simulation stdout (retire trace) is saved to <output>/out.txt"
    echo "Simulation stderr (logs/warnings) is saved to <output>/ilog.txt"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") -m core -n 2000000 -e tests/build/coremark.bare.elf"
    echo "  $(basename "$0") -m simple -n 1000 -e /path/to/program.elf -o results/run1"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--model)      MODEL="$2"; shift 2 ;;
        -n|--num-cycles) NUM_CYCLES="$2"; shift 2 ;;
        -e|--elf)        ELF="$2"; shift 2 ;;
        -o|--output)     OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help)       usage ;;
        *)               echo "Unknown option: $1"; usage 1 ;;
    esac
done

if [ -z "$MODEL" ]; then
    echo "Error: --model is required"
    usage 1
fi

if [ -z "$NUM_CYCLES" ]; then
    echo "Error: --num-cycles is required"
    usage 1
fi

if [ -z "$ELF" ]; then
    echo "Error: --elf is required"
    usage 1
fi

if [ ! -f "$ELF" ]; then
    echo "Error: ELF not found: $ELF"
    exit 1
fi
# Resolve to an absolute path: the binary runs from build/<model>/, so a
# relative ELF path would otherwise be resolved against the wrong directory.
ELF="$(readlink -f "$ELF")"

BIN_REL="${MODEL_BINARIES[$MODEL]:-}"
if [ -z "$BIN_REL" ]; then
    echo "Error: unknown model '$MODEL'"
    echo "Available models: ${!MODEL_BINARIES[*]}"
    exit 1
fi

BINARY="$RPM_ROOT/$BIN_REL"
if [ ! -x "$BINARY" ]; then
    echo "Error: binary not found at $BINARY"
    echo "Build it first: scripts/build_scripts/build_model.sh --model $MODEL"
    exit 1
fi

BUILD_DIR="$(dirname "$BINARY")"
if [ ! -f "$BUILD_DIR/config.yaml" ]; then
    echo "Error: $BUILD_DIR/config.yaml is missing or a dangling link"
    echo "  Rebuild with build_model.sh to link config.yaml from the model source."
    exit 1
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$RPM_ROOT/build/$MODEL/output/$TIMESTAMP"
fi
mkdir -p "$OUTPUT_DIR"

echo "===================================="
echo " RPM Simulation"
echo "===================================="
echo "  Model:      $MODEL"
echo "  Binary:     $BIN_REL"
echo "  Ticks:      $NUM_CYCLES"
echo "  ELF:        $ELF"
echo "  Output:     $OUTPUT_DIR"
echo "  Config:     $BUILD_DIR/config.yaml"
echo ""

# core reads config.yaml only via -c, which its legacy positional form can't take.
SIM_ARGS=("$NUM_CYCLES" "$ELF")
if [ "$MODEL" = core ]; then
    case "$ELF" in
        *-trace*|*-snap*|*.csv|*.csv.zst) INPUT_FLAG=--snapshot ;;
        *)                                INPUT_FLAG=--target-elf ;;
    esac
    SIM_ARGS=(-c config.yaml -r "$NUM_CYCLES" "$INPUT_FLAG" "$ELF")
fi

# Capture the exit code without tripping `set -e`, so the summary always prints.
if (cd "$BUILD_DIR" && "$BINARY" "${SIM_ARGS[@]}") \
        > "$OUTPUT_DIR/out.txt" 2> "$OUTPUT_DIR/ilog.txt"; then
    EXIT_CODE=0
else
    EXIT_CODE=$?
fi

echo "Simulation finished (exit code: $EXIT_CODE)"
echo "  stdout -> $OUTPUT_DIR/out.txt"
echo "  stderr -> $OUTPUT_DIR/ilog.txt"

if [ "$EXIT_CODE" -ne 0 ]; then
    echo ""
    echo "--- Last 20 lines of ilog.txt ---"
    tail -20 "$OUTPUT_DIR/ilog.txt"
fi

exit "$EXIT_CODE"
