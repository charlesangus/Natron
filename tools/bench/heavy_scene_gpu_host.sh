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
#             step judged against its budget; then the time-to-picture budgets and reveal and cancel on
#             the loaded city, Lobby and Island (populate_full, load_all, draw_budget, reveal_wide,
#             reveal_cancel_paced, orbit10k), and reveal_load_all on the unloaded city. Ends with
#             heavy_scene_budget_table.py's table when there are draw_budget records. Island is skipped, with a record, when it is not found or under
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
#   ISLAND_MIN_RAM_GB  available RAM below which Island is skipped (default 48)
#   SAMPLE=1           samples the stacks of every thread of each run's Natron process while it runs, with
#                      eu-stack every SAMPLE_INTERVAL_S (default 0.1 s), or, without eu-stack, gdb every
#                      SAMPLE_GDB_INTERVAL_S (default 2 s), into out/<label>.stacks (pid in .stacks.pid,
#                      sampler errors in .stacks.err). Each sample stops the process while it is taken, so
#                      the records say stack_sampled and their timings are for diagnosis, not for judging.
#                      Under kernel.yama.ptrace_scope 1 the run lets the sampler attach (BENCH_PTRACER_ANY=1);
#                      under 2 only root samples. Sampling that cannot work is reported and left out; it
#                      never stops the bench. heavy_scene_stack_window.py folds a scenario's samples into
#                      the top stacks of the main and render threads. A sample eu-stack takes longer than
#                      the interval to collect delays the next one; the period the samples actually kept
#                      is the "sample period" line heavy_scene_stack_window.py prints.
#   SYMBOLS            with SAMPLE=1: the symbols of the AppImage's bundle, the natron-symbols.tar.gz that
#                      tools/ci/remote/build.sh --debug-symbols downloads with that AppImage, or the
#                      directory it unpacks to. eu-stack reads the libraries' symbols from it; Natron.bin's
#                      it cannot use (see heavy_scene_stack_window.py --symbols, which names those frames
#                      afterwards from the same directory). A tarball is unpacked into FIXTURE_DIR/symbols.
#   VRAM_CARD          the card whose memory is sampled (card1, say; default the one with the most VRAM).
#                      sample_vram.sh reads its mem_info_vram_used and mem_info_gtt_used every
#                      VRAM_INTERVAL_S (default SAMPLE_INTERVAL_S with SAMPLE=1, else 0.25 s) while each run
#                      lives, into out/<label>.vram. heavy_scene_vram.py then adds vram_peak_mb, gtt_peak_mb,
#                      vram_start_mb, vram_end_mb, vram_delta_mb, gtt_delta_mb and the totals to each of the
#                      run's records, and prints them. The counters are the device's, the desktop's included.
#   LOG_LINEBUF=0      leaves Natron's stdout block buffered. By default libstdbuf.so (coreutils) makes it
#                      line buffered, so the stamps of out/<label>/natron.ts.log, a copy of natron.log with
#                      each line prefixed by the epoch seconds it was written at, are the lines' own.
#                      out/<label>.log, the runner's output, is stamped the same way.
#   BISECT             comma separated variants that set Natron's environment for every run:
#                        lights<N>   HDST_MAX_LIGHTS=N (Storm's default is 16)
#                        nolights    USDIMAGING_ENABLE_SCENE_LIGHTS=0, no scene lights reach Storm
#                        bindless    HGIGL_ENABLE_BINDLESS_TEXTURE=1, so draw items with distinct
#                                    textures batch together
#                        noglthread  mesa_glthread=false
#                        hud         GALLIUM_HUD=GPU-load,VRAM-usage,GTT-usage,fps, dumped to out/<label>.hud/
#                        shaderlog   TF_DEBUG=HD_BUFFER_ARRAY_INFO HDST_LOG_DRAWING_SHADER_PROGRAM_MISSES
#                      Each record's bisect names them (null without BISECT); the default results file
#                      becomes heavy-scene-<mode>-<variants joined by +>.jsonl and the out/ files
#                      <label>.<variants joined by +>.*, so variants never overwrite each other.
#                      Storm has no environment setting that turns materials off: unbound material
#                      pruning and material primvar filtering are already on by default.
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

BISECT_VARIANTS="lights<N> nolights bindless noglthread hud shaderlog"
BISECT_ENV=()
BISECT_HUD=""
BISECT_TF_DEBUG=""
BISECT_NAMES=()
BISECT="${BISECT:-}"
for variant in ${BISECT//,/ }; do
    case "$variant" in
        lights[0-9] | lights[0-9][0-9] | lights[0-9][0-9][0-9]) BISECT_ENV+=(HDST_MAX_LIGHTS="${variant#lights}") ;;
        nolights) BISECT_ENV+=(USDIMAGING_ENABLE_SCENE_LIGHTS=0) ;;
        bindless) BISECT_ENV+=(HGIGL_ENABLE_BINDLESS_TEXTURE=1) ;;
        noglthread) BISECT_ENV+=(mesa_glthread=false) ;;
        hud) BISECT_HUD="GPU-load,VRAM-usage,GTT-usage,fps" ;;
        shaderlog) BISECT_TF_DEBUG="HD_BUFFER_ARRAY_INFO HDST_LOG_DRAWING_SHADER_PROGRAM_MISSES" ;;
        *)
            echo "BISECT: unknown variant '$variant' (one or more of: $BISECT_VARIANTS)" >&2
            exit 2
            ;;
    esac
    BISECT_NAMES+=("$variant")
done
[ -n "$BISECT_TF_DEBUG" ] && BISECT_ENV+=(TF_DEBUG="$BISECT_TF_DEBUG")
BISECT="$(IFS=,; echo "${BISECT_NAMES[*]}")"
BISECT_TAG="${BISECT//,/+}"
if [ -n "$BISECT" ]; then
    BISECT_JSON="\"$BISECT\""
    echo "bisect: $BISECT: ${BISECT_ENV[*]}${BISECT_HUD:+ GALLIUM_HUD=$BISECT_HUD}" >&2
else
    BISECT_JSON=null
fi

RESULT="$(readlink -m "${3:-$ROOT/build/bench-gpu/heavy-scene-$MODE${BISECT_TAG:+-$BISECT_TAG}.jsonl}")"
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

SAMPLER=""
SAMPLE_PERIOD=""
if [ "${SAMPLE:-0}" = 1 ]; then
    # shellcheck source=ptrace_remedy.sh
    # shellcheck disable=SC1091
    . "$ROOT/tools/bench/ptrace_remedy.sh"
    if command -v eu-stack > /dev/null 2>&1; then
        SAMPLER="$ROOT/tools/bench/sample_stacks.sh"
        SAMPLE_PERIOD="${SAMPLE_INTERVAL_S:-0.1}"
    elif command -v gdb > /dev/null 2>&1; then
        SAMPLER="$ROOT/tools/bench/sample_stacks_gdb.sh"
        SAMPLE_PERIOD="${SAMPLE_GDB_INTERVAL_S:-2}"
    else
        echo "sample: neither eu-stack nor gdb is installed (sudo apt install elfutils); running without stack samples" >&2
    fi
    if [ -n "$SAMPLER" ]; then
        case "$(ptrace_scope)" in
            none | 0) ;;
            1)
                # The sampler is the run's sibling, not its ancestor, so the run has to name it a tracer.
                [ "$(id -u)" = 0 ] || export BENCH_PTRACER_ANY=1
                ;;
            *)
                if [ "$(id -u)" != 0 ]; then
                    echo "sample: $(ptrace_remedy); running without stack samples" >&2
                    SAMPLER=""
                fi
                ;;
        esac
    fi
    [ -n "$SAMPLER" ] && echo "sample: $(basename "$SAMPLER") every $SAMPLE_PERIOD s, ptrace_scope $(ptrace_scope); samples go to $FIXTURE_DIR/out/<label>.stacks" >&2
    if [ -n "$SAMPLER" ] && [ -n "${SYMBOLS:-}" ]; then
        if [ -d "$SYMBOLS" ]; then
            SAMPLE_DEBUGINFO_PATH="$(readlink -f "$SYMBOLS")"
        elif [ -f "$SYMBOLS" ]; then
            rm -rf -- "${FIXTURE_DIR:?}/symbols"
            mkdir -p "$FIXTURE_DIR/symbols"
            if tar -C "$FIXTURE_DIR/symbols" -xzf "$SYMBOLS"; then
                SAMPLE_DEBUGINFO_PATH="$(readlink -f "$FIXTURE_DIR/symbols")"
            else
                echo "sample: could not unpack $SYMBOLS; sampling without its symbols" >&2
            fi
        else
            echo "sample: no symbols at $SYMBOLS; sampling without them" >&2
        fi
        if [ -n "${SAMPLE_DEBUGINFO_PATH:-}" ]; then
            export SAMPLE_DEBUGINFO_PATH
            echo "sample: library symbols from $SAMPLE_DEBUGINFO_PATH; name Natron.bin's frames with heavy_scene_stack_window.py --symbols $SAMPLE_DEBUGINFO_PATH" >&2
        fi
    fi
fi

VRAM_DEVICE="$("$ROOT/tools/bench/sample_vram.sh" --pick)" || VRAM_DEVICE=""
if [ "${SAMPLE:-0}" = 1 ]; then
    VRAM_INTERVAL_S="${VRAM_INTERVAL_S:-${SAMPLE_INTERVAL_S:-0.1}}"
else
    VRAM_INTERVAL_S="${VRAM_INTERVAL_S:-0.25}"
fi
if [ -n "$VRAM_DEVICE" ]; then
    echo "vram: $VRAM_DEVICE every $VRAM_INTERVAL_S s into $FIXTURE_DIR/out/<label>.vram" >&2
else
    echo "vram: no amdgpu mem_info_vram_used under /sys/class/drm (or not VRAM_CARD's); running without VRAM samples" >&2
fi

# Samples the Natron process of the run whose launcher is $1 until the run ends; never fails the run.
sample_run() {
    local bench="$1" out="$2" pid="" sampler
    while kill -0 "$bench" 2> /dev/null; do
        if [ -s "$out.pid" ]; then
            pid="$(head -n 1 "$out.pid")"
            kill -0 "$pid" 2> /dev/null && break
        fi
        pid=""
        sleep 0.2
    done
    [ -n "$pid" ] || return 0
    "$SAMPLER" "$pid" 1000000000 "$SAMPLE_PERIOD" "$out" > /dev/null 2>&1 &
    sampler=$!
    while kill -0 "$sampler" 2> /dev/null && kill -0 "$bench" 2> /dev/null; do
        sleep 0.5
    done
    kill "$sampler" 2> /dev/null
    wait "$sampler" 2> /dev/null
    return 0
}

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
    printf '{"label": "%s", "scenario": "*", "result": "skipped", "reason": "%s", "peak_rss_mb": null, "gl_renderer": "%s", "bisect": %s}\n' \
        "$1" "$2" "$HOST_GPU" "$BISECT_JSON" >> "$RESULT"
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

if perl -MTime::HiRes -e 1 > /dev/null 2>&1; then
    stamp_lines() { perl -MTime::HiRes=time -ne 'BEGIN { $| = 1 } printf "%.3f %s", time, $_'; }
else
    stamp_lines() {
        local line LC_ALL=C
        while IFS= read -r line || [ -n "$line" ]; do
            printf '%s %s\n' "${EPOCHREALTIME:0:-3}" "$line"
        done
    }
fi

# Natron's stdout goes to a file, so stdio buffers it in blocks and a line's stamp would be the time
# of the flush that wrote it; coreutils' stdbuf library makes it line buffered.
LINEBUF_ENV=()
if [ "${LOG_LINEBUF:-1}" = 1 ]; then
    for lib in /usr/libexec/coreutils/libstdbuf.so /usr/lib/coreutils/libstdbuf.so /usr/lib/x86_64-linux-gnu/coreutils/libstdbuf.so; do
        if [ -r "$lib" ]; then
            LINEBUF_ENV=(LD_PRELOAD="$lib${LD_PRELOAD:+:$LD_PRELOAD}" _STDBUF_O=L)
            break
        fi
    done
    [ ${#LINEBUF_ENV[@]} -gt 0 ] || echo "log: no libstdbuf.so; natron.ts.log stamps lines when stdio flushes them" >&2
fi

# run-gui-test.sh writes Natron's output to <out>/natron.log itself, so the stamped copy is taken by
# following that file rather than by piping the runner.
run_bench() {
    local label="$1" stem="$2" fixture="$3" file="$4" scenarios="$5" out runner follower rc
    shift 5
    out="$FIXTURE_DIR/out/$stem"
    mkdir -p "$out"
    : > "$out/natron.log"
    nice -n 5 env BENCH_FIXTURE="$fixture" BENCH_FILE="$file" BENCH_SCENARIOS="$scenarios" BENCH_LABEL="$label" \
        BENCH_BISECT="$BISECT" NATRON_GUI_TEST_OUT="$out" "${BISECT_ENV[@]}" "${LINEBUF_ENV[@]}" "$@" \
        Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py "$APPIMAGE" > >(stamp_lines > "$out.log") 2>&1 &
    runner=$!
    tail -n +1 -F -s 0.1 --pid="$runner" "$out/natron.log" 2> /dev/null | stamp_lines > "$out/natron.ts.log" &
    follower=$!
    wait "$runner"
    rc=$?
    wait "$follower"
    return "$rc"
}

launch() {
    local label="$1" fixture="$2" file="$3" scenarios="$4" rc bench sampler="" vram_sampler="" stem stacks vram extra=()
    shift 4
    wanted "$label" || return 0
    stem="$label${BISECT_TAG:+.$BISECT_TAG}"
    stacks="$FIXTURE_DIR/out/$stem.stacks"
    vram="$FIXTURE_DIR/out/$stem.vram"
    [ -n "${ONLY_SCENARIOS:-}" ] && scenarios="$ONLY_SCENARIOS"
    echo "=== $label${BISECT:+ (bisect $BISECT)}"
    host_report
    rm -f -- "$vram"
    if [ -n "$SAMPLER" ]; then
        rm -f -- "$stacks" "$stacks.err" "$stacks.pid"
        extra=(BENCH_PID_FILE="$stacks.pid" BENCH_STACK_SAMPLED=1)
    fi
    if [ -n "$BISECT_HUD" ]; then
        mkdir -p "$FIXTURE_DIR/out/$stem.hud"
        extra+=(GALLIUM_HUD="$BISECT_HUD" GALLIUM_HUD_DUMP_DIR="$FIXTURE_DIR/out/$stem.hud/")
    fi
    run_bench "$label" "$stem" "$fixture" "$file" "$scenarios" "${extra[@]}" "$@" &
    bench=$!
    if [ -n "$SAMPLER" ]; then
        sample_run "$bench" "$stacks" &
        sampler=$!
    fi
    if [ -n "$VRAM_DEVICE" ]; then
        "$ROOT/tools/bench/sample_vram.sh" "$VRAM_DEVICE" "$VRAM_INTERVAL_S" "$vram" "$bench" 2> /dev/null &
        vram_sampler=$!
    fi
    wait "$bench"
    rc=$?
    [ -n "$vram_sampler" ] && wait "$vram_sampler"
    echo "    exit=$rc"
    if [ -n "$sampler" ]; then
        wait "$sampler"
        if [ -s "$stacks" ]; then
            echo "    stacks: $(grep -c '^=== SAMPLE' "$stacks") samples of pid $(head -n 1 "$stacks.pid") in $stacks"
        else
            echo "    stacks: none taken (no pid in $stacks.pid, or the sampler could not attach)"
        fi
        [ -s "$stacks.err" ] && echo "    stacks: sampler said: $(head -n 3 "$stacks.err" | tr '\n' ' ')"
    fi
    # The bench's watchdog starts once the scene's rows are picked; a process lost before then has
    # told the results file nothing.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ] && ! grep -q "\"label\": \"$label\"" "$RESULT"; then
        printf '{"label": "%s", "scenario": "*", "result": "aborted", "exit_code": %d, "reason": "ended before any scenario; see %s", "peak_rss_mb": null, "gl_renderer": "%s", "bisect": %s}\n' \
            "$label" "$rc" "$FIXTURE_DIR/out/$stem.log" "$HOST_GPU" "$BISECT_JSON" >> "$RESULT"
    fi
    if command -v python3 > /dev/null 2>&1; then
        python3 -I tools/bench/heavy_scene_vram.py "$vram" "$RESULT" "$label" --bisect "$BISECT" \
            || echo "    vram: could not add the samples to the records"
    fi
}

CITY="$FIXTURE_DIR/heavy_city.usda"
LARGE_BLOCKS=""
for ((block = 2; block < 2 + ${GATE_LARGE_BLOCKS:-30}; block++)); do
    LARGE_BLOCKS="$LARGE_BLOCKS${LARGE_BLOCKS:+,}/World/Block$block"
done

# Island's loaded acceptance run peaked at 42 GB RSS on the GPU host; 48 GB leaves room above that.
ISLAND_MIN_RAM_GB="${ISLAND_MIN_RAM_GB:-48}"

# Prints why Island cannot run, or nothing when it can.
island_unfit() {
    local available_gb=$(($(meminfo_kb MemAvailable) / 1048576))
    if [ ! -f "$ISLAND" ]; then
        echo "not found by name under '$ISLAND_SRC', $SCENE_SEARCH_DEPTH levels deep (set SCENE_DIR or ISLAND_DIR)"
    elif [ "$available_gb" -lt "$ISLAND_MIN_RAM_GB" ]; then
        echo "$available_gb GB available, under the $ISLAND_MIN_RAM_GB GB estimated for it (set ISLAND_MIN_RAM_GB to override)"
    else
        echo "island: $available_gb GB available, $ISLAND_MIN_RAM_GB GB wanted (42 GB peak RSS measured)" >&2
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
    local reason loaded="populate_full,load_all,draw_budget,reveal_wide,reveal_cancel_paced,orbit10k"
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
    if command -v python3 > /dev/null 2>&1 && grep -q '"scenario": "draw_budget"' "$RESULT"; then
        python3 -I tools/bench/heavy_scene_budget_table.py "$RESULT"
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
