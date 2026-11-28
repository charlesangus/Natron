#!/usr/bin/env bash
# Natron Utility, Ray mode against Raster mode, from a Natron AppImage: a card carrying a projected plate, sliding and
# turning under a panning camera over an open shutter, rendered on the GPU as sessions of consecutive frames, one
# frame per render, at each pass count in both modes. Each mode's frame times are fitted to fixed + per_pass * passes,
# the renderer's per-stage profile is read back and tabled per mode, and the motion blur of each mode is gated on PNGs
# at the larger pass counts; everything lands in <out-dir>/results.jsonl.
#
#   tools/bench/utility_raster_bench.sh <Natron.AppImage> [out-dir]
#
# Environment (see utility_raster_bench.py for the bench's own):
#   BENCH_PASSES, BENCH_FRAMES, BENCH_SESSIONS, BENCH_WARMUPS, BENCH_OUTPUT, BENCH_BLUR_PASSES, BENCH_SPEEDUP_AT,
#   BENCH_RAY_BASELINE_MS, BENCH_SIZE, BENCH_DEVICE, BENCH_PIXEL_FILTER, BENCH_VIEWER, BENCH_VIEWER_FRAMES,
#   BENCH_VIEWER_PASSES   passed through
#   BENCH_PROFILE=0      renders without the renderer's profile (default on, to <out-dir>/profile.jsonl)
#   NATRON_GPU_DEVICE    the GPU to use (default: the first hardware device)
#   BENCH_APP_ARGS       arguments placed before the script (default "-b")
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#   BENCH_DRY_RUN=1      writes the stage and lists the renders without an AppImage or Natron
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside. The node cache's disk
# tier is off, so no render is served from an earlier one. The default run is pass counts 1, 4, 16 and 64, three
# sessions of six frames each per mode, timed through an uncompressed half-float EXR Write: about 8 minutes on an
# RTX 3060 at the per-pass costs this bench was built to measure. Exit status 3 means a Raster render fell back to
# Ray, a render failed, a mode's image shows no motion blur, or the two modes' images are identical; 1 is an error in
# the bench script.
# For a quick check: BENCH_PASSES="1 16" BENCH_FRAMES=2 BENCH_SESSIONS=1 BENCH_WARMUPS=0 BENCH_SIZE="480 270".
# Results land in <out-dir>/results.jsonl, <out-dir>/profile.jsonl, <out-dir>/bench.log and the gate's PNGs.
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
PROFILE_ENV=()
if [ "${BENCH_PROFILE:-1}" != 0 ]; then
    : > "$OUT/profile.jsonl"
    PROFILE_ENV=(NATRON_UTILITY_PROFILE="$OUT/profile.jsonl")
fi

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

(cd "$REPO" && with_display env -u LD_LIBRARY_PATH -u NATRON_UTILITY_PROFILE "${PROFILE_ENV[@]}" NATRON_CACHE_DISK_TIER=off BENCH_RENDER_DIR="$OUT" BENCH_OUT="$OUT/results.jsonl" \
    timeout 1800 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" "${APP_ARGV[@]}" tools/bench/utility_raster_bench.py) 2>&1 | tee "$OUT/bench.log"
code=${PIPESTATUS[0]}
[ "$code" -eq 0 ] || echo "bench exited with $code, see $OUT/bench.log" >&2
exit "$code"
