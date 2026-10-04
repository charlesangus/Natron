#!/usr/bin/env bash
# Starts one graph_bench.py configuration in the container and samples its stacks once the graph
# is built.
#   tools/bench/profile_run.sh <name> <samples> <interval> VAR=value ...   (BENCH_* variables)
# BENCH_SETTINGS="name=value;name=value" becomes --setting arguments. With SAMPLER=states the
# thread-state sampler runs instead of eu-stack and writes build/bench/states-<name>.txt.
set -u
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$1
count=$2
interval=$3
shift 3
envs=()
for kv in "$@"; do envs+=(-e "$kv"); done
# shellcheck disable=SC2016  # expanded by the shell inside the container
inner='cd "$REPO" && args=(); IFS=";" read -ra kv <<< "${BENCH_SETTINGS:-}"; for s in "${kv[@]}"; do args+=(--setting "$s"); done; timeout 3600 xvfb-run -a build/release/Renderer/NatronRenderer "${args[@]}" -b tools/bench/graph_bench.py'
log=$repo/build/bench/logs/profile-$name.log
docker exec "${envs[@]}" -e REPO="$repo" -e OFX_PLUGIN_PATH="$repo"/build/assets/Plugins natron-dev bash -lc "$inner" > "$log" 2>&1 &
bg=$!
waited=0
until grep -q "\[bench\] built" "$log" 2>/dev/null; do
    grep -q "Traceback\|RESULT" "$log" 2>/dev/null && break
    if ! kill -0 "$bg" 2>/dev/null; then
        echo "profile_run: benchmark exited before the graph was built; see $log" >&2
        exit 1
    fi
    if [ "$waited" -ge 600 ]; then
        echo "profile_run: graph not built after 600s; see $log" >&2
        kill "$bg" 2>/dev/null
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done
pid=$(docker exec natron-dev bash -lc 'pgrep -x NatronRenderer' 2>/dev/null | grep -v '^id' | head -1)
sleep 2
if [ "${SAMPLER:-stacks}" = states ]; then
    docker exec natron-dev "$repo"/tools/bench/sample_states.sh "$pid" "$count" "$interval" \
        > "$repo"/build/bench/states-"$name".txt
else
    docker exec -u root --privileged natron-dev "$repo"/tools/bench/sample_stacks.sh "$pid" "$count" "$interval" \
        "$repo"/build/bench/samples-"$name".txt
fi
wait
grep "RESULT" "$log" | cut -c1-400
