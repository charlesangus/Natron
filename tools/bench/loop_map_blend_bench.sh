#!/usr/bin/env bash
# Map Blend culling: a LoopEnd over a driven Blur through a Map that needs every iteration
# everywhere (full) against one black on its left half (half), which culls that half to one
# iteration. The gate is half taking at most 60% of full. Run it on a quiet machine.
#
#   tools/bench/loop_map_blend_bench.sh <Natron.AppImage> [out-dir]
#
# Environment:
#   BENCH_THREADS        thread counts (default "1 8"), each passed as --setting noRenderThreads=N
#   BENCH_SIZE           frame width and height (default "1920 1080")
#   BENCH_ITERATIONS     the loop's iterations (default 16)
#   BENCH_REPS           timed renders after the untimed first one (default 5); the best is reported
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside.
# The node cache's disk tier is off: it serves renders of earlier runs and its writes land in the
# timed renders.
# Results land in <out-dir>/results.jsonl and <out-dir>/<map>-threads-<N>.log.
set -uo pipefail

APPIMAGE=${1:-}
if [ -z "$APPIMAGE" ]; then
    echo "usage: $0 <Natron.AppImage> [out-dir]" >&2
    exit 2
fi
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${2:-$REPO/build/bench/loop-map-blend}")
THREADS=${BENCH_THREADS:-1 8}
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"
APPIMAGE=$(realpath "$APPIMAGE")

mkdir -p "$OUT/render"
: > "$OUT/results.jsonl"

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

for map in full half; do
    for n in $THREADS; do
        log="$OUT/$map-threads-$n.log"
        (cd "$REPO" && with_display env -u LD_LIBRARY_PATH NATRON_CACHE_DISK_TIER=off BENCH_RENDER_DIR="$OUT/render" BENCH_OUT="$OUT/results.jsonl" \
            BENCH_BUILD=branch BENCH_MAP="$map" BENCH_THREADS="$n" BENCH_SIZE="${BENCH_SIZE:-1920 1080}" \
            BENCH_ITERATIONS="${BENCH_ITERATIONS:-16}" BENCH_REPS="${BENCH_REPS:-5}" \
            timeout 3600 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" --setting "noRenderThreads=$n" -b tools/bench/loop_map_blend_bench.py) > "$log" 2>&1
        code=$?
        [ "$code" -eq 0 ] || echo "map=$map threads=$n exited with $code, see $log" >&2
    done
done

python3 -I - "$OUT/results.jsonl" <<'PY'
import json
import sys

rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
if not rows:
    sys.exit("no results")
best = {}
for r in rows:
    best[(r["map"], r["threads"])] = min(r["times_s"] or [r["first_s"]])
failed = False
print("%8s %10s %10s %8s" % ("threads", "full_s", "half_s", "ratio"))
for threads in sorted({t for (_, t) in best}, key=lambda t: int(t) if str(t).isdigit() else 0):
    full = best.get(("full", threads))
    half = best.get(("half", threads))
    if not full or half is None:
        print("%8s %10s %10s %8s" % (threads, full or "n/a", half if half is not None else "n/a", "n/a"))
        failed = True
        continue
    ratio = half / full
    failed = failed or ratio > 0.6
    print("%8s %10.3f %10.3f %7.2fx%s" % (threads, full, half, ratio, "" if ratio <= 0.6 else "  over 0.60"))
sys.exit(1 if failed else 0)
PY
