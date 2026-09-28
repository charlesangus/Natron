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
if [ ! -x "$NATRON" ]; then
    echo "no Natron binary at $NATRON" >&2
    exit 2
fi
mkdir -p "$OUT"

GLDIR="$(mktemp -d)"
XPID=""
cleanup() {
    if [ -n "$XPID" ]; then
        kill "$XPID" 2>/dev/null
        wait "$XPID" 2>/dev/null
    fi
    rm -rf "$GLDIR"
}
trap cleanup EXIT

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
export OFX_PLUGIN_PATH="$ROOT/build/assets/Plugins"
export LD_LIBRARY_PATH="$GLDIR:/usr/local/lib:/usr/local/lib64"
export DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
export NATRON_GUI_TEST_ROOT="$ROOT" NATRON_GUI_TEST_OUT="$OUT"

if [ ! -d "$OFX_PLUGIN_PATH" ]; then
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
