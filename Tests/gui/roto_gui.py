"""Viewer and Roto helpers shared by the Roto/RotoPaint GUI checks.

Mouse and key events go straight to the ViewerGL with QApplication.sendEvent. Modifiers are read by
the roto interact from key events, so a modified gesture sends the Shift/Control/Alt key press
before its mouse events and the release after them (with_modifiers). Viewer coordinates are
calibrated from the info bar's mouse readout (ViewerMap).
"""
import math
import re
import time

import NatronEngine
from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QMainWindow

import guitest as gt
from guitest import check

T = 1.0
DRAG_STEPS = 10


def main_window():
    for w in QApplication.topLevelWidgets():
        if isinstance(w, QMainWindow) and w.isVisible():
            return w
    return None


def activate():
    win = main_window()
    if win is None:
        return
    win.activateWindow()
    win.raise_()
    deadline = time.time() + 10.0
    while not win.isActiveWindow() and time.time() < deadline:
        QApplication.processEvents()
        time.sleep(0.02)


def viewer_gl():
    visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
    return visible[0] if visible else None


def ancestor_of_class(widget, name):
    w = widget
    while w is not None:
        if gt.class_name(w) == name:
            return w
        w = w.parentWidget()
    return None


# ---- events ------------------------------------------------------------------------------------

def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
    QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
    QApplication.processEvents()


def press(widget, pos, modifiers=Qt.NoModifier):
    send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)


def release(widget, pos, modifiers=Qt.NoModifier):
    send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)


def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
    send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)


def click(widget, pos, modifiers=Qt.NoModifier):
    press(widget, pos, modifiers)
    release(widget, pos, modifiers)


def segment(start, end, steps=DRAG_STEPS):
    start = QPointF(start)
    end = QPointF(end)
    return [start + (end - start) * (i / float(steps)) for i in range(steps + 1)]


def drag_path(widget, points, modifiers=Qt.NoModifier):
    press(widget, points[0], modifiers)
    for p in points[1:]:
        move(widget, p, Qt.LeftButton, modifiers)
    release(widget, points[-1], modifiers)


def drag(widget, start, end, modifiers=Qt.NoModifier, steps=DRAG_STEPS):
    drag_path(widget, segment(start, end, steps), modifiers)


def send_key(widget, key, modifiers=Qt.NoModifier):
    QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
    QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
    QApplication.processEvents()


MOD_KEYS = ((Qt.ShiftModifier, Qt.Key_Shift), (Qt.ControlModifier, Qt.Key_Control), (Qt.AltModifier, Qt.Key_Alt))


def hold(widget, modifiers):
    held = Qt.NoModifier
    for mod, key in MOD_KEYS:
        if modifiers & mod:
            held = held | mod
            QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, held))
    QApplication.processEvents()


def unhold(widget, modifiers):
    held = modifiers
    for mod, key in reversed(MOD_KEYS):
        if modifiers & mod:
            held = held & ~mod
            QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, held))
    QApplication.processEvents()


def with_modifiers(widget, modifiers, gesture):
    hold(widget, modifiers)
    try:
        gesture(modifiers)
    finally:
        unhold(widget, modifiers)


def edit_actions():
    win = main_window()
    if win is None or win.menuBar() is None:
        return []
    for act in win.menuBar().actions():
        if act.menu() is not None and act.text().replace("&", "").lower().startswith("edit"):
            return act.menu().actions()
    return []


def trigger(prefix):
    """Triggers the Edit menu action whose text starts with prefix ("undo", "redo")."""
    for a in edit_actions():
        if a.text().replace("&", "").lower().startswith(prefix) and a.isEnabled():
            a.trigger()
            QApplication.processEvents()
            return True
    check(False, "found an enabled '%s' action in the Edit menu" % prefix)
    return False


# ---- viewer calibration ------------------------------------------------------------------------

def info_bar():
    bars = [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]
    return bars[0] if bars else None


def strip_tags(text):
    return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()


def info_mouse():
    bar = info_bar()
    if bar is None:
        return None
    for w in QApplication.allWidgets():
        try:
            if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                m = re.match(r"x=(-?\d+) y=(-?\d+)", strip_tags(w.text()))
                if m:
                    return (int(m.group(1)), int(m.group(2)))
        except RuntimeError:
            continue
    return None


class ViewerMap(object):
    """Widget pixels to canonical coordinates, calibrated from the info bar's whole-pixel mouse
    readout at two positions per axis where it steps, found by bisection."""

    def __init__(self, gl):
        self.gl = gl
        w, h = float(gl.width()), float(gl.height())
        self.ok = True
        (x1, cx1), (x2, cx2) = self._edge(0, w * 0.1, w * 0.5, h / 2.0), self._edge(0, w * 0.6, w * 0.9, h / 2.0)
        (y1, cy1), (y2, cy2) = self._edge(1, h * 0.1, h * 0.5, w / 2.0), self._edge(1, h * 0.6, h * 0.9, w / 2.0)
        if None in (x1, x2, y1, y2) or x1 == x2 or y1 == y2:
            self.ok = False
            return
        self.sx = (cx2 - cx1) / (x2 - x1)
        self.sy = (cy2 - cy1) / (y2 - y1)
        self.p0 = QPointF(x1, y1)
        self.c0 = (cx1, cy1)

    def _coord(self, axis, pos, other):
        p = QPointF(pos, other) if axis == 0 else QPointF(other, pos)
        move(self.gl, p)
        c = info_mouse()
        return None if c is None else c[axis]

    def _edge(self, axis, lo, hi, other):
        c_lo = self._coord(axis, lo, other)
        c_hi = self._coord(axis, hi, other)
        if c_lo is None or c_hi is None or c_lo == c_hi:
            return (None, None)
        for _ in range(30):
            if hi - lo < 0.05:
                break
            mid = (lo + hi) / 2.0
            if self._coord(axis, mid, other) == c_lo:
                lo = mid
            else:
                hi = mid
        stepped = self._coord(axis, hi, other)
        return (hi, max(c_lo, stepped))

    def to_canonical(self, p):
        return (self.c0[0] + (p.x() - self.p0.x()) * self.sx, self.c0[1] + (p.y() - self.p0.y()) * self.sy)

    def to_widget(self, x, y):
        return QPointF(self.p0.x() + (x - self.c0[0]) / self.sx, self.p0.y() + (y - self.c0[1]) / self.sy)

    def pixel_scale(self):
        return abs(self.sx)


class Grab(object):
    """The screen under widget, in widget coordinates: a widget grab of the GL viewer can come out
    blank under llvmpipe."""

    def __init__(self, widget):
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        self.img = QGuiApplication.primaryScreen().grabWindow(
            0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
        self.s = self.img.width() / float(max(1, widget.width()))

    def colour(self, x, y):
        xd, yd = int(round(x * self.s)), int(round(y * self.s))
        if 0 <= xd < self.img.width() and 0 <= yd < self.img.height():
            c = self.img.pixelColor(xd, yd)
            return (c.red(), c.green(), c.blue())
        return None


def untone_view():
    """The viewer defaults to a tone-mapped view; absolute pixel checks need the plain one."""
    for w in gt.widgets_of("ComboBox"):
        try:
            if not (w.isVisible() and "View:" in (w.toolTip() or "")):
                continue
            items = [str(i) for i in (w.property("natronItems") or [])]
        except RuntimeError:
            continue
        for name in ("Un-tone-mapped", "Raw"):
            if name in items:
                QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(name)))
                QApplication.processEvents()
                return True
    return False


# ---- shapes and tools --------------------------------------------------------------------------

def shapes(ctx):
    return [it for it in ctx.getBaseLayer().getChildren() if isinstance(it, NatronEngine.BezierCurve)]


def clear_shapes(ctx):
    layer = ctx.getBaseLayer()
    for item in list(layer.getChildren()):
        layer.removeItem(item)
    QApplication.processEvents()


def point(bez, i, t=T):
    return bez.getControlPointPosition(i, t)


def feather(bez, i, t=T):
    return bez.getFeatherPointPosition(i, t)


def left_off(p):
    return (p[2] - p[0], p[3] - p[1])


def right_off(p):
    return (p[4] - p[0], p[5] - p[1])


def length(v):
    return math.hypot(v[0], v[1])


def close(a, b, tol):
    return all(abs(a[i] - b[i]) <= tol for i in range(len(a)))


def select_tool(gl):
    send_key(gl, Qt.Key_Escape)
    send_key(gl, Qt.Key_Q)


def tool_button(group):
    """The visible viewer toolbar button of a tool group ("EditBezierToolButton", "CloneToolButton")."""
    for b in gt.widgets_of("ViewerToolButton"):
        if not b.isVisible() or b.defaultAction() is None:
            continue
        data = b.defaultAction().data()
        if data and len(data) == 2 and data[0] == group:
            return b
    return None


def bezier_tool_button():
    return tool_button("EditBezierToolButton")


def drawing_tool():
    """The viewer toolbar's shape-drawing button: (pressed, current tool) or None."""
    b = bezier_tool_button()
    if b is None:
        return None
    return (b.isDown(), b.defaultAction().data()[1])


def pick_tool(group, tool_id):
    """Picks tool_id from the group button's menu, as a click in that menu does."""
    b = tool_button(group)
    if b is None:
        return False
    for act in b.actions():
        data = act.data()
        if data and len(data) == 2 and data[1] == tool_id:
            act.trigger()
            QApplication.processEvents()
            return True
    return False


def pick_drawing_tool(tool_id):
    return pick_tool("EditBezierToolButton", tool_id)


def open_roto(app, plugin_id):
    """Creates a node of plugin_id with its panel open (which makes its interact the active one) and
    shows it in Viewer1. Returns (node, roto context)."""
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    node = app.createNode(plugin_id, -1, None, props)
    if node is None:
        return None, None
    viewer = app.getNode("Viewer1")
    if viewer is not None:
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        viewer.connectInput(0, node)
    QApplication.processEvents()
    return node, node.getRotoContext()
