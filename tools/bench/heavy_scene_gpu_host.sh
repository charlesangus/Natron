#!/usr/bin/env bash
# Usage: tools/bench/heavy_scene_gpu_host.sh <Natron.AppImage> [results.jsonl]
#
# The M327 matrix at 1M prims on a host with a real GPU and a desktop session: every scenario for
# the filter and session mechanisms on the flat and city fixtures, session with
# NATRON_DRAW_STATE_RESTORE=value, and the P3 load and re-match runs in both load modes. One JSON
# line per scenario goes to the results file (default build/bench-gpu/m327-1m.jsonl). Frame rates
# are valid here (BENCH_GPU=1), so orbit_fps and first_boxes_ms are judged against the budgets.
#
# Needs: DISPLAY (and XAUTHORITY) of the GPU's X session, a pxr-capable python3 for the fixtures
# unless FIXTURE_DIR already holds heavy_flat.usdc, heavy_city.usda and heavy_city_payloads/ made
# with the default --prims 1000000, and enough RAM for the fully loaded 1M city.
# Run it from a checkout of spike/m327-hydra-filter; it never builds anything.
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <Natron.AppImage> [results.jsonl]" >&2
    exit 2
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
APPIMAGE="$(readlink -f "$1")"
RESULT="$(readlink -m "${2:-$ROOT/build/bench-gpu/m327-1m.jsonl}")"
FIXTURE_DIR="$(readlink -m "${FIXTURE_DIR:-$ROOT/build/bench-gpu}")"
: "${DISPLAY:?set DISPLAY to the X session of the GPU host}"
mkdir -p "$(dirname "$RESULT")" "$FIXTURE_DIR/out"
: > "$RESULT"

if [ ! -f "$FIXTURE_DIR/heavy_flat.usdc" ] || [ ! -f "$FIXTURE_DIR/heavy_city.usda" ]; then
    python3 tools/bench/heavy_scene_make.py --out "$FIXTURE_DIR" || exit 1
fi

# The gate refuses a software renderer, which is what keeps llvmpipe numbers out of this file.
export GUI_TEST_HOST_DISPLAY="$DISPLAY"
export BENCH_GPU=1
export BENCH_RESULT="$RESULT"
export BENCH_TIMEOUT_S=1800
export GUI_TEST_TIMEOUT=7200

ALL=populate_full,prune_restore,sibling_state,noedit,reveal_cancel,reveal,orbit10k

# One fresh process per call: populate_pruned must be the only scenario, and the load actions
# compare cold page caches only within a process.
run() {
    local label="$1" fixture="$2" scenarios="$3"
    shift 3
    local file="$FIXTURE_DIR/heavy_flat.usdc"
    [ "$fixture" = city ] && file="$FIXTURE_DIR/heavy_city.usda"
    echo "=== $label"
    env BENCH_FIXTURE="$fixture" BENCH_FILE="$file" BENCH_SCENARIOS="$scenarios" BENCH_LABEL="$label" \
        NATRON_GUI_TEST_OUT="$FIXTURE_DIR/out/$label" "$@" \
        Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py "$APPIMAGE" > "$FIXTURE_DIR/out/$label.log" 2>&1
    echo "    exit=$?"
}

for fixture in flat city; do
    for mech in filter session; do
        run "$fixture-$mech-all" "$fixture" "$ALL" NATRON_DRAW_STATE_MECHANISM=$mech
        run "$fixture-$mech-pruned" "$fixture" populate_pruned NATRON_DRAW_STATE_MECHANISM=$mech
    done
    run "$fixture-session-restorevalue" "$fixture" populate_full,prune_restore,sibling_state,noedit \
        NATRON_DRAW_STATE_MECHANISM=session NATRON_DRAW_STATE_RESTORE=value
done
run city-filter-loadnone-all city "$ALL" NATRON_DRAW_STATE_MECHANISM=filter NATRON_DRAW_STATE_LOAD_NONE=1
run city-filter-loadnone-pruned city populate_pruned NATRON_DRAW_STATE_MECHANISM=filter NATRON_DRAW_STATE_LOAD_NONE=1

for mech in filter session; do
    for mode in preopen direct; do
        run "p3-$mech-$mode-load" city \
            "populate_full,load_building_$mode,load_block_$mode,load_orbit,load_hover" \
            NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=1 BENCH_LOAD_MODE=$mode
        run "p3-$mech-$mode-chain" city \
            open_chain,chain_load,rematch_chain,rematch_nomatch,rematch_match,rematch_offset \
            NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=1 BENCH_LOAD_MODE=$mode
    done
done

echo "results: $RESULT ($(wc -l < "$RESULT") lines)"
