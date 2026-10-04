#!/usr/bin/env bash
# Shared by run_matrix.sh and profile_run.sh: thermal cooldown and CPU frequency logging.
# The host clock drops after 10-16 s of full load, so runs are only comparable when they start
# from the same thermal state and their mean clock is known.

bench_cooldown() {
    local max_load=${BENCH_MAX_LOAD:-0.5}
    local timeout=${BENCH_COOLDOWN_TIMEOUT:-600}
    local waited=0 load
    while :; do
        load=$(cut -d' ' -f1 /proc/loadavg)
        if awk -v l="$load" -v m="$max_load" 'BEGIN { exit !(l < m) }'; then
            break
        fi
        if [ "$waited" -ge "$timeout" ]; then
            echo "cooldown: load $load still above $max_load after ${waited}s, continuing"
            break
        fi
        sleep 5
        waited=$((waited + 5))
    done
    local rest=${BENCH_COOLDOWN:-60}
    echo "cooldown: waited ${waited}s for load < $max_load (now $load), sleeping ${rest}s"
    sleep "$rest"
}

# Appends "epoch_seconds khz_cpu0 khz_cpu1 ..." every 0.5 s to $1 until bench_freq_stop.
bench_freq_start() {
    local file=$1
    mkdir -p "$(dirname "$file")"
    : > "$file"
    (
        while :; do
            line=$(date +%s.%N)
            for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
                line="$line $(cat "$f" 2>/dev/null || echo 0)"
            done
            echo "$line" >> "$file"
            sleep 0.5
        done
    ) &
    BENCH_FREQ_PID=$!
}

bench_freq_stop() {
    if [ -n "${BENCH_FREQ_PID:-}" ]; then
        kill "$BENCH_FREQ_PID" 2>/dev/null
        wait "$BENCH_FREQ_PID" 2>/dev/null
        BENCH_FREQ_PID=
    fi
}

# Prints "mean min" kHz over every cpu column of the frequency file, or nothing if it is empty.
bench_freq_stats() {
    python3 - "$1" <<'PY'
import sys
vals = []
for line in open(sys.argv[1]):
    vals += [int(x) for x in line.split()[1:] if int(x) > 0]
if vals:
    print(int(sum(vals) / len(vals)), min(vals))
PY
}

# Rewrites the last line of the jsonl file $1 with cpu_khz_mean and cpu_khz_min from freq file $2.
bench_record_freq() {
    local stats
    stats=$(bench_freq_stats "$2")
    [ -n "$stats" ] && [ -s "$1" ] || return 0
    python3 - "$1" "${stats% *}" "${stats#* }" <<'PY'
import json, sys
path, mean, low = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = open(path).read().splitlines()
rec = json.loads(lines[-1])
rec["cpu_khz_mean"] = mean
rec["cpu_khz_min"] = low
lines[-1] = json.dumps(rec)
open(path, "w").write("\n".join(lines) + "\n")
PY
    echo "cpu clock: mean $(echo "$stats" | cut -d' ' -f1) kHz, min $(echo "$stats" | cut -d' ' -f2) kHz"
}
