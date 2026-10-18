#!/usr/bin/env bash
# GPU on/off comparison of spatial nodes and the M82 comps, with the GPU region profile kept per run.
#
#   tools/bench/gpu_region_bench.sh <Natron.AppImage> [out-dir]
#
# Each case renders with NATRON_GPU=off and NATRON_GPU=auto, interleaved, and the order flips every round
# so thermal drift does not favour one mode. Cases: the nodes in BENCH_NODES alone (topology "node") and
# between two Grades ("gxg") at each size in BENCH_SIZES, then the comps in BENCH_COMPS.
# Results: <out-dir>/<tag>.log, <tag>.prof.* (the render profile) and results.jsonl; summarise a profile with
#   python3 -I tools/bench/gpu_region_summary.py <out-dir>/<tag>.prof
#
# Environment:
#   BENCH_ROUNDS         rounds per case (default 3)
#   BENCH_NODES          graph_bench cases (default "xf xfmb blur25 iir25 erode6 erode40 ck")
#   BENCH_SIZES          WxH or "uhd", space separated (default "uhd 7680x4320 16384x16384")
#   BENCH_COMPS          comps in tools/bench/comps (default "keying_grade cg_multipass defocus_retime")
#   BENCH_COMP_NEED_GB   free memory a comp needs to run (default 4)
#   BENCH_APP_ARGS       arguments placed before the script or project (default "-b")
#   BENCH_DRY_RUN=1      lists the planned cases, with the memory probe, and renders nothing
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside the image.
# A case is skipped when its estimated memory exceeds MemAvailable, read again before every case.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [out-dir]" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-$REPO/build/bench/gpu-region}
ROUNDS=${BENCH_ROUNDS:-3}
NODES=${BENCH_NODES:-xf xfmb blur25 iir25 erode6 erode40 ck}
SIZES=${BENCH_SIZES:-uhd 7680x4320 16384x16384}
COMPS=${BENCH_COMPS:-keying_grade cg_multipass defocus_retime}
COMP_NEED_GB=${BENCH_COMP_NEED_GB:-4}
APP_ARGS=${BENCH_APP_ARGS:--b}
DRY=${BENCH_DRY_RUN:-0}
COMP_OUT=$REPO/build/bench/m82-profile/out

declare -A CASE
CASE[xf]="BENCH_NODE=transform BENCH_MOTION_BLUR=0"
CASE[xfmb]="BENCH_NODE=transform BENCH_MOTION_BLUR=1"
CASE[blur25]="BENCH_NODE=blur BENCH_BLUR_SIZE=60 BENCH_BLUR_FILTER=firgaussian"
CASE[iir25]="BENCH_NODE=blur BENCH_BLUR_SIZE=60 BENCH_BLUR_FILTER=quasigaussian"
CASE[erode6]="BENCH_NODE=erode BENCH_ERODE_SIZE=6"
CASE[erode40]="BENCH_NODE=erode BENCH_ERODE_SIZE=40"
CASE[ck]="BENCH_NODE=chromakeyer"

mem_available_kb() { awk '/^MemAvailable:/{print $2}' /proc/meminfo; }

# Peak resident memory measured about 3.2x one float RGBA frame (source, output and the writer's half-float copy).
frame_need_kb() {
    local sz=$1 w h
    [ "$sz" = uhd ] && sz=3840x2160
    w=${sz%x*}
    h=${sz#*x}
    echo $((w * h * 16 * 4 / 1024))
}

size_label() {
    case $1 in
        uhd) echo uhd ;;
        7680x4320) echo 8k ;;
        *) echo "$((${1%%x*} / 1024))k" ;;
    esac
}

frames_for() {
    local w=${1%%x*}
    [ "$1" = uhd ] && w=3840
    if [ "$w" -le 4096 ]; then echo 4; elif [ "$w" -le 8192 ]; then echo 3; else echo 2; fi
}

cpu_busy_pct() {
    local a b c d e f g t1 i1 t2 i2
    read -r _ a b c d e f g _ < /proc/stat
    t1=$((a + b + c + d + e + f + g)); i1=$((d + e))
    sleep 1
    read -r _ a b c d e f g _ < /proc/stat
    t2=$((a + b + c + d + e + f + g)); i2=$((d + e))
    echo $((100 - 100 * (i2 - i1) / (t2 - t1)))
}

dirty_kb() { awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo; }

# The 1-minute load average decays too slowly to see the previous run finish, and its EXR write-back
# would otherwise throttle the next one, so wait for idle CPUs and drained dirty pages.
settle() {
    local i busy dirty
    sync
    for i in $(seq 1 300); do
        busy=$(cpu_busy_pct)
        dirty=$(dirty_kb)
        if [ "$busy" -lt 5 ] && [ "$dirty" -lt 262144 ]; then
            sleep 2
            return
        fi
    done
    echo "settle: still busy cpu=${busy}% dirty=${dirty}kB, continuing"
}

with_display() {
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run > /dev/null; then
        xvfb-run --auto-servernum "$@"
    else
        "$@"
    fi
}

fits() {
    local need_kb=$1 label=$2 avail_kb
    avail_kb=$(mem_available_kb)
    if [ "$avail_kb" -lt "$need_kb" ]; then
        echo "SKIP $label: needs $((need_kb / 1024)) MB, $((avail_kb / 1024)) MB available"
        return 1
    fi
    echo "PLAN $label: needs $((need_kb / 1024)) MB, $((avail_kb / 1024)) MB available"
}

run_node_case() {
    local c=$1 topo=$2 size=$3 mode=$4 round=$5 tag kv
    tag=$topo-$c-$(size_label "$size")-$mode-r$round
    fits "$(frame_need_kb "$size")" "$tag" || return 0
    [ "$DRY" = 1 ] && return 0
    grep -qs RESULT "$OUT/$tag.log" && return 0
    settle
    rm -f "$OUT/$tag.prof".*
    local res=uhd sz=""
    [ "$size" != uhd ] && { res=s$size; sz=$size; }
    local t0=$SECONDS code
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH NATRON_GPU="$mode" NATRON_RENDER_SCHEDULER=taskgraph \
        NATRON_RENDER_PROFILE="$OUT/$tag.prof" BENCH_TOPO="$topo" BENCH_TAG="$tag" BENCH_N=1 BENCH_RES="$res" \
        BENCH_SIZE="$sz" BENCH_WARMUP=1 BENCH_FRAMES="$(frames_for "$size")" BENCH_OUT="$OUT/results.jsonl" ${CASE[$c]} \
        timeout 3600 "$APPIMAGE" $APP_ARGS tools/bench/graph_bench.py) > "$OUT/$tag.log" 2>&1
    code=$?
    echo "$tag exit=$code $((SECONDS - t0))s"
}

run_comp_case() {
    local comp=$1 mode=$2 round=$3 tag
    tag=comp-$comp-$mode-r$round
    fits $((COMP_NEED_GB * 1048576)) "$tag" || return 0
    [ "$DRY" = 1 ] && return 0
    grep -qs "^done" "$OUT/$tag.log" && return 0
    settle
    rm -f "$OUT/$tag.prof".* "$COMP_OUT/$comp".*.exr
    local t0=$SECONDS
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH NATRON_GPU="$mode" NATRON_RENDER_SCHEDULER=taskgraph \
        NATRON_RENDER_PROFILE="$OUT/$tag.prof" timeout 3600 "$APPIMAGE" $APP_ARGS -w Write 1-6 "tools/bench/comps/$comp.ntp") > "$OUT/$tag.log" 2>&1
    local code=$?
    rm -f "$COMP_OUT/$comp".*.exr
    echo "done exit=$code" >> "$OUT/$tag.log"
    echo "$tag exit=$code $((SECONDS - t0))s"
}

mode_order() { if [ $(($1 % 2)) -eq 1 ]; then echo "off auto"; else echo "auto off"; fi; }

if [ "$DRY" != 1 ]; then
    [ -x "$APPIMAGE" ] || { echo "not executable: $APPIMAGE" >&2; exit 2; }
    mkdir -p "$OUT" "$COMP_OUT"
fi

echo "gpu_region_bench: rounds=$ROUNDS out=$OUT appimage=$APPIMAGE"
for r in $(seq 1 "$ROUNDS"); do
    modes=$(mode_order "$r")
    for size in $SIZES; do
        for c in $NODES; do
            for topo in node gxg; do
                for m in $modes; do
                    run_node_case "$c" "$topo" "$size" "$m" "$r"
                done
            done
        done
    done
    for comp in $COMPS; do
        for m in $modes; do
            run_comp_case "$comp" "$m" "$r"
        done
    done
done
echo "gpu_region_bench: finished"
