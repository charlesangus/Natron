#!/usr/bin/env bash
#
# Rejects fma(x, y, -z) in Engine/Gpu/Kernels/*.slang. Vulkan does not require fma() to round
# once (lavapipe rounds twice), so a product error computed as fma(a, b, -p) is not exact on every
# device. Use Dekker's split instead; see ffTwoProd in
# Engine/Gpu/Kernels/rawcolorcalibration.slang:100.
#
# Usage: tools/ci/check_kernel_fma.sh [kernels_dir]
#        tools/ci/check_kernel_fma.sh --self-test
#
# Limitations: the scan strips // comments only (not /* */), sees only literal calls spelled
# "fma" and "(" in the file, with optional whitespace between (not macros or calls built by a
# preprocessor), and treats a third argument as negated only when it starts with '-' once enclosing
# parentheses are removed. An error term written as fma(a, b, 0.0 - p) or through a negated
# temporary is not caught.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script_path="${BASH_SOURCE[0]}"
mode="check"
if [ "${1:-}" = "--self-test" ]; then
    mode="self-test"
    shift
fi
kernels_dir="${1:-${repo_root}/Engine/Gpu/Kernels}"

# M113 - Native Warp Nodes replaces these with Dekker products on its branch; delete the entry
# when that lands (the script fails once the file has no match left).
allowlist=(resample.slang)

if [ ! -d "$kernels_dir" ]; then
    echo "error: ${kernels_dir} not found" >&2
    exit 1
fi

# Prints "<line>: <call text>" for each fma call whose third top-level argument starts with '-'
# after its enclosing parentheses are unwrapped.
find_negated_fma() {
    awk '
    function trim(t) {
        sub(/^[ \t\r\n]+/, "", t)
        sub(/[ \t\r\n]+$/, "", t)
        return t
    }
    function unwrap(t,    d, k, ch, close_at) {
        t = trim(t)
        while (substr(t, 1, 1) == "(") {
            d = 0; close_at = 0
            for (k = 1; k <= length(t); k++) {
                ch = substr(t, k, 1)
                if (ch == "(") d++
                else if (ch == ")") { d--; if (d == 0) { close_at = k; break } }
            }
            if (close_at != length(t)) break
            t = trim(substr(t, 2, close_at - 2))
        }
        return t
    }
    {
        line = $0
        sub(/\/\/.*/, "", line)
        text = text line "\n"
        starts[NR] = length(text) - length(line) - 1
    }
    END {
        n = length(text)
        pos = 1
        while (match(substr(text, pos), /fma[ \t\r\n]*\(/) > 0) {
            start = pos + RSTART - 1
            pos = start + RLENGTH
            if (start > 1 && substr(text, start - 1, 1) ~ /[A-Za-z0-9_]/) continue
            depth = 1; arg = 1; third = ""
            for (i = pos; i <= n && depth > 0; i++) {
                c = substr(text, i, 1)
                if (c == "(") depth++
                else if (c == ")") { depth--; if (depth == 0) break }
                else if (c == "," && depth == 1) { arg++; continue }
                if (arg == 3 && depth >= 1) third = third c
            }
            if (arg == 3 && substr(unwrap(third), 1, 1) == "-") {
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

if [ "$mode" = "self-test" ]; then
    fixture_dir="$(mktemp -d)"
    trap 'rm -rf "${fixture_dir:?}"' EXIT
    failures=0
    case_n=0
    expect_hit() {
        case_n=$((case_n + 1))
        printf '%s\n' "$2" > "${fixture_dir}/case${case_n}.slang"
        if [ -z "$(find_negated_fma "${fixture_dir}/case${case_n}.slang")" ]; then
            echo "self-test: not caught: $1" >&2
            failures=$((failures + 1))
        fi
    }
    expect_clean() {
        case_n=$((case_n + 1))
        printf '%s\n' "$2" > "${fixture_dir}/case${case_n}.slang"
        if [ -n "$(find_negated_fma "${fixture_dir}/case${case_n}.slang")" ]; then
            echo "self-test: flagged: $1" >&2
            failures=$((failures + 1))
        fi
    }
    expect_hit "plain" 'float e = fma(a, b, -p);'
    expect_hit "space before paren" 'float e = fma (a, b, -p);'
    expect_hit "newline before paren" $'float e = fma\n(a, b, -p);'
    expect_hit "parenthesised negation" 'float e = fma(a, b, (-p));'
    expect_hit "doubly parenthesised negation" 'float e = fma(a, b, ( ( -p ) ));'
    expect_hit "call split across lines" $'float e = fma(a,\n b,\n -p);'
    expect_clean "positive addend" 'float e = fma(a, b, p);'
    expect_clean "negation inside a larger expression" 'float e = fma(a, b, (x) - p);'
    expect_clean "negated first argument" 'float e = fma(-a, b, p);'
    expect_clean "longer identifier" 'float e = myfma(a, b, -p);'
    expect_clean "commented out" '// float e = fma(a, b, -p);'
    if [ "$failures" -ne 0 ]; then
        echo "error: ${failures} self-test failure(s) in ${script_path}" >&2
        exit 1
    fi
    echo "Kernel fma self-test passed (${case_n} cases)."
    exit 0
fi

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
