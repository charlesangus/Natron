#!/usr/bin/env bash
#
# Rejects fma(x, y, -z) in Engine/Gpu/Kernels/*.slang. Vulkan does not require fma() to round
# once (lavapipe rounds twice), so a product error computed as fma(a, b, -p) is not exact on every
# device. Use Dekker's split instead; see ffTwoProd in
# Engine/Gpu/Kernels/rawcolorcalibration.slang:100.
#
# Usage: tools/ci/check_kernel_fma.sh [kernels_dir]
#
# Limitations: the scan strips // comments only (not /* */), sees only literal calls spelled
# "fma(" in the file (not macros or calls built by a preprocessor), and treats a third argument as
# negated only when it starts with '-'. An error term written as fma(a, b, 0.0 - p) or through
# a negated temporary is not caught.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
kernels_dir="${1:-${repo_root}/Engine/Gpu/Kernels}"

# M113 - Native Warp Nodes replaces these with Dekker products on its branch; delete the entry
# when that lands (the script fails once the file has no match left).
allowlist=(resample.slang)

if [ ! -d "$kernels_dir" ]; then
    echo "error: ${kernels_dir} not found" >&2
    exit 1
fi

# Prints "<line>: <call text>" for each fma( call whose third top-level argument starts with '-'.
find_negated_fma() {
    awk '
    {
        line = $0
        sub(/\/\/.*/, "", line)
        text = text line "\n"
        starts[NR] = length(text) - length(line) - 1
    }
    END {
        n = length(text)
        pos = 1
        while ((hit = index(substr(text, pos), "fma(")) > 0) {
            start = pos + hit - 1
            pos = start + 4
            if (start > 1 && substr(text, start - 1, 1) ~ /[A-Za-z0-9_]/) continue
            depth = 1; arg = 1; third = ""; seen = 0
            for (i = pos; i <= n && depth > 0; i++) {
                c = substr(text, i, 1)
                if (c == "(") depth++
                else if (c == ")") { depth--; if (depth == 0) break }
                else if (c == "," && depth == 1) { arg++; continue }
                if (arg == 3 && depth >= 1) third = third c
            }
            sub(/^[ \t\r\n]+/, "", third)
            if (arg == 3 && substr(third, 1, 1) == "-") {
                ln = 1
                while (ln < NR && starts[ln + 1] < start) ln++
                call = substr(text, start, i - start + 1)
                gsub(/[ \t\r\n]+/, " ", call)
                print ln ": " call
            }
        }
    }' "$1"
}

is_allowlisted() {
    local name="$1" entry
    for entry in "${allowlist[@]}"; do
        [ "$entry" = "$name" ] && return 0
    done
    return 1
}

violation_count=0

for file in "${kernels_dir}"/*.slang; do
    [ -e "$file" ] || continue
    name="$(basename "$file")"
    matches="$(find_negated_fma "$file")"

    if is_allowlisted "$name"; then
        if [ -z "$matches" ]; then
            echo "error: ${name} is allowlisted but has no fma(x, y, -z) left; remove it from the allowlist in tools/ci/check_kernel_fma.sh" >&2
            violation_count=$((violation_count + 1))
        fi
        continue
    fi

    while IFS= read -r match; do
        [ -n "$match" ] || continue
        echo "${file}:${match}" >&2
        echo "  fma(x, y, -z) may round twice under Vulkan; use Dekker's split (Engine/Gpu/Kernels/rawcolorcalibration.slang:100)" >&2
        violation_count=$((violation_count + 1))
    done <<< "$matches"
done

if [ "$violation_count" -ne 0 ]; then
    echo "error: ${violation_count} kernel fma violation(s) found" >&2
    exit 1
fi

echo "Kernel fma check passed."
