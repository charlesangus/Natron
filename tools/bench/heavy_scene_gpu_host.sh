#!/usr/bin/env bash
# Usage: tools/bench/heavy_scene_gpu_host.sh settle <Natron.AppImage> [results.jsonl]
#        tools/bench/heavy_scene_gpu_host.sh all <Natron.AppImage> [results.jsonl]
#        tools/bench/heavy_scene_gpu_host.sh fixtures
#
# Heavy scene runs on a host with a real GPU and a desktop session. One JSON line per scenario goes
# to the results file (default build/bench-gpu/heavy-scene-<mode>.jsonl; an earlier file is kept as
# <file>.prev). Frame rates are valid here (BENCH_GPU=1), so orbit_fps and first_frame_ms are judged
# against the budgets.
#
#   settle    The short run. Session with NATRON_DRAW_STATE_LOAD_NONE=0 on the city, then session and
#             filter on Lobby with it loaded.
#   all       The full matrix: filter and session on the flat and city fixtures, session with
#             NATRON_DRAW_STATE_RESTORE=value, the load and re-match runs in both load modes, then
#             Lobby and Island for filter and session. Island is last because it is the heaviest.
#   fixtures  Only makes the fixtures; needs neither a display nor an AppImage.
#
# A scenario that outlasts its limit gets a timed_out record and ends its process, so the matrix moves
# on: BENCH_SCENARIO_LIMIT_S (default 300) for a state change or a reveal, BENCH_OPEN_LIMIT_S (default
# 1800) for one that opens the file. A scenario that does not apply to a scene gets a skipped record.
# Each sibling_shot scenario leaves screen grabs in $FIXTURE_DIR/out/<label>/. RAM: the 1M fixtures
# passed 12 GB under llvmpipe and need that much at least; Lobby peaked under 5 GB; Island, below.
#
# Needs: DISPLAY (and XAUTHORITY) of the GPU's X session, and the AppImage. No system PySide6 or pxr:
# the scenes are read with the AppImage's own Python. Docker and the published dev image are needed
# only while FIXTURE_DIR lacks the fixtures.
#
# Environment:
#   FIXTURE_DIR        where fixtures and per-run output go (default build/bench-gpu)
#   FIXTURE_MAKE_ARGS  extra heavy_scene_make.py arguments, split on spaces (default none: 1M prims)
#   DEV_IMAGE          image that makes the fixtures (default what tools/ci/local/dev-image-tag.sh prints)
#   SCENE_DIR          holds World_Lobby.usd and usd/island.usda (default .local of the main checkout)
#   ISLAND_MIN_RAM_GB  available RAM below which Island is skipped (default 24)
#
# Run it from a checkout of the bench's branch; it never builds anything.
set -u

usage() {
    echo "usage: $0 settle|all <Natron.AppImage> [results.jsonl]" >&2
    echo "       $0 fixtures" >&2
    exit 2
}

[ $# -ge 1 ] || usage
MODE="$1"
case "$MODE" in
    settle|all) [ $# -ge 2 ] || usage ;;
    fixtures) ;;
    *) usage ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
FIXTURE_DIR="$(readlink -m "${FIXTURE_DIR:-$ROOT/build/bench-gpu}")"
mkdir -p "$FIXTURE_DIR/out"

have_fixtures() {
    [ -f "$FIXTURE_DIR/heavy_flat.usdc" ] && [ -f "$FIXTURE_DIR/heavy_city.usda" ] && [ -d "$FIXTURE_DIR/heavy_city_payloads" ]
}

# pxr lives in the dev image, not on the host. The checkout is mounted at its own path so the paths
# the fixtures record are the paths the host reads them from.
make_fixtures() {
    if have_fixtures; then
        echo "fixtures: present in $FIXTURE_DIR"
        return 0
    fi
    local image="${DEV_IMAGE:-$(tools/ci/local/dev-image-tag.sh)}"
    if [ -z "$image" ]; then
        echo "fixtures: tools/ci/local/dev-image-tag.sh printed no tag; set DEV_IMAGE" >&2
        return 1
    fi
    if ! command -v docker > /dev/null 2>&1; then
        echo "fixtures: $FIXTURE_DIR lacks the fixtures and making them needs docker, which is not on PATH." >&2
        echo "          Install docker, or copy heavy_flat.usdc, heavy_city.usda and heavy_city_payloads/ there." >&2
        return 1
    fi
    if ! docker image inspect "$image" > /dev/null 2>&1; then
        echo "fixtures: docker has no image $image (or cannot reach its daemon)." >&2
        echo "          Run: docker pull $image" >&2
        echo "          or point DEV_IMAGE at another image with pxr." >&2
        return 1
    fi
    echo "fixtures: making them in $image"
    # shellcheck disable=SC2086
    docker run --rm --entrypoint python3 -u "$(id -u):$(id -g)" -e HOME=/tmp -e PYTHONDONTWRITEBYTECODE=1 \
        -v "$ROOT:$ROOT" -v "$FIXTURE_DIR:$FIXTURE_DIR" -w "$ROOT" "$image" \
        tools/bench/heavy_scene_make.py --out "$FIXTURE_DIR" ${FIXTURE_MAKE_ARGS:-}
}

if [ "$MODE" = fixtures ]; then
    make_fixtures
    exit $?
fi

: "${DISPLAY:?set DISPLAY to the X session of the GPU host}"
[ -e "$2" ] || { echo "no AppImage at $2" >&2; exit 2; }
APPIMAGE="$(readlink -f "$2")"
RESULT="$(readlink -m "${3:-$ROOT/build/bench-gpu/heavy-scene-$MODE.jsonl}")"
mkdir -p "$(dirname "$RESULT")"
make_fixtures || exit 1
[ -s "$RESULT" ] && mv -f "$RESULT" "$RESULT.prev"
: > "$RESULT"

if [ -z "${SCENE_DIR:-}" ]; then
    SCENE_DIR="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")/.local"
fi
LOBBY="$SCENE_DIR/World_Lobby.usd"
ISLAND="$SCENE_DIR/usd/island.usda"

# The gate refuses a software renderer, which is what keeps llvmpipe numbers out of this file.
export GUI_TEST_HOST_DISPLAY="$DISPLAY"
export BENCH_GPU=1
export BENCH_RESULT="$RESULT"
export BENCH_ROWS_DIR="$FIXTURE_DIR/rows"
export BENCH_TIMEOUT_S=1800
export GUI_TEST_TIMEOUT=7200

ALL=populate_full,prune_restore,sibling_state,sibling_shot,noedit,reveal_cancel,reveal,orbit10k
SETTLE=populate_full,prune_restore,sibling_state,sibling_shot,reveal_cancel,orbit10k

record_skip() {
    printf '{"label": "%s", "scenario": "*", "result": "skipped", "reason": "%s"}\n' "$1" "$2" >> "$RESULT"
    echo "=== $1: skipped: $2"
}

# One fresh process per call: populate_pruned must be the only scenario, and the load actions
# compare cold page caches only within a process.
launch() {
    local label="$1" fixture="$2" file="$3" scenarios="$4" rc
    shift 4
    echo "=== $label"
    env BENCH_FIXTURE="$fixture" BENCH_FILE="$file" BENCH_SCENARIOS="$scenarios" BENCH_LABEL="$label" \
        NATRON_GUI_TEST_OUT="$FIXTURE_DIR/out/$label" "$@" \
        Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py "$APPIMAGE" > "$FIXTURE_DIR/out/$label.log" 2>&1
    rc=$?
    echo "    exit=$rc"
    # The bench's watchdog starts once the scene's rows are picked; a process lost before then has
    # told the results file nothing.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ] && ! grep -q "\"label\": \"$label\"" "$RESULT"; then
        printf '{"label": "%s", "scenario": "*", "result": "aborted", "exit_code": %d, "reason": "ended before any scenario; see %s"}\n' \
            "$label" "$rc" "$FIXTURE_DIR/out/$label.log" >> "$RESULT"
    fi
}

run() {
    local label="$1" fixture="$2" scenarios="$3" file="$FIXTURE_DIR/heavy_flat.usdc"
    shift 3
    [ "$fixture" = city ] && file="$FIXTURE_DIR/heavy_city.usda"
    launch "$label" "$fixture" "$file" "$scenarios" "$@"
}

run_load_scenarios() {
    local prefix="$1" fixture="$2" file="$3" mech mode
    for mech in filter session; do
        for mode in preopen direct; do
            launch "$prefix-$mech-$mode-load" "$fixture" "$file" \
                "populate_full,load_building_$mode,load_block_$mode,load_orbit,load_hover" \
                NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=1 BENCH_LOAD_MODE=$mode
            launch "$prefix-$mech-$mode-chain" "$fixture" "$file" \
                open_chain,chain_load,rematch_chain,rematch_nomatch,rematch_match,rematch_offset \
                NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=1 BENCH_LOAD_MODE=$mode
        done
    done
}

run_scene() {
    local name="$1" file="$2" scenarios="$3" mech
    shift 3
    if [ ! -f "$file" ]; then
        record_skip "$name" "no such file: $file (set SCENE_DIR)"
        return
    fi
    for mech in filter session; do
        launch "$name-$mech-all" scene "$file" "$scenarios" \
            NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=0 "$@"
        launch "$name-$mech-pruned" scene "$file" populate_pruned \
            NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=0 "$@"
    done
    run_load_scenarios "$name" scene "$file"
}

# Lobby's state changes finish within the default reveal batch of 2000 rows, leaving nothing to cancel.
LOBBY_ENV=(BENCH_REVEAL_BATCH=100)

if [ "$MODE" = settle ]; then
    run settle-city-session city "$SETTLE" NATRON_DRAW_STATE_MECHANISM=session NATRON_DRAW_STATE_LOAD_NONE=0
    if [ -f "$LOBBY" ]; then
        for mech in session filter; do
            launch "settle-lobby-$mech" scene "$LOBBY" "$SETTLE" \
                NATRON_DRAW_STATE_MECHANISM=$mech NATRON_DRAW_STATE_LOAD_NONE=0 "${LOBBY_ENV[@]}"
        done
    else
        record_skip settle-lobby "no such file: $LOBBY (set SCENE_DIR)"
    fi
    echo "results: $RESULT ($(wc -l < "$RESULT") lines); screen grabs in $FIXTURE_DIR/out/settle-*"
    exit 0
fi

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
run_load_scenarios p3 city "$FIXTURE_DIR/heavy_city.usda"

run_scene lobby "$LOBBY" "$ALL" "${LOBBY_ENV[@]}"

# 24 GB is twice an estimate of 12 GB, from a per-element tally outside Natron: about 3.5 GB of
# composed stages with their geometry read, 2 GB of instance transforms for 31M point instances,
# 2 GB of buffers for 37M points and 152M face vertices, 3.5 GB for 119k meshes at Hydra's cost per
# prim, 1 GB of Natron. Doubled because Island has never been loaded in Natron to measure it.
ISLAND_MIN_RAM_GB="${ISLAND_MIN_RAM_GB:-24}"
AVAILABLE_GB=$(($(sed -n 's/^MemAvailable: *\([0-9]*\) kB/\1/p' /proc/meminfo) / 1048576))
if [ "$AVAILABLE_GB" -ge "$ISLAND_MIN_RAM_GB" ]; then
    echo "island: $AVAILABLE_GB GB available, $ISLAND_MIN_RAM_GB GB wanted (about 12 GB estimated, doubled)"
    run_scene island "$ISLAND" "$ALL"
else
    record_skip island "$AVAILABLE_GB GB available, under the $ISLAND_MIN_RAM_GB GB estimated for it (set ISLAND_MIN_RAM_GB to override)"
fi

echo "results: $RESULT ($(wc -l < "$RESULT") lines)"
