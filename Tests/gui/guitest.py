"""Plumbing shared by the Natron GUI test scripts that Tests/gui/run-gui-test.sh runs.

Results go to $NATRON_GUI_TEST_OUT/results.txt (default build/gui-test-out/) as PASS/FAIL/SHOT/
NOTE lines, one SECTION line per section when sections are used, and a final SUMMARY line; the
process exits 0 only if every check passed.

Steps run as a generator on the Qt event loop (see run()), so Natron keeps rendering between them:
    yield ("sleep", ms)
    ok = yield ("until", predicate, timeout_seconds)
"""
import os
import time
import traceback

import NatronEngine
from PySide6.QtCore import QTimer
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QApplication

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
OUT = os.environ.get("NATRON_GUI_TEST_OUT", os.path.join(ROOT, "build", "gui-test-out"))
FIXTURES = os.path.join(ROOT, "Tests", "fixtures")

if not os.path.isdir(OUT):
    os.makedirs(OUT)
_results = open(os.path.join(OUT, "results.txt"), "w")
_failures = []
_sections = []


def report(line):
    _results.write(line + "\n")
    _results.flush()


def section(name):
    """Later checks count towards, and are prefixed with, this section until the next one."""
    _sections.append([name, 0, 0])


def check(ok, label):
    prefix = ("[%s] " % _sections[-1][0]) if _sections else ""
    report(("PASS " if ok else "FAIL ") + prefix + label)
    if _sections:
        _sections[-1][1 if ok else 2] += 1
    if not ok:
        _failures.append(prefix + label)
    return ok


def finish():
    for name, passed, failed in _sections:
        report("SECTION %s: %d passed, %d failed" % (name, passed, failed))
    report("SUMMARY %d failure(s)" % len(_failures))
    _results.close()
    os._exit(1 if _failures else 0)


def require(ok, label, detail=""):
    """A failed setup step ends the run at once: nothing after it could mean anything."""
    check(ok, label + (" (" + detail + ")" if detail and not ok else ""))
    if not ok:
        finish()


_plugin_ids = set(NatronEngine.natron.getPluginIDs())


def plugin_problem(plugin_id):
    return "plugin %s; OFX_PLUGIN_PATH=%r" % (
        "registered but createNode returned None" if plugin_id in _plugin_ids else "not registered",
        os.environ.get("OFX_PLUGIN_PATH"))


def class_name(w):
    return w.metaObject().className().split("::")[-1]


def widgets_of(name):
    found = []
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == name:
                found.append(w)
        except RuntimeError:
            continue
    return found


def close_all_panels():
    from PySide6.QtCore import QMetaObject
    for w in QApplication.allWidgets():
        try:
            if w.metaObject().indexOfMethod("closePanel()") >= 0 and w.isVisible():
                QMetaObject.invokeMethod(w, "closePanel")
        except RuntimeError:
            continue
    QApplication.processEvents()


def any_panel_open():
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == "NodeSettingsPanel" and w.isVisible():
                return True
        except RuntimeError:
            continue
    return False


def viewer_up():
    return any(w.isVisible() for w in widgets_of("ViewerGL"))


def viewer_messages():
    """The persistent messages the viewer is drawing, one string per message."""
    messages = []
    for w in widgets_of("ViewerGL"):
        value = w.property("natronPersistentMessages")
        if value:
            messages.extend(str(m) for m in value)
    return messages


def viewer_centre_colour():
    """The colour on screen at the centre of the first visible viewer, or None if there is none.
    Read from a screen grab: a widget grab of the GL viewer can come out blank under llvmpipe."""
    visible = [w for w in widgets_of("ViewerGL") if w.isVisible()]
    if not visible:
        return None
    w = visible[0]
    centre = w.mapToGlobal(w.rect().center())
    image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
    c = image.pixelColor(centre.x(), centre.y())
    return (c.red(), c.green(), c.blue())


def shot(name):
    QApplication.processEvents()
    path = os.path.join(OUT, name)
    if QGuiApplication.primaryScreen().grabWindow(0).save(path):
        report("SHOT " + path)
    else:
        report("NOTE could not save screenshot " + path)
    return path


def run(gen):
    """Drives gen on the Qt event loop and calls finish() when it ends or raises."""
    def advance(value):
        try:
            cmd = gen.send(value)
        except StopIteration:
            finish()
            return
        except Exception:
            check(False, "script error: " + traceback.format_exc())
            finish()
            return
        if cmd[0] == "sleep":
            QTimer.singleShot(cmd[1], lambda: advance(None))
            return
        predicate, timeout = cmd[1], cmd[2]
        deadline = time.time() + timeout

        def poll():
            try:
                ok = bool(predicate())
            except Exception:
                ok = False
            if ok:
                advance(True)
            elif time.time() > deadline:
                advance(False)
            else:
                QTimer.singleShot(50, poll)

        poll()

    QTimer.singleShot(0, lambda: advance(None))
