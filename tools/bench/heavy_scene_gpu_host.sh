#!/usr/bin/env bash
# Usage: tools/bench/heavy_scene_gpu_host.sh gate <Natron.AppImage> [results.jsonl]
#        tools/bench/heavy_scene_gpu_host.sh budget <Natron.AppImage> [results.jsonl]
#        tools/bench/heavy_scene_gpu_host.sh acceptance <Natron.AppImage> [results.jsonl]
#        tools/bench/heavy_scene_gpu_host.sh fixtures
#
# Heavy scene runs on a host with a real GPU and a desktop session. One JSON line per scenario goes
# to the results file (default build/bench-gpu/heavy-scene-<mode>.jsonl; an earlier file is kept as
# <file>.prev). Frame rates are valid here (BENCH_GPU=1), so first_frame_ms is judged against its
# budget, and every record names the GPU and the memory used.
#
#   gate      What the deferred viewer stage has to pass, one fresh process per line, so each load
#             starts from an unloaded stage:
#               flat            open
#               city building   open, then one building's load with the mouse moving throughout
#               city block      open, then one block with its descendants
#               city larger     open, then GATE_LARGE_BLOCKS blocks with their descendants at once,
#                               the load the continuity of mouse moves is judged on
#               city twirl      open, then one building twirled open in the Scene Graph panel
#               lobby           the same building, block and twirl runs on Lobby's own rows
#               island          building and twirl, last, and only with RAM to spare
#             and what the viewer draw states have to pass:
#               draw flat pruned   populate_pruned
#               draw flat          populate_full, prune_restore, sibling_state, noedit, reveal_cancel,
#                                  reveal, sibling_shot, orbit10k, boxes_shot, draw_budget,
#                                  reveal_wide, reveal_cancel_paced
#               draw city pruned   populate_pruned, then load_all under the pruned root
#               draw city          populate_full, load_all, then the flat run's scenarios
#               draw city reveal   populate_full, then reveal_load_all at the root of the unloaded
#                                  stage, the loads joining the paced reveal as they land
#               draw lobby         as draw city, on Lobby's own rows
#               draw island        populate_full, prune_restore, noedit, draw_budget, under island's
#                                  RAM guard
#             The draw runs other than pruned ones set NATRON_DRAW_STATE_TIMING=1.
#   budget    Only where a draw state change's time goes, with NATRON_DRAW_STATE_TIMING=1, one process
#             per scene: draw_budget on the sibling and the widest row, after populate_full on flat,
#             populate_full and load_all on city and Lobby, and populate_full on Island under its RAM
#             guard. Ends with heavy_scene_budget_table.py's table of the results: time to picture and
#             stall against 100 ms per row, the untraced critical path, and the traced stages.
#   acceptance  The end-to-end budget run, with NATRON_DRAW_STATE_TIMING=1, one process per line: the
#             workflow scenario (open, twirl, Full on an asset, Full + Load on a block, a pattern node, a
#             node switch, a prim in the inspector, save, reopen) on flat, the city, Lobby and Island, each
#             step judged against its budget; then reveal and cancel on the loaded city, Lobby and Island
#             (populate_full, load_all, reveal_wide, reveal_cancel_paced, orbit10k), and reveal_load_all
#             on the unloaded city. Island is skipped, with a record, when it is not found or under
#             ISLAND_MIN_RAM_GB; its loaded run picks rows from the fully loaded scene, so
#             BENCH_PICK_LIMIT_S is raised for it (default 3600).
#   fixtures Only makes the fixtures; needs neither a display nor an AppImage.
#
# A scenario that outlasts its limit gets a timed_out record and ends its process, so the run moves
# on: BENCH_SCENARIO_LIMIT_S (default 300) for a load or a twirl, BENCH_OPEN_LIMIT_S (default 1800) for
# one that opens the file. A scenario that does not apply to a scene gets a skipped record.
#
# Needs: DISPLAY (and XAUTHORITY) of the GPU's X session, and the AppImage. No system PySide6 or pxr:
# the scenes are read with the AppImage's own Python. Docker and the published dev image are needed
# only while FIXTURE_DIR lacks the fixtures.
#
# Environment:
#   BENCH_MEM_FLOOR_GB, BENCH_MEM_LIMIT_GB, BENCH_SWAP_GROWTH_GB  see below
#   FIXTURE_DIR        where fixtures and per-run output go (default build/bench-gpu)
#   FIXTURE_MAKE_ARGS  extra heavy_scene_make.py arguments, split on spaces (default none: 1M prims)
#   DEV_IMAGE          image that makes the fixtures (default what tools/ci/local/dev-image-tag.sh prints)
#   SCENE_DIR          searched for World_Lobby.usd and island.usda by name, a few levels deep (default
#                      .local of the main checkout); the one beside an Assembly, Assets or Materials
#                      directory wins when several match, and the choice is printed
#   LOBBY_DIR, ISLAND_DIR  each replaces SCENE_DIR for that scene alone. A directory is searched by name
#                      as above; a path to the file itself is used as is. Relative paths resolve as
#                      SCENE_DIR does. What each scene resolved to is printed.
#   GATE_LARGE_BLOCKS  how many blocks the larger load asks for, from Block2 up (default 30, a tenth of
#                      the default city); a city made smaller through FIXTURE_MAKE_ARGS needs fewer
#   ONLY_LABELS        space or comma separated labels (budget-island, gate-draw-city, ...); the other
#                      runs are left out, with no record. For profiling one run.
#   ONLY_SCENARIOS     comma separated scenarios that replace the chosen runs' own list (budget-island
#                      with populate_full alone, say); needs ONLY_LABELS
#   BENCH_HOLD_FILE    passed to the run: the populate scenarios report "BEGIN populate" and wait (up to
#                      BENCH_HOLD_S, default 120) for this file to exist before they open the file, so a
#                      stack sampler can attach first; BENCH_PTRACER_ANY=1 lets any process of the user attach
#   ISLAND_MIN_RAM_GB  available RAM below which Island is skipped (default 24)
#
# Memory guard: heavy_scene_watchdog.sh polls once a second and kills the run with SIGKILL when
# MemAvailable falls under BENCH_MEM_FLOOR_GB (default the larger of 4 and 10% of MemTotal), when the
# AppImage's process tree passes BENCH_MEM_LIMIT_GB RSS (default MemTotal minus the floor), or when
# swap in use grows by more than BENCH_SWAP_GROWTH_GB (default 2) during a scenario. The scenario gets
# an out_of_memory record, the rest of that run's scenarios get skipped, and the run moves on.
# Every run starts under nice -n 5 and prints the host's MemTotal, MemAvailable, swap and VRAM first.
#
# The guard cannot catch a GPU driver hang or reset: those stall the machine without using RAM, and
# nothing in user space is left to run. After a reboot, tell the two apart with
#   journalctl -b -1 -k | grep -iE 'out of memory|oom|amdgpu|gpu reset|hung'
# An OOM-killer line points at memory; amdgpu, "gpu reset" or "hung task" lines point at the driver.
#
# Run it from a checkout of the bench's branch; it never builds anything.
set -u

usage() {
    echo "usage: $0 gate <Natron.AppImage> [results.jsonl]" >&2
    echo "       $0 budget <Natron.AppImage> [results.jsonl]" >&2
    echo "       $0 acceptance <Natron.AppImage> [results.jsonl]" >&2
    echo "       $0 fixtures" >&2
    exit 2
}

[ $# -ge 1 ] || usage
MODE="$1"
case "$MODE" in
    gate | budget | acceptance) [ $# -ge 2 ] || usage ;;
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
SCENE_SEARCH_DEPTH="${SCENE_SEARCH_DEPTH:-5}"

# Prints the absolute path of the best file called $2 under $1. Among several, the one whose directory
# or parent holds the most of the sibling directories a scene of that name ships with ($3...).
find_scene() {
    local dir="$1" name="$2" best="" best_score=-1 score file here parent sibling
    shift 2
    if [ ! -d "$dir" ]; then
        echo "scene: $dir is not a directory or file" >&2
        return 1
    fi
    dir="$(cd "$dir" && pwd)"
    while IFS= read -r file; do
        here="$(dirname "$file")"
        parent="$(dirname "$here")"
        score=0
        for sibling in "$@"; do
            { [ -d "$here/$sibling" ] || [ -d "$parent/$sibling" ]; } && score=$((score + 1))
        done
        if [ "$score" -gt "$best_score" ]; then
            best="$file"
            best_score="$score"
        fi
        echo "scene: candidate $file (matches $score of $# sibling directories)" >&2
    done < <(find "$dir" -maxdepth "$SCENE_SEARCH_DEPTH" -type f -name "$name" 2> /dev/null | LC_ALL=C sort)
    if [ -z "$best" ]; then
        echo "scene: no $name within $SCENE_SEARCH_DEPTH levels of $dir (set SCENE_DIR or the scene's own *_DIR)" >&2
        return 1
    fi
    echo "scene: using $best" >&2
    printf '%s\n' "$best"
}

LOBBY_SRC="${LOBBY_DIR:-$SCENE_DIR}"
ISLAND_SRC="${ISLAND_DIR:-$SCENE_DIR}"

# A path to the file itself is used as is; anything else is searched by name.
resolve_scene() {
    local src="$1"
    shift
    if [ -f "$src" ]; then
        src="$(readlink -f "$src")"
        echo "scene: using $src (given as a file)" >&2
        printf '%s\n' "$src"
        return
    fi
    find_scene "$src" "$@"
}

LOBBY="$(resolve_scene "$LOBBY_SRC" World_Lobby.usd Assembly Assets Materials Source)" || LOBBY=""
ISLAND="$(resolve_scene "$ISLAND_SRC" island.usda elements materials)" || ISLAND=""
echo "scene: lobby  -> ${LOBBY:-not found (looked in $LOBBY_SRC)}" >&2
echo "scene: island -> ${ISLAND:-not found (looked in $ISLAND_SRC)}" >&2

# run-gui-test.sh refuses a software renderer on a host display, which keeps llvmpipe numbers out of this file.
export GUI_TEST_HOST_DISPLAY="$DISPLAY"
export BENCH_GPU=1
export BENCH_RESULT="$RESULT"
export BENCH_ROWS_DIR="$FIXTURE_DIR/rows"
export BENCH_TIMEOUT_S=1800
export GUI_TEST_TIMEOUT=7200

# What the display reports, for a record no Natron process lived to write.
host_gpu_name() {
    command -v glxinfo > /dev/null 2>&1 || return 0
    timeout 5 glxinfo -B 2> /dev/null | sed -n 's/^ *OpenGL renderer string: *//p' | head -n 1 | tr -d "\"\\\\"
}
HOST_GPU="$(host_gpu_name)"

# True when ONLY_LABELS is unset or names the label.
wanted() {
    [ -z "${ONLY_LABELS:-}" ] && return 0
    case " ${ONLY_LABELS//,/ } " in
        *" $1 "*) return 0 ;;
    esac
    return 1
}

record_skip() {
    wanted "$1" || return 0
    printf '{"label": "%s", "scenario": "*", "result": "skipped", "reason": "%s", "peak_rss_mb": null, "gl_renderer": "%s"}\n' \
        "$1" "$2" "$HOST_GPU" >> "$RESULT"
    echo "=== $1: skipped: $2"
}

meminfo_kb() {
    sed -n "s/^$1: *\\([0-9]*\\) kB/\\1/p" /proc/meminfo
}

# Whichever source answers first; none of them is required.
vram_report() {
    local card total
    if command -v glxinfo > /dev/null 2>&1; then
        total="$(timeout 5 glxinfo -B 2> /dev/null | sed -n 's/^ *Video memory: *//p' | head -n 1)"
        [ -n "$total" ] && { echo "$total"; return; }
    fi
    for card in /sys/class/drm/card*/device/mem_info_vram_total; do
        [ -r "$card" ] || continue
        echo "$(($(cat "$card") / 1048576)) MB ($card)"
        return
    done
    echo "unknown"
}

host_report() {
    echo "    host: MemTotal $(($(meminfo_kb MemTotal) / 1024)) MB, MemAvailable $(($(meminfo_kb MemAvailable) / 1024)) MB, swap in use $((($(meminfo_kb SwapTotal) - $(meminfo_kb SwapFree)) / 1024)) MB, VRAM $(vram_report), GPU ${HOST_GPU:-unknown}"
}

launch() {
    local label="$1" fixture="$2" file="$3" scenarios="$4" rc
    shift 4
    wanted "$label" || return 0
    [ -n "${ONLY_SCENARIOS:-}" ] && scenarios="$ONLY_SCENARIOS"
    echo "=== $label"
    host_report
    nice -n 5 env BENCH_FIXTURE="$fixture" BENCH_FILE="$file" BENCH_SCENARIOS="$scenarios" BENCH_LABEL="$label" \
        NATRON_GUI_TEST_OUT="$FIXTURE_DIR/out/$label" "$@" \
        Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py "$APPIMAGE" > "$FIXTURE_DIR/out/$label.log" 2>&1
    rc=$?
    echo "    exit=$rc"
    # The bench's watchdog starts once the scene's rows are picked; a process lost before then has
    # told the results file nothing.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ] && ! grep -q "\"label\": \"$label\"" "$RESULT"; then
        printf '{"label": "%s", "scenario": "*", "result": "aborted", "exit_code": %d, "reason": "ended before any scenario; see %s", "peak_rss_mb": null, "gl_renderer": "%s"}\n' \
            "$label" "$rc" "$FIXTURE_DIR/out/$label.log" "$HOST_GPU" >> "$RESULT"
    fi
}

CITY="$FIXTURE_DIR/heavy_city.usda"
LARGE_BLOCKS=""
for ((block = 2; block < 2 + ${GATE_LARGE_BLOCKS:-30}; block++)); do
    LARGE_BLOCKS="$LARGE_BLOCKS${LARGE_BLOCKS:+,}/World/Block$block"
done

# 24 GB is twice an estimate of 12 GB, from a per-element tally outside Natron: about 3.5 GB of
# composed stages with their geometry read, 2 GB of instance transforms for 31M point instances,
# 2 GB of buffers for 37M points and 152M face vertices, 3.5 GB for 119k meshes at Hydra's cost per
# prim, 1 GB of Natron. Doubled because Island has never been loaded in Natron to measure it.
ISLAND_MIN_RAM_GB="${ISLAND_MIN_RAM_GB:-24}"

# Prints why Island cannot run, or nothing when it can.
island_unfit() {
    local available_gb=$(($(meminfo_kb MemAvailable) / 1048576))
    if [ ! -f "$ISLAND" ]; then
        echo "not found by name under '$ISLAND_SRC', $SCENE_SEARCH_DEPTH levels deep (set SCENE_DIR or ISLAND_DIR)"
    elif [ "$available_gb" -lt "$ISLAND_MIN_RAM_GB" ]; then
        echo "$available_gb GB available, under the $ISLAND_MIN_RAM_GB GB estimated for it (set ISLAND_MIN_RAM_GB to override)"
    else
        echo "island: $available_gb GB available, $ISLAND_MIN_RAM_GB GB wanted (about 12 GB estimated, doubled)" >&2
    fi
}

# The timing records cost the render thread a glFinish per change, so only these runs turn them on.
budget_runs() {
    local reason
    launch budget-flat flat "$FIXTURE_DIR/heavy_flat.usdc" populate_full,draw_budget NATRON_DRAW_STATE_TIMING=1
    launch budget-city city "$CITY" populate_full,load_all,draw_budget BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
    if [ -f "$LOBBY" ]; then
        launch budget-lobby scene "$LOBBY" populate_full,load_all,draw_budget BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
    else
        record_skip budget-lobby "not found by name under '$LOBBY_SRC', $SCENE_SEARCH_DEPTH levels deep (set SCENE_DIR or LOBBY_DIR)"
    fi
    reason="$(island_unfit)"
    if [ -z "$reason" ]; then
        launch budget-island scene "$ISLAND" populate_full,draw_budget NATRON_DRAW_STATE_TIMING=1
    else
        record_skip budget-island "$reason"
    fi
}

# The workflow reopens the project, so each one runs alone in its process.
acceptance_runs() {
    local reason loaded="populate_full,load_all,reveal_wide,reveal_cancel_paced,orbit10k"
    launch accept-flat flat "$FIXTURE_DIR/heavy_flat.usdc" workflow NATRON_DRAW_STATE_TIMING=1
    launch accept-city city "$CITY" workflow NATRON_DRAW_STATE_TIMING=1
    launch accept-city-loaded city "$CITY" "$loaded" BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
    launch accept-city-reveal city "$CITY" populate_full,reveal_load_all
    if [ -f "$LOBBY" ]; then
        launch accept-lobby scene "$LOBBY" workflow NATRON_DRAW_STATE_TIMING=1
        launch accept-lobby-loaded scene "$LOBBY" "$loaded" BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
    else
        record_skip accept-lobby "not found by name under '$LOBBY_SRC', $SCENE_SEARCH_DEPTH levels deep (set SCENE_DIR or LOBBY_DIR)"
    fi
    reason="$(island_unfit)"
    if [ -z "$reason" ]; then
        launch accept-island scene "$ISLAND" workflow NATRON_DRAW_STATE_TIMING=1
        launch accept-island-loaded scene "$ISLAND" "$loaded" BENCH_LOAD_ALL=1 BENCH_PICK_LIMIT_S="${BENCH_PICK_LIMIT_S:-3600}" NATRON_DRAW_STATE_TIMING=1
    else
        record_skip accept-island "$reason"
    fi
}

summarize() {
    if command -v python3 > /dev/null 2>&1; then
        python3 tools/bench/heavy_scene_budget_table.py "$RESULT"
    fi
    echo "results: $RESULT ($(wc -l < "$RESULT") lines: $(grep -c '"result": "pass"' "$RESULT") pass, $(grep -c '"result": "fail"' "$RESULT") fail, $(grep -c -E '"result": "(timed_out|out_of_memory|aborted)"' "$RESULT") did not finish, $(grep -c '"result": "skipped"' "$RESULT") skipped)"
}

if [ "$MODE" = acceptance ]; then
    acceptance_runs
    summarize
    exit 0
fi

if [ "$MODE" = budget ]; then
    budget_runs
    summarize
    exit 0
fi

launch gate-flat flat "$FIXTURE_DIR/heavy_flat.usdc" open
launch gate-city-building city "$CITY" open,load
launch gate-city-block city "$CITY" open,load BENCH_LOAD_DESCENDANTS=1
launch gate-city-larger city "$CITY" open,load BENCH_LOAD_PATHS="$LARGE_BLOCKS" BENCH_LOAD_DESCENDANTS=1
launch gate-city-twirl city "$CITY" open,twirl

# The gate's draw runs carry draw_budget and the timing records the time to picture is judged on, so
# the budget runs need not repeat them.
DRAW_SCENARIOS="prune_restore,sibling_state,noedit,reveal_cancel,reveal,sibling_shot,orbit10k,boxes_shot,draw_budget,reveal_wide,reveal_cancel_paced"
launch gate-draw-flat-pruned flat "$FIXTURE_DIR/heavy_flat.usdc" populate_pruned
launch gate-draw-flat flat "$FIXTURE_DIR/heavy_flat.usdc" "populate_full,$DRAW_SCENARIOS" NATRON_DRAW_STATE_TIMING=1
launch gate-draw-city-pruned city "$CITY" populate_pruned,load_all BENCH_LOAD_ALL=1
launch gate-draw-city city "$CITY" "populate_full,load_all,$DRAW_SCENARIOS" BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
launch gate-draw-city-reveal city "$CITY" populate_full,reveal_load_all

if [ -f "$LOBBY" ]; then
    launch gate-lobby-building scene "$LOBBY" open,load
    launch gate-lobby-block scene "$LOBBY" open,load BENCH_LOAD_DESCENDANTS=1
    launch gate-lobby-twirl scene "$LOBBY" open,twirl
    launch gate-draw-lobby scene "$LOBBY" "populate_full,load_all,$DRAW_SCENARIOS" BENCH_LOAD_ALL=1 NATRON_DRAW_STATE_TIMING=1
else
    record_skip gate-lobby "not found by name under '$LOBBY_SRC', $SCENE_SEARCH_DEPTH levels deep (set SCENE_DIR or LOBBY_DIR)"
fi

ISLAND_REASON="$(island_unfit)"
if [ -z "$ISLAND_REASON" ]; then
    launch gate-island-building scene "$ISLAND" open,load
    launch gate-island-twirl scene "$ISLAND" open,twirl
    launch gate-draw-island scene "$ISLAND" populate_full,prune_restore,noedit,draw_budget NATRON_DRAW_STATE_TIMING=1
else
    record_skip gate-island "$ISLAND_REASON"
fi

summarize
