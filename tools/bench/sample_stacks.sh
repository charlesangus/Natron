#!/usr/bin/env bash
# Wall-clock stack sampler (perf is not available in the container). Samples every thread of
# PID with eu-stack COUNT times, one start every INTERVAL seconds, into OUT. Must run with ptrace rights:
#   docker exec --privileged natron-dev tools/bench/sample_stacks.sh PID COUNT INTERVAL OUT
# Each sample starts with "=== SAMPLE <i> <start epoch> <ms eu-stack took>". A sample that takes
# longer than INTERVAL delays the next one rather than queueing more. SAMPLE_DEBUGINFO_PATH, when
# set, is passed to eu-stack as --debuginfo-path (separate debug files for the libraries).
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
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/ptrace_remedy.sh"
if ! command -v eu-stack > /dev/null 2>&1; then
    echo "sample_stacks.sh: eu-stack is not installed (sudo apt install elfutils)" | tee -a "$out.err" >&2
    exit 2
fi
debuginfo=()
[ -n "${SAMPLE_DEBUGINFO_PATH:-}" ] && debuginfo=(--debuginfo-path="$SAMPLE_DEBUGINFO_PATH")
sample_err=$(mktemp)
sample_out=$(mktemp)
trap 'rm -f "$sample_err" "$sample_out"' EXIT
interval_us=$(awk -v s="$interval" 'BEGIN { printf "%d", s * 1000000 }')
now_us() { local t=${EPOCHREALTIME/./}; echo $((10#$t)); }
next=$(now_us)
for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    start=$(now_us)
    eu-stack -p "$pid" -m -n 80 "${debuginfo[@]}" > "$sample_out" 2> "$sample_err"
    end=$(now_us)
    printf '=== SAMPLE %d %d.%06d %d\n' "$i" $((start / 1000000)) $((start % 1000000)) $(((end - start) / 1000)) >> "$out"
    cat "$sample_out" >> "$out"
    if ! grep -q '^TID ' "$sample_out" && kill -0 "$pid" 2> /dev/null; then
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
    next=$((next + interval_us))
    end=$(now_us)
    if [ "$next" -gt "$end" ]; then
        sleep "$(awk -v us=$((next - end)) 'BEGIN { printf "%.6f", us / 1000000 }')"
    else
        next=$end
    fi
done
