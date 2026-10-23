#!/usr/bin/env bash
# Thread scaling of the Develop RAW chain on a camera raw file: renders it through RawPrepare,
# RawTemperature, RawHighlights, RawDemosaic and RawInputColor at each thread count and prints the time
# per count with the speedup over the first. Run it from a Natron AppImage.
#
#   tools/bench/raw_large_bench.sh <Natron.AppImage> [raw-file] [out-dir]
#
# Environment:
#   BENCH_THREADS        thread counts (default "1 2 4"), each passed as --setting noRenderThreads=N
#   BENCH_REPS           timed renders per count after the untimed decoding one (default 3); the best is reported
#   BENCH_APP_ARGS       arguments placed before the script (default "-b")
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#
# The default raw file is the Canon fixture; pass a larger file (a 100 MP DNG, say) to see the scaling
# at that size. The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside.
# Results land in <out-dir>/results.jsonl and <out-dir>/threads-<N>.log.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [raw-file] [out-dir]" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RAW=${2:-$REPO/Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2}
OUT=$(realpath -m "${3:-$REPO/build/bench/raw-large}")
THREADS=${BENCH_THREADS:-1 2 4}
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"
read -r -a APP_ARGV <<<"${BENCH_APP_ARGS:--b}"
[ -f "$RAW" ] || { echo "no raw file $RAW" >&2; exit 2; }

mkdir -p "$OUT/render"
: > "$OUT/results.jsonl"

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

for n in $THREADS; do
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH BENCH_RAW_FILE="$(realpath "$RAW")" BENCH_RENDER_DIR="$OUT/render" \
        BENCH_THREADS="$n" BENCH_OUT="$OUT/results.jsonl" BENCH_REPS="${BENCH_REPS:-3}" \
        timeout 3600 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" --setting "noRenderThreads=$n" "${APP_ARGV[@]}" tools/bench/raw_large_bench.py) > "$OUT/threads-$n.log" 2>&1
    code=$?
    [ "$code" -eq 0 ] || echo "threads=$n exited with $code, see $OUT/threads-$n.log" >&2
done

python3 -I - "$OUT/results.jsonl" <<'PY'
import json
import sys

rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
if not rows:
    sys.exit("no results")
base = min(rows[0]["times_s"] or [rows[0]["first_s"]])
print("%8s %10s %10s %9s %12s" % ("threads", "first_s", "best_s", "speedup", "peak_rss_mb"))
for r in rows:
    best = min(r["times_s"] or [r["first_s"]])
    print("%8s %10.3f %10.3f %8.2fx %12s" % (r["threads"], r["first_s"], best, base / best if best else 0, r["peak_rss_mb"]))
PY
