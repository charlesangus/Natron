"""Helpers shared by the Tests/gui/m373_*.py scripts, which drive the in-panel knob editor of node
settings panels. Every helper looks its widgets up again, so no PySide wrapper of a panel's child
is held across processEvents (the editor rebuilds the panel's knob widgets after every edit)."""
from PySide6.QtCore import QByteArray, QEvent, QMetaObject, QMimeData, QObject, QPoint, QPointF, Qt, Q_ARG, Q_RETURN_ARG
from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent, QMouseEvent
from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QMainWindow, QWidget

import NatronEngine
import shiboken6

import guitest as gt
from guitest import check

EDITOR_MIME = "application/x-natron-knob-editor"
KNOB_LINK_MIME = "KnobLink"


_app = []


def natron_app():
    return _app[0] if _app else NatronEngine.natron.getInstance(0)


def use_app(app):
    """Later lookups go to app, e.g. the instance a loaded project opened in."""
    del _app[:]
    _app.append(app)


def start(steps, watchdog_s=200):
    """Runs the steps generator under a watchdog; a setup error ends the run as a failure."""
    import faulthandler
    import os
    import traceback
    try:
        watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
        faulthandler.dump_traceback_later(watchdog_s, exit=True, file=watchdog_file)
        gt.run(steps)
    except Exception:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()


def wait_for_graph():
    """The first steps of every script: the node graph is up and no panel is open."""
    ok = yield ("until", lambda: root_graph() is not None, 60.0)
    gt.require(ok, "node graph widget is up")
    gt.close_all_panels()
    yield ("sleep", 400)


def open_blur(x=0, y=0, editing=True, plugin_id="Blur"):
    """Creates a node (a Blur by default) with its panel open, optionally in edit mode; returns its
    script name."""
    name = create(plugin_id, x, y)
    ok = yield ("until", lambda: panel_of(name) is not None, 10.0)
    gt.require(ok, "the settings panel of %s is open" % name)
    if editing:
        gt.require(set_editing(name, True), "the pencil of %s is enabled" % name)
        ok = yield ("until", lambda: is_editing(name), 5.0)
        gt.require(ok, "%s's panel is in edit mode" % name)
    return name


def create(plugin_id, x, y):
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    node = natron_app().createNode(plugin_id, -1, None, props)
    gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
    node.setPosition(x, y)
    return node.getScriptName()


def root_graph():
    graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
    return graphs[0] if graphs else None


def main_window():
    for w in QApplication.topLevelWidgets():
        if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
            return w
    return None


def edit_menu_action(prefix):
    win = main_window()
    if win is None or win.menuBar() is None:
        return None
    for act in win.menuBar().actions():
        menu = gt.menu_of(act)
        if menu is not None and act.text().lower().startswith("edit"):
            for a in menu.actions():
                if a.text().lower().startswith(prefix):
                    return a
    return None


def undo():
    a = edit_menu_action("undo")
    if not check(a is not None and a.isEnabled(), "an enabled Undo action is in the Edit menu"):
        return False
    a.trigger()
    QApplication.processEvents()
    return True


def panel_of(name):
    """The settings panel of the node with that script name, found by the label in its header."""
    node = natron_app().getNode(name)
    if node is None:
        return None
    label = node.getLabel()
    for p in gt.widgets_of("NodeSettingsPanel"):
        for le in p.findChildren(QLineEdit):
            if gt.class_name(le) == "LineEdit" and le.text() == label:
                return p
    return None


def mode_of(name):
    p = panel_of(name)
    return p.findChild(QObject, "KnobEditMode") if p is not None else None


def is_editing(name):
    m = mode_of(name)
    return m is not None and bool(m.property("active"))


def selected(name):
    m = mode_of(name)
    return m.property("selectedKnob") if m is not None else None


def pencil(name):
    p = panel_of(name)
    return p.findChild(QAbstractButton, "DockablePanelEditButton") if p is not None else None


def set_editing(name, on):
    b = pencil(name)
    if b is None or not b.isEnabled():
        return False
    if is_editing(name) != on:
        b.click()
        QApplication.processEvents()
    return True


def toggle_from_menu(name):
    """What the panel's right-click "Edit parameters" entry runs."""
    p = panel_of(name)
    if p is None:
        return False
    QMetaObject.invokeMethod(p, "onEditParametersActionTriggered")
    QApplication.processEvents()
    return True


def select(name, knob):
    m = mode_of(name)
    if m is None:
        return False
    return bool(QMetaObject.invokeMethod(m, "selectKnob", Q_RETURN_ARG(bool), Q_ARG(str, knob)))


def child_names(name, parent=""):
    m = mode_of(name)
    if m is None:
        return []
    return list(QMetaObject.invokeMethod(m, "getChildNames", Q_RETURN_ARG("QStringList"), Q_ARG(str, parent)))


def invoke(name, slot, *args):
    m = mode_of(name)
    if m is None:
        return False
    QMetaObject.invokeMethod(m, slot, *[Q_ARG(str, a) for a in args])
    QApplication.processEvents()
    return True


def add(name, entry_id):
    return invoke(name, "addKnobOfEntry", entry_id)


def palette_button(name, entry_id):
    p = panel_of(name)
    return p.findChild(QAbstractButton, "KnobEditPalette_" + entry_id) if p is not None else None


def visible_child(name, cls, object_name):
    """The visible child of that name: a rebuilt property form leaves its predecessor hidden until it
    is deleted."""
    p = panel_of(name)
    if p is None:
        return None
    for w in p.findChildren(cls, object_name):
        if w.isVisible():
            return w
    return None


def properties(name):
    return visible_child(name, QWidget, "KnobEditProperties")


def property_error(name):
    w = properties(name)
    return w.property("error") if w is not None else None


def property_title(name):
    p = panel_of(name)
    b = p.findChild(QAbstractButton, "KnobEditPropertiesHeader") if p is not None else None
    return b.text() if b is not None else ""


def commit_line(name, object_name, text):
    """Types text into a property line edit and finishes the edit, which commits it."""
    w = visible_child(name, QLineEdit, object_name)
    if w is None:
        return False
    w.setText(text)
    w.editingFinished.emit()
    QApplication.processEvents()
    return True


def param(name, knob):
    node = natron_app().getNode(name)
    return node.getParam(knob) if node is not None else None


def overlay(name):
    return visible_child(name, QWidget, "KnobEditOverlay")


def editor_payload(name, kind, value):
    m = mode_of(name)
    return ("%d|%s|%s" % (shiboken6.getCppPointer(m)[0], kind, value)).encode("utf-8")


def mime_of(fmt, payload):
    mime = QMimeData()
    mime.setData(fmt, QByteArray(payload))
    return mime


def send_drop(widget, pos, mime, modifiers=Qt.NoModifier):
    """Sends a drag enter, a move and a drop at pos (widget coordinates); returns whether the drop
    was accepted."""
    p = QPointF(pos)
    actions = Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
    enter = QDragEnterEvent(p.toPoint(), actions, mime, Qt.LeftButton, modifiers)
    QApplication.sendEvent(widget, enter)
    move = QDragMoveEvent(p.toPoint(), actions, mime, Qt.LeftButton, modifiers)
    QApplication.sendEvent(widget, move)
    drop = QDropEvent(p, actions, mime, Qt.LeftButton, modifiers)
    QApplication.sendEvent(widget, drop)
    accepted = drop.isAccepted()
    QApplication.processEvents()
    return accepted


def plain_text(text):
    out, inside = [], False
    for c in text:
        if c == "<":
            inside = True
        elif c == ">":
            inside = False
        elif not inside:
            out.append(c)
    return "".join(out).replace("&nbsp;", " ").strip()


def label_widget(name, label):
    """The visible widget of the panel that shows label as its text, e.g. a knob's name label."""
    p = panel_of(name)
    if p is None:
        return None
    for w in p.findChildren(QLabel) + p.findChildren(QAbstractButton):
        try:
            if w.isVisible() and plain_text(w.text()) == label:
                return w
        except RuntimeError:
            continue
    return None


def point_in(widget, target, fy=0.5):
    """A point of widget, in target's coordinates, at fraction fy of its height."""
    g = widget.mapToGlobal(QPoint(widget.width() // 2, int(widget.height() * fy)))
    return target.mapFromGlobal(g)


def start_label_drag(name, label, modifiers, during):
    """Presses on the knob label of that panel and moves past the drag distance, which starts the
    knob's drag; during() runs from a timer inside the drag's event loop, while the drag is live,
    and must end it (QDrag.cancel())."""
    from PySide6.QtCore import QTimer
    w = label_widget(name, label)
    if w is None:
        return False
    start = QPointF(w.width() / 2.0, w.height() / 2.0)
    end = start + QPointF(QApplication.startDragDistance() * 4, 0)
    QTimer.singleShot(150, during)
    QApplication.sendEvent(w, QMouseEvent(QEvent.MouseButtonPress, start, w.mapToGlobal(start), Qt.LeftButton,
                                          Qt.LeftButton, modifiers))
    QApplication.sendEvent(w, QMouseEvent(QEvent.MouseMove, end, w.mapToGlobal(end), Qt.NoButton, Qt.LeftButton,
                                          modifiers))
    QApplication.processEvents()
    return True
