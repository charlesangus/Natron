#!/usr/bin/env bash
# Usage: Tests/gui/m225_clean_shutdown.sh [natron-binary|Natron.AppImage] [scenario:action ...]
#
# Closes Natron every supported way and checks that nothing of it is left behind:
#   - every process of its session, render processes included, is gone within
#     CLEAN_SHUTDOWN_WAIT seconds (default 20);
#   - it exits without a crash (no SIGSEGV or SIGABRT status);
#   - its temporary directory (TMPDIR, so Qt's sockets and temp files) is empty;
#   - no POSIX (/dev/shm) or System V shared memory segment it created survives;
#   - no RENDER_SAVE project copy is left in its autosave directory;
#   - a program it spawns (xdg-open for Help > Website) inherits neither the launcher's
#     keep-alive descriptor nor a cache file.
# Natron is driven by Tests/gui/m225_clean_shutdown.py. Actions: menu (File > Quit), window
# (closing the main window), sigterm, sigint, sighup. Scenarios: idle, playback, write_fg,
# write_bg (a background render, in its own process), modal (a file dialog open), url.
#
# Run in the natron-dev container, e.g.
#   docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && Tests/gui/m225_clean_shutdown.sh'
# The default binary is build/release/App/Natron. An .AppImage is extracted into the output
# directory for the run and removed afterwards; the AppRun of an extracted one is used as is. Xvfb runs on :$CLEAN_SHUTDOWN_DISPLAY (default
# 78) with the GL setup of run-gui-test.sh. Output: $CLEAN_SHUTDOWN_OUT_ROOT/<scenario>-<action>/,
# by default build/clean-shutdown-out/.
#
# Exits 0 when every case passes, 1 otherwise.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER="$ROOT/Tests/gui/m225_clean_shutdown.py"
NATRON="${1:-$ROOT/build/release/App/Natron}"
shift $(( $# > 0 ? 1 : 0 ))
WAIT_S="${CLEAN_SHUTDOWN_WAIT:-20}"
READY_TIMEOUT_S="${CLEAN_SHUTDOWN_READY_TIMEOUT:-120}"
DISPLAY_NUM="${CLEAN_SHUTDOWN_DISPLAY:-78}"
OUT_ROOT="${CLEAN_SHUTDOWN_OUT_ROOT:-$ROOT/build/clean-shutdown-out}"

CASES=("$@")
if [ ${#CASES[@]} -eq 0 ]; then
    CASES=(
        idle:menu idle:window idle:sigterm idle:sigint idle:sighup
        playback:menu playback:window playback:sigterm playback:sigint
        write_fg:menu write_fg:window write_fg:sigterm write_fg:sigint
        write_bg:menu write_bg:window write_bg:sigterm write_bg:sigint
        modal:window modal:sigterm modal:sigint
        url:menu
    )
fi

mkdir -p "$OUT_ROOT"
OUT_ROOT="$(cd "$OUT_ROOT" && pwd)"
WORK="$(mktemp -d "$OUT_ROOT/work.XXXXXX")"
XPID=""
KEEPALIVE_PID=""
cleanup() {
    if [ -n "$KEEPALIVE_PID" ]; then
        kill "$KEEPALIVE_PID" 2>/dev/null
    fi
    if [ -n "$XPID" ]; then
        kill "$XPID" 2>/dev/null
        wait "$XPID" 2>/dev/null
    fi
    rm -rf "${WORK:?}"
}
trap cleanup EXIT

APPIMAGE=0
case "$NATRON" in
    *.AppImage) APPIMAGE=1 ;;
    # An AppImage extracted beforehand runs the same way, without another extraction.
    */AppRun) APPIMAGE=2 ;;
esac
if [ "$APPIMAGE" -eq 1 ]; then
    NATRON="$(cd "$(dirname "$NATRON")" && pwd)/$(basename "$NATRON")"
    chmod +x "$NATRON"
    if ! (cd "$WORK" && "$NATRON" --appimage-extract > /dev/null 2>&1) \
        || [ ! -x "$WORK/squashfs-root/AppRun" ]; then
        echo "could not extract $NATRON" >&2
        exit 2
    fi
    NATRON="$WORK/squashfs-root/AppRun"
elif [ ! -x "$NATRON" ]; then
    echo "no Natron binary at $NATRON" >&2
    exit 2
fi

GLDIR="$WORK/gl"
mkdir -p "$GLDIR"
for lib in libGLX.so.0 libGL.so.1 libOpenGL.so.0 libGLdispatch.so.0 libEGL.so.1 libGLX_mesa.so.0 libLLVM.so.21.1; do
    if [ ! -e "/usr/lib64/$lib" ]; then
        echo "missing /usr/lib64/$lib" >&2
        exit 2
    fi
    ln -s "/usr/lib64/$lib" "$GLDIR/$lib"
done
if xset -display ":$DISPLAY_NUM" q > /dev/null 2>&1; then
    echo "display :$DISPLAY_NUM is already in use (set CLEAN_SHUTDOWN_DISPLAY)" >&2
    exit 2
fi
LD_LIBRARY_PATH=/usr/lib64 Xvfb ":$DISPLAY_NUM" -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
XPID=$!
for _ in $(seq 1 100); do
    xset -display ":$DISPLAY_NUM" q > /dev/null 2>&1 && break
    sleep 0.1
done
if ! xset -display ":$DISPLAY_NUM" q > /dev/null 2>&1; then
    echo "Xvfb failed to start on :$DISPLAY_NUM" >&2
    exit 2
fi

# Stands in for the browser: records the descriptors it inherited and exits.
TOOLS="$WORK/tools"
mkdir -p "$TOOLS"
cat > "$TOOLS/xdg-open" <<'EOF'
#!/bin/bash
ls -l /proc/$$/fd > "$CLEAN_SHUTDOWN_OUT/xdg-open.fds" 2>&1
exit 0
EOF
chmod +x "$TOOLS/xdg-open"

# Stands in for the AppImage runtime's keep-alive pipe: a descriptor the launcher leaves open
# across exec and expects only Natron to hold.
exec 9< <(sleep 100000)
KEEPALIVE_PID=$!
KEEPALIVE="$(readlink /proc/$$/fd/9)"

UID_NOW="$(id -u)"
shm_list() { find /dev/shm -maxdepth 1 -mindepth 1 -user "$UID_NOW" -printf '%f\n' 2>/dev/null | sort; }
# System V segments of ours that are not already marked for destruction.
ipc_list() { ipcs -m 2>/dev/null | awk -v u="$(id -un 2>/dev/null || echo "$UID_NOW")" '$3==u && $0 !~ /dest/ {print $2}' | sort; }
session_pids() { ps -eo pid=,sid= | awk -v s="$1" '$2==s {print $1}'; }

FAILED=0
PASSED=0
fail() {
    echo "FAIL $CASE_NAME: $*"
    echo "FAIL $*" >> "$CASE_OUT/summary"
    CASE_OK=0
}

for CASE in "${CASES[@]}"; do
    SCENARIO="${CASE%%:*}"
    ACTION="${CASE##*:}"
    CASE_NAME="$SCENARIO-$ACTION"
    CASE_OUT="$OUT_ROOT/$CASE_NAME"
    rm -rf "${CASE_OUT:?}"
    mkdir -p "$CASE_OUT/tmp" "$CASE_OUT/home/.config/INRIA" "$CASE_OUT/home/.cache"
    printf '[General]\ncheckForUpdates=false\n' > "$CASE_OUT/home/.config/INRIA/Natron.conf"
    CASE_OK=1
    case "$ACTION" in
        menu|window) DRIVER_ACTION="$ACTION" ;;
        sigterm|sigint|sighup) DRIVER_ACTION=signal ;;
        *) echo "unknown action $ACTION" >&2; exit 2 ;;
    esac

    shm_list > "$CASE_OUT/shm.before"
    ipc_list > "$CASE_OUT/ipc.before"

    (
        export HOME="$CASE_OUT/home" XDG_CONFIG_HOME="$CASE_OUT/home/.config" XDG_CACHE_HOME="$CASE_OUT/home/.cache"
        export TMPDIR="$CASE_OUT/tmp"
        export DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
        export PATH="$TOOLS:$PATH"
        export CLEAN_SHUTDOWN_SCENARIO="$SCENARIO" CLEAN_SHUTDOWN_ACTION="$DRIVER_ACTION" CLEAN_SHUTDOWN_OUT="$CASE_OUT"
        if [ "$APPIMAGE" -ne 0 ]; then
            export LD_LIBRARY_PATH="$GLDIR"
        else
            export LD_LIBRARY_PATH="$GLDIR:/usr/local/lib:/usr/local/lib64"
            export OFX_PLUGIN_PATH="${OFX_PLUGIN_PATH:-$ROOT/build/assets/Plugins}"
        fi
        exec setsid "$NATRON" "$DRIVER" > "$CASE_OUT/natron.log" 2>&1 < /dev/null
    ) &
    LPID=$!

    for _ in $(seq 1 $((READY_TIMEOUT_S * 5))); do
        [ -f "$CASE_OUT/ready" ] && break
        kill -0 "$LPID" 2>/dev/null || break
        sleep 0.2
    done
    if [ ! -f "$CASE_OUT/ready" ]; then
        fail "the driver never reported ready (see driver.log, natron.log)"
    fi
    RENDER_CHILD="$(sed -n 's/^render_child=//p' "$CASE_OUT/ready" 2>/dev/null)"
    if [ "$SCENARIO" = write_bg ] && [ -z "$RENDER_CHILD" ]; then
        fail "the background render did not run in its own process"
    fi
    ps -eo pid,ppid,sid,stat,etime,rss,cmd | awk -v s="$LPID" 'NR==1 || $3==s' | cut -c1-200 > "$CASE_OUT/ps.before-close"

    T0=$(date +%s%N)
    case "$ACTION" in
        sigterm) kill -TERM "$LPID" ;;
        sigint) kill -INT "$LPID" ;;
        sighup) kill -HUP "$LPID" ;;
    esac

    GONE_MS=""
    for _ in $(seq 1 $((WAIT_S * 10))); do
        if [ -z "$(session_pids "$LPID")" ] && { [ -z "$RENDER_CHILD" ] || [ ! -e "/proc/$RENDER_CHILD" ]; }; then
            GONE_MS=$(( ($(date +%s%N) - T0) / 1000000 ))
            break
        fi
        sleep 0.1
    done
    if [ -z "$GONE_MS" ]; then
        fail "processes left after ${WAIT_S}s (see leftovers)"
        for p in $(session_pids "$LPID") $RENDER_CHILD; do
            [ -e "/proc/$p" ] || continue
            {
                echo "== pid $p: $(tr '\0' ' ' < "/proc/$p/cmdline" | cut -c1-200)"
                grep -E '^(State|PPid|Threads|VmRSS)' "/proc/$p/status"
                for t in /proc/$p/task/*; do
                    echo "  $(basename "$t") $(cat "$t/comm") $(awk '{print $3}' "$t/stat") $(cat "$t/wchan" 2>/dev/null)"
                done
            } >> "$CASE_OUT/leftovers"
        done
        kill -KILL -- "-$LPID" 2>/dev/null
        [ -n "$RENDER_CHILD" ] && kill -KILL "$RENDER_CHILD" 2>/dev/null
        sleep 1
    fi
    { wait "$LPID"; } 2>/dev/null
    RC=$?
    echo "exit status $RC, gone after ${GONE_MS:-never} ms" >> "$CASE_OUT/summary"
    case "$RC" in
        134|139) fail "Natron crashed while closing (exit status $RC, see natron.log)" ;;
    esac
    if grep -q "Caught segmentation fault\|^Backtrace:" "$CASE_OUT/natron.log" 2>/dev/null; then
        fail "natron.log reports a crash"
    fi

    LEFT_TMP="$(find "$CASE_OUT/tmp" -mindepth 1 -printf '%P\n' 2>/dev/null)"
    [ -n "$LEFT_TMP" ] && fail "left in TMPDIR: $(echo "$LEFT_TMP" | tr '\n' ' ')"
    LEFT_SAVES="$(find "$CASE_OUT/home" -name 'RENDER_SAVE*' -printf '%f\n' 2>/dev/null)"
    [ -n "$LEFT_SAVES" ] && fail "render project copies left: $(echo "$LEFT_SAVES" | tr '\n' ' ')"
    shm_list > "$CASE_OUT/shm.after"
    ipc_list > "$CASE_OUT/ipc.after"
    NEW_SHM="$(comm -13 "$CASE_OUT/shm.before" "$CASE_OUT/shm.after")"
    [ -n "$NEW_SHM" ] && fail "left in /dev/shm: $(echo "$NEW_SHM" | tr '\n' ' ')"
    NEW_IPC="$(comm -13 "$CASE_OUT/ipc.before" "$CASE_OUT/ipc.after")"
    [ -n "$NEW_IPC" ] && fail "System V segments left: $(echo "$NEW_IPC" | tr '\n' ' ')"

    if [ "$SCENARIO" = url ]; then
        if [ ! -f "$CASE_OUT/xdg-open.fds" ]; then
            fail "Help > Website did not run xdg-open"
        else
            grep -qF "$KEEPALIVE" "$CASE_OUT/xdg-open.fds" && fail "xdg-open inherited the launcher's keep-alive descriptor"
            grep -q -i "cache" "$CASE_OUT/xdg-open.fds" && fail "xdg-open inherited a cache file"
        fi
    fi

    if [ "$CASE_OK" -eq 1 ]; then
        PASSED=$((PASSED + 1))
        echo "PASS $CASE_NAME (gone after ${GONE_MS} ms${RENDER_CHILD:+, render process $RENDER_CHILD})"
    else
        FAILED=$((FAILED + 1))
    fi
done

echo "$PASSED passed, $FAILED failed (details in $OUT_ROOT)"
[ "$FAILED" -eq 0 ]
