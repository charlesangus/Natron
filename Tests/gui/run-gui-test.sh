#!/usr/bin/env bash
# Usage: Tests/gui/run-gui-test.sh <script.py> [natron-binary]
#
# Runs a Natron GUI test script under a GLX-capable Xvfb and exits with the script's status
# (124 on timeout). Meant for the natron-dev container, e.g.
#   docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && Tests/gui/run-gui-test.sh Tests/gui/viewer_error_scrub.py'
#
# Plain xvfb-run aborts there with "Could not initialize GLX": /etc/ld.so.preload forces a glvnd
# libGL without a mesa vendor, which Xvfb and Natron would both pick up. Xvfb therefore runs
# against /usr/lib64, and Natron gets only the system GL libraries through a directory of
# symlinks (all of /usr/lib64 first would shadow the ASWF freetype, harfbuzz, zlib...).
#
# An AppImage binary (name ending in .AppImage) is extracted once into a temp directory and its
# AppRun is launched, so no FUSE is needed. Its bundled plugins and libraries are used as shipped,
# with two test-only additions: PySide6.QtTest from the container's matching PySide6 (the bundle
# leaves QtTest out and the test scripts import it), and an oiiotool shim that keeps the
# container's library path. It runs under a throwaway HOME seeded only with
# checkForUpdates=false, so it neither depends on nor rewrites the container's Natron settings
# and caches, and the first-run "check for updates?" question cannot block the script.
#
# Startup scripts a test needs live in <script>.natronpath/, which is put on NATRON_PLUGIN_PATH.
#
# GUI_TEST_HOST_DISPLAY=:0 skips Xvfb, the GL symlinks and the mesa software-GL settings and runs
# against an existing display with the host's own glvnd (the caller's XAUTHORITY applies). It
# refuses a software renderer unless GUI_TEST_ALLOW_SOFTWARE_GL=1.
#
# Environment: NATRON_GUI_TEST_OUT (screenshots, results.txt and natron.log, default build/gui-test-out),
# GUI_TEST_TIMEOUT (seconds, default 300), GUI_TEST_DISPLAY (default 79).
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <script.py> [natron-binary]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
NATRON="${2:-$ROOT/build/release/App/Natron}"
TIMEOUT="${GUI_TEST_TIMEOUT:-300}"
DISPLAY_NUM="${GUI_TEST_DISPLAY:-79}"
OUT="${NATRON_GUI_TEST_OUT:-$ROOT/build/gui-test-out}"
SCREEN_SIZE=1920x1080

if [ ! -f "$SCRIPT" ]; then
    echo "no such script: $1" >&2
    exit 2
fi
APPIMAGE=0
case "$NATRON" in
    *.AppImage) APPIMAGE=1 ;;
esac
if [ "$APPIMAGE" -eq 1 ] && [ -f "$NATRON" ]; then
    NATRON="$(cd "$(dirname "$NATRON")" && pwd)/$(basename "$NATRON")"
    chmod +x "$NATRON"
fi
if [ ! -x "$NATRON" ]; then
    echo "no Natron binary at $NATRON" >&2
    exit 2
fi
mkdir -p "$OUT"

GLDIR="$(mktemp -d)"
XPID=""
EXTRACT_DIR=""
cleanup() {
    if [ -n "$XPID" ]; then
        kill "$XPID" 2>/dev/null
        wait "$XPID" 2>/dev/null
    fi
    rm -rf "$GLDIR"
    if [ -n "$EXTRACT_DIR" ]; then
        rm -rf "$EXTRACT_DIR"
    fi
}
trap cleanup EXIT

if [ "$APPIMAGE" -eq 1 ]; then
    EXTRACT_DIR="$(mktemp -d)"
    if ! (cd "$EXTRACT_DIR" && "$NATRON" --appimage-extract > /dev/null 2>&1) \
        || [ ! -x "$EXTRACT_DIR/squashfs-root/AppRun" ]; then
        echo "could not extract $NATRON" >&2
        exit 2
    fi
    NATRON="$EXTRACT_DIR/squashfs-root/AppRun"

    BUNDLE_PYSIDE="$(ls -d "$EXTRACT_DIR"/squashfs-root/lib/python3*/site-packages/PySide6 2>/dev/null | head -n 1)"
    HOST_PYSIDE="$(python3 -c 'import os, PySide6; print(os.path.dirname(PySide6.__file__))' 2>/dev/null)"
    if [ -z "$BUNDLE_PYSIDE" ] || [ -z "$HOST_PYSIDE" ]; then
        echo "no PySide6 in the bundle or in the container's python3" >&2
        exit 2
    fi
    if [ ! -e "$BUNDLE_PYSIDE/QtTest.abi3.so" ]; then
        if ! cmp -s "$BUNDLE_PYSIDE/_git_pyside_version.py" "$HOST_PYSIDE/_git_pyside_version.py"; then
            echo "the container's PySide6 ($HOST_PYSIDE) is not the bundle's version; cannot add QtTest" >&2
            exit 2
        fi
        QTTEST_LIB="$(ldd "$HOST_PYSIDE/QtTest.abi3.so" 2>/dev/null | sed -n 's/^[[:space:]]*libQt6Test\.so\.6 => \([^ ]*\).*/\1/p')"
        if [ -z "$QTTEST_LIB" ]; then
            echo "cannot find PySide6.QtTest or libQt6Test.so.6 in the container" >&2
            exit 2
        fi
        # QtTest.abi3.so's RUNPATH starts with $ORIGIN, so libQt6Test goes beside it.
        cp "$HOST_PYSIDE/QtTest.abi3.so" "$BUNDLE_PYSIDE/"
        cp -L "$QTTEST_LIB" "$BUNDLE_PYSIDE/libQt6Test.so.6"
    fi

    TEST_HOME="$EXTRACT_DIR/home"
    mkdir -p "$TEST_HOME/.config/INRIA" "$TEST_HOME/.cache"
    printf '[General]\ncheckForUpdates=false\n' > "$TEST_HOME/.config/INRIA/Natron.conf"

    # Scripts write fixtures with the container's oiiotool, which needs the library path that
    # Natron itself must not see.
    TOOL_DIR="$EXTRACT_DIR/tools"
    mkdir -p "$TOOL_DIR"
    if OIIOTOOL="$(command -v oiiotool)"; then
        printf '#!/bin/bash\nLD_LIBRARY_PATH=%q exec %q "$@"\n' "${LD_LIBRARY_PATH:-}" "$OIIOTOOL" > "$TOOL_DIR/oiiotool"
        chmod +x "$TOOL_DIR/oiiotool"
    fi
fi

HOST_DISPLAY="${GUI_TEST_HOST_DISPLAY:-}"
if [ -n "$HOST_DISPLAY" ]; then
    # The library path Natron gets in host mode, which the GL probe must see too.
    if [ "$APPIMAGE" -eq 1 ]; then
        HOST_LD_ENV=(-u LD_LIBRARY_PATH)
    else
        HOST_LD_ENV=(LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64)
    fi
    if command -v xset > /dev/null 2>&1; then
        display_ok() { env -u LD_LIBRARY_PATH xset -display "$HOST_DISPLAY" q > /dev/null 2>&1; }
    elif command -v xdpyinfo > /dev/null 2>&1; then
        display_ok() { env -u LD_LIBRARY_PATH xdpyinfo -display "$HOST_DISPLAY" > /dev/null 2>&1; }
    elif command -v glxinfo > /dev/null 2>&1; then
        display_ok() { env -u LD_LIBRARY_PATH glxinfo -display "$HOST_DISPLAY" -B > /dev/null 2>&1; }
    else
        echo "none of xset, xdpyinfo or glxinfo found; cannot probe display $HOST_DISPLAY" >&2
        exit 2
    fi
    if ! display_ok; then
        echo "cannot open host display $HOST_DISPLAY (check DISPLAY value and XAUTHORITY)" >&2
        exit 2
    fi
    if command -v glxinfo > /dev/null 2>&1; then
        GL_RENDERER="$(DISPLAY="$HOST_DISPLAY" env "${HOST_LD_ENV[@]}" glxinfo -B 2>/dev/null | sed -n 's/^OpenGL renderer string: //p')"
        if [ -z "$GL_RENDERER" ]; then
            echo "glxinfo reported no GL renderer on $HOST_DISPLAY" >&2
            exit 2
        fi
        echo "host display $HOST_DISPLAY GL renderer: $GL_RENDERER"
        case "$GL_RENDERER" in
            *llvmpipe*|*softpipe*|*swrast*)
                if [ "${GUI_TEST_ALLOW_SOFTWARE_GL:-0}" != "1" ]; then
                    echo "host display $HOST_DISPLAY uses a software GL renderer; set GUI_TEST_ALLOW_SOFTWARE_GL=1 to run anyway" >&2
                    exit 2
                fi
                ;;
        esac
    else
        echo "glxinfo not found; GL renderer of $HOST_DISPLAY not checked" >&2
    fi
else
    for lib in libGLX.so.0 libGL.so.1 libOpenGL.so.0 libGLdispatch.so.0 libEGL.so.1 libGLX_mesa.so.0 libLLVM.so.21.1; do
        if [ ! -e "/usr/lib64/$lib" ]; then
            echo "missing /usr/lib64/$lib" >&2
            exit 2
        fi
        ln -s "/usr/lib64/$lib" "$GLDIR/$lib"
    done

    if [ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ] || [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
        echo "display :$DISPLAY_NUM is already in use (set GUI_TEST_DISPLAY or stop the old Xvfb)" >&2
        exit 2
    fi
    LD_LIBRARY_PATH=/usr/lib64 Xvfb ":$DISPLAY_NUM" -screen 0 "${SCREEN_SIZE}x24" +extension GLX > /dev/null 2>&1 &
    XPID=$!
    # /tmp/.X11-unix is root-owned in the container, so Xvfb listens on an abstract socket only and
    # no socket file ever appears; ask the server itself whether it is up.
    ready=0
    for _ in $(seq 1 100); do
        if xset -display ":$DISPLAY_NUM" q > /dev/null 2>&1; then
            ready=1
            break
        fi
        if ! kill -0 "$XPID" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if [ "$ready" -ne 1 ]; then
        echo "Xvfb failed to start on :$DISPLAY_NUM" >&2
        exit 2
    fi
fi

cd "$ROOT" || exit 2
if [ "$APPIMAGE" -eq 1 ]; then
    # The bundle carries its own plugins and libraries; /usr/local would shadow them.
    if [ -n "$HOST_DISPLAY" ]; then
        unset LD_LIBRARY_PATH
    else
        export LD_LIBRARY_PATH="$GLDIR"
    fi
    export HOME="$TEST_HOME" XDG_CONFIG_HOME="$TEST_HOME/.config" XDG_CACHE_HOME="$TEST_HOME/.cache"
    export PATH="$TOOL_DIR:$PATH"
else
    export OFX_PLUGIN_PATH="$ROOT/build/assets/Plugins"
    if [ -n "$HOST_DISPLAY" ]; then
        export LD_LIBRARY_PATH="/usr/local/lib:/usr/local/lib64"
    else
        export LD_LIBRARY_PATH="$GLDIR:/usr/local/lib:/usr/local/lib64"
    fi
fi
if [ -n "$HOST_DISPLAY" ]; then
    export DISPLAY="$HOST_DISPLAY"
    unset WAYLAND_DISPLAY
    export QT_QPA_PLATFORM=xcb
else
    export DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
fi
export NATRON_GUI_TEST_ROOT="$ROOT" NATRON_GUI_TEST_OUT="$OUT" GUI_TEST_SCREEN_SIZE="$SCREEN_SIZE"

# A script that needs startup scripts (init.py, initGui.py) keeps them in <script>.natronpath/
# beside it: they must be on the search path before Natron starts, long before the script runs.
SEARCH_DIR="${SCRIPT%.py}.natronpath"
if [ -d "$SEARCH_DIR" ]; then
    export NATRON_PLUGIN_PATH="$SEARCH_DIR${NATRON_PLUGIN_PATH:+:$NATRON_PLUGIN_PATH}"
fi

if [ "$APPIMAGE" -eq 0 ] && [ ! -d "$OFX_PLUGIN_PATH" ]; then
    echo "OFX_PLUGIN_PATH $OFX_PLUGIN_PATH does not exist" >&2
    exit 2
fi

# Natron's output goes to a file: a caller that stops draining our stdout would otherwise block
# Natron on a write and turn any failure into a timeout.
rm -f "$OUT/results.txt"
timeout "$TIMEOUT" "$NATRON" "$SCRIPT" > "$OUT/natron.log" 2>&1 < /dev/null
rc=$?
if [ -f "$OUT/results.txt" ]; then
    cat "$OUT/results.txt"
else
    echo "no results.txt was written"
fi
if [ "$rc" -ne 0 ]; then
    echo "--- last lines of $OUT/natron.log"
    tail -n 30 "$OUT/natron.log"
fi
if [ "$rc" -eq 124 ]; then
    echo "FAIL timed out after ${TIMEOUT}s"
fi
echo "natron exit=$rc (results in $OUT)"
exit "$rc"
