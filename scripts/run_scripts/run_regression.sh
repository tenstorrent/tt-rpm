#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODEL=""
NUM_CYCLES=""
TRACE_DIR=""
OUTPUT_ROOT=""

usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Run a batch of simulations across a directory of traces."
    echo ""
    echo "Options:"
    echo "  -m, --model NAME          Model to run: simple, core (required)"
    echo "  -n, --num-cycles N        Tick budget per simulation, as run_sim.sh -n (required)"
    echo "  -t, --trace-dir DIR       Directory of trace files to sweep (required)"
    echo "  -o, --output DIR          Root output directory (default: build/<model>/regression/<timestamp>)"
    echo "  -h, --help                Show this help"
    echo ""
    echo "Runs one simulation per trace file found in --trace-dir."
    echo "Each run's stdout/stderr is saved to a separate subdirectory."
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") -m core -n 20000000 -t /path/to/elfs/"
    echo "  $(basename "$0") -m core -n 5000000 -t /path/to/traces/"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--model)      MODEL="$2"; shift 2 ;;
        -n|--num-cycles) NUM_CYCLES="$2"; shift 2 ;;
        -t|--trace-dir)  TRACE_DIR="$2"; shift 2 ;;
        -o|--output)     OUTPUT_ROOT="$2"; shift 2 ;;
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
if [ -z "$TRACE_DIR" ]; then
    echo "Error: --trace-dir is required"
    usage 1
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -z "$OUTPUT_ROOT" ]; then
    OUTPUT_ROOT="$RPM_ROOT/build/$MODEL/regression/$TIMESTAMP"
fi
mkdir -p "$OUTPUT_ROOT"

TRACES=()
while IFS= read -r -d '' f; do
    TRACES+=("$f")
done < <(find "$TRACE_DIR" -type f \( -name '*.csv.zst' -o -name '*.csv' -o -name '*.elf' \) -print0 | sort -z)

if [ ${#TRACES[@]} -eq 0 ]; then
    echo "No trace/elf files found in $TRACE_DIR"
    exit 1
fi

echo "===================================="
echo " RPM Regression Run"
echo "===================================="
echo "  Model:      $MODEL"
echo "  Cycles:     $NUM_CYCLES"
echo "  Traces:     ${#TRACES[@]}"
echo "  Output:     $OUTPUT_ROOT"
echo ""

TOTAL=0
PASSED=0
FAILED=0
SUMMARY_FILE="$OUTPUT_ROOT/summary.csv"
echo "trace,exit_code,output_dir" > "$SUMMARY_FILE"

run_one() {
    local trace="$1"
    local trace_name
    trace_name=$(basename "$trace")
    local run_dir="$OUTPUT_ROOT/$trace_name"

    mkdir -p "$run_dir"
    local rc=0
    "$SCRIPT_DIR/run_sim.sh" -m "$MODEL" -n "$NUM_CYCLES" -e "$trace" -o "$run_dir" || rc=$?

    echo "$trace,$rc,$run_dir" >> "$SUMMARY_FILE"
    if [ "$rc" -eq 0 ]; then
        echo "  PASS: $trace_name"
    else
        echo "  FAIL: $trace_name (exit $rc)"
    fi
    return "$rc"
}

for trace in "${TRACES[@]}"; do
    TOTAL=$((TOTAL + 1))
    if run_one "$trace"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "===================================="
echo " Regression Summary"
echo "===================================="
echo "  Total:  $TOTAL"
echo "  Passed: $PASSED"
echo "  Failed: $FAILED"
echo "  Details: $SUMMARY_FILE"

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
