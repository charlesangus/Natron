"""Checks that startup never leaves Qt's widget compositor without its GL context.

Qt records which GL context it made current on each thread and skips its own make-current while
that record matches. When startup bound a Natron context on the GUI thread and then released it
to nothing, Qt went on compiling its compositor shaders with no context bound: "Failed to compile
shader" with an empty log, then a crash flushing the backing store. With NATRON_DIAG_GL=1 Natron
logs a checkpoint at each startup step and marks it MISMATCH whenever Qt's record and the real
binding disagree, so a launch can show the stale binding even when the compositor happens not to
flush in that window.

This file is both the driver and the script each launch runs. Run it with python3 inside the
natron-dev container (it relaunches Natron, so it cannot run inside Natron itself):
    docker exec -w /home/bosley/git/Natron natron-dev \\
        python3 Tests/gui/m161_startup_compositor.py build/release/App/Natron

The binary may be an AppImage. Every launch goes through Tests/gui/run-gui-test.sh, which starts a
fresh Xvfb and runs this file inside Natron. 10 launches put
Tests/gui/m161_startup_compositor.fixture/ (an initGui.py) on NATRON_PLUGIN_PATH and 10 do not;
in each half every other launch sets NATRON_DIAG_GL_FORCE_PROBE_CONTEXT=1, which makes the GLX
renderer probe bind a probe context even under Mesa, the path NVIDIA drivers take.

Each launch gets a fresh HOME whose Natron.conf has checkForUpdates=false. A launch passes when
Natron exits 0 after the main window was shown, exposed and closed by this script; its log has no
"Failed to compile shader", "Failed to build graphics pipeline" or segmentation fault line and no
MISMATCH checkpoint; the diagnostics ran (checkpoints up to first-expose are logged); a forced
launch took the probe-context path; and with the fixture, initGui.py ran and its action is under
the Merge submenu.

Results go to $NATRON_GUI_TEST_OUT/results.txt (default build/gui-test-out/m161-startup-compositor/)
as PASS/FAIL/NOTE lines, one SECTION per launch and a SUMMARY line; each launch's natron.log is in
launch-NN-<config>/. Exit status is 0 only if every check passed.

Environment: M161_LAUNCHES_PER_HALF (default 10), M161_LAUNCH_TIMEOUT (seconds per launch,
default 120), M161_SETTLE_MS (time the window stays up once exposed, default 3000), and
GUI_TEST_DISPLAY, passed on to run-gui-test.sh.
"""
import os
import sys

try:
    import NatronEngine  # noqa: F401
    IN_NATRON = True
except ImportError:
    IN_NATRON = False

FORCE_PROBE_ENV = "NATRON_DIAG_GL_FORCE_PROBE_CONTEXT"
FIXTURE_ACTION = "M161 Merge Plus"
EXPECT_INITGUI_ENV = "M161_EXPECT_INITGUI"
LAUNCH_TAG = "[m161-launch]"


def launch_main():
    """Runs inside Natron: waits for the main window to be shown and exposed, checks the fixture's
    action when initGui.py was expected, then closes the project so Natron quits on its own."""
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication

    out = os.environ.get("NATRON_GUI_TEST_OUT", os.getcwd())
    if not os.path.isdir(out):
        os.makedirs(out)
    results = open(os.path.join(out, "results.txt"), "w")
    settle_ms = int(os.environ.get("M161_SETTLE_MS", "3000"))
    expect_initgui = os.environ.get(EXPECT_INITGUI_ENV) == "1"
    start_timeout_ms = 60000
    held = []

    def say(line):
        results.write(line + "\n")
        results.flush()
        # Natron's sys.stderr catcher has no flush(), so the descriptor itself is written.
        os.write(2, ("%s %s\n" % (LAUNCH_TAG, line)).encode())

    def main_window():
        for w in QApplication.topLevelWidgets():
            if w.inherits("QMainWindow") and w.isVisible():
                return w
        return None

    def instance():
        g = globals()
        for name in ("app", "app1"):
            if g.get(name) is not None:
                return g[name]
        return NatronEngine.natron.getInstance(0)

    def force_exit():
        say("FAIL Natron did not quit within 10s of closing the project")
        results.close()
        os._exit(1)

    def check_fixture():
        import NatronGui
        menu = NatronGui.natron.nodesMenu()
        held.append(menu)
        merge = [a for a in menu.actions() if a.text() == "Merge"]
        if not merge:
            say("FAIL the nodes menu has no Merge submenu")
            return
        held.append(merge[0])
        sub = merge[0].menu()
        found = sub is not None and any(a.text() == FIXTURE_ACTION for a in sub.actions())
        say(("PASS" if found else "FAIL") + " initGui.py's action is under the Merge submenu")

    def quit_natron():
        if expect_initgui:
            check_fixture()
        say("NOTE quitting")
        QTimer.singleShot(10000, force_exit)
        try:
            instance().closeProject()
        except Exception as e:
            say("FAIL closeProject raised %r" % (e,))
            results.close()
            os._exit(1)

    waited = [0]

    def poll():
        w = main_window()
        handle = w.windowHandle() if w is not None else None
        if handle is not None and handle.isExposed():
            say("PASS main window shown and exposed")
            QTimer.singleShot(settle_ms, quit_natron)
            return
        waited[0] += 100
        if waited[0] >= start_timeout_ms:
            say("FAIL main window not exposed within %ds" % (start_timeout_ms // 1000))
            results.close()
            os._exit(1)
        QTimer.singleShot(100, poll)

    say("NOTE launch script started")
    QTimer.singleShot(0, poll)


# --- driver ------------------------------------------------------------------------------------

def driver_main(argv):
    import shutil
    import subprocess

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    binary = argv[1] if len(argv) > 1 else os.path.join(root, "build", "release", "App", "Natron")
    binary = os.path.abspath(binary)
    out = os.path.abspath(os.environ.get("NATRON_GUI_TEST_OUT")
                          or os.path.join(root, "build", "gui-test-out", "m161-startup-compositor"))
    per_half = int(os.environ.get("M161_LAUNCHES_PER_HALF", "10"))
    timeout_s = os.environ.get("M161_LAUNCH_TIMEOUT", "120")
    runner = os.path.join(here, "run-gui-test.sh")
    fixture = os.path.join(here, "m161_startup_compositor.fixture")
    shader_failures = ("Failed to compile shader", "Failed to build graphics pipeline", "qt-gl-failure")

    if not os.path.exists(binary):
        print("no Natron binary at %s" % binary, file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    results = open(os.path.join(out, "results.txt"), "w")
    failures = []
    sections = []

    def report(line):
        results.write(line + "\n")
        results.flush()
        print(line, flush=True)

    def check(ok, label):
        name = sections[-1][0]
        report(("PASS " if ok else "FAIL ") + "[%s] %s" % (name, label))
        sections[-1][1 if ok else 2] += 1
        if not ok:
            failures.append("[%s] %s" % (name, label))
        return ok

    launches = []
    for with_initgui in (True, False):
        for i in range(per_half):
            launches.append((with_initgui, i % 2 == 0))

    report("NOTE binary %s, %d launches" % (binary, len(launches)))
    for n, (with_initgui, forced) in enumerate(launches, 1):
        name = "launch-%02d-%s-%s" % (n, "initgui" if with_initgui else "plain",
                                      "forced-probe" if forced else "native-probe")
        sections.append([name, 0, 0])
        launch_out = os.path.join(out, name)
        if os.path.isdir(launch_out):
            shutil.rmtree(launch_out)
        home = os.path.join(launch_out, "home")
        os.makedirs(os.path.join(home, ".config", "INRIA"))
        os.makedirs(os.path.join(home, ".cache"))
        with open(os.path.join(home, ".config", "INRIA", "Natron.conf"), "w") as f:
            f.write("[General]\ncheckForUpdates=false\n")

        env = dict(os.environ)
        for var in (FORCE_PROBE_ENV, EXPECT_INITGUI_ENV, "NATRON_PLUGIN_PATH"):
            env.pop(var, None)
        env.update({
            "NATRON_DIAG_GL": "1",
            "NATRON_GUI_TEST_OUT": launch_out,
            "GUI_TEST_TIMEOUT": timeout_s,
            # run-gui-test.sh replaces these with its own fresh HOME for an AppImage.
            "HOME": home,
            "XDG_CONFIG_HOME": os.path.join(home, ".config"),
            "XDG_CACHE_HOME": os.path.join(home, ".cache"),
        })
        if forced:
            env[FORCE_PROBE_ENV] = "1"
        if with_initgui:
            env["NATRON_PLUGIN_PATH"] = fixture
            env[EXPECT_INITGUI_ENV] = "1"

        p = subprocess.run([runner, os.path.abspath(__file__), binary], cwd=root, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with open(os.path.join(launch_out, "runner.txt"), "w") as f:
            f.write(p.stdout)
        log_path = os.path.join(launch_out, "natron.log")
        try:
            with open(log_path, errors="replace") as f:
                log = f.read().splitlines()
        except OSError:
            log = []
        launch_lines = [l.split(LAUNCH_TAG, 1)[1].strip() for l in log if LAUNCH_TAG in l]

        if not check(p.returncode == 0, "Natron exited 0 (exit %d)" % p.returncode):
            for l in p.stdout.splitlines()[-5:]:
                report("NOTE   " + l)
        check(not any("segmentation fault" in l.lower() for l in log), "no segmentation fault")
        check("PASS main window shown and exposed" in launch_lines, "main window shown and exposed")
        check("NOTE quitting" in launch_lines, "the launch script closed the project")
        for l in launch_lines:
            if l.startswith("FAIL "):
                check(False, l[len("FAIL "):])

        bad = [l for l in log if any(s in l for s in shader_failures)]
        check(not bad, "no compositor shader or pipeline failure"
              + ((": " + bad[0].strip()[:200]) if bad else ""))
        mismatches = [l for l in log if "MISMATCH" in l]
        check(not mismatches, "no MISMATCH checkpoint (%d found)" % len(mismatches))
        for l in mismatches[:3]:
            report("NOTE   " + l.strip()[:300])

        checkpoints = [l for l in log if "[natron-gl-diag]" in l and "checkpoint name=" in l]
        check(any("checkpoint name=show:after" in l for l in checkpoints)
              and any("checkpoint name=first-expose:before" in l for l in checkpoints),
              "NATRON_DIAG_GL checkpoints ran through first-expose (%d logged)" % len(checkpoints))
        if forced:
            check(any(FORCE_PROBE_ENV + ": probed the OpenGL renderer through a probe context" in l for l in log),
                  "the renderer probe bound a probe context")
        if with_initgui:
            check(any("[m161-fixture] initGui.py ran" in l for l in log), "initGui.py ran")
        stale = sum(1 for l in log if "QT-STALE" in l)
        if stale:
            report("NOTE [%s] %d QT-STALE make-current line(s)" % (name, stale))

    for name, passed, failed in sections:
        report("SECTION %s: %d passed, %d failed" % (name, passed, failed))
    report("SUMMARY %d failure(s)" % len(failures))
    results.close()
    return 1 if failures else 0


if IN_NATRON:
    launch_main()
elif __name__ == "__main__":
    sys.exit(driver_main(sys.argv))
