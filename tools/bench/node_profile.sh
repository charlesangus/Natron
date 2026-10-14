#!/usr/bin/env bash
# Profiles single native nodes with tools/bench/node_profile.py and writes build/bench/m85-nodes/<tag>-<node>.steady.jsonl,
# the profile without its first (cold) frame.
#
#   tools/bench/node_profile.sh gpu    CHAIN node...   debug build, GPU on, the NATRON_GPU=auto placement
#   tools/bench/node_profile.sh cpu    CHAIN node...   release build, GPU off, all threads
#   tools/bench/node_profile.sh serial CHAIN node...   release build, GPU off, noRenderThreads=1
#
# The GPU tag is gpu<CHAIN>; a CPU run's tag is its mode. Summarise with node_profile_summary.py.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
mode=$1
chain=$2
shift 2
out="$repo/build/bench/m85-nodes"
mkdir -p "$out"

case "$mode" in
    gpu)
        shell=(--gpu)
        binary=build/debug/Renderer/NatronRenderer
        gpu=auto
        args=""
        tag="gpu$chain"
        ;;
    cpu | serial)
        shell=()
        binary=build/release/Renderer/NatronRenderer
        gpu=off
        args=$([ "$mode" = serial ] && echo "--setting noRenderThreads=1" || true)
        tag="$mode"
        ;;
    *)
        echo "unknown mode $mode" >&2
        exit 2
        ;;
esac

for node in "$@"; do
    base="$out/$tag-$node"
    rm -f "$base.jsonl".* "$base.steady.jsonl"
    "$repo/tools/ci/local/devshell.sh" "${shell[@]}" bash -lc "cd '$repo' && NP_NODE='$node' NP_CHAIN='$chain' NP_WORK=/tmp/np-$tag \
        NATRON_GPU=$gpu NATRON_RENDER_PROFILE='$base.jsonl' OFX_PLUGIN_PATH='$repo/build/assets/Plugins' \
        xvfb-run -a $binary $args -b tools/bench/node_profile.py" > "$base.log" 2>&1 || { echo "$node: render failed, see $base.log" >&2; continue; }
    python3 - "$base" <<'PY'
import glob, json, sys
base = sys.argv[1]
src = glob.glob(base + ".jsonl.*")[0]
with open(src) as f:
    recs = [json.loads(line) for line in f if line.strip()]
# NatronRenderer renders the project's range once more after the script, from warm caches; only the
# first sequence rendered at each time is a real render, and the very first of those is cold.
time_of = {r["frame"]: r["time"] for r in recs if "time" in r}
keep = []
seen = set()
for frame in sorted(time_of):
    if time_of[frame] not in seen:
        seen.add(time_of[frame])
        keep.append(frame)
keep = set(keep[1:])
with open(base + ".steady.jsonl", "w") as out:
    out.writelines(json.dumps(r, separators=(",", ":")) + "\n" for r in recs if r["frame"] in keep)
PY
    rm -f "$base.jsonl".*
    echo "$node: $base.steady.jsonl"
done
