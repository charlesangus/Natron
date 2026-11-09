#!/usr/bin/env bash
# Wall-clock stack sampler (perf is not available in the container). Samples every thread of
# PID with eu-stack COUNT times, INTERVAL seconds apart, into OUT. Must run with ptrace rights:
#   docker exec --privileged natron-dev tools/bench/sample_stacks.sh PID COUNT INTERVAL OUT
# Exits 3 with eu-stack's reason and the remedy on stderr (also in OUT.err) when the first sample gets
# no stacks; later failures go to OUT.err only.
set -u
pid=$1
count=$2
interval=$3
out=$4
: > "$out"
: > "$out.err"
# shellcheck source=ptrace_remedy.sh
. "$(dirname "${BASH_SOURCE[0]}")/ptrace_remedy.sh"
if ! command -v eu-stack > /dev/null 2>&1; then
    echo "sample_stacks.sh: eu-stack is not installed (sudo apt install elfutils)" | tee -a "$out.err" >&2
    exit 2
fi
sample_err=$(mktemp)
trap 'rm -f "$sample_err"' EXIT
for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    echo "=== SAMPLE $i $(date +%s.%N)" >> "$out"
    before=$(grep -c '^TID ' "$out")
    eu-stack -p "$pid" -m -n 80 >> "$out" 2> "$sample_err"
    if [ "$(grep -c '^TID ' "$out")" = "$before" ] && kill -0 "$pid" 2> /dev/null; then
        {
            echo "sample $i of pid $pid got no stacks; eu-stack said:"
            head -n 5 "$sample_err"
            ptrace_remedy
        } >> "$out.err"
        if [ "$i" = 0 ]; then
            cat "$out.err" >&2
            exit 3
        fi
    fi
    sleep "$interval"
done
