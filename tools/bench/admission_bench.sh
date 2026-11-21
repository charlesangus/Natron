#!/usr/bin/env bash
# Both cache admission policies over the edit-latency benchmark, with NATRON_GPU=off.
#
#   tools/bench/admission_bench.sh <Natron.AppImage> [out-dir]
#
# Runs tools/bench/edit_latency.sh with this matrix, each list overridable through the same variable:
#   BENCH_REPS=3                         outermost: the whole matrix again
#   BENCH_SCENARIOS="chain offbranch"    the viewed chain alone, and the chain with off-branch work
#   BENCH_CASES="grade hsv"
#   BENCH_RAMS="default 1"               maxRAMPercent: as shipped, and low enough to evict and demote
#   BENCH_POLICIES="all priorityFloor"   innermost, so the policies alternate run by run
# with BENCH_ZOOMS=fit, BENCH_PANELS=closed, BENCH_GPUS=off, BENCH_AUTOPROXYS=off and BENCH_EDITS=10.
#
# Results: <out-dir>/edit_latency.jsonl (default out-dir build/bench/admission), one line per edit, and one
# directory per run. The AppImage is extracted under <out-dir>/tmp, so the throwaway disk tier is on that
# disk rather than on a /tmp that may be tmpfs. The out-dir has to be new: lines append, and the attribution
# expects one render profile per run.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [out-dir]" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${2:-$REPO/build/bench/admission}")
if [ -e "$OUT/edit_latency.jsonl" ]; then
    echo "$OUT already holds results; pass a new out-dir" >&2
    exit 2
fi
mkdir -p "$OUT"

export BENCH_REPS=${BENCH_REPS:-3}
export BENCH_SCENARIOS=${BENCH_SCENARIOS:-chain offbranch}
export BENCH_CASES=${BENCH_CASES:-grade hsv}
export BENCH_RAMS=${BENCH_RAMS:-default 1}
export BENCH_POLICIES=${BENCH_POLICIES:-all priorityFloor}
export BENCH_ZOOMS=${BENCH_ZOOMS:-fit}
export BENCH_PANELS=${BENCH_PANELS:-closed}
export BENCH_GPUS=${BENCH_GPUS:-off}
export BENCH_AUTOPROXYS=${BENCH_AUTOPROXYS:-off}
export BENCH_EDITS=${BENCH_EDITS:-10}
export BENCH_TMPDIR=${BENCH_TMPDIR:-$OUT/tmp}
mkdir -p "$BENCH_TMPDIR"

{
    echo "host=$(hostname) date=$(date -Is) nproc=$(nproc)"
    grep -m1 'model name' /proc/cpuinfo
    grep -m1 MemTotal /proc/meminfo
    df -hT "$BENCH_TMPDIR" | tail -n 1
    env | grep -E '^BENCH_' | sort
} > "$OUT/host.txt"

exec "$REPO/tools/bench/edit_latency.sh" "$APPIMAGE" "$OUT"
