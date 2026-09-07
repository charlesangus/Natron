#!/usr/bin/env bash
# Start the packaged binaries and verify the bundle's *data* dependencies
# resolve: the Python standard library, the fontconfig configuration, the
# OpenColorIO config, the Qt xcb platform plugin and the OFX plugin layer.
#
# check-relocatable.sh answers a different question. It reads DT_NEEDED out of
# the ELF headers, so it sees shared libraries and, by construction, nothing
# else: a bundle can pass it completely and still abort on startup because no
# lib/python<X.Y> was staged. Neither check subsumes the other.
#
# "Far enough" is past every point at which the defects of this milestone would
# have surfaced. --version is not far enough: it returns from CLArgs before the
# interpreter, the settings and the plugin cache are touched, which is exactly
# why a bundle that could not import 'encodings' shipped. The interpreter mode
# (-t) runs the full AppManager::loadInternal() -- fontconfig, Python, OFX
# plugins, OpenColorIO -- and then reads Python from stdin, so it sources a
# probe script and exits on EOF. The GUI binary is then started separately
# against a real X server, because background mode builds a QCoreApplication
# and so never loads a platform plugin at all: this is what exercises xcb, the
# library closure that broke first in this milestone. Running the GUI under
# QT_QPA_PLATFORM=offscreen would not -- offscreen never opens libqxcb.so.
#
# Passing here does not mean the bundle renders. Xvfb has no GLX, so Natron
# disables OpenGL and the viewer cannot draw; that is a property of the test
# server, not of the bundle, and is deliberately not treated as a failure. The
# main window and the node graph are Qt raster widgets and do come up, which is
# what is asserted.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: check-startup.sh <staging-dir>" >&2
    echo "" >&2
    echo "Example: tools/release/check-startup.sh /tmp/natron-stage" >&2
    echo "" >&2
    echo "Environment:" >&2
    echo "  NATRON_STARTUP_CHECK_PRELOAD  LD_PRELOAD for the GUI probe only, for" >&2
    echo "                                hosts whose Mesa and LLVM disagree." >&2
    echo "  NATRON_STARTUP_CHECK_SCREENSHOT  write the X root grab here as PNG." >&2
    exit 1
fi

STAGE_DIR="$1"

if [[ ! -d "$STAGE_DIR" ]]; then
    echo "error: staging dir $STAGE_DIR does not exist" >&2
    exit 1
fi

for tool in timeout Xvfb; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: $tool not found in PATH" >&2
        exit 1
    fi
done

STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"

for binary in Natron NatronRenderer natron-python; do
    if [[ ! -x "$STAGE_DIR/bin/$binary" ]]; then
        echo "error: $STAGE_DIR/bin/$binary is missing or not executable" >&2
        exit 1
    fi
done

GUI_PRELOAD="${NATRON_STARTUP_CHECK_PRELOAD:-}"
SCREENSHOT="${NATRON_STARTUP_CHECK_SCREENSHOT:-}"

if [[ -n "$GUI_PRELOAD" ]]; then
    echo "warning: LD_PRELOAD=$GUI_PRELOAD is being forced into the GUI probe." >&2
    echo "warning: that is a property of this host, not of the bundle, and it is" >&2
    echo "warning: not what users will run. Do not enable it in release CI." >&2
fi

WORK_DIR="$(mktemp -d)"
XVFB_PID=""

cleanup() {
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# The bundle must start on a machine that has never seen Natron, so strip every
# variable that could let the host stand in for something the bundle failed to
# ship: an inherited PYTHONHOME or LD_LIBRARY_PATH would make this pass on the
# packaging machine and nowhere else.
BUNDLE_ENV=(
    env
    -u LD_LIBRARY_PATH
    -u LD_PRELOAD
    -u PYTHONHOME
    -u PYTHONPATH
    -u PYTHONSTARTUP
    -u FONTCONFIG_FILE
    -u FONTCONFIG_PATH
    -u OCIO
    -u QT_PLUGIN_PATH
    -u QT_QPA_PLATFORM
    -u QT_QPA_PLATFORM_PLUGIN_PATH
    -u NATRON_PLUGIN_PATH
    # Without this the probes leave __pycache__ directories behind in the very
    # tree they are inspecting, so the gate would change the artefact it exists
    # to check.
    PYTHONDONTWRITEBYTECODE=1
)

# Every probe gets its own home, and each one is a first run. Natron saves its
# settings as it goes, and a start that *finds* settings from another appearance
# or shortcut version stops on a modal question -- so reusing one home would
# leave the second probe blocked on a dialog nobody can answer. The one thing
# seeded is the answer to the update-check question, which a genuine first run
# asks and would otherwise block on too.
PROBE_ENV=()

probe_env() {
    local home="$WORK_DIR/$1"
    mkdir -p "$home/.config/INRIA" "$home/.cache"
    cat > "$home/.config/INRIA/Natron.conf" <<'NATRONCONF'
[General]
checkForUpdates=false
NATRONCONF
    PROBE_ENV=(
        "${BUNDLE_ENV[@]}"
        "HOME=$home"
        "XDG_CONFIG_HOME=$home/.config"
        "XDG_CACHE_HOME=$home/.cache"
    )
}

# Patterns that mean the bundle is incomplete even when the process manages to
# exit 0 anyway -- Natron reports several of these and carries on degraded,
# which is how they reached a user rather than the gate. GLX and OpenGL
# complaints are deliberately absent: Xvfb has no GLX, so they say nothing
# about the bundle.
FATAL_PATTERNS=(
    'Fatal Python error'
    'Failed to import encodings'
    'ModuleNotFoundError'
    'Could not find platform independent libraries'
    'Could not find platform dependent libraries'
    'not setting FONTCONFIG_PATH'
    'Failed to import PySide6'
    'no Qt platform plugin could be initialized'
    'Could not load the Qt platform plugin'
    'natron-startup-probe: FAIL'
)

SENTINEL='natron-startup-probe: OK'

report_failure() {
    local label="$1" logfile="$2" reason="$3"
    echo "error: $label: $reason" >&2
    echo "--- output of $label ---" >&2
    cat "$logfile" >&2
    echo "--- end of output ---" >&2
    exit 1
}

# Runs one probe, then insists on all three of: a zero exit status, the
# sentinel the probe script prints as its last act, and no fatal pattern
# anywhere in the combined output.
run_probe() {
    local label="$1" logfile="$2"
    shift 2

    echo "==> $label"

    local status=0
    # -k because a Natron that is already stuck on a modal dialog will not act
    # on SIGTERM either, and the gate must still come back.
    timeout -k 10 300 "$@" > "$logfile" 2>&1 < /dev/null || status=$?

    if [[ $status -eq 124 ]]; then
        report_failure "$label" "$logfile" "timed out after 300s"
    fi
    if [[ $status -ne 0 ]]; then
        report_failure "$label" "$logfile" "exited with status $status"
    fi
    if ! grep -qF "$SENTINEL" "$logfile"; then
        report_failure "$label" "$logfile" "did not reach the end of startup"
    fi

    local pattern
    for pattern in "${FATAL_PATTERNS[@]}"; do
        if grep -qF "$pattern" "$logfile"; then
            report_failure "$label" "$logfile" "reported \"$pattern\""
        fi
    done
}

cat > "$WORK_DIR/interpreter-probe.py" <<PROBE
import glob
import os
import sys
import xml.dom.minidom

BUNDLE = os.path.realpath("$STAGE_DIR")


def fail(message):
    print("natron-startup-probe: FAIL: " + message)
    sys.stdout.flush()
    # The interpreter is embedded in a running Natron; raising here would only
    # be caught and logged by interpretPythonScript(), leaving the exit status
    # zero. Leave immediately instead, with a status the caller can see.
    os._exit(3)


for attribute in ("prefix", "exec_prefix"):
    value = os.path.realpath(getattr(sys, attribute))
    if not (value == BUNDLE or value.startswith(BUNDLE + os.sep)):
        fail("sys.%s is %s, which is outside the bundle" % (attribute, value))

for name in ("encodings", "codecs", "os", "json", "sqlite3", "ssl", "ctypes"):
    try:
        __import__(name)
    except ImportError as error:
        fail("the bundled standard library cannot import %s: %s" % (name, error))

for module in (sys.modules["encodings"], sys.modules["ctypes"]):
    location = os.path.realpath(module.__file__)
    if not location.startswith(BUNDLE + os.sep):
        fail("%s was imported from %s, outside the bundle" % (module.__name__, location))

try:
    import NatronEngine  # noqa: F401
except ImportError as error:
    fail("NatronEngine did not import: %s" % error)

try:
    import PySide6  # noqa: F401
    from PySide6 import QtCore, QtGui, QtWidgets  # noqa: F401
except ImportError as error:
    fail("PySide6 did not import: %s" % error)

fontconfig_path = os.environ.get("FONTCONFIG_PATH", "")
if not fontconfig_path.startswith(BUNDLE + os.sep):
    fail("FONTCONFIG_PATH is %r, not the bundle's own configuration" % fontconfig_path)

configs = sorted(glob.glob(os.path.join(fontconfig_path, "*.conf")))
configs += sorted(glob.glob(os.path.join(fontconfig_path, "conf.d", "*.conf")))
if not any(os.path.basename(config) == "fonts.conf" for config in configs):
    fail("no fonts.conf in %s" % fontconfig_path)
for config in configs:
    try:
        xml.dom.minidom.parse(config)
    except Exception as error:
        fail("%s is not well-formed XML: %s" % (config, error))

# Settings::tryLoadOpenColorIOConfig() exports OCIO long after the interpreter
# snapshotted the environment into os.environ, so read the live one.
import ctypes

libc = ctypes.CDLL(None)
libc.getenv.restype = ctypes.c_char_p
ocio = libc.getenv(b"OCIO")
if not ocio:
    fail("no OpenColorIO configuration was resolved")

print("natron-startup-probe: interpreter prefix %s" % sys.prefix)
print("natron-startup-probe: fontconfig %s" % fontconfig_path)
print("natron-startup-probe: OCIO %s" % ocio.decode("utf-8", "replace"))
print("$SENTINEL")
sys.stdout.flush()
PROBE

cat > "$WORK_DIR/gui-probe.py" <<PROBE
import os
import sys
import traceback

SCREENSHOT = "$SCREENSHOT"


# In GUI mode AppInstance::loadPythonScript() routes the interpreter's stdout
# into the Script Editor panel, so anything printed here would never reach the
# packaging log. Write to the process's own stderr instead.
def say(message):
    os.write(2, ("natron-startup-probe: " + message + "\n").encode())


def fail(message):
    say("FAIL: " + message)
    os._exit(3)


try:
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError:
    fail("PySide6 did not import in the GUI process:\n" + traceback.format_exc())

qapplication = QtWidgets.QApplication.instance()
if qapplication is None:
    fail("no QApplication exists by the time the startup script runs")

platform = QtGui.QGuiApplication.platformName()
if platform != "xcb":
    fail("Qt came up on the %r platform plugin, not xcb" % platform)


def main_window():
    for widget in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(widget, QtWidgets.QMainWindow) and widget.isVisible():
            return widget
    return None


def unique_colours(image):
    seen = set()
    for y in range(0, image.height(), 7):
        for x in range(0, image.width(), 7):
            seen.add(image.pixel(x, y))
            if len(seen) > 64:
                return len(seen)
    return len(seen)


DEADLINE = QtCore.QDeadlineTimer(30000)


def check():
    try:
        window = main_window()
        handle = window.windowHandle() if window is not None else None

        # isExposed() only becomes true once the X server has mapped the window
        # and sent the first Expose, so this is the server's answer, not Qt's
        # intent.
        if window is None or handle is None or not handle.isExposed():
            if DEADLINE.hasExpired():
                fail("no top-level window was mapped within 30s")
            return

        # And this reads the root window's pixels back out of the X server: a
        # display with nothing mapped on it grabs as one flat colour.
        grab = QtGui.QGuiApplication.primaryScreen().grabWindow(0)
        colours = unique_colours(grab.toImage())
        if colours < 16:
            if DEADLINE.hasExpired():
                fail("the X root window grabbed as %d colours -- nothing painted" % colours)
            return

        views = window.findChildren(QtWidgets.QGraphicsView)
        if not views:
            fail("the main window has no QGraphicsView, so no node graph came up")

        if SCREENSHOT:
            grab.save(SCREENSHOT)

        say("platform plugin %s" % platform)
        say("window %r %dx%d mapped and exposed" % (
            window.windowTitle(), window.width(), window.height()))
        say("root window grab %dx%d, %d+ distinct colours" % (
            grab.width(), grab.height(), colours))
        say("node graph views: %d" % len(views))
        say("OK")
    except SystemExit:
        raise
    except Exception:
        fail("the GUI probe raised:\n" + traceback.format_exc())
    timer.stop()
    qapplication.quit()


# GuiApplicationManager sources this script before it enters the event loop, so
# the first tick lands once the main window is up. Retry rather than assume:
# mapping and the first paint are asynchronous, and how long they take is the
# X server's business.
timer = QtCore.QTimer()
timer.setInterval(250)
timer.timeout.connect(check)
timer.start()
PROBE

probe_env interpreter-home
run_probe "bundled interpreter (natron-python)" "$WORK_DIR/natron-python.log" \
    "${PROBE_ENV[@]}" "$STAGE_DIR/bin/natron-python" -c "
import os, sys
bundle = os.path.realpath('$STAGE_DIR')
prefix = os.path.realpath(sys.prefix)
if not (prefix == bundle or prefix.startswith(bundle + os.sep)):
    print('natron-startup-probe: FAIL: sys.prefix is ' + prefix)
    raise SystemExit(3)
import encodings, ctypes, json
print('$SENTINEL')
"

probe_env renderer-home
run_probe "engine startup (NatronRenderer -t)" "$WORK_DIR/renderer.log" \
    "${PROBE_ENV[@]}" "$STAGE_DIR/bin/NatronRenderer" -t "$WORK_DIR/interpreter-probe.py"

echo "==> Starting Xvfb"

# Xvfb's own -displayfd search is not usable here: it walks display numbers
# from zero and treats a socket it cannot create as a taken display, so on a
# host where /tmp/.X11-unix is not writable by this user -- a container with
# the directory bind-mounted in, for one -- it exhausts its range and dies,
# even though the abstract socket it also opens works perfectly. Pick the
# display from the lock files instead, and confirm the server by connecting a
# real Qt client from the bundle.
probe_env gui-home

DISPLAY_NUMBER=""
for candidate in $(seq 90 120); do
    [[ -e "/tmp/.X${candidate}-lock" ]] && continue

    Xvfb ":$candidate" -screen 0 1600x1000x24 -nolisten tcp \
        > "$WORK_DIR/xvfb.log" 2>&1 &
    XVFB_PID=$!

    for _ in $(seq 1 40); do
        if ! kill -0 "$XVFB_PID" 2>/dev/null; then
            break
        fi
        if env "${PROBE_ENV[@]:1}" "DISPLAY=:$candidate" \
               "$STAGE_DIR/bin/natron-python" -c '
import sys
from PySide6 import QtGui
application = QtGui.QGuiApplication(sys.argv)
raise SystemExit(0 if application.platformName() == "xcb" else 1)
' >/dev/null 2>&1; then
            DISPLAY_NUMBER="$candidate"
            break
        fi
        sleep 0.5
    done

    [[ -n "$DISPLAY_NUMBER" ]] && break

    kill "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
    XVFB_PID=""
done

if [[ -z "$DISPLAY_NUMBER" ]]; then
    echo "error: no Xvfb display could be reached with the bundle's own Qt" >&2
    cat "$WORK_DIR/xvfb.log" >&2
    exit 1
fi

echo "    Xvfb display :$DISPLAY_NUMBER, reached over xcb"

GUI_ENV=("${PROBE_ENV[@]}" "DISPLAY=:$DISPLAY_NUMBER")
if [[ -n "$GUI_PRELOAD" ]]; then
    GUI_ENV+=("LD_PRELOAD=$GUI_PRELOAD" "LIBGL_ALWAYS_SOFTWARE=1")
fi

run_probe "GUI startup on xcb (Natron under Xvfb :$DISPLAY_NUMBER)" "$WORK_DIR/gui.log" \
    "${GUI_ENV[@]}" "$STAGE_DIR/bin/Natron" "$WORK_DIR/gui-probe.py"

grep -h '^natron-startup-probe: ' "$WORK_DIR/renderer.log" "$WORK_DIR/gui.log" \
    | grep -v "$SENTINEL" | sed 's/^natron-startup-probe: /    /'

echo "==> Startup verified: Python, fontconfig, OpenColorIO, OFX and the xcb GUI all initialise"
