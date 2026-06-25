#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ERRORS=0

check_cmd() {
    local cmd="$1"
    local min_version="${2:-}"
    if command -v "$cmd" &>/dev/null; then
        local ver
        ver=$("$cmd" --version 2>&1 | head -1)
        echo -e "  ${GREEN}[OK]${NC} $cmd  ($ver)"
    else
        echo -e "  ${RED}[MISSING]${NC} $cmd"
        ERRORS=$((ERRORS + 1))
    fi
}

check_pkg() {
    local pkg="$1"
    local fallback_header="${2:-}"
    if pkg-config --exists "$pkg" 2>/dev/null; then
        local ver
        ver=$(pkg-config --modversion "$pkg" 2>/dev/null || echo "unknown")
        echo -e "  ${GREEN}[OK]${NC} $pkg  (${ver})"
    elif [ -n "$fallback_header" ] && echo "#include <${fallback_header}>" | g++ -x c++ -E - &>/dev/null 2>&1; then
        echo -e "  ${GREEN}[OK]${NC} $pkg  (found via header)"
    else
        echo -e "  ${RED}[MISSING]${NC} $pkg — install via your package manager"
        ERRORS=$((ERRORS + 1))
    fi
}

check_lib() {
    local name="$1"
    local header="$2"
    if echo "#include <${header}>" | g++ -x c++ -E - &>/dev/null 2>&1; then
        echo -e "  ${GREEN}[OK]${NC} $name"
    else
        echo -e "  ${RED}[MISSING]${NC} $name  (could not find <${header}>)"
        ERRORS=$((ERRORS + 1))
    fi
}

echo "=============================="
echo " RPM Environment Check"
echo "=============================="
echo ""

echo "Build tools:"
check_cmd cmake
check_cmd make
check_cmd g++
check_cmd git
echo ""

echo "Libraries (pkg-config / header fallback):"
check_pkg yaml-cpp "yaml-cpp/yaml.h"
check_pkg sqlite3 "sqlite3.h"
check_pkg zlib "zlib.h"
check_pkg RapidJSON "rapidjson/rapidjson.h"
echo ""

echo "Libraries (header probe):"
check_lib "Boost"    "boost/version.hpp"
echo ""

echo "Submodules:"
for sub in whisper map; do
    sub_path="$RPM_ROOT/ext/$sub"
    if [ -d "$sub_path" ] && [ "$(ls -A "$sub_path" 2>/dev/null)" ]; then
        echo -e "  ${GREEN}[OK]${NC} ext/$sub"
    else
        echo -e "  ${YELLOW}[EMPTY]${NC} ext/$sub — run: git submodule update --init --recursive"
        ERRORS=$((ERRORS + 1))
    fi
done
echo ""

echo "Tests (optional — bare-metal RISC-V toolchain for tests/):"
if command -v riscv64-unknown-elf-gcc &>/dev/null; then
    echo -e "  ${GREEN}[OK]${NC} riscv64-unknown-elf-gcc  ($(riscv64-unknown-elf-gcc --version | head -1))"
else
    echo -e "  ${YELLOW}[OPTIONAL]${NC} riscv64-unknown-elf-gcc not found — needed only to build tests/ (CoreMark/Dhrystone)."
    echo -e "             Install (newlib, not glibc): bash tests/install-toolchain-conda.sh && conda activate riscv"
fi
echo ""

if [ "$ERRORS" -gt 0 ]; then
    echo -e "${RED}Found $ERRORS issue(s). Please resolve them before building.${NC}"
    exit 1
else
    echo -e "${GREEN}All checks passed. Ready to build.${NC}"
fi
