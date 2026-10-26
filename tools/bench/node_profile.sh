#!/usr/bin/env bash
# Profiles single native nodes with tools/bench/node_profile.py and writes build/bench/m85-nodes/<tag>-<node>.steady.jsonl,
# the profile without its first (cold) frame.
#
#   tools/bench/node_profile.sh gpu    CHAIN node...   debug build, GPU on, the NATRON_GPU=auto placement
#   tools/bench/node_profile.sh cpu    CHAIN node...   release build, GPU off, all threads
#   tools/bench/node_profile.sh serial CHAIN node...   release build, GPU off, noRenderThreads=1
#
# The GPU tag is gpu<CHAIN>; a CPU run's tag is its mode. Summarise with node_profile_summary.py.
# A gpu run needs CHAIN 2 or more (see node_profile.py), and fails when any profiled node rendered on the CPU,
# since its profile would then hold CPU times under a GPU tag.
#
# NODE_PROFILE_BINARY=<path to a NatronRenderer> (cpu and serial modes only) runs that binary directly on the
# host under xvfb-run instead of build/release/Renderer/NatronRenderer in the dev container, and leaves
# OFX_PLUGIN_PATH alone so the plugins next to the binary are used. For an extracted release AppImage:
#   NODE_PROFILE_BINARY=build/release-appimage/squashfs-root/bin/NatronRenderer tools/bench/node_profile.sh serial 1 iblur
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
        args=
        if [ "$mode" = serial ]; then args="--setting noRenderThreads=1"; fi
        tag="$mode"
        ;;
    *)
        echo "unknown mode $mode" >&2
        exit 2
        ;;
esac

external=${NODE_PROFILE_BINARY:-}
if [ -n "$external" ] && [ "$mode" = gpu ]; then
    echo "NODE_PROFILE_BINARY is for cpu and serial modes only" >&2
    exit 2
fi
if [ -n "$external" ]; then external=$(realpath "$external"); fi

status=0
for node in "$@"; do
    base="$out/$tag-$node"
    rm -f "$base.jsonl".* "$base.steady.jsonl"
    if [ -n "$external" ]; then
        (cd "$repo" && env -u LD_LIBRARY_PATH NP_NODE="$node" NP_CHAIN="$chain" NP_WORK=/tmp/np-$tag \
            NATRON_GPU=$gpu NATRON_RENDER_PROFILE="$base.jsonl" \
            xvfb-run -a "$external" $args -b tools/bench/node_profile.py) > "$base.log" 2>&1 || { echo "$node: render failed, see $base.log" >&2; status=1; continue; }
    else
        "$repo/tools/ci/local/devshell.sh" "${shell[@]}" bash -lc "cd '$repo' && NP_NODE='$node' NP_CHAIN='$chain' NP_WORK=/tmp/np-$tag \
            NATRON_GPU=$gpu NATRON_RENDER_PROFILE='$base.jsonl' OFX_PLUGIN_PATH='$repo/build/assets/Plugins' \
            xvfb-run -a $binary $args -b tools/bench/node_profile.py" > "$base.log" 2>&1 || { echo "$node: render failed, see $base.log" >&2; status=1; continue; }
    fi
    python3 - "$base" "$mode" "$node" <<'PY' || status=1
import glob, json, sys
base, mode, node = sys.argv[1:4]
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
if mode == "gpu":
    prefix = "np_%s_" % node
    tasks = [r for r in recs if r["frame"] in keep and r.get("node", "").startswith(prefix)]
    on_cpu = sorted({r["node"] for r in tasks if r.get("device") != "gpu"})
    regions = [r for r in recs if r["frame"] in keep and r.get("kind") == "region"]
    if not tasks or on_cpu or not regions:
        print("%s: NOT PROFILED ON THE GPU: %d task(s), on the CPU: %s, %d region record(s)"
              % (node, len(tasks), " ".join(on_cpu) or "none", len(regions)), file=sys.stderr)
        sys.exit(1)
PY
    rm -f "$base.jsonl".*
    echo "$node: $base.steady.jsonl"
done
exit $status
