#!/usr/bin/env bash
# Runs a GUI benchmark driver and writes build/bench/gui/<UTC>-<host>-<mode>[-roto].jsonl.
#
#   tools/bench/gui/run.sh --xvfb [--appimage PATH] [options]   # in natron-dev, under Xvfb
#   tools/bench/gui/run.sh --host --appimage PATH [options]     # on this machine's display
#
# Options:
#   --driver gui|roto        gui_bench.py (node graph, default) or roto_bench.py (roto/paint)
#   --preset gate|full       the matrix (default gate): gate is comp at 300,3000 nodes; full is chain,
#                            wide, comp and mixed at 300,1000,3000. Both use --reps auto and the graph cache
#   --sizes N[,N...]         node counts (overrides the preset)
#   --topologies T[,T...]    chain, wide, comp, mixed (overrides the preset)
#   --reps N|auto            repetitions per op (overrides the preset; auto repeats a short op until 5
#                            samples or 3 s)
#   --ops OP[,OP...]         run only these ops (default all)
#   --rerun OP[,OP...]       re-run only these ops to confirm a suspected regression; --reps defaults to 5
#   --baseline FILE          a result whose medians set each op's timeout (GUI_BENCH_BASELINE); for --xvfb
#                            it must be inside the checkout, which is all the container sees
#   --graph-cache DIR        generated graphs kept between runs (default build/bench/gui/graph-cache)
#   --fresh-graphs           use an empty cache in the run's own dir, so every graph is generated (and
#                            its generation timed) rather than opened from the shared cache
#   --out DIR                result directory (default build/bench/gui)
#   --no-forced-repaint      replace each interaction event's forced viewport repaint with a
#                            processEvents pass inside the step (gui driver)
#   --self-check             add the 300 ms blocked-main-thread check per graph
#   --wait                   --xvfb only: run in the foreground rather than detached
#
# --sizes, --topologies and --self-check apply to the gui driver; the roto driver reads its
# ROTO_BENCH_* variables (see roto_bench.py) and also writes NATRON_ROTO_LATENCY_LOG beside the result.
#
# --xvfb runs detached (setsid, immune to the caller exiting): it prints the result path and
# writes the exit code to <result>.done when it finishes. --host runs in the foreground.
#
# The gui driver runs each topology and size in its own Natron process, appending to the one result
# file. An op that wedges Natron is recorded as a wedged over_budget and Natron is restarted to run
# the remaining ops (logs of each restart in <topology>-<n>-restart<k>/); at larger sizes that op is
# recorded as over_budget without running (GUI_BENCH_SKIP_WEDGED=0 runs it). The exit code is the
# last non-zero one, 3 when an op wedged.
#
# A --appimage run records the AppImage's name, size, mtime and sha256 in the host record, which
# compare.py prints, so results from different builds cannot pass for one another.
#
# --xvfb runs Tests/gui/run-gui-test.sh inside the natron-dev container (re-entering it through
# tools/ci/local/devshell.sh from the host) with build/release/App/Natron, or the AppImage given.
# Only one --xvfb run at a time: a second one waits on a lock rather than share the container.
# --host extracts the AppImage and runs it directly on $DISPLAY (default :0) with no container,
# so it measures the user's own machine and GPU; it does not go through run-gui-test.sh, which
# needs the container's PySide6 to add QtTest to the bundle.
#
# Both modes run Natron under a throwaway HOME seeded with checkForUpdates=false, set
# NATRON_STALL_LOG so the stall monitor records main-thread stalls beside the result, and pass
# GUI_BENCH_OP_TIMEOUT_S (default 30, the cap on every per-rep timeout) and GUI_BENCH_IDLE_S through. GUI_BENCH_TIMEOUT caps the
# whole run in seconds (default 21600), and GUI_BENCH_EXTRACT_S the AppImage extraction (--host, default
# 300). Natron's start-up and the hold-file wait are bounded in gui_bench.py and report which wait expired.
set -u

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
self="$repo/tools/bench/gui/run.sh"

usage() {
    sed -n '2,32p' "$self" >&2
    exit 2
}

mode=""
driver=gui
appimage=""
preset=gate
sizes=""
topologies=""
reps=""
ops=""
rerun=""
baseline=${GUI_BENCH_BASELINE:-}
graph_cache=${GUI_BENCH_GRAPH_CACHE:-}
fresh_graphs=0
outdir="$repo/build/bench/gui"
self_check=0
forced_repaint=1
detach=-1
while [ $# -gt 0 ]; do
    case $1 in
        --xvfb) mode=xvfb ;;
        --host) mode=host ;;
        --driver) driver=${2:?--driver needs gui or roto}; shift ;;
        --appimage) appimage=${2:?--appimage needs a path}; shift ;;
        --preset) preset=${2:?--preset needs gate or full}; shift ;;
        --sizes) sizes=${2:?--sizes needs a list}; shift ;;
        --topologies) topologies=${2:?--topologies needs a list}; shift ;;
        --reps) reps=${2:?--reps needs a count}; shift ;;
        --ops) ops=${2:?--ops needs a list}; shift ;;
        --rerun) rerun=${2:?--rerun needs a list of ops}; shift ;;
        --baseline) baseline=${2:?--baseline needs a result file}; shift ;;
        --graph-cache) graph_cache=${2:?--graph-cache needs a directory}; shift ;;
        --fresh-graphs) fresh_graphs=1 ;;
        --out) outdir=${2:?--out needs a directory}; shift ;;
        --self-check) self_check=1 ;;
        --no-forced-repaint) forced_repaint=0 ;;
        --wait) detach=0 ;;
        --detached-child) detach=0 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
    shift
done
[ -n "$mode" ] || usage
case $driver in
    gui) suffix="" ;;
    roto) suffix="-roto" ;;
    *) echo "unknown driver: $driver (gui or roto)" >&2; exit 2 ;;
esac
case $preset in
    gate) preset_sizes=300,3000 preset_topologies=comp ;;
    full) preset_sizes=300,1000,3000 preset_topologies=chain,wide,comp,mixed ;;
    *) echo "unknown preset: $preset (gate or full)" >&2; exit 2 ;;
esac
if [ -n "$rerun" ]; then
    if [ -n "$ops" ]; then
        echo "--rerun and --ops both name the ops to run; give one" >&2
        exit 2
    fi
    ops=$rerun
    reps=${reps:-5}
fi
sizes=${sizes:-$preset_sizes}
topologies=${topologies:-$preset_topologies}
reps=${reps:-${GUI_BENCH_REPS:-auto}}
graph_cache=${graph_cache:-$repo/build/bench/gui/graph-cache}
case $graph_cache in /*) ;; *) graph_cache=$(pwd)/$graph_cache ;; esac
if [ -n "$baseline" ]; then
    case $baseline in /*) ;; *) baseline=$(pwd)/$baseline ;; esac
    if [ ! -f "$baseline" ]; then
        echo "no baseline at $baseline" >&2
        exit 2
    fi
fi
if [ "$detach" -lt 0 ]; then
    if [ "$mode" = xvfb ]; then detach=1; else detach=0; fi
fi
case $outdir in /*) ;; *) outdir=$repo/$outdir ;; esac
if [ -n "$appimage" ]; then
    case $appimage in /*) ;; *) appimage=$(pwd)/$appimage ;; esac
    if [ ! -f "$appimage" ]; then
        echo "no AppImage at $appimage" >&2
        exit 2
    fi
fi
if [ "$mode" = host ] && [ -z "$appimage" ]; then
    echo "--host needs --appimage" >&2
    exit 2
fi
# Normalised, so a re-exec (detached or in the container) sees absolute paths.
args=("--$mode" --driver "$driver" --preset "$preset" --sizes "$sizes" --topologies "$topologies" --reps "$reps"
      --graph-cache "$graph_cache" --out "$outdir")
[ "$forced_repaint" = 0 ] && args+=(--no-forced-repaint)
[ -n "$appimage" ] && args+=(--appimage "$appimage")
[ -n "$ops" ] && args+=(--ops "$ops")
[ "$self_check" -eq 1 ] && args+=(--self-check)
[ -n "$baseline" ] && args+=(--baseline "$baseline")
[ "$fresh_graphs" -eq 1 ] && args+=(--fresh-graphs)

if [ "$detach" -eq 1 ]; then
    mkdir -p "$outdir"
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    log="$outdir/run-$stamp.log"
    GUI_BENCH_STAMP=$stamp setsid nohup "$self" "${args[@]}" --detached-child > "$log" 2>&1 < /dev/null &
    echo "started in the background; log $log"
    echo "result $outdir/$stamp-$(hostname -s)-$mode$suffix.jsonl (exit code in .done)"
    exit 0
fi

stamp=${GUI_BENCH_STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
export GUI_BENCH_STAMP=$stamp

if [ "$mode" = xvfb ] && [ "${NATRON_IN_CONTAINER:-}" != 1 ]; then
    roto_env=()
    for name in $(compgen -e | grep '^ROTO_BENCH_'); do
        roto_env+=("$name=${!name}")
    done
    # The host name belongs to the machine, not the container.
    exec "$repo/tools/ci/local/devshell.sh" env GUI_BENCH_STAMP="$stamp" GUI_BENCH_HOSTNAME="$(hostname -s)" \
        GUI_BENCH_OP_TIMEOUT_S="${GUI_BENCH_OP_TIMEOUT_S:-30}" GUI_BENCH_IDLE_S="${GUI_BENCH_IDLE_S:-5}" \
        GUI_BENCH_TIMEOUT="${GUI_BENCH_TIMEOUT:-21600}" GUI_BENCH_WATCHDOG_S="${GUI_BENCH_WATCHDOG_S:-21600}" \
        GUI_BENCH_SKIP_WEDGED="${GUI_BENCH_SKIP_WEDGED:-1}" GUI_BENCH_HOLD_FILE="${GUI_BENCH_HOLD_FILE:-}" \
        GUI_BENCH_HOLD_S="${GUI_BENCH_HOLD_S:-120}" GUI_BENCH_PTRACER_ANY="${GUI_BENCH_PTRACER_ANY:-0}" \
        "${roto_env[@]}" "$self" "${args[@]}" --wait
fi

host=${GUI_BENCH_HOSTNAME:-$(hostname -s)}
result="$outdir/$stamp-$host-$mode$suffix.jsonl"
work="$outdir/$stamp-$host-$mode$suffix"
mkdir -p "$work"
rm -f "$result" "$result.done"

finish() {
    echo "$1" > "$result.done"
    echo "result $result (exit $1)"
    exit "$1"
}

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
home="$scratch/home"
mkdir -p "$home/.config/INRIA" "$home/.cache"
# The version keys matter only to AppImages built before Natron stopped asking at start-up whether
# to take a new default appearance and new shortcuts (commit 9a757f381): those ask on every start
# after the first in this HOME, before the driver runs, unless the saved versions are current. Only
# the comparison matters, so any large version passes.
printf '[General]\ncheckForUpdates=false\nappearanceVersion=1000\nNATRON_SHORTCUTS_DEFAULT_VERSION=1000\n' \
    > "$home/.config/INRIA/Natron.conf"

if [ "$fresh_graphs" -eq 1 ]; then
    graph_cache=$work/graph-cache
fi
mkdir -p "$graph_cache"
export GUI_BENCH_SIZES=$sizes GUI_BENCH_TOPOLOGIES=$topologies GUI_BENCH_REPS=$reps GUI_BENCH_OPS=$ops
export GUI_BENCH_GRAPH_CACHE=$graph_cache GUI_BENCH_BASELINE=$baseline
export GUI_BENCH_OP_TIMEOUT_S=${GUI_BENCH_OP_TIMEOUT_S:-30}
export GUI_BENCH_FORCED_REPAINT=$forced_repaint
export GUI_BENCH_SELF_CHECK=$self_check GUI_BENCH_OUT=$result GUI_BENCH_MODE=$mode
export GUI_BENCH_GIT_SHA GUI_BENCH_APPIMAGE_NAME="" GUI_BENCH_APPIMAGE_BYTES="" GUI_BENCH_APPIMAGE_MTIME="" \
    GUI_BENCH_APPIMAGE_SHA256=""
GUI_BENCH_GIT_SHA=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)
# The AppImage carries no commit of its own (GIT_COMMIT is a committed constant), so its file is the identity.
if [ -n "$appimage" ]; then
    GUI_BENCH_APPIMAGE_NAME=$(basename "$appimage")
    GUI_BENCH_APPIMAGE_BYTES=$(stat -L -c %s "$appimage")
    GUI_BENCH_APPIMAGE_MTIME=$(date -u -r "$appimage" +%Y-%m-%dT%H:%M:%SZ)
    GUI_BENCH_APPIMAGE_SHA256=$(sha256sum "$appimage" | cut -c1-64)
    echo "AppImage $GUI_BENCH_APPIMAGE_NAME, $GUI_BENCH_APPIMAGE_BYTES bytes, modified $GUI_BENCH_APPIMAGE_MTIME, sha256 ${GUI_BENCH_APPIMAGE_SHA256:0:16}"
fi
timeout_s=${GUI_BENCH_TIMEOUT:-21600}
script="$repo/tools/bench/gui/${driver}_bench.py"
if [ "$driver" = roto ]; then
    export NATRON_ROTO_LATENCY_LOG="$work/latency.jsonl"
    rm -f "$NATRON_ROTO_LATENCY_LOG"
else
    unset NATRON_ROTO_LATENCY_LOG
fi

wedged_exit=3

# Calls run_one with the seconds left of the whole run's cap, once per topology and size. A fresh
# Natron per size keeps one size's project reset from leaving the next size's nodes unclickable.
# When an op wedges Natron (exit 3, the op recorded as a wedged over_budget), starts a fresh Natron
# that skips what the result file already holds, so the remaining ops still run; each start gets its
# own log dir. Every start after the run's first resumes, so an op that wedged at a smaller size is
# recorded as over_budget at the larger ones without being run.
run_cases() {
    local deadline=$(( $(date +%s) + timeout_s )) overall=0 topology size rc left attempt lines label started=0
    local -a list size_list
    IFS=, read -r -a list <<< "$topologies"
    IFS=, read -r -a size_list <<< "$sizes"
    for topology in "${list[@]}"; do
        for size in "${size_list[@]}"; do
            label=$topology-$size
            attempt=0
            while :; do
                left=$(( deadline - $(date +%s) ))
                if [ "$left" -le 0 ]; then
                    echo "the ${timeout_s}s cap ended the run in $label"
                    rc=124
                    break
                fi
                export GUI_BENCH_TOPOLOGIES=$topology GUI_BENCH_SIZES=$size GUI_BENCH_RESUME=$started
                if [ "$attempt" = 0 ]; then
                    export NATRON_GUI_TEST_OUT="$work/$label"
                else
                    export NATRON_GUI_TEST_OUT="$work/$label-restart$attempt"
                fi
                export NATRON_STALL_LOG="$NATRON_GUI_TEST_OUT/stalls.jsonl"
                mkdir -p "$NATRON_GUI_TEST_OUT"
                rm -f "$NATRON_STALL_LOG"
                lines=$(result_lines)
                run_one "$left"
                rc=$?
                started=1
                if [ "$rc" != "$wedged_exit" ] || [ "$driver" != gui ]; then
                    break
                fi
                if [ "$(result_lines)" = "$lines" ]; then
                    echo "$label: Natron exited with code $rc without recording anything; not restarting"
                    break
                fi
                attempt=$((attempt + 1))
                echo "$label: an op wedged Natron and is recorded as over budget; restarting Natron (restart $attempt) to run the rest"
            done
            if [ "$attempt" -gt 0 ] && [ "$rc" = 0 ]; then
                rc=$wedged_exit
                echo "$label finished after $attempt restart(s); the wedged ops are recorded as over budget"
            elif [ "$rc" -ne 0 ]; then
                echo "$label ended with exit code $rc"
            fi
            [ "$rc" -ne 0 ] && overall=$rc
            [ "$rc" = 124 ] && break 2
        done
    done
    finish "$overall"
}

result_lines() {
    if [ -f "$result" ]; then
        wc -l < "$result"
    else
        echo 0
    fi
}

# The roto driver has no topologies, so it is one process writing directly into $work.
run_driver() {
    if [ "$driver" != roto ]; then
        run_cases
    fi
    export NATRON_GUI_TEST_OUT="$work" NATRON_STALL_LOG="$work/stalls.jsonl"
    rm -f "$NATRON_STALL_LOG"
    run_one "$timeout_s"
    finish $?
}

if [ "$mode" = xvfb ]; then
    if [ -n "$appimage" ]; then
        natron=$appimage
        export GUI_BENCH_BUILD_TYPE=appimage GUI_BENCH_APPIMAGE=$appimage
    else
        natron="$repo/build/release/App/Natron"
        export GUI_BENCH_BUILD_TYPE=release GUI_BENCH_APPIMAGE=
        # run-gui-test.sh seeds a HOME only for an AppImage.
        export HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache"
    fi
    lock="$repo/build/bench/gui/.xvfb.lock"
    mkdir -p "$(dirname "$lock")"
    exec 9> "$lock"
    if ! flock -n 9; then
        echo "another --xvfb run holds $lock; waiting"
        flock 9
    fi
    run_one() {
        GUI_TEST_TIMEOUT=$1 "$repo/Tests/gui/run-gui-test.sh" "$script" "$natron"
    }
    run_driver
fi

# --host
display=${DISPLAY:-:0}
if ! (cd "$scratch" && timeout --kill-after=10 "${GUI_BENCH_EXTRACT_S:-300}" "$appimage" --appimage-extract > /dev/null 2>&1) || [ ! -x "$scratch/squashfs-root/AppRun" ]; then
    echo "wait expired or failed: could not extract $appimage within ${GUI_BENCH_EXTRACT_S:-300} s" >&2
    finish 2
fi
export GUI_BENCH_BUILD_TYPE=appimage GUI_BENCH_APPIMAGE=$appimage
export HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache"
export DISPLAY="$display" QT_QPA_PLATFORM=xcb NATRON_GUI_TEST_ROOT="$repo" GUI_TEST_HOST_DISPLAY="$display"
unset LD_LIBRARY_PATH WAYLAND_DISPLAY
cd "$repo" || finish 2
echo "running on $display; leave the mouse and keyboard alone"
echo "result $result"
run_one() {
    local log="$NATRON_GUI_TEST_OUT/natron.log" size natron_pid tail_pid rc
    size=$(stat -c %s "$result" 2> /dev/null || echo 0)
    echo "Natron log $log; progress follows"
    # --foreground keeps Natron in the terminal's process group: in a background group, anything in
    # the bundle that touches the terminal stops the process, leaving an open window that does nothing.
    timeout --foreground --kill-after=20 "$1" "$scratch/squashfs-root/AppRun" "$script" > "$log" 2>&1 < /dev/null &
    natron_pid=$!
    tail --pid="$natron_pid" -n +1 -F "$NATRON_GUI_TEST_OUT/results.txt" 2> /dev/null &
    tail_pid=$!
    wait "$natron_pid"
    rc=$?
    wait "$tail_pid" 2> /dev/null
    if [ "$(stat -c %s "$result" 2> /dev/null || echo 0)" = "$size" ]; then
        echo "the driver wrote no results; last lines of $log:"
        tail -n 30 "$log"
    fi
    [ "$rc" -eq 124 ] && echo "wait expired: Natron still running after the ${1} s cap"
    return "$rc"
}
run_driver
