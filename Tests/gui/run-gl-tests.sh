#!/usr/bin/env bash
# Usage: Tests/gui/run-gl-tests.sh <gtest-filter> [Tests-binary]
#
# Runs the gtest binary under a GLX-capable Xvfb with NATRON_TEST_REQUIRE_GL=1, so a GL test that
# cannot get a context fails instead of skipping, and exits with gtest's status. Meant for the
# natron-dev container, e.g.
#   docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && Tests/gui/run-gl-tests.sh "GLScheduler.*"'
#
# Plain xvfb-run aborts there with "Could not initialize GLX": /etc/ld.so.preload forces a glvnd
# libGL without a mesa vendor, which Xvfb and the tests would both pick up. Xvfb therefore runs
# against /usr/lib64, and the tests get only the system GL libraries through a directory of
# symlinks (all of /usr/lib64 first would shadow the ASWF freetype, harfbuzz, zlib...).
#
# Environment: GL_TEST_DISPLAY (default 78).
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <gtest-filter> [Tests-binary]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FILTER="$1"
TESTS="${2:-$ROOT/build/release/Tests/Tests}"
DISPLAY_NUM="${GL_TEST_DISPLAY:-78}"
PLUGINS="$ROOT/build/assets/Plugins"

if [ ! -x "$TESTS" ]; then
    echo "no Tests binary at $TESTS" >&2
    exit 2
fi
if [ ! -d "$PLUGINS" ]; then
    echo "OFX_PLUGIN_PATH $PLUGINS does not exist" >&2
    exit 2
fi

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
    echo "display :$DISPLAY_NUM is already in use (set GL_TEST_DISPLAY or stop the old Xvfb)" >&2
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
export OFX_PLUGIN_PATH="$PLUGINS"
export LD_LIBRARY_PATH="$GLDIR:/usr/local/lib:/usr/local/lib64"
export DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
export NATRON_TEST_REQUIRE_GL=1

"$TESTS" --gtest_filter="$FILTER"
exit $?
