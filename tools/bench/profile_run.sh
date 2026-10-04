#!/usr/bin/env bash
# Starts one graph_bench.py configuration in the container and samples its stacks once the graph
# is built.
#   tools/bench/profile_run.sh <name> <samples> <interval> VAR=value ...   (BENCH_* variables)
set -u
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$1
count=$2
interval=$3
shift 3
envs=()
for kv in "$@"; do envs+=(-e "$kv"); done
log=$repo/build/bench/logs/profile-$name.log
docker exec "${envs[@]}" -e OFX_PLUGIN_PATH="$repo"/build/assets/Plugins natron-dev bash -lc \
    "cd $repo && timeout 3600 xvfb-run -a build/release/Renderer/NatronRenderer -b tools/bench/graph_bench.py" > "$log" 2>&1 &
until grep -q "\[bench\] built" "$log" 2>/dev/null; do
    grep -q "Traceback\|RESULT" "$log" 2>/dev/null && break
    sleep 1
done
pid=$(docker exec natron-dev bash -lc 'pgrep -x NatronRenderer' 2>/dev/null | grep -v '^id' | head -1)
sleep 2
docker exec -u root --privileged natron-dev "$repo"/tools/bench/sample_stacks.sh "$pid" "$count" "$interval" \
    "$repo"/build/bench/samples-"$name".txt
wait
grep "RESULT" "$log" | cut -c1-400
