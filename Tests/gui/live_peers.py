"""Shared plumbing for the two-process live session GUI tests: one Natron hosts on loopback and
starts a second one, the client, on the same display. The host writes commands to
<sync>/cmd-<n>, the client answers in <sync>/ack-<n>, and the client's own checks are copied into
the host's results at the end. A test calls bind(app, role) first, and the role-specific helpers
take their callbacks as arguments.
"""
import os
import re
import subprocess
import time
import traceback

import NatronEngine
from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
from PySide6.QtGui import QAction, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import (QApplication, QComboBox, QDialogButtonBox, QLabel, QLineEdit, QMainWindow,
                               QSpinBox, QWidget)

from shiboken6 import isValid
import guitest as gt
from guitest import check, report, require

STARTUP_TIMEOUT_S = 90.0
COMMAND_TIMEOUT_S = 60.0
SETTLE_MS = 300

_state = {"app": None, "role": "host", "sync": None, "process": None, "n": 0, "win": None}
_keep = []


def bind(app, role_variable):
    """Remembers the app and returns this process's role and sync directory."""
    _state["app"] = app
    _state["role"] = os.environ.get(role_variable, "host")
    sync = os.environ.get(role_variable + "_SYNC") or os.path.join(gt.OUT, "sync")
    if not os.path.isdir(sync):
        os.makedirs(sync)
    _state["sync"] = sync
    return _state["role"], sync


def app():
    return _state["app"]


# ---- windows and widgets -------------------------------------------------------------------

def main_gui():
    """The main window, always through the same wrapper.

    PySide makes a wrapper that findChild returns a child of the window's wrapper and marks it
    deleted once that wrapper is collected, although the C++ object lives on; a temporary window
    wrapper would kill the action or label found through it as soon as the lookup returns."""
    win = _state.get("win")
    if win is not None and isValid(win):
        return win
    for w in QApplication.topLevelWidgets():
        if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
            _state["win"] = w
            return w
    return None


def activate():
    win = main_gui()
    if win is None:
        return
    win.activateWindow()
    win.raise_()
    deadline = time.time() + 10.0
    while not win.isActiveWindow() and time.time() < deadline:
        QApplication.processEvents()
        time.sleep(0.02)


def graph_view():
    views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
    return views[0] if views else None


def viewer_gl():
    views = [v for v in gt.widgets_of("ViewerGL") if v.isVisible()]
    return views[0] if views else None


def send_key(widget, key, modifiers=Qt.NoModifier):
    QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
    QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
    QApplication.processEvents()


def _window_action(prefix=None, object_name=None):
    win = main_gui()
    if win is None:
        return None
    if object_name is not None:
        return win.findChild(QAction, object_name)
    for a in win.actions():
        if a.text().replace("&", "").startswith(prefix):
            return a
    return None


def action_enabled(object_name):
    a = _window_action(object_name=object_name)
    return a is not None and a.isEnabled()


def trigger_action(object_name):
    a = _window_action(object_name=object_name)
    if a is None:
        return False
    a.trigger()
    return True


def undo_text():
    """The Undo action's text while it is enabled, or ""."""
    a = _window_action(prefix="Undo")
    return a.text() if a is not None and a.isEnabled() else ""


def redo_text():
    a = _window_action(prefix="Redo")
    return a.text() if a is not None and a.isEnabled() else ""


def trigger_undo(redo=False):
    a = _window_action(prefix="Redo" if redo else "Undo")
    if a is None or not a.isEnabled():
        return False
    a.trigger()
    QApplication.processEvents()
    return True


def status_text():
    win = main_gui()
    label = win.findChild(QLabel, "liveSessionStatus") if win is not None else None
    if label is None or not label.isVisible():
        return ""
    return label.text()


def status_message():
    win = main_gui()
    return win.statusBar().currentMessage() if win is not None else ""


def write_file(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def read_file(path):
    with open(path) as f:
        return f.read()


def fill_dialog(object_name, fields, seen):
    """Fills the modal dialog with that object name once it is up and clicks its OK button."""
    def attempt(tries):
        dialog = QApplication.activeModalWidget()
        if dialog is None or dialog.objectName() != object_name:
            if tries > 0:
                QTimer.singleShot(200, lambda: attempt(tries - 1))
            return
        _keep.append(dialog)
        seen["dialog"] = True
        for name, value in fields.items():
            w = dialog.findChild(QWidget, name)
            if w is None:
                seen.setdefault("missing", []).append(name)
                continue
            _keep.append(w)
            if isinstance(w, QComboBox):
                if w.isEditable():
                    w.setEditText(value)
                else:
                    w.setCurrentIndex(w.findData(value))
            elif isinstance(w, QSpinBox):
                w.setValue(int(value))
            elif isinstance(w, QLineEdit):
                w.setText(value)
        box = dialog.findChild(QDialogButtonBox)
        ok = box.button(QDialogButtonBox.Ok) if box is not None else None
        if ok is not None and ok.isEnabled():
            ok.click()
        else:
            dialog.reject()

    QTimer.singleShot(200, lambda: attempt(50))


def panel_of(name):
    node = app().getNode(name)
    if node is None:
        return None
    label = node.getLabel()
    for p in gt.widgets_of("NodeSettingsPanel"):
        try:
            if not p.isVisible():
                continue
            for le in p.findChildren(QLineEdit):
                if gt.class_name(le) == "LineEdit" and le.text() == label:
                    return p
        except RuntimeError:
            continue
    return None


def open_panel(name):
    for attempt in range(2):
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        node = app().getNode(name)
        if node is None:
            return False
        app().clearSelection()
        activate()
        app().selectNode(node, True)
        QApplication.processEvents()
        view = graph_view()
        if view is not None:
            QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(name) is not None, 10.0)
        if ok:
            return True
        report("NOTE the settings panel of %s did not open (attempt %d)" % (name, attempt + 1))
    return False


def drag(widget, start, end, steps=6):
    def mouse(kind, p, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(kind, QPointF(p), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    mouse(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    for i in range(1, steps + 1):
        p = QPointF(start.x() + (end.x() - start.x()) * i / steps, start.y() + (end.y() - start.y()) * i / steps)
        mouse(QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def wheel(widget, notches=1):
    centre = QPointF(widget.rect().center())
    event = QWheelEvent(centre, QPointF(widget.mapToGlobal(widget.rect().center())), QPoint(0, 0),
                        QPoint(0, 120 * notches), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    QApplication.sendEvent(widget, event)
    QApplication.processEvents()


def node_point(graph, name):
    node = app().getNode(name)
    x, y = node.getPosition()
    w, h = node.getSize()
    graph.resetTransform()
    graph.centerOn(x + w / 2.0, y + h / 2.0)
    QApplication.processEvents()
    return graph.mapFromScene(QPointF(x + w / 2.0, y + h / 2.0))


def drag_mix_slider(name):
    """Drags the Mix slider in the open panel of the Grade name. "ok" or what went wrong."""
    ok = yield from open_panel(name)
    if not ok:
        return "no panel for " + name
    found = {"field": None, "panel": None}

    def field():
        p = panel_of(name)
        if p is None:
            return None
        for w in p.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and "Mix factor" in w.toolTip():
                    # A child wrapper dies with its parent's wrapper, so without this reference the
                    # field found on one pass is never the same object on the next.
                    found["panel"] = p
                    return w
            except RuntimeError:
                continue
        return None

    def settled():
        now = field()
        previous = found["field"]
        found["field"] = now
        return now is not None and previous is not None and isValid(previous) and previous is now

    ok = yield ("until", settled, 30.0)
    if not ok:
        p = panel_of(name)
        if p is None:
            return "no mix field: the panel of %s is gone" % name
        firsts = [w for w in p.findChildren(QWidget) if w.objectName() == "dim-0"]
        matching = [w for w in firsts if "Mix factor" in w.toolTip()]
        visible = len([w for w in matching if w.isVisible()])
        if visible:
            return ("the mix field in the panel of %s never stayed the same widget over two passes "
                    "(%d first fields, %d matching, %d of those visible)" % (name, len(firsts), len(matching), visible))
        return ("no mix field in the panel of %s (%d first fields, %d matching, %d of those visible)"
                % (name, len(firsts), len(matching), visible))
    slider = None
    a = found["field"].parentWidget()
    for _ in range(4):
        if a is None:
            break
        sliders = [w for w in a.findChildren(QWidget) if gt.class_name(w) == "ScaleSliderQWidget" and w.isVisible()]
        if sliders:
            slider = sliders[0]
            break
        a = a.parentWidget()
    if slider is None:
        return "no slider beside the mix field"
    _keep.append(slider)
    before = app().getNode(name).getParam("mix").get()
    y = slider.height() / 2.0
    drag(slider, QPointF(slider.width() * 0.8, y), QPointF(slider.width() * 0.3, y))
    yield ("sleep", SETTLE_MS)
    after = app().getNode(name).getParam("mix").get()
    gt.close_all_panels()
    if abs(after - before) < 1e-6:
        return "the mix field and slider were found, but the drag left mix at %g" % after
    return "ok"


def move_node(name, dx, dy):
    gt.close_all_panels()
    activate()
    graph = graph_view()
    if graph is None:
        return "no node graph"
    before = app().getNode(name).getPosition()
    start = node_point(graph, name)
    drag(graph.viewport(), QPointF(start), QPointF(start.x() + dx, start.y() + dy))
    yield ("sleep", SETTLE_MS)
    after = app().getNode(name).getPosition()
    if abs(after[0] - before[0]) < 1.0 and abs(after[1] - before[1]) < 1.0:
        return "the drag did not move %s (%r)" % (name, after)
    return "ok"


def delete_node(name):
    gt.close_all_panels()
    activate()
    node = app().getNode(name)
    if node is None:
        return "no node " + name
    app().clearSelection()
    app().selectNode(node, True)
    graph = graph_view()
    graph.setFocus()
    send_key(graph, Qt.Key_Backspace)
    ok = yield ("until", lambda: app().getNode(name) is None, 10.0)
    return "ok" if ok else "Backspace left " + name


# ---- client --------------------------------------------------------------------------------

def client_join(port, display_name):
    if not action_enabled("actionJoinLiveSession"):
        return "no enabled Join Live Session action"
    seen = {}
    fill_dialog("joinLiveSessionDialog", {"host": "127.0.0.1", "port": port, "displayName": display_name}, seen)
    trigger_action("actionJoinLiveSession")
    if not seen.get("dialog"):
        return "the join dialog did not show"
    ok = yield ("until", lambda: status_text().startswith("Live: joined"), 30.0)
    return "ok" if ok else "the status bar never said joined (%r)" % status_text()


def client_steps(command):
    """Carries out the host's commands until quit. command(line) is a generator giving the answer."""
    sync = _state["sync"]
    ok = yield ("until", lambda: main_gui() is not None, STARTUP_TIMEOUT_S)
    require(ok, "the client's main window is up")
    write_file(os.path.join(sync, "ready"), "ready")
    n = 0
    while True:
        n += 1
        path = os.path.join(sync, "cmd-%03d" % n)
        ok = yield ("until", lambda: os.path.exists(path), COMMAND_TIMEOUT_S * 3)
        require(ok, "command %d arrives from the host" % n)
        line = read_file(path).strip()
        words = line.split()
        try:
            if words[0] == "quit":
                result = "bye"
            elif words[0] == "join":
                result = yield from client_join(words[1], words[2])
            elif words[0] == "has":
                result = "yes" if app().getNode(words[1]) is not None else "no"
            elif words[0] == "get":
                result = repr(app().getNode(words[1]).getParam(words[2]).get())
            else:
                result = yield from command(words)
        except Exception:
            result = "error: " + traceback.format_exc().replace("\n", " | ")
        check(not result.startswith("error") and not result.startswith("unknown"), "client: %s -> %s" % (line, result))
        write_file(os.path.join(sync, "ack-%03d" % n), result)
        if line == "quit":
            return


# ---- host ----------------------------------------------------------------------------------

def client_do(line, timeout=COMMAND_TIMEOUT_S):
    _state["n"] += 1
    n = _state["n"]
    sync = _state["sync"]
    ack = os.path.join(sync, "ack-%03d" % n)
    write_file(os.path.join(sync, "cmd-%03d" % n), line)
    proc = _state["process"]
    ok = yield ("until", lambda: os.path.exists(ack) or proc.poll() is not None, timeout)
    if os.path.exists(ack):
        return read_file(ack)
    return "client exited" if proc.poll() is not None else "timeout"


def host_and_join(role_variable, host_name, client_name):
    """Hosts the saved project, starts the client and has it join. Returns the port."""
    binary = os.environ.get("NATRON_GUI_TEST_BINARY", "")
    require(os.access(binary, os.X_OK), "the harness names the Natron binary (NATRON_GUI_TEST_BINARY)", binary)
    script = os.environ.get("NATRON_GUI_TEST_SCRIPT", "")
    require(os.path.isfile(script), "the harness names this script (NATRON_GUI_TEST_SCRIPT)", script)
    seen = {}
    fill_dialog("hostLiveSessionDialog", {"bindAddress": "127.0.0.1", "port": "0", "displayName": host_name}, seen)
    trigger_action("actionHostLiveSession")
    require(seen.get("dialog"), "Host Live Session shows its dialog")
    hosted = re.compile(r"Live: hosting on 127\.0\.0\.1:(\d+)")
    ok = yield ("until", lambda: hosted.match(status_text()) is not None, 10.0)
    require(ok, "the status bar says the project is hosted on a port", status_text())
    port = int(hosted.match(status_text()).group(1))

    sync = _state["sync"]
    client_out = os.path.join(gt.OUT, "client")
    if not os.path.isdir(client_out):
        os.makedirs(client_out)
    for leftover in ("results.txt", "natron.log"):
        if os.path.exists(os.path.join(client_out, leftover)):
            os.remove(os.path.join(client_out, leftover))
    for leftover in os.listdir(sync):
        os.remove(os.path.join(sync, leftover))
    env = dict(os.environ)
    env[role_variable] = "client"
    env[role_variable + "_SYNC"] = sync
    env["NATRON_GUI_TEST_OUT"] = client_out
    log = open(os.path.join(client_out, "natron.log"), "w")
    _state["process"] = subprocess.Popen([binary, script], env=env, stdout=log,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    proc = _state["process"]
    ok = yield ("until", lambda: os.path.exists(os.path.join(sync, "ready")) or proc.poll() is not None,
                STARTUP_TIMEOUT_S)
    require(ok and proc.poll() is None, "the client Natron starts")
    answer = yield from client_do("join %d %s" % (port, client_name))
    require(answer == "ok", "the client joins through Join Live Session", answer)
    ok = yield ("until", lambda: "1 user" in status_text(), 10.0)
    check(ok, "the host's status bar counts the client: %r" % status_text())
    return port


def until_client(line, expected, timeout=10.0):
    """Asks the client line until it answers expected. The last answer."""
    deadline = time.time() + timeout
    while True:
        answer = yield from client_do(line)
        if answer == expected or time.time() > deadline:
            return answer
        yield ("sleep", 250)


def host_cleanup():
    proc = _state["process"]
    if proc is None:
        return
    try:
        proc.wait(30)
    except Exception:
        proc.kill()
        proc.wait(10)
    results = os.path.join(gt.OUT, "client", "results.txt")
    lines = read_file(results).splitlines() if os.path.exists(results) else []
    for line in lines:
        if line.startswith("FAIL "):
            check(False, "client " + line[5:])
    summary = [line for line in lines if line.startswith("SUMMARY ")]
    check(bool(summary) and summary[-1] == "SUMMARY 0 failure(s)",
          "the client process passed its own checks (%s)" % (summary[-1] if summary else "no results.txt"))
    check(proc.returncode == 0, "the client process exits 0 (%s)" % proc.returncode)


def run(role, host_steps, client_command):
    if role == "client":
        gt.run(client_steps(client_command))
        return
    finish = gt.finish

    def finish_with_client():
        gt.finish = finish
        try:
            host_cleanup()
        except Exception:
            check(False, "client cleanup error: " + traceback.format_exc())
        finish()

    gt.finish = finish_with_client
    gt.run(host_steps())
