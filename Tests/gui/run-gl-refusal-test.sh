#!/usr/bin/env bash
# Usage: Tests/gui/run-gl-refusal-test.sh [natron-binary]
#
# Checks that the GUI refuses an OpenGL context older than 4.5 compatibility: Natron is started
# through run-gui-test.sh with an OpenGL 2.1 request (NATRON_GUI_GL_REQUEST_VERSION) on a Mesa
# capped at 2.1 (MESA_GL_VERSION_OVERRIDE; llvmpipe would otherwise hand out its newest version),
# and must exit with status 1 and the refusal message on stderr, before any startup script runs.
# NatronRenderer is not affected by the check; render something with it separately to confirm.
#
# Environment: GL_REFUSAL_VERSION (default 2.1) and GL_REFUSAL_PROFILE (unset, or core) change
# the forced request; everything run-gui-test.sh reads applies too.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${NATRON_GUI_TEST_OUT:-$ROOT/build/gui-test-out}"
VERSION="${GL_REFUSAL_VERSION:-2.1}"

export NATRON_GUI_GL_REQUEST_VERSION="$VERSION"
if [ -n "${GL_REFUSAL_PROFILE:-}" ]; then
    export NATRON_GUI_GL_REQUEST_PROFILE="$GL_REFUSAL_PROFILE"
else
    export MESA_GL_VERSION_OVERRIDE="$VERSION"
fi
export GUI_TEST_TIMEOUT="${GUI_TEST_TIMEOUT:-120}"

"$ROOT/Tests/gui/run-gui-test.sh" "$ROOT/Tests/gui/gl_context_refusal.py" "$@"
rc=$?

status=0
if [ "$rc" -ne 1 ]; then
    echo "FAIL Natron exited with $rc, expected 1"
    status=1
fi
if grep -q "needs an OpenGL 4.5 compatibility-profile context" "$OUT/natron.log" 2>/dev/null; then
    echo "PASS refusal message:"
    grep -A3 "needs an OpenGL 4.5 compatibility-profile context" "$OUT/natron.log"
else
    echo "FAIL no refusal message in $OUT/natron.log"
    status=1
fi
if [ -f "$OUT/results.txt" ]; then
    echo "FAIL a startup script ran, so the GUI got past the check"
    status=1
fi
exit "$status"
