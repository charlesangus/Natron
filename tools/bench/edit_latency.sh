#!/usr/bin/env bash
# Edit-latency matrix: runs tools/bench/edit_latency_bench.py once per configuration under
# Tests/gui/run-gui-test.sh and collects one JSON line per edit.
#
#   tools/bench/edit_latency.sh <Natron.AppImage> [out-dir]
#
# Environment (each a space-separated list; the defaults give the full matrix):
#   BENCH_CASES       grade hsv       the edited node: Grade, or HSVKeyer after the same RAW chain
#   BENCH_ZOOMS       fit 100         viewer zoom while editing
#   BENCH_PANELS      closed open     the edited node's settings panel
#   BENCH_GPUS        off auto        NATRON_GPU
#   BENCH_AUTOPROXYS  off on          the autoProxyScrubbing preference
#   BENCH_EDITS       edits per configuration (default 20); BENCH_WARMUP untimed edits before them (default 3)
#   BENCH_RAW_FILE    the raw file (default Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2)
#   BENCH_LABEL       free text stored in every line
#   GUI_TEST_TIMEOUT  seconds allowed per configuration (default 1800)
#   GUI_TEST_HOST_DISPLAY  e.g. :0 to run against the host's display; without it the run uses Xvfb with
#                          software GL, where NATRON_GPU=auto has no GPU to use
#
# Results: <out-dir>/edit_latency.jsonl (default out-dir build/bench/edit-latency), plus each
# configuration's render profile, results.txt and natron.log under <out-dir>/<node>-<zoom>-panel-<panel>-gpu-<gpu>-proxy-<proxy>/.
# The AppImage is extracted afresh for every configuration.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [out-dir]" >&2; exit 2; }
[ -f "$APPIMAGE" ] || { echo "no such AppImage: $APPIMAGE" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
APPIMAGE=$(realpath "$APPIMAGE")
OUT=$(realpath -m "${2:-$REPO/build/bench/edit-latency}")
mkdir -p "$OUT"

failed=0
for node in ${BENCH_CASES:-grade hsv}; do
    for zoom in ${BENCH_ZOOMS:-fit 100}; do
        for panel in ${BENCH_PANELS:-closed open}; do
            for gpu in ${BENCH_GPUS:-off auto}; do
                for proxy in ${BENCH_AUTOPROXYS:-off on}; do
                    tag="$node-$zoom-panel-$panel-gpu-$gpu-proxy-$proxy"
                    dir="$OUT/$tag"
                    mkdir -p "$dir"
                    rm -f "$dir"/render.prof.*
                    echo "== $tag"
                    env NATRON_GPU="$gpu" NATRON_RENDER_SCHEDULER=taskgraph \
                        NATRON_RENDER_PROFILE="$dir/render.prof" NATRON_GUI_TEST_OUT="$dir" \
                        BENCH_CASE="$node" BENCH_ZOOM="$zoom" BENCH_PANEL="$panel" BENCH_AUTOPROXY="$proxy" \
                        BENCH_RESULT="$OUT/edit_latency.jsonl" GUI_TEST_TIMEOUT="${GUI_TEST_TIMEOUT:-1800}" \
                        "$REPO/Tests/gui/run-gui-test.sh" "$REPO/tools/bench/edit_latency_bench.py" "$APPIMAGE" \
                        > "$dir/run.log" 2>&1
                    code=$?
                    if [ "$code" -ne 0 ]; then
                        echo "$tag exited with $code, see $dir/run.log" >&2
                        failed=$((failed + 1))
                    fi
                done
            done
        done
    done
done

python3 -I - "$OUT/edit_latency.jsonl" <<'PY'
import collections
import json
import os
import sys

if not os.path.exists(sys.argv[1]):
    sys.exit("no results")
groups = collections.OrderedDict()
for line in open(sys.argv[1]):
    if line.strip():
        r = json.loads(line)
        key = (r["case"], r["zoom"], r["panel"], r["gpu"], r["autoproxy"])
        groups.setdefault(key, []).append(r)
print("%-6s %-5s %-7s %-5s %-6s %5s %10s %10s %12s" % ("case", "zoom", "panel", "gpu", "proxy", "edits", "median_ms", "max_ms", "upstream_ex"))
for key, rows in groups.items():
    lat = sorted(r["latency_ms"] for r in rows if r.get("latency_ms") is not None)
    up = sorted(r.get("upstream_executed", 0) for r in rows)
    print("%-6s %-5s %-7s %-5s %-6s %5d %10s %10s %12s" % (key + (len(rows), lat[len(lat) // 2] if lat else "-", lat[-1] if lat else "-", up[len(up) // 2])))
PY
python3 -I "$REPO/tools/bench/edit_latency_bench.py" --classify "$OUT" || failed=$((failed + 1))
exit $((failed > 0))
