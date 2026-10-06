#!/usr/bin/env bash
# Counts running (R) and uninterruptible (D) threads of PID every INTERVAL seconds, COUNT times,
# without stopping the process. Prints a histogram of running-thread counts.
pid=$1; count=$2; interval=$3
for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    awk '{ sub(/.*\) /, ""); s[substr($0, 1, 1)]++ } END { printf "%d %d\n", s["R"] + 0, s["D"] + 0 }' /proc/"$pid"/task/*/stat 2>/dev/null
    sleep "$interval"
done | sort | uniq -c | sort -k2n
