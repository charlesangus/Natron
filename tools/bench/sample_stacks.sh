#!/usr/bin/env bash
# Wall-clock stack sampler (perf is not available in the container). Samples every thread of
# PID with eu-stack COUNT times, one start every INTERVAL seconds, into OUT. Must run with ptrace rights:
#   docker exec --privileged natron-dev tools/bench/sample_stacks.sh PID COUNT INTERVAL OUT
# Each sample starts with "=== SAMPLE <i> <start epoch> <ms eu-stack took>". A sample that takes
# longer than INTERVAL delays the next one rather than queueing more. SAMPLE_DEBUGINFO_PATH, when
# set, is passed to eu-stack as --debuginfo-path (separate debug files for the libraries).
# SAMPLE_TID_COMM, an extended regex, restricts the samples to the main thread and the threads whose
# /proc/PID/task/<tid>/comm matches it (re-read every sample, so threads started later are caught), each
# walked with eu-stack -1 -p TID. A whole-process eu-stack stops and unwinds every thread, which costs
# around 0.15 s a sample on the heavy scene whatever INTERVAL says (the "ms eu-stack took" field); a
# couple of threads cost a fraction of that, which is what makes 20 Hz (INTERVAL 0.05) reachable. The
# achieved period is the difference of consecutive start epochs, which heavy_scene_stack_window.py prints.
# SAMPLE_TID_COMM='^$' walks the main thread alone, '.' every thread one at a time (each thread's stack
# is taken at a slightly different moment), and unset is the whole-process eu-stack.
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
tid_comm=${SAMPLE_TID_COMM:-}
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
    if [ -z "$tid_comm" ]; then
        eu-stack -p "$pid" -m -n 80 "${debuginfo[@]}" > "$sample_out" 2> "$sample_err"
    else
        : > "$sample_out"
        : > "$sample_err"
        tids=$pid
        for task in /proc/"$pid"/task/*; do
            tid=${task##*/}
            [ "$tid" = "$pid" ] && continue
            grep -Eq -- "$tid_comm" "$task/comm" 2> /dev/null && tids="$tids $tid"
        done
        for tid in $tids; do
            # Only the frame lines are kept: the header eu-stack prints for a single thread varies, so
            # the "TID n:" line the readers expect is written here.
            frames=$(eu-stack -1 -p "$tid" -m -n 80 "${debuginfo[@]}" 2>> "$sample_err" | grep '^#')
            [ -n "$frames" ] && printf 'TID %s:\n%s\n' "$tid" "$frames" >> "$sample_out"
        done
    fi
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
