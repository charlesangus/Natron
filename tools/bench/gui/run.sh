#!/usr/bin/env bash
# Runs the GUI benchmark (gui_bench.py) and writes build/bench/gui/<UTC>-<host>-<mode>.jsonl.
#
#   tools/bench/gui/run.sh --xvfb [--appimage PATH] [options]   # in natron-dev, under Xvfb
#   tools/bench/gui/run.sh --host --appimage PATH [options]     # on this machine's display
#
# Options:
#   --sizes N[,N...]         node counts (default 300)
#   --topologies T[,T...]    chain, wide, comp, mixed (default mixed)
#   --reps N                 repetitions per op (default 5)
#   --ops OP[,OP...]         run only these ops (default all)
#   --out DIR                result directory (default build/bench/gui)
#   --self-check             add the 300 ms blocked-main-thread check per graph
#   --wait                   --xvfb only: run in the foreground rather than detached
#
# --xvfb runs detached (setsid, immune to the caller exiting): it prints the result path and
# writes the exit code to <result>.done when it finishes. --host runs in the foreground.
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
# GUI_BENCH_OP_TIMEOUT_S (default 120) and GUI_BENCH_IDLE_S through. GUI_BENCH_TIMEOUT caps the
# whole run in seconds (default 21600).
set -u

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
self="$repo/tools/bench/gui/run.sh"

usage() {
    sed -n '2,18p' "$self" >&2
    exit 2
}

mode=""
appimage=""
sizes=300
topologies=mixed
reps=5
ops=""
outdir="$repo/build/bench/gui"
self_check=0
detach=-1
while [ $# -gt 0 ]; do
    case $1 in
        --xvfb) mode=xvfb ;;
        --host) mode=host ;;
        --appimage) appimage=${2:?--appimage needs a path}; shift ;;
        --sizes) sizes=${2:?--sizes needs a list}; shift ;;
        --topologies) topologies=${2:?--topologies needs a list}; shift ;;
        --reps) reps=${2:?--reps needs a count}; shift ;;
        --ops) ops=${2:?--ops needs a list}; shift ;;
        --out) outdir=${2:?--out needs a directory}; shift ;;
        --self-check) self_check=1 ;;
        --wait) detach=0 ;;
        --detached-child) detach=0 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
    shift
done
[ -n "$mode" ] || usage
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
args=("--$mode" --sizes "$sizes" --topologies "$topologies" --reps "$reps" --out "$outdir")
[ -n "$appimage" ] && args+=(--appimage "$appimage")
[ -n "$ops" ] && args+=(--ops "$ops")
[ "$self_check" -eq 1 ] && args+=(--self-check)

if [ "$detach" -eq 1 ]; then
    mkdir -p "$outdir"
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    log="$outdir/run-$stamp.log"
    GUI_BENCH_STAMP=$stamp setsid nohup "$self" "${args[@]}" --detached-child > "$log" 2>&1 < /dev/null &
    echo "started in the background; log $log"
    echo "result $outdir/$stamp-$(hostname -s)-$mode.jsonl (exit code in .done)"
    exit 0
fi

stamp=${GUI_BENCH_STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
export GUI_BENCH_STAMP=$stamp

if [ "$mode" = xvfb ] && [ "${NATRON_IN_CONTAINER:-}" != 1 ]; then
    # The host name belongs to the machine, not the container.
    exec "$repo/tools/ci/local/devshell.sh" env GUI_BENCH_STAMP="$stamp" GUI_BENCH_HOSTNAME="$(hostname -s)" \
        GUI_BENCH_OP_TIMEOUT_S="${GUI_BENCH_OP_TIMEOUT_S:-120}" GUI_BENCH_IDLE_S="${GUI_BENCH_IDLE_S:-5}" \
        GUI_BENCH_TIMEOUT="${GUI_BENCH_TIMEOUT:-21600}" GUI_BENCH_WATCHDOG_S="${GUI_BENCH_WATCHDOG_S:-21600}" "$self" "${args[@]}" --wait
fi

host=${GUI_BENCH_HOSTNAME:-$(hostname -s)}
result="$outdir/$stamp-$host-$mode.jsonl"
work="$outdir/$stamp-$host-$mode"
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
printf '[General]\ncheckForUpdates=false\n' > "$home/.config/INRIA/Natron.conf"

export GUI_BENCH_SIZES=$sizes GUI_BENCH_TOPOLOGIES=$topologies GUI_BENCH_REPS=$reps GUI_BENCH_OPS=$ops
export GUI_BENCH_SELF_CHECK=$self_check GUI_BENCH_OUT=$result GUI_BENCH_MODE=$mode
export GUI_BENCH_GIT_SHA
GUI_BENCH_GIT_SHA=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)
export NATRON_STALL_LOG="$work/stalls.jsonl" NATRON_GUI_TEST_OUT="$work"
rm -f "$NATRON_STALL_LOG"
timeout_s=${GUI_BENCH_TIMEOUT:-21600}
script="$repo/tools/bench/gui/gui_bench.py"

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
    GUI_TEST_TIMEOUT=$timeout_s "$repo/Tests/gui/run-gui-test.sh" "$script" "$natron"
    finish $?
fi

# --host
display=${DISPLAY:-:0}
if ! (cd "$scratch" && "$appimage" --appimage-extract > /dev/null 2>&1) || [ ! -x "$scratch/squashfs-root/AppRun" ]; then
    echo "could not extract $appimage" >&2
    finish 2
fi
export GUI_BENCH_BUILD_TYPE=appimage GUI_BENCH_APPIMAGE=$appimage
export HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache"
export DISPLAY="$display" QT_QPA_PLATFORM=xcb NATRON_GUI_TEST_ROOT="$repo" GUI_TEST_HOST_DISPLAY="$display"
unset LD_LIBRARY_PATH WAYLAND_DISPLAY
cd "$repo" || finish 2
timeout "$timeout_s" "$scratch/squashfs-root/AppRun" "$script" > "$work/natron.log" 2>&1 < /dev/null
rc=$?
if [ -f "$work/results.txt" ]; then
    tail -n 5 "$work/results.txt"
fi
[ "$rc" -eq 124 ] && echo "timed out after ${timeout_s}s"
finish "$rc"
