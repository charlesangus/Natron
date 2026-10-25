#!/usr/bin/env bash
# GPU off/auto first-frame time of the raw Develop chain (Read, RawPrepare, RawHotPixels, RawDenoise,
# RawTemperature, RawHighlights in Clip mode, RawDemosaic, RawInputColor, Write to EXR). Every render is
# a fresh process with an empty pipeline cache and no warm-up, so decode, pipeline compile and upload are
# part of the first frame. Off and auto are interleaved per case and round, the order flipping every round.
#
#   tools/bench/raw_gpu_bench.sh --app <Natron.AppImage> [--rounds 3] [--out DIR] [--big FILE] [--dry-run]
#
# Cases: raf-mk1 and raf-mk3 (X-Trans, Markesteijn 1 and 3 pass) and cr2-amaze (Bayer, AMaZE) on the
# fixtures in Tests/fixtures/raw; with --big (or the first of BENCH_BIG_DIRS' raw files) also big-amaze or
# big-mk3, picked by the file's extension (.raf is X-Trans, anything else Bayer).
# Results: DIR/results.jsonl (one line per render), DIR/table.txt (median first-frame ms and speed-up),
# DIR/<tag>.log, DIR/<tag>.prof* (the render profile), DIR/skips.log.
#
# Environment: BENCH_BIG_DIRS (default "build/assets Tests/fixtures", searched for a raw of 80 MP or more
# by file size, 60 MB), BENCH_APPIMAGE_ARGS (default "--appimage-extract-and-run", the host has no FUSE),
# BENCH_NEED_FACTOR (memory needed = raw file size times this, default 40; at least 4 GiB).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
APPIMAGE='' OUT='' BIG='' ROUNDS=3 DRY=0
BIG_DIRS=${BENCH_BIG_DIRS:-"build/assets Tests/fixtures"}
NEED_FACTOR=${BENCH_NEED_FACTOR:-40}
read -r -a APPIMAGE_ARGV <<<"${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}"

usage() { echo "usage: $0 --app <AppImage> [--rounds N] [--out DIR] [--big FILE] [--dry-run]" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
        --app) APPIMAGE=${2:-}; shift 2 ;;
        --rounds) ROUNDS=${2:-}; shift 2 ;;
        --out) OUT=${2:-}; shift 2 ;;
        --big) BIG=${2:-}; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        *) usage ;;
    esac
done
[ "$DRY" = 1 ] || [ -n "$APPIMAGE" ] || usage
[ -z "$APPIMAGE" ] || APPIMAGE=$(realpath -m "$APPIMAGE")
OUT=$(realpath -m "${OUT:-$REPO/build/bench/raw-gpu}")
case $ROUNDS in '' | *[!0-9]*) usage ;; esac

RAW_DIR=$REPO/Tests/fixtures/raw
# Case format: name|file|bayer method index|x-trans method index (RawDemosaic choice indices).
CASES=(
    "raf-mk1|$RAW_DIR/fujifilm-x-pro2-14bit-uncompressed.raf|2|0"
    "raf-mk3|$RAW_DIR/fujifilm-x-pro2-14bit-uncompressed.raf|2|1"
    "cr2-amaze|$RAW_DIR/canon-eos-5d-mark-iii.cr2|2|1"
)

find_big() {
    local d f
    for d in $BIG_DIRS; do
        [ -d "$REPO/$d" ] || continue
        for f in "$REPO/$d"/*; do
            case ${f,,} in
                *.cr2 | *.cr3 | *.raf | *.dng | *.nef | *.arw | *.iiq | *.3fr | *.fff | *.rw2 | *.orf) ;;
                *) continue ;;
            esac
            case $f in "$RAW_DIR"/*) continue ;; esac
            [ "$(stat -c %s "$f")" -ge 60000000 ] && { echo "$f"; return; }
        done
    done
}
[ -n "$BIG" ] || BIG=$(find_big)
if [ -n "$BIG" ]; then
    [ -f "$BIG" ] || { echo "--big file not found: $BIG" >&2; exit 2; }
    BIG=$(realpath "$BIG")
    case ${BIG,,} in
        *.raf) CASES+=("big-mk3|$BIG|2|1") ;;
        *) CASES+=("big-amaze|$BIG|2|1") ;;
    esac
fi

mem_available_kb() { awk '/^MemAvailable:/{print $2}' /proc/meminfo; }
boot_id() { cat /proc/sys/kernel/random/boot_id; }

need_kb_for() {
    local kb=$(($(stat -c %s "$1") * NEED_FACTOR / 1024))
    [ "$kb" -lt 4194304 ] && kb=4194304
    echo "$kb"
}

# Mode order flips with the round so neither mode is always first.
mode_order() { if [ $(($1 % 2)) -eq 1 ]; then echo "off auto"; else echo "auto off"; fi; }

if [ "$DRY" = 1 ]; then
    echo "app: ${APPIMAGE:-<none>}"
    echo "out: $OUT"
    echo "rounds: $ROUNDS"
    echo "boot: $(boot_id)"
    for spec in "${CASES[@]}"; do
        IFS='|' read -r name file bayer xtrans <<<"$spec"
        for r in $(seq 1 "$ROUNDS"); do
            for mode in $(mode_order "$r"); do
                echo "CASE $name mode=$mode round=$r file=$file needs=$(($(need_kb_for "$file") / 1024))MB available=$(($(mem_available_kb) / 1024))MB"
            done
        done
    done
    [ -n "$BIG" ] || echo "no big raw found (looked in $BIG_DIRS); pass --big FILE"
    exit 0
fi

[ -x "$APPIMAGE" ] || { echo "AppImage not executable: $APPIMAGE" >&2; exit 2; }
mkdir -p "$OUT"
DRIVER=$OUT/chain_driver.py

# NatronRenderer runs this. app.render runs the Write's pass twice and only the first carries the
# first-frame costs, so the render callbacks time that pass alone.
cat > "$DRIVER" <<'PY'
import json
import os
import time

RAW = os.environ["RB_RAW"]
OUT = os.environ["RB_OUT"]
RESULT = os.environ["RB_RESULT"]
CHAIN = (
    "fr.natron.RawPrepare",
    "fr.natron.RawHotPixels",
    "fr.natron.RawDenoise",
    "fr.natron.RawTemperature",
    "fr.natron.RawHighlights",
    "fr.natron.RawDemosaic",
    "fr.natron.RawInputColor",
)
stamps = []


def rbBefore(thisNode, app):
    stamps.append(("start", time.time()))


def rbAfter(aborted, thisNode, app):
    stamps.append(("end", time.time()))


def param(node, name):
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return p


prev = app.createReader(RAW)
if prev is None:
    raise RuntimeError("could not create a reader for " + RAW)
nodes = {}
for plugin_id in CHAIN:
    node = app.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    node.connectInput(0, prev)
    nodes[plugin_id] = node
    prev = node

param(nodes["fr.natron.RawHighlights"], "mode").set(0)
param(nodes["fr.natron.RawDemosaic"], "method").set(int(os.environ["RB_BAYER"]))
param(nodes["fr.natron.RawDemosaic"], "xtransMethod").set(int(os.environ["RB_XTRANS"]))

writer = app.createWriter(OUT)
writer.connectInput(0, prev)
writer.getParam("ocioOutputSpace").set(writer.getParam("ocioWorkingSpace").get())
writer.getParam("ocioOutputSpaceSet").set(True)
writer.getParam("compression").set("none")
writer.getParam("beforeRender").set("rbBefore")
writer.getParam("afterRender").set("rbAfter")

t0 = time.time()
app.render(writer, 1, 1)
call_ms = (time.time() - t0) * 1000.0
starts = [t for k, t in stamps if k == "start"]
ends = [t for k, t in stamps if k == "end"]
first_ms = (ends[0] - starts[0]) * 1000.0 if starts and ends else None
with open(RESULT, "a") as f:
    f.write(json.dumps({"first_ms": first_ms and round(first_ms, 2), "call_ms": round(call_ms, 2), "passes": len(starts)}) + "\n")
PY

# Region count of the first profiled frame: distinct regionId >= 0, and the plugins that ran in one.
REGIONS_PY='
import glob, json, sys
frames = {}
for path in glob.glob(sys.argv[1] + "*"):
    with open(path) as f:
        for line in f:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            frames.setdefault(rec.get("frame"), []).append(rec)
if not frames:
    print("null 0")
    sys.exit(0)
recs = frames[min(frames, key=lambda k: (k is None, k))]
regions = {r["regionId"] for r in recs if r.get("regionId", -1) >= 0}
gpu = sum(1 for r in recs if r.get("regionId", -1) >= 0)
print(len(regions), gpu)
'

TABLE_PY='
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
cases = []
for r in rows:
    if r["case"] not in cases:
        cases.append(r["case"])
def med(case, mode):
    v = [r["first_ms"] for r in rows if r["case"] == case and r["mode"] == mode and r.get("first_ms") is not None and r["exit"] == 0]
    return (statistics.median(v), len(v)) if v else (None, 0)
print("%-12s %12s %12s %9s %9s %s" % ("case", "off ms", "auto ms", "speed-up", "regions", "n(off/auto)"))
for c in cases:
    off, no = med(c, "off")
    auto, na = med(c, "auto")
    reg = sorted({r["regions"] for r in rows if r["case"] == c and r["mode"] == "auto" and r.get("regions") is not None})
    sp = "%.2fx" % (off / auto) if off and auto else "n/a"
    f = lambda x: "%.1f" % x if x is not None else "n/a"
    print("%-12s %12s %12s %9s %9s %d/%d" % (c, f(off), f(auto), sp, ",".join(map(str, reg)) or "n/a", no, na))
'

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

run_one() {
    local name=$1 file=$2 bayer=$3 xtrans=$4 mode=$5 round=$6 tag avail need cache render_dir t0 code regions gpu_nodes first
    tag=$name-$mode-r$round
    avail=$(mem_available_kb)
    need=$(need_kb_for "$file")
    echo "MEM $tag: MemAvailable $((avail / 1024)) MB, needs $((need / 1024)) MB"
    if [ "$avail" -lt "$need" ]; then
        echo "SKIP $tag: needs $((need / 1024)) MB, $((avail / 1024)) MB available" | tee -a "$OUT/skips.log"
        return
    fi
    cache=$(mktemp -d "$OUT/xdg.XXXXXX")
    render_dir=$OUT/render/$tag
    mkdir -p "$render_dir"
    rm -f "$OUT/$tag.prof"* "$OUT/$tag.marks.jsonl"
    sync
    sleep 2
    t0=$SECONDS
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH XDG_CACHE_HOME="$cache" NATRON_GPU="$mode" \
        NATRON_RENDER_SCHEDULER=taskgraph NATRON_RENDER_PROFILE="$OUT/$tag.prof" \
        RB_RAW="$file" RB_OUT="$render_dir/out_####.exr" RB_RESULT="$OUT/$tag.marks.jsonl" \
        RB_BAYER="$bayer" RB_XTRANS="$xtrans" \
        timeout 1800 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" -b "$DRIVER") > "$OUT/$tag.log" 2>&1
    code=$?
    rm -rf "$render_dir" "$cache"
    first=$(python3 -I -c 'import json,sys
try:
    print(json.loads(open(sys.argv[1]).readline())["first_ms"])
except Exception:
    print("null")' "$OUT/$tag.marks.jsonl")
    read -r regions gpu_nodes <<<"$(python3 -I -c "$REGIONS_PY" "$OUT/$tag.prof")"
    printf '{"case":"%s","mode":"%s","round":%d,"first_ms":%s,"regions":%s,"gpu_tasks":%s,"exit":%d,"boot":"%s","mem_available_kb":%d}\n' \
        "$name" "$mode" "$round" "$first" "$regions" "$gpu_nodes" "$code" "$(boot_id)" "$avail" >> "$OUT/results.jsonl"
    echo "$tag exit=$code first_ms=$first regions=$regions $((SECONDS - t0))s"
}

for r in $(seq 1 "$ROUNDS"); do
    for spec in "${CASES[@]}"; do
        IFS='|' read -r name file bayer xtrans <<<"$spec"
        for mode in $(mode_order "$r"); do
            run_one "$name" "$file" "$bayer" "$xtrans" "$mode" "$r"
        done
    done
done

python3 -I -c "$TABLE_PY" "$OUT/results.jsonl" | tee "$OUT/table.txt"
