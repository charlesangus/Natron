#!/usr/bin/env bash
# Renders FIRST-LAST of each comp with NATRON_RENDER_PROFILE on, then writes <comp>.steady.jsonl
# without the first rendered frame's records (cold caches and plugin loading). The profile's "frame"
# field numbers the renders from 0; "time" is not a usable key because time-offset nodes request other times.
#
#   [TAG=serial RENDERER_ARGS="--setting noRenderThreads=1"] tools/bench/comps/run_comps.sh [FIRST LAST [comp ...]]
#
# TAG is appended to the output names (<comp>-<TAG>.steady.jsonl) and RENDERER_ARGS is passed to
# NatronRenderer; a one-thread run gives each node's cost without contention from its siblings.
#
# Waits for other benchmarks on the host to finish first. Output lands in build/bench/m82-profile/.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
FIRST=${1:-1}
LAST=${2:-6}
shift 2 2>/dev/null || true
COMPS=${*:-keying_grade cg_multipass defocus_retime}
OUT="$REPO/build/bench/m82-profile"
TAG=${TAG:+-$TAG}
RENDERER_ARGS=${RENDERER_ARGS:-}
mkdir -p "$OUT/out"

busy() {
    pgrep -af 'graph_bench|NatronRenderer|GpuBench' | grep -v -e pgrep -e run_comps -e 'shell-snapshots' -e 'docker exec' -e 'bash -lc' || true
}

for comp in $COMPS; do
    while [ -n "$(busy)" ]; do
        echo "host busy, waiting: $(busy | head -1 | cut -c1-120)"
        sleep 60
    done
    echo "host load before $comp$TAG: $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/host.log"
    rm -f "$OUT/$comp$TAG.jsonl".* "$OUT/out/$comp".*.exr
    start=$(date +%s.%N)
    docker exec -e REPO="$REPO" -e COMP="$comp" -e FIRST="$FIRST" -e LAST="$LAST" -e RENDERER_ARGS="$RENDERER_ARGS" \
        -e NATRON_RENDER_PROFILE="$OUT/$comp$TAG.jsonl" -e OFX_PLUGIN_PATH="$REPO/build/assets/Plugins" natron-dev bash -lc \
        'cd "$REPO" && xvfb-run --auto-servernum build/release/Renderer/NatronRenderer $RENDERER_ARGS -w Write $FIRST-$LAST tools/bench/comps/$COMP.ntp' \
        > "$OUT/$comp$TAG.log" 2>&1
    end=$(date +%s.%N)
    echo "$comp$TAG frames $FIRST-$LAST wall $(python3 -c "print(round($end - $start, 1))") s; load after $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/host.log"
    python3 - "$OUT/$comp$TAG" <<'PY'
import glob, json, sys
base = sys.argv[1]
src = glob.glob(base + ".jsonl.*")[0]
with open(src) as f, open(base + ".steady.jsonl", "w") as out:
    lines = [line for line in f if line.strip()]
    first = min(json.loads(line)["frame"] for line in lines)
    out.writelines(line for line in lines if json.loads(line)["frame"] != first)
PY
    rm -f "$OUT/out/$comp".*.exr
done
