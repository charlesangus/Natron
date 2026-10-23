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
LD_LIBRARY_PATH=/usr/lib64 Xvfb ":$DISPLAY_NUM" -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
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

cd "$ROOT" || exit 2
if [ "$APPIMAGE" -eq 1 ]; then
    # The bundle carries its own plugins and libraries; /usr/local would shadow them.
    export LD_LIBRARY_PATH="$GLDIR"
    export HOME="$TEST_HOME" XDG_CONFIG_HOME="$TEST_HOME/.config" XDG_CACHE_HOME="$TEST_HOME/.cache"
    export PATH="$TOOL_DIR:$PATH"
else
    export OFX_PLUGIN_PATH="$ROOT/build/assets/Plugins"
    export LD_LIBRARY_PATH="$GLDIR:/usr/local/lib:/usr/local/lib64"
fi
export DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
export NATRON_GUI_TEST_ROOT="$ROOT" NATRON_GUI_TEST_OUT="$OUT"

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
