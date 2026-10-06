#!/usr/bin/env bash
# Wall-clock stack sampler (perf is not available in the container). Samples every thread of
# PID with eu-stack COUNT times, INTERVAL seconds apart, into OUT. Must run with ptrace rights:
#   docker exec --privileged natron-dev tools/bench/sample_stacks.sh PID COUNT INTERVAL OUT
set -u
pid=$1
count=$2
interval=$3
out=$4
: > "$out"
for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    echo "=== SAMPLE $i $(date +%s.%N)" >> "$out"
    eu-stack -p "$pid" -m -n 80 >> "$out" 2>/dev/null
    sleep "$interval"
done
