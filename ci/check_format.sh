#!/usr/bin/env bash
# Check C++ formatting against the repo's .clang-format config.
# Returns non-zero if any file needs reformatting.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ERRORS=0

while IFS= read -r -d '' file; do
    if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
        echo "FAIL: $file"
        clang-format --dry-run "$file" 2>&1 || true
        ERRORS=$((ERRORS + 1))
    fi
done < <(find "$RPM_ROOT/models" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0)

if [ "$ERRORS" -gt 0 ]; then
    echo ""
    echo "$ERRORS file(s) need reformatting."
    echo "Run:  find models/ -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | xargs clang-format -i"
    exit 1
fi

echo "All files pass clang-format check."
