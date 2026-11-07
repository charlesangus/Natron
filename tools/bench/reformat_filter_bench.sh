#!/usr/bin/env bash
# CPU cost of a 4x Reformat downscale with Cubic and Lanczos 3, a build without the Lanczos filters
# against one with them. A filter a build lacks is printed as n/a. Run it on a quiet machine.
#
#   tools/bench/reformat_filter_bench.sh <main.AppImage> <branch.AppImage> [out-dir]
#
# Environment:
#   BENCH_FILTERS        filter IDs to time (default "cubic lanczos3")
#   BENCH_THREADS        thread counts (default "1 8"), each passed as --setting noRenderThreads=N
#   BENCH_SIZE           source width and height (default "3840 2160")
#   BENCH_REPS           timed renders after the untimed first one (default 5); the best is reported
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside.
# Results land in <out-dir>/results.jsonl and <out-dir>/<build>-<filter>-threads-<N>.log.
set -uo pipefail

MAIN_APPIMAGE=${1:-}
BRANCH_APPIMAGE=${2:-}
if [ -z "$MAIN_APPIMAGE" ] || [ -z "$BRANCH_APPIMAGE" ]; then
    echo "usage: $0 <main.AppImage> <branch.AppImage> [out-dir]" >&2
    exit 2
fi
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${3:-$REPO/build/bench/reformat-filter}")
FILTERS=${BENCH_FILTERS:-cubic lanczos3}
THREADS=${BENCH_THREADS:-1 8}
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"

mkdir -p "$OUT/render"
: > "$OUT/results.jsonl"

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

run_build() {
    local build=$1 appimage=$2 filter n log code
    for filter in $FILTERS; do
        for n in $THREADS; do
            log="$OUT/$build-$filter-threads-$n.log"
            (cd "$REPO" && with_display env -u LD_LIBRARY_PATH BENCH_RENDER_DIR="$OUT/render" BENCH_OUT="$OUT/results.jsonl" \
                BENCH_BUILD="$build" BENCH_FILTER="$filter" BENCH_THREADS="$n" BENCH_SIZE="${BENCH_SIZE:-3840 2160}" BENCH_REPS="${BENCH_REPS:-5}" \
                timeout 3600 "$appimage" "${APPIMAGE_ARGV[@]}" --setting "noRenderThreads=$n" -b tools/bench/reformat_filter_bench.py) > "$log" 2>&1
            code=$?
            [ "$code" -eq 0 ] || echo "build=$build filter=$filter threads=$n exited with $code, see $log" >&2
        done
    done
}

run_build main "$(realpath "$MAIN_APPIMAGE")"
run_build branch "$(realpath "$BRANCH_APPIMAGE")"

python3 -I - "$OUT/results.jsonl" <<'PY'
import json
import sys

rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
if not rows:
    sys.exit("no results")
best = {}
for r in rows:
    key = (r["build"], r["filter"], r["threads"])
    best[key] = None if r.get("unavailable") else min(r["times_s"] or [r["first_s"]])
print("%8s %10s %8s %10s %10s" % ("build", "filter", "threads", "best_s", "/main cubic"))
for (build, filter_id, threads), value in best.items():
    base = best.get(("main", "cubic", threads))
    if value is None:
        print("%8s %10s %8s %10s %10s" % (build, filter_id, threads, "n/a", "n/a"))
    elif base:
        print("%8s %10s %8s %10.3f %9.2fx" % (build, filter_id, threads, value, value / base))
    else:
        print("%8s %10s %8s %10.3f %10s" % (build, filter_id, threads, value, "n/a"))
PY
