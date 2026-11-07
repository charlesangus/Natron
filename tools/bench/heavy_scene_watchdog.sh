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
    printf '{"scenario": "%s", "result": "%s", "reason": "%s", "limit_s": %s, "elapsed_s": %s, %s"time": "%s", %s}\n' \
        "$SCENARIO" "$result" "$(json_escape "$reason")" "$LIMIT" "$elapsed" "$extra" \
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

while kill -0 "$PID" 2>/dev/null; do
    if read_progress && [ "$LIMIT" -gt 0 ] && [ $(($(date +%s) - START)) -ge "$LIMIT" ]; then
        write_outcome timed_out "over the scenario's wall-clock limit" "$(process_fields)"
        end_process
        exit 0
    fi
    sleep 1
done

if read_progress; then
    write_outcome aborted "the process ended during the scenario; see natron.log and results.txt" ""
fi
