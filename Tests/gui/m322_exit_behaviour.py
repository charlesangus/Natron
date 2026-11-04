"""Natron leaves promptly and with the right exit code, both when the user quits and when it is told
to terminate. Each case starts a separate Natron on the same display with a small child script: the
child builds Constant -> Viewer, saves the project (so quitting asks nothing), writes a marker file,
and then either picks File > Quit itself or starts playback and waits for a signal from here.

  - File > Quit: exit code 0, within EXIT_LIMIT_S of the click.
  - SIGTERM during playback: exit code 143 (128 + SIGTERM), within EXIT_LIMIT_S.
  - two SIGTERMs in a row during playback: exit code 143, within SECOND_SIGNAL_LIMIT_S.

A child that is still running EXIT_TIMEOUT_S after the quit or signal is killed and fails its case.
That timeout is above the default shutdown watchdog (30 s), so a teardown the watchdog had to cut
short shows as exit code 124 rather than as a kill. Each child's output is kept in
$NATRON_GUI_TEST_OUT/m322/<case>.log.

The run measures the Natron process itself, not the script inside it, which is why the cases run in
child processes: guitest ends its own process with os._exit(), which never reaches Natron's exit path.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m322_exit_behaviour.py
"""
import os
import signal
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

READY_TIMEOUT_S = 240.0
EXIT_TIMEOUT_S = 45.0
EXIT_LIMIT_S = 15.0
SECOND_SIGNAL_LIMIT_S = 5.0
SECOND_SIGNAL_DELAY_S = 0.2

CHILD_SCRIPT = r'''
import os
import time
import traceback

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication

MODE = os.environ["M322_CHILD_MODE"]
MARKER = os.environ["M322_CHILD_MARKER"]
PROJECT = os.environ["M322_CHILD_PROJECT"]
STARTUP_TIMEOUT_S = 180.0
RENDER_SETTLE_MS = 2000
PLAYBACK_SETTLE_MS = 1500


def mark(text):
    partial = MARKER + ".part"
    with open(partial, "w") as f:
        f.write(text + "\n")
    os.replace(partial, MARKER)


def class_name(w):
    return w.metaObject().className().split("::")[-1]


def viewer_up():
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == "ViewerGL" and w.isVisible():
                return True
        except RuntimeError:
            continue
    return False


def quit_action():
    for w in QApplication.allWidgets():
        try:
            for action in w.actions():
                if action.text().replace("&", "") == "Quit":
                    return action
        except RuntimeError:
            continue
    return None


def guarded(fn):
    def call():
        try:
            fn()
        except Exception:
            mark("error " + traceback.format_exc().replace("\n", " | "))
    return call


deadline = time.time() + STARTUP_TIMEOUT_S


@guarded
def wait_for_viewer():
    if viewer_up():
        build_graph()
    elif time.time() > deadline:
        mark("error the viewer never came up")
    else:
        QTimer.singleShot(200, wait_for_viewer)


def build_graph():
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    constant = app.createNode("net.sf.openfx.ConstantPlugin")
    constant.getParam("color").set(0.2, 0.6, 0.3, 1.0)
    viewer.connectInput(0, constant)
    QTimer.singleShot(RENDER_SETTLE_MS, after_render)


@guarded
def after_render():
    if not app.saveProjectAs(PROJECT):
        mark("error could not save the project to " + PROJECT)
        return
    if MODE == "quit":
        action = quit_action()
        if action is None:
            mark("error no Quit action in any menu")
            return
        mark("ready %.6f" % time.time())
        # Off this callback's stack: quitting closes the project whose Python app runs this script.
        QTimer.singleShot(0, action.trigger)
    else:
        app.getViewer("Viewer1").startForward()
        QTimer.singleShot(PLAYBACK_SETTLE_MS, guarded(lambda: mark("ready %.6f" % time.time())))


QTimer.singleShot(0, wait_for_viewer)
'''

try:
    import guitest as gt
    from guitest import check, report, require

    OUT_DIR = os.path.join(gt.OUT, "m322")
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)
    CHILD_PATH = os.path.join(OUT_DIR, "exit_child.py")
    with open(CHILD_PATH, "w") as f:
        f.write(CHILD_SCRIPT)
    # In an AppImage this is the bundled binary AppRun exec'd, with AppRun's environment inherited.
    NATRON = os.readlink("/proc/self/exe")

    CASES = [
        # name, child mode, signals sent once ready, expected exit code, time limit
        ("menu_quit", "quit", [], 0, EXIT_LIMIT_S),
        ("sigterm_playback", "play", [signal.SIGTERM], 128 + signal.SIGTERM, EXIT_LIMIT_S),
        ("double_sigterm_playback", "play", [signal.SIGTERM, signal.SIGTERM], 128 + signal.SIGTERM,
         SECOND_SIGNAL_LIMIT_S),
    ]

    def read_marker(path):
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            return ""

    def run_case(name, mode, signals, expected_rc, limit_s):
        gt.section(name)
        marker = os.path.join(OUT_DIR, name + ".marker")
        for stale in (marker, marker + ".part"):
            if os.path.exists(stale):
                os.remove(stale)
        env = dict(os.environ)
        env["M322_CHILD_MODE"] = mode
        env["M322_CHILD_MARKER"] = marker
        env["M322_CHILD_PROJECT"] = os.path.join(OUT_DIR, name + ".ntp")
        log_path = os.path.join(OUT_DIR, name + ".log")
        log = open(log_path, "w")
        child = subprocess.Popen([NATRON, CHILD_PATH], env=env, stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT)
        report("NOTE %s: started pid %d, output in %s" % (name, child.pid, log_path))
        try:
            ok = yield ("until", lambda: read_marker(marker) or child.poll() is not None, READY_TIMEOUT_S)
            state = read_marker(marker)
            ready = ok and state.startswith("ready ")
            check(ready, "the child reached its quit point (marker %r, exit code %r)" % (state, child.poll()))
            if not ready:
                return

            if signals:
                start = time.time()
                for i, sig in enumerate(signals):
                    if i:
                        yield ("sleep", int(SECOND_SIGNAL_DELAY_S * 1000))
                    if child.poll() is None:
                        os.kill(child.pid, sig)
            else:
                start = float(state.split()[1])

            exited = yield ("until", lambda: child.poll() is not None, EXIT_TIMEOUT_S)
            elapsed = time.time() - start
            rc = child.poll()
            report("NOTE %s: exit code %r after %.2fs" % (name, rc, elapsed))
            check(exited, "the child exits within %ds" % EXIT_TIMEOUT_S)
            if not exited:
                return
            check(rc == expected_rc, "exit code is %d (got %r)" % (expected_rc, rc))
            check(elapsed <= limit_s, "exits within %.0fs (took %.2fs)" % (limit_s, elapsed))
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
                report("NOTE %s: killed the child" % name)
            log.close()

    def steps():
        require(os.access(NATRON, os.X_OK), "the Natron binary is executable", NATRON)
        for case in CASES:
            yield from run_case(*case)

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
