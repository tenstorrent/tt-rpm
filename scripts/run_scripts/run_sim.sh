#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODEL=""
NUM_CYCLES=""
TRACE=""
ELF=""
OUTPUT_DIR=""

declare -A MODEL_BINARIES=(
    [simple]="build/simple/simple"
    [core]="build/core/core"
)

usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Run an RPM simulation model."
    echo ""
    echo "Options:"
    echo "  -m, --model NAME        Model to run: simple, core (required)"
    echo "  -n, --num-cycles N      Number of cycles to simulate (required)"
    echo "  -t, --trace PATH        Trace file for trace-driven mode (e.g. ...-trace.csv.zst)"
    echo "  -e, --elf PATH          ELF binary for execution-driven mode"
    echo "  -o, --output DIR        Output directory (default: build/<model>/output/<timestamp>)"
    echo "  -h, --help              Show this help"
    echo ""
    echo "The binary reads config.yaml from its working directory (build/<model>/)."
    echo "build_model.sh copies config.yaml there automatically."
    echo ""
    echo "Simulation stdout (retire trace) is saved to <output>/out.txt"
    echo "Simulation stderr (logs/warnings) is saved to <output>/ilog.txt"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") -m core -n 20000000 -t /path/to/trace.csv.zst"
    echo "  $(basename "$0") -m simple -n 1000 -e /path/to/program.elf"
    echo "  $(basename "$0") -m core -n 5000000 -t /path/to/trace.csv.zst -o results/run1"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--model)      MODEL="$2"; shift 2 ;;
        -n|--num-cycles) NUM_CYCLES="$2"; shift 2 ;;
        -t|--trace)      TRACE="$2"; shift 2 ;;
        -e|--elf)        ELF="$2"; shift 2 ;;
        -o|--output)     OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help)       usage ;;
        *)               echo "Unknown option: $1"; usage ;;
    esac
done

if [ -z "$MODEL" ]; then
    echo "Error: --model is required"
    usage
fi

if [ -z "$NUM_CYCLES" ]; then
    echo "Error: --num-cycles is required"
    usage
fi

if [ -n "$TRACE" ] && [ -n "$ELF" ]; then
    echo "Error: specify --trace or --elf, not both"
    exit 1
fi

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
    echo "Warning: config.yaml not found in $BUILD_DIR"
    echo "  The model will use scalar defaults (retire_width=1, fetch_width=1)."
    echo "  Rebuild with build_model.sh to copy config.yaml automatically."
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$RPM_ROOT/build/$MODEL/output/$TIMESTAMP"
fi
mkdir -p "$OUTPUT_DIR"

TARGET=""
if [ -n "$TRACE" ]; then
    TARGET="$TRACE"
elif [ -n "$ELF" ]; then
    TARGET="$ELF"
fi

echo "===================================="
echo " RPM Simulation"
echo "===================================="
echo "  Model:      $MODEL"
echo "  Binary:     $BIN_REL"
echo "  Cycles:     $NUM_CYCLES"
if [ -n "$TRACE" ]; then
    echo "  Trace:      $TRACE"
elif [ -n "$ELF" ]; then
    echo "  ELF:        $ELF"
fi
echo "  Output:     $OUTPUT_DIR"
if [ -f "$BUILD_DIR/config.yaml" ]; then
    echo "  Config:     $BUILD_DIR/config.yaml"
fi
echo ""

SIM_ARGS=("$NUM_CYCLES")
if [ -n "$TARGET" ]; then
    SIM_ARGS+=("$TARGET")
fi

(cd "$BUILD_DIR" && "$BINARY" "${SIM_ARGS[@]}") \
    > "$OUTPUT_DIR/out.txt" \
    2> "$OUTPUT_DIR/ilog.txt"
EXIT_CODE=$?

echo "Simulation finished (exit code: $EXIT_CODE)"
echo "  stdout -> $OUTPUT_DIR/out.txt"
echo "  stderr -> $OUTPUT_DIR/ilog.txt"

if [ "$EXIT_CODE" -ne 0 ]; then
    echo ""
    echo "--- Last 20 lines of ilog.txt ---"
    tail -20 "$OUTPUT_DIR/ilog.txt"
fi

exit "$EXIT_CODE"
