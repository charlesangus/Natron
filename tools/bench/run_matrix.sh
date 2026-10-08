#!/usr/bin/env bash
# Runs graph_bench.py over a matrix of topologies, sizes and resolutions, one NatronRenderer
# process per configuration, from the host. Results go to build/bench/results-<tag>.jsonl.
#
#   tools/bench/run_matrix.sh <tag> <res> <frames> <range> <topo:n,n,n> [<topo:n,n> ...]
#   e.g. tools/bench/run_matrix.sh overhead tiny 5 0 chain:0,10,100 wide:10,100
# BENCH_SETTINGS="name=value;name=value" is passed to NatronRenderer as --setting arguments.
# BENCH_KEEP=1 copies each configuration's written frames to build/bench/keep/<tag>/ first.
# BENCH_PLATES_DIR (plates from make_plates.py, default build/bench/fixtures) is forwarded when set.
# Each configuration waits for the load average to drop (BENCH_MAX_LOAD, BENCH_COOLDOWN_TIMEOUT)
# and then sleeps BENCH_COOLDOWN seconds, and its CPU clock is logged to
# build/bench/freq-<tag>-<topo>-<n>.txt and summarised into the result record.
# shellcheck source-path=SCRIPTDIR
set -u
status=0
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
tag=$1
res=$2
frames=$3
range=$4
shift 4
out=$repo/build/bench/results-$tag.jsonl
# Expands BENCH_SETTINGS inside the container so values need no extra quoting layer.
# shellcheck disable=SC2016  # expanded by the shell inside the container
inner='cd "$REPO" && args=(); IFS=";" read -ra kv <<< "${BENCH_SETTINGS:-}"; for s in "${kv[@]}"; do args+=(--setting "$s"); done; timeout "$BENCH_TIMEOUT" xvfb-run --auto-servernum build/release/Renderer/NatronRenderer "${args[@]}" -b tools/bench/graph_bench.py'
logs=$repo/build/bench/logs
mkdir -p "$logs"
# shellcheck source=lib.sh
. "$repo/tools/bench/lib.sh"
for spec in "$@"; do
    topo=${spec%%:*}
    IFS=, read -ra sizes <<< "${spec#*:}"
    for n in "${sizes[@]}"; do
        log=$logs/$tag-$topo-$n-$res.log
        freq=$repo/build/bench/freq-$tag-$topo-$n.txt
        bench_cooldown
        before=$(wc -l < "$out" 2>/dev/null || echo 0)
        start=$(date +%s)
        bench_freq_start "$freq"
        docker exec -e BENCH_TOPO="$topo" -e BENCH_N="$n" -e BENCH_RES="$res" \
            -e BENCH_FRAMES="$frames" -e BENCH_RANGE="$range" -e BENCH_OUT="$out" -e BENCH_NAMED="${BENCH_NAMED:-1}" \
            -e BENCH_SETTINGS="${BENCH_SETTINGS:-}" -e BENCH_TIMEOUT="${BENCH_TIMEOUT:-1800}" -e REPO="$repo" \
            -e OMP_WAIT_POLICY -e GOMP_SPINCOUNT -e OMP_THREAD_LIMIT -e OMP_DISPLAY_ENV -e BENCH_RENDER_STATS -e BENCH_PLATES_DIR \
            -e OFX_PLUGIN_PATH="$repo"/build/assets/Plugins natron-dev bash -lc "$inner" \
            > "$log" 2>&1
        code=$?
        bench_freq_stop
        if [ "$(wc -l < "$out" 2>/dev/null || echo 0)" -gt "$before" ]; then
            bench_record_freq "$out" "$freq"
        fi
        if [ "$code" -ne 0 ]; then status=1; fi
        if [ -n "${BENCH_KEEP:-}" ]; then
            mkdir -p "$repo/build/bench/keep/$tag"
            cp "$repo"/build/bench/work/*.exr "$repo/build/bench/keep/$tag/" 2>/dev/null
        fi
        rm -f "$repo"/build/bench/work/*.exr
        echo "$tag $topo n=$n res=$res exit=$code $(( $(date +%s) - start ))s"
    done
done
exit "$status"
