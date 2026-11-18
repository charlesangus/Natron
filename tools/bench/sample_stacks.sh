#!/usr/bin/env bash
# Wall-clock stack sampler (perf is not available in the container). Samples every thread of
# PID with eu-stack COUNT times, one start every INTERVAL seconds, into OUT. Must run with ptrace rights:
#   docker exec --privileged natron-dev tools/bench/sample_stacks.sh PID COUNT INTERVAL OUT
# Each sample starts with "=== SAMPLE <i> <start epoch> <ms eu-stack took>". A sample that takes
# longer than INTERVAL delays the next one rather than queueing more. SAMPLE_DEBUGINFO_PATH, when
# set, is passed to eu-stack as --debuginfo-path (separate debug files for the libraries).
# SAMPLE_TID_COMM, an extended regex, restricts the samples to the main thread and the threads whose
# /proc/PID/task/<tid>/comm matches it (re-read every sample, so threads started later are caught), each
# walked with eu-stack -1 -p TID. A whole-process eu-stack stops and unwinds every thread, which took
# 0.15-0.3 s a sample on the heavy scene and up to 1.5 s on a slower VM, whatever INTERVAL says (the "ms
# eu-stack took" field); a couple of threads cost a fraction of that. The achieved period is the
# difference of consecutive start epochs, which heavy_scene_stack_window.py prints.
# SAMPLE_TID_COMM='^$' walks the main thread alone, '.' every thread one at a time (each thread's stack
# is taken at a slightly different moment), and unset (with SAMPLE_TID_FRAME unset) is the whole-process
# eu-stack.
# SAMPLE_TID_FRAME, an extended regex, adds the threads with a frame matching it in a whole-process
# sample, taken first and then every SAMPLE_DISCOVER_S seconds (default 10) and kept in OUT like any
# other; a thread found stays walked while it lives. Setting it alone walks the main thread and the
# threads it finds. The scene viewer's render thread is named NatronSceneRndr, so main and render
# threads alone are SAMPLE_TID_COMM='^NatronSceneRndr$'. A build without that name leaves the render thread
# with the process's comm (Natron.bin, as the load and worker threads and TBB's pool have), which no
# SAMPLE_TID_COMM can pick out; its stacks run through HdEngine::Execute, which the USD libraries export:
#   SAMPLE_TID_COMM='^$' SAMPLE_TID_FRAME='HdEngine::Execute'
# SAMPLE_GATE_FILE, when set, is heavy_scene_bench.py's bench_progress: no sample is taken while the
# file is missing or its first line is "idle", that is between scenarios and after the last.
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
tid_frame=${SAMPLE_TID_FRAME:-}
gate=${SAMPLE_GATE_FILE:-}
per_thread=0
[ -n "$tid_comm" ] || [ -n "$tid_frame" ] && per_thread=1
debuginfo=()
[ -n "${SAMPLE_DEBUGINFO_PATH:-}" ] && debuginfo=(--debuginfo-path="$SAMPLE_DEBUGINFO_PATH")
sample_err=$(mktemp)
sample_out=$(mktemp)
trap 'rm -f "$sample_err" "$sample_out"' EXIT
interval_us=$(awk -v s="$interval" 'BEGIN { printf "%d", s * 1000000 }')
discover_us=$(awk -v s="${SAMPLE_DISCOVER_S:-10}" 'BEGIN { printf "%d", s * 1000000 }')
now_us() { local t=${EPOCHREALTIME/./}; echo $((10#$t)); }
gate_open() {
    [ -z "$gate" ] && return 0
    local first=""
    [ -r "$gate" ] && read -r first < "$gate"
    [ -n "$first" ] && [ "$first" != idle ]
}
found_tids=""
next_discovery=0
next=$(now_us)
i=0
while [ "$i" -lt "$count" ]; do
    kill -0 "$pid" 2>/dev/null || break
    if ! gate_open; then
        sleep "$interval"
        next=$(now_us)
        continue
    fi
    start=$(now_us)
    discovery=0
    [ -n "$tid_frame" ] && [ "$start" -ge "$next_discovery" ] && discovery=1
    if [ "$per_thread" = 0 ] || [ "$discovery" = 1 ]; then
        eu-stack -p "$pid" -m -n 80 "${debuginfo[@]}" > "$sample_out" 2> "$sample_err"
        if [ "$discovery" = 1 ]; then
            next_discovery=$((start + discover_us))
            for tid in $(awk -v re="$tid_frame" '/^TID /{ tid = $2; sub(":", "", tid) } /^#/ && tid != "" && $0 ~ re { print tid; tid = "" }' "$sample_out"); do
                [ "$tid" = "$pid" ] && continue
                case " $found_tids " in *" $tid "*) ;; *) found_tids="$found_tids $tid" ;; esac
            done
        fi
    else
        : > "$sample_out"
        : > "$sample_err"
        tids=$pid
        alive=""
        for tid in $found_tids; do
            [ -d /proc/"$pid"/task/"$tid" ] && alive="$alive $tid" && tids="$tids $tid"
        done
        found_tids=$alive
        if [ -n "$tid_comm" ]; then
            for task in /proc/"$pid"/task/*; do
                tid=${task##*/}
                [ "$tid" = "$pid" ] && continue
                case " $found_tids " in *" $tid "*) continue ;; esac
                grep -Eq -- "$tid_comm" "$task/comm" 2> /dev/null && tids="$tids $tid"
            done
        fi
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
    i=$((i + 1))
    next=$((next + interval_us))
    end=$(now_us)
    if [ "$next" -gt "$end" ]; then
        sleep "$(awk -v us=$((next - end)) 'BEGIN { printf "%.6f", us / 1000000 }')"
    else
        next=$end
    fi
done
