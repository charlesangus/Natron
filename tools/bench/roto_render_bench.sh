#!/usr/bin/env bash
# Roto render throughput over a matrix of scenarios, one NatronRenderer run per scenario
# (tools/bench/roto_render_bench.py). Run it on a quiet machine.
#
#   tools/bench/roto_render_bench.sh <Natron.AppImage> [out-dir]
#
# Two runs diff with tools/bench/compare.py <out-a>/results.jsonl <out-b>/results.jsonl.
#
# Environment:
#   BENCH_ROTO_NODES     node kinds of the shape scenarios (default "roto rotopaint")
#   BENCH_ROTO_COUNTS    shape counts (default "1 30 100 300")
#   BENCH_ROTO_FEATHERS  feather distances in pixels (default "0 20")
#   BENCH_ROTO_MOTIONS   still and/or keyed (default "still keyed")
#   BENCH_ROTO_RES       2k and/or uhd (default "2k uhd")
#   BENCH_ROTO_STROKES   stroke counts of the stroke scenarios (default "50 300", empty for none); the
#                        projects come from tools/bench/gui/make_roto_strokes.py and a missing one is skipped
#   BENCH_FRAMES         frames per scenario (default 12)
#   BENCH_THREADS        passed as --setting noRenderThreads=N when set
#   BENCH_LABEL          free text stored with every record
#   BENCH_ROTO_PROFILE   1 to write a per-stage profile per scenario to <out-dir>/profile/<scenario>.jsonl
#                        (NATRON_ROTO_RENDER_PROFILE); profiled times do not compare with unprofiled ones
#   BENCH_PLATES_DIR, BENCH_ROTO_STROKES_DIR  as in roto_render_bench.py
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside.
# Results land in <out-dir>/results.jsonl, one log per scenario in <out-dir>/logs.
set -uo pipefail

APPIMAGE=${1:-}
if [ -z "$APPIMAGE" ]; then
    echo "usage: $0 <Natron.AppImage> [out-dir]" >&2
    exit 2
fi
APPIMAGE=$(realpath "$APPIMAGE")
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${2:-$REPO/build/bench/roto-render}")
NODES=${BENCH_ROTO_NODES:-roto rotopaint}
COUNTS=${BENCH_ROTO_COUNTS:-1 30 100 300}
FEATHERS=${BENCH_ROTO_FEATHERS:-0 20}
MOTIONS=${BENCH_ROTO_MOTIONS:-still keyed}
RESOLUTIONS=${BENCH_ROTO_RES:-2k uhd}
STROKES=${BENCH_ROTO_STROKES-50 300}
STROKES_DIR=${BENCH_ROTO_STROKES_DIR:-$REPO/build/bench/fixtures}
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"
SETTING_ARGV=()
SETTINGS=""
if [ -n "${BENCH_THREADS:-}" ]; then
    SETTING_ARGV=(--setting "noRenderThreads=$BENCH_THREADS")
    SETTINGS="noRenderThreads=$BENCH_THREADS"
fi

mkdir -p "$OUT/logs" "$OUT/render" "$OUT/profile"
: > "$OUT/results.jsonl"

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

# run <name> VAR=value...: one renderer process with the scenario in its environment.
run() {
    local name=$1 log code profile=()
    shift
    log="$OUT/logs/$name.log"
    if [ "${BENCH_ROTO_PROFILE:-0}" = 1 ]; then
        profile=(NATRON_ROTO_RENDER_PROFILE="$OUT/profile/$name.jsonl")
    fi
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH BENCH_RENDER_DIR="$OUT/render" BENCH_OUT="$OUT/results.jsonl" \
        BENCH_FRAMES="${BENCH_FRAMES:-12}" BENCH_SETTINGS="$SETTINGS" BENCH_LABEL="${BENCH_LABEL:-}" "${profile[@]}" "$@" \
        timeout 3600 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" "${SETTING_ARGV[@]}" -b tools/bench/roto_render_bench.py) > "$log" 2>&1
    code=$?
    [ "$code" -eq 0 ] || echo "$name exited with $code, see $log" >&2
    return "$code"
}

for res in $RESOLUTIONS; do
    run "plate-$res" BENCH_RES="$res" BENCH_ROTO_PLATES_ONLY=1 || exit 1
done

for res in $RESOLUTIONS; do
    for node in $NODES; do
        for n in $COUNTS; do
            for feather in $FEATHERS; do
                for motion in $MOTIONS; do
                    run "$node-shapes-$n-$res-f$feather-$motion" BENCH_ROTO_NODE="$node" BENCH_ROTO_SOURCE=shapes \
                        BENCH_N="$n" BENCH_RES="$res" BENCH_ROTO_FEATHER="$feather" BENCH_ROTO_MOTION="$motion"
                done
            done
        done
    done
    for n in $STROKES; do
        if [ ! -f "$STROKES_DIR/roto_strokes_${n}_$res.ntp" ]; then
            echo "skipping $n strokes at $res: no $STROKES_DIR/roto_strokes_${n}_$res.ntp (tools/bench/gui/make_roto_strokes.py)" >&2
            continue
        fi
        run "rotopaint-strokes-$n-$res" BENCH_ROTO_SOURCE=strokes BENCH_N="$n" BENCH_RES="$res" BENCH_ROTO_STROKES_DIR="$STROKES_DIR"
    done
done

python3 -I - "$OUT/results.jsonl" <<'PY'
import json
import sys

rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
if not rows:
    sys.exit("no results")
cols = ("frame_cold_s", "frame_wall_med_s", "frame_p95_s", "edit_color_s", "edit_grade_s")
print("%-36s %5s %4s %s" % ("topo", "n", "res", " ".join("%16s" % c for c in cols)))
for r in rows:
    print("%-36s %5s %4s %s" % (r["topo"], r["n"], r["res"], " ".join("%16.4f" % r[c] for c in cols)))
PY
