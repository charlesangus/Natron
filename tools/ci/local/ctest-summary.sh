#!/usr/bin/env bash
# Usage: ctest-summary.sh <captured ctest stdout>
#
# Reads the output of `ctest -j N --output-on-failure` and prints what failed:
#   [  FAILED  ] Suite.Case           gtest cases that reported a failure
#   [  CRASHED ] 637 - Suite (STATUS) tests that crashed, aborted or timed out
#   [  NO CASE ] 680 - Suite (Failed) tests ctest failed with no gtest case failure
# A crash takes the rest of that binary's cases with it, and those never print
# anything, so a CRASHED line means the case list above it is incomplete.
# Prints nothing when ctest reports no failed tests. Always exits 0.
set -u

if [[ $# -ne 1 || ! -r "$1" ]]; then
    echo "usage: ctest-summary.sh <captured ctest stdout>" >&2
    exit 2
fi

# Log lines may carry a "<job>\t<step>\t<timestamp>Z " prefix (as in a downloaded
# Actions log); the ctest text starts after it.
sed -E 's/^.*[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z //; s/\r$//' "$1" | awk '
    # ctest prints a failed test'"'"'s output in one piece right after its result line.
    /^[0-9]+\/[0-9]+ +Test +#[0-9]+: / || /^ +Start +[0-9]+: / || /tests passed, [0-9]+ tests failed/ {
        block = ""
    }
    /^[0-9]+\/[0-9]+ +Test +#[0-9]+: / {
        line = $0
        sub(/^[0-9]+\/[0-9]+ +Test +#[0-9]+: /, "", line)
        sub(/ \.+.*$/, "", line)
        block = line
        next
    }
    /^\[  FAILED  \] [^ ]+\.[^ ]+/ {
        name = $4
        sub(/,$/, "", name)
        if (!(name in cases)) { cases[name] = 1; corder[++k] = name }
        if (block != "") block_cases[block]++
        next
    }
    /^The following tests FAILED:/ { in_list = 1; next }
    in_list && /^[ \t]+[0-9]+ - .* \(.*\)$/ {
        entry = $0
        sub(/^[ \t]+/, "", entry)
        status = entry
        sub(/^.* \(/, "", status)
        sub(/\)$/, "", status)
        tname = entry
        sub(/^[0-9]+ - /, "", tname)
        sub(/ \([^(]*\)$/, "", tname)
        if (!(entry in seen)) { seen[entry] = 1; order[++n] = entry; st[entry] = status; nm[entry] = tname }
        next
    }
    in_list && !/^[ \t]*$/ { in_list = 0 }
    END {
        if (k > 0) {
            print "Failing cases:"
            for (i = 1; i <= k; i++) print "[  FAILED  ] " corder[i]
        }
        header = 0
        for (i = 1; i <= n; i++) {
            e = order[i]
            if (st[e] == "Failed" || st[e] == "Not Run") continue
            if (!header) { print "Crashed, aborted or timed out (later cases in that test binary did not run and are not listed above):"; header = 1 }
            print "[  CRASHED ] " e
        }
        header = 0
        for (i = 1; i <= n; i++) {
            e = order[i]
            if (st[e] != "Failed" && st[e] != "Not Run") continue
            if (st[e] == "Failed" && block_cases[nm[e]] > 0) continue
            if (!header) { print "Failed with no gtest FAILED line (assert abort, non-zero exit or not run):"; header = 1 }
            print "[  NO CASE ] " e
        }
    }
'
exit 0
