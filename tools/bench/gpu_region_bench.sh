#!/usr/bin/env bash
# GPU on/off comparison of spatial nodes and the M82 comps in the workloads artists and farms run, with the
# GPU region profile kept per run. Nothing is warmed up first: one-time costs (pipeline compile, host
# import and pinning, pool warm-up, first-frame placement) are part of what a kind measures.
#
#   tools/bench/gpu_region_bench.sh <Natron.AppImage> [out-dir]
#
# Kinds (BENCH_KINDS, run in this order; each is NATRON_GPU=off and auto, interleaved, the order flipping every round):
#   a-cold    one frame in a fresh process, pipeline disk cache empty
#   a-warm    one frame in a fresh process, pipeline disk cache primed by an earlier untimed run of the case
#   bc        in one session: the frame, BENCH_RERENDERS re-renders of it after a knob change (b), then a jump to
#             another frame (c); the pipeline disk cache is the primed one
#   playback  BENCH_PLAYBACK_FRAMES frames as one range, in a fresh process (secondary)
# Cases: the nodes in BENCH_NODES alone (topology "node") and between two Grades ("gxg") at each size in
# BENCH_SIZES, then the comps in BENCH_COMPS at each size in BENCH_COMP_SIZES.
# Results: <out-dir>/<tag>.log, <tag>.prof.* (the render profile), <tag>.marks.jsonl (each render's label and
# in-process time), results.jsonl (graph_bench.py) and driver.jsonl (process wall, exit status). The tag is
# <case>-<size>-<gpu mode>-<kind>-r<round>. Summarise a run with
#   python3 -I tools/bench/gpu_region_summary.py <out-dir>/<tag>.prof
#
# Environment:
#   BENCH_ROUNDS         rounds per case (default 3)
#   BENCH_KINDS          kinds to run (default "a-cold a-warm bc playback")
#   BENCH_NODES          graph_bench cases (default "erode40 blur100 ck xf"; also xfmb blur25 iir25 iir100 erode6)
#   BENCH_SIZES          WxH or "uhd", space separated (default "uhd 7680x4320 16384x16384 24576x24576")
#   BENCH_COMPS          comps in tools/bench/comps (default "keying_grade cg_multipass defocus_retime")
#   BENCH_COMP_SIZES     sizes of the comps (default "uhd 7680x4320"); the comps' own geometry stays UHD-sized
#   BENCH_COMP_NEED_GB   free memory a UHD comp needs, scaled by pixel count for larger sizes (default 4)
#   BENCH_NEED_NODE, BENCH_NEED_GXG   float RGBA frames of memory a node / gxg session needs (default 6 / 9);
#                        a playback session needs twice that
#   BENCH_RERENDERS, BENCH_PLAYBACK_FRAMES, BENCH_SESSION_FRAME, BENCH_JUMP_FRAME   see graph_bench.py
#   BENCH_RENDER_BASE    absolute directory the per-run output directories are made in and removed from
#                        (default <out-dir>/render); point it at tmpfs to keep write-back off disk
#   BENCH_APP_ARGS       arguments placed before the script (default "-b")
#   BENCH_APPIMAGE_ARGS  arguments for the AppImage runtime (default "--appimage-extract-and-run", the host has no FUSE)
#   BENCH_DRY_RUN=1      lists the planned cases, with the memory probe, and renders nothing
#
# The AppImage runs with LD_LIBRARY_PATH removed: a host Qt on that path shadows the one inside the image.
# A case is skipped, with the reason logged to <out-dir>/skips.log, when its estimated memory exceeds
# MemAvailable, which is read from /proc/meminfo again before every run.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [out-dir]" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${2:-$REPO/build/bench/gpu-region}")
ROUNDS=${BENCH_ROUNDS:-3}
KINDS=${BENCH_KINDS:-a-cold a-warm bc playback}
NODES=${BENCH_NODES:-erode40 blur100 ck xf}
SIZES=${BENCH_SIZES:-uhd 7680x4320 16384x16384 24576x24576}
COMPS=${BENCH_COMPS:-keying_grade cg_multipass defocus_retime}
COMP_SIZES=${BENCH_COMP_SIZES:-uhd 7680x4320}
COMP_NEED_GB=${BENCH_COMP_NEED_GB:-4}
NEED_NODE=${BENCH_NEED_NODE:-6}
NEED_GXG=${BENCH_NEED_GXG:-9}
APP_ARGS=${BENCH_APP_ARGS:--b}
APPIMAGE_ARGS=${BENCH_APPIMAGE_ARGS:---appimage-extract-and-run}
read -r -a APP_ARGV <<<"$APP_ARGS"
read -r -a APPIMAGE_ARGV <<<"$APPIMAGE_ARGS"
DRY=${BENCH_DRY_RUN:-0}
RENDER_BASE=$(realpath -m "${BENCH_RENDER_BASE:-$OUT/render}")

declare -A CASES
CASES[xf]="BENCH_NODE=transform BENCH_MOTION_BLUR=0"
CASES[xfmb]="BENCH_NODE=transform BENCH_MOTION_BLUR=1"
CASES[blur25]="BENCH_NODE=blur BENCH_BLUR_SIZE=60 BENCH_BLUR_FILTER=firgaussian"
CASES[iir25]="BENCH_NODE=blur BENCH_BLUR_SIZE=60 BENCH_BLUR_FILTER=quasigaussian"
CASES[blur100]="BENCH_NODE=blur BENCH_BLUR_SIZE=240 BENCH_BLUR_FILTER=firgaussian"
CASES[iir100]="BENCH_NODE=blur BENCH_BLUR_SIZE=240 BENCH_BLUR_FILTER=quasigaussian"
CASES[erode6]="BENCH_NODE=erode BENCH_ERODE_SIZE=6"
CASES[erode40]="BENCH_NODE=erode BENCH_ERODE_SIZE=40"
CASES[ck]="BENCH_NODE=chromakeyer"

mem_available_kb() { awk '/^MemAvailable:/{print $2}' /proc/meminfo; }

size_dims() {
    [ "$1" = uhd ] && { echo "3840 2160"; return; }
    echo "${1%x*} ${1#*x}"
}

# One float RGBA frame is 16 bytes per pixel.
frame_kb() {
    local w h
    read -r w h <<< "$(size_dims "$1")"
    echo $((w * h * 16 / 1024))
}

size_label() {
    case $1 in
        uhd) echo uhd ;;
        7680x4320) echo 8k ;;
        *) echo "$((${1%%x*} / 1024))k" ;;
    esac
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
    local busy dirty
    sync
    for _ in $(seq 1 300); do
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
        [ "$DRY" = 1 ] || echo "SKIP $label: needs $((need_kb / 1024)) MB, $((avail_kb / 1024)) MB available" >> "$OUT/skips.log"
        return 1
    fi
    echo "PLAN $label: needs $((need_kb / 1024)) MB, $((avail_kb / 1024)) MB available"
}

# need_mult is in float RGBA frames; playback holds more frames in flight than one render.
need_kb_for() {
    local size=$1 frames=$2 kind=$3
    [ "$kind" = playback ] && frames=$((frames * 2))
    echo $(($(frame_kb "$size") * frames))
}

comp_need_kb() {
    local w h
    read -r w h <<< "$(size_dims "$1")"
    echo $((COMP_NEED_GB * 1048576 * w * h / (3840 * 2160)))
}

session_of() {
    case $1 in
        a-cold | a-warm) echo a ;;
        *) echo "$1" ;;
    esac
}

# Runs one session. Args: tag kind mode cache-dir, then the BENCH_* variables of the case.
run_session() {
    local tag=$1 kind=$2 mode=$3 cache=$4 render_dir="$RENDER_BASE/$1" t0 code
    shift 4
    rm -f "$OUT/$tag.prof".* "$OUT/$tag.marks.jsonl"
    mkdir -p "$render_dir" "$cache"
    t0=$SECONDS
    (cd "$REPO" && with_display env -u LD_LIBRARY_PATH XDG_CACHE_HOME="$cache" NATRON_GPU="$mode" \
        NATRON_RENDER_SCHEDULER=taskgraph NATRON_RENDER_PROFILE="$OUT/$tag.prof" BENCH_TAG="$tag" \
        BENCH_SESSION="$(session_of "$kind")" BENCH_LABEL="$kind" BENCH_RENDER_DIR="$render_dir" \
        BENCH_MARKS="$OUT/$tag.marks.jsonl" BENCH_OUT="$OUT/results.jsonl" BENCH_N=1 "$@" \
        timeout 3600 "$APPIMAGE" "${APPIMAGE_ARGV[@]}" "${APP_ARGV[@]}" tools/bench/graph_bench.py) > "$OUT/$tag.log" 2>&1
    code=$?
    rm -rf "$render_dir"
    printf '{"tag":"%s","kind":"%s","gpu":"%s","exit":%d,"proc_wall_s":%d}\n' "$tag" "$kind" "$mode" "$code" $((SECONDS - t0)) >> "$OUT/driver.jsonl"
    echo "$tag exit=$code $((SECONDS - t0))s"
}

# The pipeline disk cache is keyed by XDG_CACHE_HOME. Cold runs get a fresh empty one; warm runs share one
# per case, primed by an untimed run the first time it is needed.
run_kind() {
    local key=$1 tag=$2 kind=$3 mode=$4
    shift 4
    grep -qs RESULT "$OUT/$tag.log" && return 0
    settle
    case $kind in
        a-cold)
            local cold
            cold=$(mktemp -d "$OUT/xdg-cold.XXXXXX")
            run_session "$tag" "$kind" "$mode" "$cold" "$@"
            rm -rf "$cold"
            ;;
        *)
            local warm="$OUT/xdg-warm/$key"
            if [ ! -e "$warm/.primed" ]; then
                run_session "prime-$key" a-warm auto "$warm" "$@"
                rm -f "$OUT/prime-$key.prof".* "$OUT/prime-$key.marks.jsonl"
                touch "$warm/.primed"
                settle
            fi
            run_session "$tag" "$kind" "$mode" "$warm" "$@"
            ;;
    esac
}

run_node_case() {
    local c=$1 topo=$2 size=$3 kind=$4 mode=$5 round=$6 key tag frames
    key=$topo-$c-$(size_label "$size")
    tag=$key-$mode-$kind-r$round
    frames=$NEED_NODE
    [ "$topo" = gxg ] && frames=$NEED_GXG
    fits "$(need_kb_for "$size" "$frames" "$kind")" "$tag" || return 0
    [ "$DRY" = 1 ] && return 0
    local res=uhd sz=""
    [ "$size" != uhd ] && { res=s$size; sz=$size; }
    # shellcheck disable=SC2086
    run_kind "$key" "$tag" "$kind" "$mode" BENCH_TOPO="$topo" BENCH_RES="$res" BENCH_SIZE="$sz" ${CASES[$c]}
}

run_comp_case() {
    local comp=$1 size=$2 kind=$3 mode=$4 round=$5 key tag need
    key=comp-$comp-$(size_label "$size")
    tag=$key-$mode-$kind-r$round
    need=$(comp_need_kb "$size")
    [ "$kind" = playback ] && need=$((need * 2))
    fits "$need" "$tag" || return 0
    [ "$DRY" = 1 ] && return 0
    local sz=""
    [ "$size" != uhd ] && sz=$size
    run_kind "$key" "$tag" "$kind" "$mode" BENCH_TOPO=ntp BENCH_COMP="$REPO/tools/bench/comps/$comp.ntp" \
        BENCH_RES=uhd BENCH_SIZE="$sz"
}

mode_order() { if [ $(($1 % 2)) -eq 1 ]; then echo "off auto"; else echo "auto off"; fi; }

if [ "$DRY" != 1 ]; then
    [ -x "$APPIMAGE" ] || { echo "not executable: $APPIMAGE" >&2; exit 2; }
    mkdir -p "$OUT" "$RENDER_BASE"
fi

echo "gpu_region_bench: rounds=$ROUNDS kinds=\"$KINDS\" out=$OUT render_base=$RENDER_BASE appimage=$APPIMAGE"
for r in $(seq 1 "$ROUNDS"); do
    modes=$(mode_order "$r")
    for kind in $KINDS; do
        for size in $SIZES; do
            for c in $NODES; do
                for topo in node gxg; do
                    for m in $modes; do
                        run_node_case "$c" "$topo" "$size" "$kind" "$m" "$r"
                    done
                done
            done
        done
        for comp in $COMPS; do
            for size in $COMP_SIZES; do
                for m in $modes; do
                    run_comp_case "$comp" "$size" "$kind" "$m" "$r"
                done
            done
        done
    done
done
echo "gpu_region_bench: finished"
