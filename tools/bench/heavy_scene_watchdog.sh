#!/usr/bin/env bash
# Usage: heavy_scene_watchdog.sh <pid> <progress-file> <results.jsonl>
#
# Started by heavy_scene_bench.py. Enforces each scenario's wall-clock limit from outside Natron and
# leaves a record for a scenario that never reported: a main thread stuck in C++ services no timer,
# and it can hold the GIL, so nothing inside the process can be trusted to write.
#
# The bench replaces the progress file atomically. Its first line is "idle", or for a running
# scenario these four lines:
#   <start, seconds since the epoch> <limit in seconds, 0 for none>
#   <scenario>
#   <the scenarios after it, space separated>
#   <fields common to every record, as a JSON object body with no braces>
#
# On expiry: a timed_out record, a skipped record per remaining scenario, then SIGTERM and SIGKILL.
# If the process ends on its own during a scenario, the same with an aborted record.
#
# Memory guard, polled once a second. <pid> is the Natron process that runs the bench script; the
# guard counts it and every descendant. It sends SIGKILL at once (a swapping process will not get to
# a SIGTERM handler) and writes an out_of_memory record, then skipped records, when any of these hold:
#   MemAvailable is under the floor      BENCH_MEM_FLOOR_GB, default the larger of 4 and 10% of MemTotal
#   the tree's RSS is over the limit     BENCH_MEM_LIMIT_GB, default MemTotal minus the floor
#   swap in use grew by more than        BENCH_SWAP_GROWTH_GB, default 2, since the scenario began
# While a scenario runs, <progress-file>.mem holds "<start> <lowest MemAvailable kB> <peak tree RSS kB>"
# for the bench to put on the scenario's record. The tree's RSS counts a page shared between processes
# once per process, so it can overstate a tree of several processes.
#
# This cannot catch a GPU driver hang or reset, which stalls the machine without using RAM.
# After a reboot: journalctl -b -1 -k | grep -iE 'out of memory|oom|amdgpu|gpu reset|hung'
set -u

if [ $# -ne 3 ]; then
    echo "usage: $0 <pid> <progress-file> <results.jsonl>" >&2
    exit 2
fi
PID="$1"
PROGRESS="$2"
RESULTS="$3"
# Natron's SIGTERM handler only asks the main thread to quit, so a hung process needs the SIGKILL.
KILL_GRACE_S="${BENCH_KILL_GRACE_S:-15}"
STATS="$PROGRESS.mem"

gb_to_kb() {
    awk -v gb="$1" 'BEGIN { printf "%d", gb * 1048576 }'
}

kb_to_gb() {
    printf '%d.%02d' $(($1 / 1048576)) $((($1 % 1048576) * 100 / 1048576))
}

MEM_TOTAL_KB=0 MEM_AVAIL_KB=0 SWAP_USED_KB=0
read_meminfo() {
    local name value rest swap_total=0 swap_free=0
    while read -r name value rest; do
        case "$name" in
            MemTotal:) MEM_TOTAL_KB=$value ;;
            MemAvailable:) MEM_AVAIL_KB=$value ;;
            SwapTotal:) swap_total=$value ;;
            SwapFree:) swap_free=$value ;;
        esac
    done < /proc/meminfo
    SWAP_USED_KB=$((swap_total - swap_free))
}

read_meminfo
if [ -n "${BENCH_MEM_FLOOR_GB:-}" ]; then
    FLOOR_KB="$(gb_to_kb "$BENCH_MEM_FLOOR_GB")"
else
    FLOOR_KB=$((MEM_TOTAL_KB / 10))
    [ "$FLOOR_KB" -lt 4194304 ] && FLOOR_KB=4194304
fi
if [ -n "${BENCH_MEM_LIMIT_GB:-}" ]; then
    LIMIT_KB="$(gb_to_kb "$BENCH_MEM_LIMIT_GB")"
else
    LIMIT_KB=$((MEM_TOTAL_KB - FLOOR_KB))
fi
SWAP_GROWTH_KB="$(gb_to_kb "${BENCH_SWAP_GROWTH_GB:-2}")"

TREE_KB=0 TREE_PIDS="$PID"
read_tree() {
    local sample
    sample="$(ps -e -o pid=,ppid=,rss= 2>/dev/null | awk -v root="$PID" '
        { parent[$1] = $2; rss[$1] = $3; ids[NR] = $1 }
        END {
            member[root] = 1
            do {
                grew = 0
                for (i = 1; i <= NR; i++) {
                    p = ids[i]
                    if (!(p in member) && (parent[p] in member)) { member[p] = 1; grew = 1 }
                }
            } while (grew)
            for (p in member) if (p in rss) { sum += rss[p]; list = list " " p }
            print sum + 0 list
        }')"
    [ -n "$sample" ] && read -r TREE_KB TREE_PIDS <<< "$sample"
}

# Read again just before the kill: a child started since the last poll would otherwise be orphaned
# to init and outlive its parent.
kill_tree() {
    read_tree
    # shellcheck disable=SC2086
    kill -KILL "$PID" $TREE_PIDS 2>/dev/null
}

read_progress() {
    START="" LIMIT="" SCENARIO="" REMAINING="" COMMON=""
    [ -f "$PROGRESS" ] || return 1
    {
        read -r START LIMIT
        read -r SCENARIO
        read -r REMAINING
        read -r COMMON
    } < "$PROGRESS"
    [ "$START" != "idle" ] && [ -n "$SCENARIO" ]
}

json_escape() {
    local text="${1//\\/\\\\}"
    printf '%s' "${text//\"/\\\"}"
}

write_outcome() {
    local result="$1" reason="$2" extra="$3" now elapsed next
    now="$(date +%s)"
    elapsed=$((now - START))
    [ -n "$COMMON" ] || COMMON="\"label\": \"$(json_escape "${BENCH_LABEL:-}")\""
    printf '{"scenario": "%s", "result": "%s", "reason": "%s", "limit_s": %s, "elapsed_s": %s, %s"time": "%s", %s}\n' \
        "$SCENARIO" "$result" "$(json_escape "$reason")" "${LIMIT:-0}" "$elapsed" "$extra" \
        "$(date +%Y-%m-%dT%H:%M:%S)" "$COMMON" >> "$RESULTS"
    for next in $REMAINING; do
        printf '{"scenario": "%s", "result": "skipped", "reason": "%s", "time": "%s", %s}\n' \
            "$next" "$(json_escape "the process ended in $SCENARIO ($result)")" \
            "$(date +%Y-%m-%dT%H:%M:%S)" "$COMMON" >> "$RESULTS"
    done
}

# What the main thread was doing separates a busy hang from a blocked one, and the peak says whether
# memory is the cause; both are gone once the process is.
process_fields() {
    local state="" peak="" name value rest
    if [ -r "/proc/$PID/stat" ]; then
        read -r _ _ state _ < "/proc/$PID/stat"
    fi
    if [ -r "/proc/$PID/status" ]; then
        while read -r name value rest; do
            if [ "$name" = "VmHWM:" ]; then
                peak=$((value / 1024))
            fi
        done < "/proc/$PID/status"
    fi
    printf '"main_thread_state": "%s", "peak_rss_mb": %s, ' "$state" "${peak:-null}"
}

end_process() {
    local waited=0
    kill -TERM "$PID" 2>/dev/null
    while kill -0 "$PID" 2>/dev/null && [ "$waited" -lt "$KILL_GRACE_S" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    kill -KILL "$PID" 2>/dev/null
}

memory_fields() {
    local growth_kb=$((SWAP_USED_KB - SWAP_BASE_KB))
    [ "$growth_kb" -lt 0 ] && growth_kb=0
    printf '"peak_rss_gb": %s, "mem_available_gb": %s, "mem_available_min_gb": %s, "swap_used_gb": %s, "swap_growth_gb": %s, "mem_floor_gb": %s, "rss_limit_gb": %s, ' \
        "$(kb_to_gb "$PEAK_KB")" "$(kb_to_gb "$MEM_AVAIL_KB")" "$(kb_to_gb "$MIN_AVAIL_KB")" \
        "$(kb_to_gb "$SWAP_USED_KB")" "$(kb_to_gb "$growth_kb")" \
        "$(kb_to_gb "$FLOOR_KB")" "$(kb_to_gb "$LIMIT_KB")"
}

SCENARIO_KEY="" SWAP_BASE_KB=$SWAP_USED_KB MIN_AVAIL_KB=$MEM_AVAIL_KB PEAK_KB=0
WATCH_START="$(date +%s)"
while kill -0 "$PID" 2>/dev/null; do
    read_meminfo
    read_tree
    running=0
    read_progress && running=1
    if [ "$running" -eq 1 ]; then
        key="$START:$SCENARIO"
    else
        key="idle"
    fi
    if [ "$key" != "$SCENARIO_KEY" ]; then
        SCENARIO_KEY="$key"
        SWAP_BASE_KB=$SWAP_USED_KB MIN_AVAIL_KB=$MEM_AVAIL_KB PEAK_KB=0
    fi
    [ "$MEM_AVAIL_KB" -lt "$MIN_AVAIL_KB" ] && MIN_AVAIL_KB=$MEM_AVAIL_KB
    [ "$TREE_KB" -gt "$PEAK_KB" ] && PEAK_KB=$TREE_KB
    [ "$running" -eq 1 ] && printf '%s %s %s\n' "$START" "$MIN_AVAIL_KB" "$PEAK_KB" > "$STATS"

    reason=""
    if [ "$MEM_AVAIL_KB" -lt "$FLOOR_KB" ]; then
        reason="MemAvailable $(kb_to_gb "$MEM_AVAIL_KB") GB fell under the $(kb_to_gb "$FLOOR_KB") GB floor"
    elif [ "$TREE_KB" -gt "$LIMIT_KB" ]; then
        reason="the process tree's RSS $(kb_to_gb "$TREE_KB") GB passed the $(kb_to_gb "$LIMIT_KB") GB limit"
    elif [ $((SWAP_USED_KB - SWAP_BASE_KB)) -gt "$SWAP_GROWTH_KB" ]; then
        reason="swap in use grew by $(kb_to_gb $((SWAP_USED_KB - SWAP_BASE_KB))) GB, over the $(kb_to_gb "$SWAP_GROWTH_KB") GB allowed"
    fi
    if [ -n "$reason" ]; then
        kill_tree
        if [ "$running" -eq 0 ]; then
            START="$WATCH_START" SCENARIO="*" REMAINING="" COMMON="" LIMIT=0
        fi
        write_outcome out_of_memory "$reason" "$(memory_fields)"
        exit 0
    fi

    if [ "$running" -eq 1 ] && [ "$LIMIT" -gt 0 ] && [ $(($(date +%s) - START)) -ge "$LIMIT" ]; then
        write_outcome timed_out "over the scenario's wall-clock limit" "$(process_fields)"
        end_process
        exit 0
    fi
    sleep 1
done

if read_progress; then
    write_outcome aborted "the process ended during the scenario; see natron.log and results.txt" ""
fi
