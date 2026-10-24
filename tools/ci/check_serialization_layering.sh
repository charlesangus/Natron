#!/usr/bin/env bash
#
# Enforces the Serialization/README.md layering rule: Serialization/ is a
# plain-data library with no Engine/, Gui/, Qt, Python or boost.archive /
# boost.serialization includes.
#
# Usage: tools/ci/check_serialization_layering.sh

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
serialization_dir="${repo_root}/Serialization"

if [ ! -d "$serialization_dir" ]; then
    echo "error: ${serialization_dir} not found" >&2
    exit 1
fi

violation_count=0

report() {
    local relative_path="$1"
    local line_number="$2"
    local reason="$3"
    local matched_line="$4"

    echo "${relative_path}:${line_number}: ${reason}: ${matched_line}" >&2
    violation_count=$((violation_count + 1))
}

check_pattern() {
    local file="$1"
    local relative_path="$2"
    local pattern="$3"
    local reason="$4"

    while IFS=: read -r line_number matched_line; do
        [ -n "$line_number" ] || continue
        report "$relative_path" "$line_number" "$reason" "$matched_line"
    done < <(grep -n -E "$pattern" "$file" || true)
}

while IFS= read -r -d '' file; do
    relative_path="${file#"${repo_root}"/}"

    check_pattern "$file" "$relative_path" \
        '^[[:space:]]*#include[[:space:]]*[<"]Engine/' \
        "Serialization/ must not include Engine/"

    check_pattern "$file" "$relative_path" \
        '^[[:space:]]*#include[[:space:]]*[<"]Gui/' \
        "Serialization/ must not include Gui/"

    check_pattern "$file" "$relative_path" \
        '^[[:space:]]*#include[[:space:]]*[<"]Q(t|[A-Z])' \
        "Serialization/ must not include a Qt header"

    check_pattern "$file" "$relative_path" \
        '^[[:space:]]*#include[[:space:]]*[<"]Python\.h[">]' \
        "Serialization/ must not include Python.h"

    check_pattern "$file" "$relative_path" \
        '^[[:space:]]*#include[[:space:]]*[<"]boost/(archive|serialization)/' \
        "Serialization/ must not include boost.archive or boost.serialization"
done < <(find "$serialization_dir" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) -print0)

if [ "$violation_count" -ne 0 ]; then
    echo "error: ${violation_count} Serialization/ layering violation(s) found" >&2
    exit 1
fi

echo "Serialization/ layering check passed."
