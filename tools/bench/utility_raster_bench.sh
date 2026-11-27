#!/usr/bin/env bash
# Natron Utility, Ray mode against Raster mode, from a Natron AppImage: a card carrying a projected plate, sliding and
# turning over an open shutter, rendered through a Write on the GPU at each sample count in both modes, with the
# frame times of each (median, p95) and the Ray over Raster ratios written to <out-dir>/results.jsonl.
#
#   tools/bench/utility_raster_bench.sh <Natron.AppImage> [out-dir]
#
# Environment (see utility_raster_bench.py for the bench's own):
#   BENCH_SIDES, BENCH_REPS, BENCH_WARMUPS, BENCH_SIZE   passed through
#   NATRON_GPU_DEVICE    the GPU to use (default: the first hardware device)
#   BENCH_APP_ARGS       arguments placed before the script (default "-b")
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#   BENCH_DRY_RUN=1      writes the stage and lists the renders without an AppImage or Natron
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside. The node cache's disk
# tier is off, so no render is served from an earlier one. The default run is two sample counts of 12 timed renders
# per mode, about 5 minutes on one GPU. Exit status 3 means a Raster render fell back to Ray or a render failed.
# Results land in <out-dir>/results.jsonl, <out-dir>/bench.log and the PNG of each mode's last render.
set -uo pipefail

[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] && { sed -n '2,/^set -uo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0; }
APPIMAGE=${1:-}
if [ -z "$APPIMAGE" ] && [ "${BENCH_DRY_RUN:-0}" != 1 ]; then
    echo "usage: $0 <Natron.AppImage> [out-dir]" >&2
    exit 2
fi
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${2:-$REPO/build/bench/utility-raster}")
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"
read -r -a APP_ARGV <<<"${BENCH_APP_ARGS:--b}"
mkdir -p "$OUT"

if [ "${BENCH_DRY_RUN:-0}" = 1 ]; then
    BENCH_DRY_RUN=1 BENCH_RENDER_DIR="$OUT" python3 -I "$REPO/tools/bench/utility_raster_bench.py"
    exit $?
fi
[ -f "$APPIMAGE" ] || { echo "no AppImage at $APPIMAGE" >&2; exit 2; }
APPIMAGE=$(realpath "$APPIMAGE")
: > "$OUT/results.jsonl"

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

(cd "$REPO" && with_display env -u LD_LIBRARY_PATH NATRON_CACHE_DISK_TIER=off BENCH_RENDER_DIR="$OUT" BENCH_OUT="$OUT/results.jsonl" \
    timeout 1800 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" "${APP_ARGV[@]}" tools/bench/utility_raster_bench.py) 2>&1 | tee "$OUT/bench.log"
code=${PIPESTATUS[0]}
[ "$code" -eq 0 ] || echo "bench exited with $code, see $OUT/bench.log" >&2
exit "$code"
