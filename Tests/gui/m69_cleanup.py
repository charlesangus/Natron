"""Interaction checks and screenshots for the minor-cleanup changes: no crash when a node dies with
its panel's undo stack active, easier-to-grab input stubs, pick-up connection mode and the
disconnect on a click away from a node, pipe pick-up that tolerates jitter under the drag distance
and carries the end of the pipe nearest the click, output stubs picked up to start a connection,
Y/Shift+Y and U/Shift+U connection shortcuts, paste-onto-a-node and paste when the graph holds
only the click focus, the pane menu from anywhere in the tab header, scene-referred viewer picking
with input/output modifiers, the multi-point roto feather drag and broken tangent, curve-editor
tangent break, join and handle length, a folded knob's single curve and its edits, and an expanded knob staying expanded until
it is folded by hand, with the fold state kept through a save and reload.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m69-shots/out -e GUI_TEST_TIMEOUT=300 \
        -w /home/bosley/git/Natron natron-dev \
        Tests/gui/run-gui-test.sh Tests/gui/m69_cleanup.py build/fast/App/Natron

Conventions shared with Tests/gui/m55_node_graph.py:
  - nodes are created with CreateNodeArgsPropSettingsOpened=True, as the toolbar does;
  - connections are read through the Effect API and on-screen state through screen grabs, never
    through NodeGraph.scene(), which raises in this binary. Node-graph items are never touched from
    Python at all: PySide deletes an item whose parentItem() it is asked for;
  - every interaction is a synthetic QMouseEvent/QKeyEvent/QWheelEvent sent to the widget's
    viewport, at positions mapped from scene coordinates;
  - Undo is the Edit menu's action, triggered directly, because the Ctrl+Z shortcut is matched by
    the shortcut map before a sent key event reaches a widget.

Positions that only the C++ side knows are derived instead of read:
  - an unconnected input stub runs from its node's centre at the angle NodeGui::initializeInputs
    gives it (inputs spread over pi/(n+1) steps, the first mask at 0) to 30 scene units past the
    node's box (the default "Disconnected arrow length");
  - the viewer's widget-to-canonical mapping is calibrated from the info bar's mouse coordinates,
    bisecting hover moves for the positions where the whole-pixel readout steps;
  - the curve widget's value axis follows CurveWidget::frameAll(), which fills the widget's height
    with the keys' range grown by a tenth each way; its time axis is read from where the orange
    current-time line is drawn at two frames;
  - a middle-button pan in the graph moves its root item, not the view, so node positions are
    offset by the total pan from then on.

A popup menu runs a nested event loop, so wherever one may open, a timer that records and closes
any popup is armed first.
"""
import math
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 870
SETTLE_MS = 400
RENDER_MS = 1500
DRAG_STEPS = 4
DISCONNECTED_ARROW_LENGTH = 30.0
TOL = 1e-3

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMetaObject, QPoint, QPointF, Q_ARG, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QEnterEvent, QGuiApplication, QKeyEvent, QMouseEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import (QApplication, QLabel, QLineEdit, QListView, QMainWindow, QMenu,
                                   QPushButton, QTextEdit, QTreeWidget, QWidget)

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- nodes ----------------------------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def same(a, b):
        return a is not None and b is not None and a.getScriptName() == b.getScriptName()

    def input_of(node, input_nb):
        return node.getInput(input_nb)

    def input_connected_from(node, source):
        for i in range(node.getMaxInputCount()):
            if same(node.getInput(i), source):
                return i
        return None

    def input_index(node, label):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == label:
                return i
        return None

    def set_all(param, value):
        for d in range(param.getNumDimensions()):
            param.setValue(value, d)

    def set_rgba(param, rgba):
        for d in range(min(4, param.getNumDimensions())):
            param.setValue(rgba[d], d)

    def child_names():
        return set(n.getScriptName() for n in app.getChildren())

    def new_children(before):
        return [n for n in app.getChildren() if n.getScriptName() not in before]

    def destroy_nodes(nodes):
        for n in nodes:
            n.destroy()
        QApplication.processEvents()

    _viewer = {}

    def viewer_node():
        return _viewer.get("node")

    # ---- windows, graph, events -----------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    # A middle-button pan moves the graph's root item rather than the view, so node positions
    # (relative to the root) are offset from scene coordinates by the total pan so far.
    _root_offset = [0.0, 0.0]

    def box(effect):
        x, y = effect.getPosition()
        w, h = effect.getSize()
        return x + _root_offset[0], y + _root_offset[1], w, h

    def centre(effect):
        x, y, w, h = box(effect)
        return (x + w / 2.0, y + h / 2.0)

    def view_at(graph, scene_centre, zoom):
        # The scene rectangle can still grow from an earlier arrow drag once events run, which
        # scrolls the view, so the centring is repeated after they have.
        graph.resetTransform()
        graph.scale(zoom, zoom)
        for _ in range(3):
            QApplication.processEvents()
            graph.centerOn(scene_centre[0], scene_centre[1])
        QApplication.processEvents()

    def frame(graph, nodes, margin=80.0, max_zoom=1.5):
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y, w, h = box(n)
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        vp = graph.viewport()
        zoom = min(max_zoom, vp.width() / (max(xe) - min(xs) + 2 * margin),
                   vp.height() / (max(ye) - min(ys) + 2 * margin))
        view_at(graph, ((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0), zoom)

    def zoom_of(graph):
        return graph.transform().m11()

    def to_vp(graph, scene_pt):
        return QPointF(graph.mapFromScene(QPointF(scene_pt[0], scene_pt[1])))

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def press(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, button, button, modifiers)

    def release(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, button, Qt.NoButton, modifiers)

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        press(widget, pos, button, modifiers)
        release(widget, pos, button, modifiers)

    def drag(widget, start, end, button=Qt.LeftButton, modifiers=Qt.NoModifier, steps=DRAG_STEPS):
        start = QPointF(start)
        end = QPointF(end)
        press(widget, start, button, modifiers)
        for i in range(1, steps + 1):
            f = i / float(steps)
            move(widget, start + (end - start) * f, button, modifiers)
        release(widget, end, button, modifiers)

    def wheel(widget, pos, delta_y):
        pos = QPointF(pos)
        glob = QPointF(widget.mapToGlobal(pos.toPoint()))
        ev = QWheelEvent(pos, glob, QPoint(0, 0), QPoint(0, delta_y), Qt.NoButton, Qt.NoModifier,
                         Qt.NoScrollPhase, False)
        QApplication.sendEvent(widget, ev)
        QApplication.processEvents()

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        try:
            QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        except RuntimeError:
            # A value box can be rebuilt by the edit its Return key commits.
            pass
        QApplication.processEvents()

    def graph_key(key, modifiers=Qt.NoModifier):
        send_key(root_graph(), key, modifiers)

    def graph_image(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        return pix, pix.toImage()

    def widget_shot(widget, name):
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to crop, skipping the shot" % name)
            return
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def graph_shot(name):
        g = root_graph()
        widget_shot(g.viewport() if g is not None else None, name)

    def widget_image(widget):
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()

    def time_cursor_x(cw):
        """The column of the curve widget's orange current-time line."""
        img = widget_image(cw)
        best, best_x = 0, None
        for x in range(img.width()):
            n = 0
            for y in range(0, img.height(), 4):
                c = img.pixelColor(x, y)
                if c.red() > 200 and 80 < c.green() < 190 and c.blue() < 90:
                    n += 1
            if n > best:
                best, best_x = n, x
        return best_x

    def differing_pixels(a, b, bounds, tolerance=60):
        """(x, y, colour of a) of every pixel inside bounds = (x0, y0, x1, y1) where the two
        same-sized images differ."""
        out = []
        x0, y0, x1, y1 = bounds
        for y in range(max(0, int(y0)), min(a.height(), b.height(), int(y1) + 1)):
            for x in range(max(0, int(x0)), min(a.width(), b.width(), int(x1) + 1)):
                if a.pixel(x, y) == b.pixel(x, y):
                    continue
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > tolerance:
                    out.append((x, y, ca))
        return out

    def drawn_near(graph, vp_pos, radius=5, tolerance=40):
        """How many pixels within radius of vp_pos differ from the viewport's corner colour."""
        _pix, img = graph_image(graph)
        bg = img.pixelColor(2, 2)
        hits = 0
        cx, cy = int(round(vp_pos.x())), int(round(vp_pos.y()))
        for y in range(cy - radius, cy + radius + 1):
            for x in range(cx - radius, cx + radius + 1):
                if 0 <= x < img.width() and 0 <= y < img.height():
                    c = img.pixelColor(x, y)
                    if abs(c.red() - bg.red()) + abs(c.green() - bg.green()) + abs(c.blue() - bg.blue()) > tolerance:
                        hits += 1
        return hits

    # ---- Edit menu and popups --------------------------------------------------------------

    def edit_actions():
        win = main_window()
        if win is None or win.menuBar() is None:
            return []
        for act in win.menuBar().actions():
            if act.menu() is not None and act.text().replace("&", "").lower().startswith("edit"):
                return act.menu().actions()
        return []

    def undo_action():
        for a in edit_actions():
            if a.text().replace("&", "").lower().startswith("undo"):
                return a
        return None

    def undo_text():
        a = undo_action()
        return (a.text(), a.isEnabled()) if a is not None else (None, False)

    def trigger(prefix):
        for a in edit_actions():
            if a.text().replace("&", "").lower().startswith(prefix) and a.isEnabled():
                a.trigger()
                QApplication.processEvents()
                return True
        check(False, "found an enabled '%s' action in the Edit menu" % prefix)
        return False

    class PopupGuard(object):
        """Polls for popup menus while armed; each one seen is recorded and closed."""

        def __init__(self):
            self.seen = []
            self.armed = False

        def arm(self):
            self.armed = True
            self.seen = []
            QTimer.singleShot(50, self._poll)

        def disarm(self):
            self.armed = False

        def _poll(self):
            if not self.armed:
                return
            popup = QApplication.activePopupWidget()
            if isinstance(popup, QMenu) and popup.isVisible():
                self.seen.append({
                    "pos": popup.mapToGlobal(QPoint(0, 0)),
                    "height": popup.height(),
                    "texts": [a.text() for a in popup.actions() if not a.isSeparator()],
                })
                popup.close()
            QTimer.singleShot(50, self._poll)

    popups = PopupGuard()

    # ---- unconnected input stub geometry ---------------------------------------------------

    def stub_direction(node, input_nb):
        """The stub's direction in scene coordinates (y down), from NodeGui::initializeInputs,
        for the main inputs and the first mask."""
        labels = [node.getInputLabel(i) for i in range(node.getMaxInputCount())]
        # Merge's extra A2... inputs are not spread with the others.
        non_masks = [i for i, l in enumerate(labels) if l.lower() != "mask" and not re.match(r"^A\d+$", l)]
        step = math.pi / (len(non_masks) + 1)
        if input_nb in non_masks:
            angle = step * (non_masks.index(input_nb) + 1)
        else:
            masks = [i for i, l in enumerate(labels) if l.lower() == "mask"]
            angle = 0.0 if masks.index(input_nb) == 0 else math.pi
        return (math.cos(angle), -math.sin(angle))

    def stub_tip(node, input_nb):
        x, y, w, h = box(node)
        c = (x + w / 2.0, y + h / 2.0)
        d = stub_direction(node, input_nb)
        to_edge = min((w / 2.0) / abs(d[0]) if abs(d[0]) > 1e-9 else 1e18,
                      (h / 2.0) / abs(d[1]) if abs(d[1]) > 1e-9 else 1e18)
        length = to_edge + DISCONNECTED_ARROW_LENGTH
        return (c[0] + d[0] * length, c[1] + d[1] * length), d, to_edge

    def on_stub(node, input_nb):
        """A scene point on the stub, half way along the part outside the node."""
        tip, d, to_edge = stub_tip(node, input_nb)
        back = DISCONNECTED_ARROW_LENGTH / 2.0
        return (tip[0] - d[0] * back, tip[1] - d[1] * back)

    # ---- viewer ---------------------------------------------------------------------------

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def viewer_tab():
        tabs = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        return tabs[0] if tabs else None

    def info_bar():
        bars = [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]
        return bars[0] if bars else None

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_labels():
        """(text, visible) of every label in the first visible info bar. Wrappers are looked up
        fresh and read at once: a stale one raises."""
        bar = info_bar()
        if bar is None:
            return []
        out = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                    out.append((w.text(), w.isVisible()))
            except RuntimeError:
                continue
        return out

    def info_rgba():
        for text, visible in info_labels():
            if "d93232" in text and visible:
                nums = re.findall(r"-?\d+\.\d+|-?\d+", strip_tags(text))
                try:
                    return [float(n) for n in nums]
                except ValueError:
                    return None
        return None

    def info_source():
        for text, visible in info_labels():
            t = strip_tags(text)
            if t in ("in", "out"):
                return t if visible else None
        return None

    def info_mouse():
        for text, _visible in info_labels():
            m = re.match(r"x=(-?\d+) y=(-?\d+)", strip_tags(text))
            if m:
                return (int(m.group(1)), int(m.group(2)))
        return None

    class ViewerMap(object):
        """Widget pixels to canonical coordinates. The info bar shows whole pixels only, so each
        axis is calibrated from two positions where the readout steps to the next pixel, found by
        bisection between hover moves."""

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
            """A widget position in [lo, hi] where the readout steps, and the value it steps to.
            A pixel's value holds from its own edge on, so the step lands on that pixel's edge."""
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
            # The readout steps to c_lo + 1 going right, or to c_lo - 1 going down (y is up); the
            # boundary between the two pixels is the larger value.
            return (hi, max(c_lo, stepped))

        def to_canonical(self, p):
            return (self.c0[0] + (p.x() - self.p0.x()) * self.sx, self.c0[1] + (p.y() - self.p0.y()) * self.sy)

        def to_widget(self, x, y):
            return QPointF(self.p0.x() + (x - self.c0[0]) / self.sx, self.p0.y() + (y - self.c0[1]) / self.sy)

        def pixel_scale(self):
            return abs(self.sx)

    def view(node):
        v = viewer_node()
        if v.getInput(0) is not None:
            v.disconnectInput(0)
        check(v.connectInput(0, node), "the Viewer now shows %s" % node.getLabel())
        QApplication.processEvents()

    def close(a, b):
        return a is not None and b is not None and abs(a - b) <= TOL

    def rgb_is(values, want):
        return values is not None and len(values) >= 3 and all(close(values[i], want[i]) for i in range(3))

    # ---- panes ----------------------------------------------------------------------------

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def make_tab_current(widget):
        """Brings widget's tab to the front of its pane, the way clicking the tab does."""
        pane = ancestor_of_class(widget, "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        bar = bars[0]
        target = None
        for i in range(bar.count()):
            text = bar.tabText(i).lower()
            if gt.class_name(widget) == "CurveEditor" and "curve" in text:
                target = i
            if gt.class_name(widget) == "NodeGraph" and "graph" in text:
                target = i
        if target is None:
            return False
        QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, target))
        QApplication.processEvents()
        return True

    def panel_spinboxes():
        found = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "SpinBox" and w.isVisible() and ancestor_of_class(w, "NodeSettingsPanel") is not None:
                    w.text()
                    found.append(w)
            except RuntimeError:
                continue
        return found

    # ---- steps ----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active",
                   "QGraphicsItem::isActive() is false in an inactive window, so presses miss every node")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        gt.require(viewer is not None, "a Viewer node exists")
        _viewer["node"] = viewer
        yield ("sleep", SETTLE_MS)
        viewer.setPosition(0, 2400)

        # =====================================================================================
        gt.section("delete-with-active-undo-stack")
        grade = create("net.sf.openfx.GradePlugin", 0, -2000)
        yield ("sleep", SETTLE_MS)
        spins = panel_spinboxes()
        if check(len(spins) > 0, "the Grade's panel has a value box to edit"):
            spins[0].setText("0.5")
            send_key(spins[0], Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        text, enabled = undo_text()
        report("NOTE undo action after editing the Grade: %r enabled=%r" % (text, enabled))
        check(enabled, "editing the Grade's panel made its undo stack active (Undo is enabled)")
        grade_name = grade.getScriptName()
        grade.destroy()
        yield ("sleep", SETTLE_MS)
        check(app.getNode(grade_name) is None, "the Grade was destroyed")
        blur_p1 = create("net.sf.cimg.CImgBlur", 300, -2000)
        yield ("sleep", SETTLE_MS)
        spins = panel_spinboxes()
        if check(len(spins) > 0, "the Blur's panel has a value box to edit"):
            spins[0].setText("1")
            send_key(spins[0], Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        text, enabled = undo_text()
        report("NOTE undo action after editing the Blur: %r enabled=%r" % (text, enabled))
        check(enabled and text is not None and "Blur" in text,
              "after the Grade died, the Edit menu shows the Blur's undo text (%r)" % text)
        gt.shot("p1-after-delete-blur-undo-entry.png")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        redo = [a for a in edit_actions() if a.text().replace("&", "").lower().startswith("redo")]
        check(redo and redo[0].isEnabled() and "Blur" in redo[0].text(),
              "Edit > Undo undid the Blur's edit without crashing (redo now reads %r)"
              % (redo[0].text() if redo else None))
        gt.close_all_panels()

        # =====================================================================================
        gt.section("stub-grab-radius")
        merge = create("Merge", 0, 0)
        src = create("net.sf.openfx.ConstantPlugin", -400, -250)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        labels = [merge.getInputLabel(i) for i in range(merge.getMaxInputCount())]
        report("NOTE Merge inputs: %r" % labels)
        target_input = input_index(merge, "B")
        if target_input is None:
            target_input = 0
        for zoom in (1.0, 0.5):
            for px, want in ((12, True), (16, True), (30, False)):
                view_at(graph, centre(merge), zoom)
                yield ("sleep", 100)
                tip, d, _edge = stub_tip(merge, target_input)
                press_scene = (tip[0] + d[0] * px / zoom, tip[1] + d[1] * px / zoom)
                start = to_vp(graph, press_scene)
                end = to_vp(graph, centre(src))
                if zoom == 1.0 and px == 16:
                    move(graph.viewport(), start)
                    graph_shot("grab-01-press-point-16px-100pct")
                drag(graph.viewport(), start, end)
                yield ("sleep", 150)
                got = input_connected_from(merge, src)
                label = "%dpx past the B stub at %d%% zoom" % (px, int(zoom * 100))
                if want:
                    check(got == target_input, "%s starts an arrow drag that connects B (got input %r)" % (label, got))
                    if got is not None:
                        trigger("undo")
                        yield ("sleep", 150)
                        check(input_connected_from(merge, src) is None, "%s: one undo disconnects it" % label)
                else:
                    check(got is None, "%s does not grab the stub (got input %r)" % (label, got))
                app.clearSelection()
        view_at(graph, centre(merge), 0.5)
        graph_shot("grab-02-merge-stubs-50pct")
        report("NOTE nearest-of-two-crossing-pipes is covered by ctest EdgeNearbyGrab")

        # =====================================================================================
        gt.section("pick-up")
        pb = create("net.sf.cimg.CImgBlur", 1500, 0)
        pc = create("net.sf.openfx.ConstantPlugin", 1150, -300)
        pk = create("net.sf.openfx.CheckerBoardPlugin", 1850, -300)
        pb2 = create("net.sf.cimg.CImgBlur", 1850, 0)
        connect(pb2, 0, pk)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        frame(graph, [pb, pc, pk, pb2], max_zoom=1.0)
        yield ("sleep", 150)
        vp = graph.viewport()

        def aim(scene_pt):
            """Moves the cursor onto scene_pt. The held arrow can grow the scene rectangle, and
            QGraphicsView then scrolls the content under a still cursor, so the move is repeated
            until the point stays put."""
            p = to_vp(graph, scene_pt)
            for _ in range(6):
                move(vp, p)
                QApplication.processEvents()
                now = to_vp(graph, scene_pt)
                if abs(now.x() - p.x()) < 0.5 and abs(now.y() - p.y()) < 0.5:
                    break
                p = now
            return p

        def pick_up(node, scene_pt):
            p = to_vp(graph, scene_pt)
            click(vp, p)
            return p

        def empty_scene():
            x, y, w, h = box(pb)
            return (x + w / 2.0 - 120.0, y - 110.0)

        def empty_point():
            return to_vp(graph, empty_scene())

        undo_before = undo_text()
        pick_up(pb, on_stub(pb, 0))
        held = aim(empty_scene())
        yield ("sleep", 100)
        hits = drawn_near(graph, held)
        report("NOTE pick-up: %d drawn px at the cursor after the first move" % hits)
        check(hits > 0, "pick-up: after a click on the input and a move, the arrow is drawn at the cursor")
        graph_shot("pickup-01-held")
        pan_to = held + QPointF(70, 40)
        press(vp, held, Qt.MiddleButton)
        for i in range(1, 5):
            move(vp, held + (pan_to - held) * (i / 4.0), Qt.MiddleButton)
        release(vp, pan_to, Qt.MiddleButton)
        _root_offset[0] += (pan_to.x() - held.x()) / zoom_of(graph)
        _root_offset[1] += (pan_to.y() - held.y()) / zoom_of(graph)
        yield ("sleep", 100)
        hits = drawn_near(graph, pan_to)
        report("NOTE pick-up: %d drawn px at the cursor after the pan" % hits)
        check(hits > 0, "pick-up: after a middle-button pan the arrow tip is still at the cursor")
        graph_shot("pickup-02-after-pan")
        zoom_before = zoom_of(graph)
        wheel(vp, pan_to, 120)
        yield ("sleep", 100)
        check(abs(zoom_of(graph) - zoom_before) > 1e-6, "pick-up: the wheel zoomed the graph while the input was held")
        hits = drawn_near(graph, pan_to)
        report("NOTE pick-up: %d drawn px at the cursor after the zoom" % hits)
        check(hits > 0, "pick-up: after a wheel zoom the arrow tip is still at the cursor")
        graph_shot("pickup-03-after-zoom")
        on_constant = aim(centre(pc))
        yield ("sleep", 100)
        on_constant = aim(centre(pc))
        graph_shot("pickup-03b-over-constant")
        click(vp, on_constant)
        yield ("sleep", SETTLE_MS)
        check(same(input_of(pb, 0), pc), "pick-up: a click on the Constant connects the Blur's input to it")
        graph_shot("pickup-04-connected")
        trigger("undo")
        yield ("sleep", 200)
        check(input_of(pb, 0) is None, "pick-up: one undo removes the connection")
        undo_before = undo_text()

        def cancel_run(label, cancel):
            app.clearSelection()
            frame(graph, [pb, pc, pk, pb2], max_zoom=1.0)
            pick_up(pb, on_stub(pb, 0))
            held_pt = aim(empty_scene())
            check(drawn_near(graph, held_pt) > 0, "%s: the input is held at the cursor before cancelling" % label)
            popups.arm()
            cancel(held_pt)
            popups.disarm()
            QApplication.processEvents()
            check(not popups.seen, "%s: no menu opened (%d seen)" % (label, len(popups.seen)))
            graph_shot("pickup-cancel-%s" % label.replace(" ", "-"))
            click(vp, aim(centre(pc)))
            QApplication.processEvents()
            check(input_of(pb, 0) is None, "%s: the input is no longer held (a click on the Constant does not connect)" % label)
            check(undo_text() == undo_before, "%s: no undo entry was added (%r)" % (label, undo_text()[0]))

        cancel_run("escape", lambda p: graph_key(Qt.Key_Escape))
        yield ("sleep", 100)
        cancel_run("right click", lambda p: (press(vp, p, Qt.RightButton), release(vp, p, Qt.RightButton)))
        yield ("sleep", 100)
        cancel_run("empty click on an unconnected stub", lambda p: click(vp, p))
        yield ("sleep", 100)

        pp_src = create("net.sf.openfx.ConstantPlugin", 2200, -300)
        pp_dst = create("net.sf.cimg.CImgBlur", 2200, 0)
        pp_dst.connectInput(0, pp_src)
        pb2.connectInput(0, pk)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        def pipe_scene():
            sx, sy, sw, sh = box(pp_src)
            dx, dy, dw, dh = box(pp_dst)
            return (dx + dw / 2.0, (sy + sh + dy) / 2.0)

        def held_connected_run(label, how, target, disconnects, nodes):
            app.clearSelection()
            frame(graph, nodes, max_zoom=1.0)
            pb2.connectInput(0, pk)
            QApplication.processEvents()
            check(same(input_of(pb2, 0), pk), "%s: the Blur starts connected to the CheckerBoard" % label)
            x2, y2, w2, h2 = box(pb2)
            undo_then = undo_text()
            pick_up(pb2, (x2 + w2 / 2.0, y2 - 20.0))
            held_pt = aim(target())
            check(drawn_near(graph, held_pt) > 0, "%s: the connected input is held at the cursor" % label)
            popups.arm()
            if how == "click":
                click(vp, held_pt)
            elif how == "right click":
                press(vp, held_pt, Qt.RightButton)
                release(vp, held_pt, Qt.RightButton)
            else:
                graph_key(Qt.Key_Escape)
            popups.disarm()
            QApplication.processEvents()
            check(not popups.seen, "%s: no menu opened (%d seen)" % (label, len(popups.seen)))
            graph_shot("pickup-%s" % label.replace(" ", "-"))
            click(vp, aim(centre(pc)))
            QApplication.processEvents()
            if disconnects:
                check(input_of(pb2, 0) is None, "%s: the held input is dropped, disconnected" % label)
                trigger("undo")
                QApplication.processEvents()
                check(same(input_of(pb2, 0), pk), "%s: one undo reconnects the CheckerBoard" % label)
                check(undo_text() == undo_then, "%s: that undo leaves the stack as it was (%r)" % (label, undo_text()[0]))
            else:
                check(same(input_of(pb2, 0), pk), "%s: the connection is unchanged and the input no longer held" % label)
                check(undo_text() == undo_then, "%s: no undo entry was added (%r)" % (label, undo_text()[0]))

        pu_nodes = [pb, pc, pk, pb2, pp_src, pp_dst]
        held_connected_run("empty canvas click", "click", empty_scene, True, pu_nodes)
        yield ("sleep", 100)
        held_connected_run("pipe click", "click", pipe_scene, True, pu_nodes)
        yield ("sleep", 100)
        held_connected_run("holding node click", "click", lambda: centre(pb2), False, pu_nodes)
        yield ("sleep", 100)
        held_connected_run("right click", "right click", empty_scene, False, pu_nodes)
        yield ("sleep", 100)
        held_connected_run("escape", "escape", empty_scene, False, pu_nodes)
        yield ("sleep", 100)

        bx0, by0 = pb.getPosition()
        backdrop = create("fr.inria.built-in.BackDrop", bx0 - 230, by0 - 230)
        backdrop.setSize(220, 120)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        held_connected_run("backdrop click", "click", lambda: centre(backdrop), True, pu_nodes + [backdrop])
        yield ("sleep", 100)
        app.clearSelection()
        destroy_nodes([backdrop, pp_dst, pp_src, pb2, pk, pc, pb])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        # A click on a connected pipe stays a click under the drag distance, and picks up the
        # end of the pipe nearest the click: the arrowhead end re-routes the source to another
        # node's preferred input, the source end re-routes the input to another source.
        gt.section("pipe-pick-up")
        ep_src = create("net.sf.openfx.ConstantPlugin", 1500, -300)
        ep_dst = create("net.sf.cimg.CImgBlur", 1500, 0)
        ep_other_dst = create("net.sf.cimg.CImgBlur", 1850, 0)
        ep_other_src = create("net.sf.openfx.CheckerBoardPlugin", 1850, -300)
        ep_nodes = [ep_src, ep_dst, ep_other_dst, ep_other_src]
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        vp = graph.viewport()
        drag_distance = QApplication.startDragDistance()
        report("NOTE pipe-pick-up: drag distance %d px" % drag_distance)

        def ep_reset():
            app.clearSelection()
            ep_other_dst.disconnectInput(0)
            ep_dst.connectInput(0, ep_src)
            frame(graph, ep_nodes, max_zoom=1.0)
            QApplication.processEvents()
            check(same(input_of(ep_dst, 0), ep_src), "pipe-pick-up: the Blur starts fed by the Constant")
            check(input_of(ep_other_dst, 0) is None, "pipe-pick-up: the other Blur starts unconnected")

        def ep_on_pipe(y_from_dest):
            """A scene point on the pipe, y_from_dest scene units above the Blur's top edge,
            following the line between the two nodes' centres that the pipe is clipped from."""
            dcx, dcy = centre(ep_dst)
            scx, scy = centre(ep_src)
            _x, top, _w, _h = box(ep_dst)
            y = top - y_from_dest
            t = (dcy - y) / (dcy - scy)
            return (dcx + (scx - dcx) * t, y)

        def ep_gap():
            _x, top, _w, _h = box(ep_dst)
            _sx, sy, _sw, sh = box(ep_src)
            return top - (sy + sh)

        def ep_near_dest():
            return ep_on_pipe(15.0)

        def ep_near_source():
            return ep_on_pipe(ep_gap() - 15.0)

        def ep_mid_source_side():
            return ep_on_pipe(ep_gap() / 2.0 + 10.0)

        def ep_empty():
            x, y, w, h = box(ep_dst)
            return (x + w / 2.0 - 160.0, y - 110.0)

        def ep_held(label):
            p = aim(ep_empty())
            QApplication.processEvents()
            hits = drawn_near(graph, p)
            report("NOTE %s: %d drawn px at the cursor" % (label, hits))
            return hits > 0

        def ep_jitter_click(scene_pt, modifiers=Qt.NoModifier):
            p = to_vp(graph, scene_pt)
            press(vp, p, Qt.LeftButton, modifiers)
            for d in (QPointF(1, 0), QPointF(2, 1), QPointF(1, 2), QPointF(2, 1)):
                move(vp, p + d, Qt.LeftButton, modifiers)
            release(vp, p + QPointF(2, 1), Qt.LeftButton, modifiers)

        # A jittered click near the source end picks the source end up, and a click on another
        # source node re-routes the Blur's input to it.
        ep_reset()
        undo_then = undo_text()
        ep_jitter_click(ep_near_source())
        QApplication.processEvents()
        check(same(input_of(ep_dst, 0), ep_src), "jitter click: the pipe is not disconnected as a drag would")
        check(ep_held("jitter click"), "jitter click: the pipe is held at the cursor")
        graph_shot("pipe-pickup-01-jitter-held")
        click(vp, aim(centre(ep_other_src)))
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ep_dst, 0), ep_other_src), "jitter click near the source: a click on the CheckerBoard feeds the Blur from it")
        check(input_of(ep_other_dst, 0) is None, "jitter click near the source: the other Blur is untouched")
        trigger("undo")
        yield ("sleep", 150)
        check(same(input_of(ep_dst, 0), ep_src), "jitter click near the source: one undo restores the Constant")
        check(undo_text() == undo_then, "jitter click near the source: that undo leaves the stack as it was (%r)" % (undo_text()[0],))

        # A press that travels past the drag distance is still a drag.
        ep_reset()
        undo_then = undo_text()
        start = to_vp(graph, ep_near_source())
        press(vp, start)
        move(vp, start + QPointF(drag_distance + 4, 0), Qt.LeftButton)
        target = to_vp(graph, centre(ep_other_src))
        for i in range(1, DRAG_STEPS + 1):
            move(vp, start + (target - start) * (i / float(DRAG_STEPS)), Qt.LeftButton)
        release(vp, target)
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ep_dst, 0), ep_other_src), "drag past the drag distance: releasing on the CheckerBoard connects it at once")
        trigger("undo")
        yield ("sleep", 150)
        check(same(input_of(ep_dst, 0), ep_src), "drag past the drag distance: one undo restores the Constant")
        click(vp, aim(ep_empty()))
        QApplication.processEvents()
        check(same(input_of(ep_dst, 0), ep_src), "drag past the drag distance: nothing is held afterwards")
        check(undo_text() == undo_then, "drag past the drag distance: the stack is as it was (%r)" % (undo_text()[0],))

        # A click just past the midpoint on the source side carries the source end too.
        ep_reset()
        undo_then = undo_text()
        click(vp, to_vp(graph, ep_mid_source_side()))
        check(ep_held("mid-pipe click"), "mid-pipe click: the pipe is held at the cursor")
        click(vp, aim(centre(ep_other_dst)))
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ep_dst, 0), ep_other_dst), "mid-pipe click on the source side: the source end moved, the Blur is fed by the other Blur")
        check(input_of(ep_other_dst, 0) is None, "mid-pipe click on the source side: the other Blur's input is untouched")
        trigger("undo")
        yield ("sleep", 150)
        check(same(input_of(ep_dst, 0), ep_src), "mid-pipe click on the source side: one undo restores the Constant")
        check(undo_text() == undo_then, "mid-pipe click on the source side: the stack is as it was (%r)" % (undo_text()[0],))

        # A click near the arrowhead carries the destination end: dropped on another node, the
        # same source feeds that node's preferred input and the original input is let go.
        ep_reset()
        undo_then = undo_text()
        ep_jitter_click(ep_near_dest())
        check(ep_held("arrowhead click"), "arrowhead click: the pipe is held at the cursor")
        graph_shot("pipe-pickup-02-arrowhead-held")
        click(vp, aim(centre(ep_other_dst)))
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ep_other_dst, 0), ep_src), "arrowhead click: the other Blur is now fed by the Constant")
        check(input_of(ep_dst, 0) is None, "arrowhead click: the original Blur's input is let go")
        graph_shot("pipe-pickup-03-arrowhead-moved")
        trigger("undo")
        yield ("sleep", 150)
        check(same(input_of(ep_dst, 0), ep_src), "arrowhead click: one undo feeds the original Blur again")
        check(input_of(ep_other_dst, 0) is None, "arrowhead click: the same undo disconnects the other Blur")
        check(undo_text() == undo_then, "arrowhead click: the move is one undo step (%r)" % (undo_text()[0],))

        # Arrowhead carried, dropped on empty space: the input is disconnected, as a drag would.
        ep_reset()
        undo_then = undo_text()
        click(vp, to_vp(graph, ep_near_dest()))
        check(ep_held("arrowhead empty drop"), "arrowhead empty drop: the pipe is held at the cursor")
        click(vp, aim(ep_empty()))
        yield ("sleep", SETTLE_MS)
        check(input_of(ep_dst, 0) is None, "arrowhead empty drop: the Blur's input is disconnected")
        trigger("undo")
        yield ("sleep", 150)
        check(same(input_of(ep_dst, 0), ep_src), "arrowhead empty drop: one undo reconnects it")
        check(undo_text() == undo_then, "arrowhead empty drop: the stack is as it was (%r)" % (undo_text()[0],))

        def ep_cancel_run(label, where, cancel):
            ep_reset()
            undo_then = undo_text()
            click(vp, to_vp(graph, where()))
            check(ep_held(label), "%s: the pipe is held at the cursor" % label)
            popups.arm()
            cancel()
            popups.disarm()
            QApplication.processEvents()
            check(not popups.seen, "%s: no menu opened (%d seen)" % (label, len(popups.seen)))
            check(same(input_of(ep_dst, 0), ep_src), "%s: the original connection is kept" % label)
            click(vp, aim(centre(ep_other_dst)))
            QApplication.processEvents()
            check(input_of(ep_other_dst, 0) is None, "%s: nothing is held any more (a click on the other Blur connects nothing)" % label)
            check(same(input_of(ep_dst, 0), ep_src), "%s: the connection is still the original one" % label)
            check(undo_text() == undo_then, "%s: no undo entry was added (%r)" % (label, undo_text()[0]))

        ep_cancel_run("arrowhead escape", ep_near_dest, lambda: graph_key(Qt.Key_Escape))
        yield ("sleep", 100)
        ep_cancel_run("source-end escape", ep_near_source, lambda: graph_key(Qt.Key_Escape))
        yield ("sleep", 100)
        ep_cancel_run("arrowhead click on its source", ep_near_dest, lambda: click(vp, aim(centre(ep_src))))
        yield ("sleep", 100)

        # Ctrl no longer turns a still pipe click into a drag.
        ep_reset()
        undo_then = undo_text()
        click(vp, to_vp(graph, ep_near_source()), Qt.LeftButton, Qt.ControlModifier)
        QApplication.processEvents()
        check(same(input_of(ep_dst, 0), ep_src), "ctrl click on a pipe: it is not disconnected as a drag would")
        check(ep_held("ctrl click"), "ctrl click on a pipe: the pipe is held at the cursor")
        graph_key(Qt.Key_Escape)
        QApplication.processEvents()
        check(same(input_of(ep_dst, 0), ep_src), "ctrl click on a pipe: Escape keeps the connection")
        check(undo_text() == undo_then, "ctrl click on a pipe: no undo entry was added (%r)" % (undo_text()[0],))

        # The output stub below a node that feeds nothing is picked up by a click too, carrying
        # its loose end: a click on another node feeds that node's preferred input from it.
        def ep_on_output_stub(node):
            """A scene point on the output stub, a little below the node's bottom edge."""
            x, y, w, h = box(node)
            return (x + w / 2.0, y + h + 6.0)

        ep_reset()
        undo_then = undo_text()
        ep_jitter_click(ep_on_output_stub(ep_other_src))
        QApplication.processEvents()
        check(ep_held("output stub click"), "output stub click: the stub's loose end is held at the cursor")
        graph_shot("pipe-pickup-04-output-stub-held")
        click(vp, aim(centre(ep_other_dst)))
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ep_other_dst, 0), ep_other_src), "output stub click: the other Blur is now fed by the CheckerBoard")
        check(same(input_of(ep_dst, 0), ep_src), "output stub click: the original Blur is untouched")
        graph_shot("pipe-pickup-05-output-stub-connected")
        trigger("undo")
        yield ("sleep", 150)
        check(input_of(ep_other_dst, 0) is None, "output stub click: one undo disconnects the other Blur again")
        check(undo_text() == undo_then, "output stub click: the connection is one undo step (%r)" % (undo_text()[0],))

        def ep_output_cancel_run(label, cancel):
            ep_reset()
            undo_then = undo_text()
            click(vp, to_vp(graph, ep_on_output_stub(ep_other_src)))
            check(ep_held(label), "%s: the stub's loose end is held at the cursor" % label)
            popups.arm()
            cancel()
            popups.disarm()
            QApplication.processEvents()
            check(not popups.seen, "%s: no menu opened (%d seen)" % (label, len(popups.seen)))
            check(input_of(ep_other_dst, 0) is None, "%s: nothing was connected" % label)
            click(vp, aim(centre(ep_other_dst)))
            QApplication.processEvents()
            check(input_of(ep_other_dst, 0) is None, "%s: nothing is held any more (a click on the other Blur connects nothing)" % label)
            check(same(input_of(ep_dst, 0), ep_src), "%s: the original Blur is untouched" % label)
            check(undo_text() == undo_then, "%s: no undo entry was added (%r)" % (label, undo_text()[0]))

        ep_output_cancel_run("output stub escape", lambda: graph_key(Qt.Key_Escape))
        yield ("sleep", 100)
        ep_output_cancel_run("output stub empty drop", lambda: click(vp, aim(ep_empty())))
        yield ("sleep", 100)
        ep_output_cancel_run("output stub click on its own node", lambda: click(vp, aim(centre(ep_other_src))))
        yield ("sleep", 100)

        app.clearSelection()
        destroy_nodes([ep_other_src, ep_other_dst, ep_dst, ep_src])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("y-shift-y")
        graph = root_graph()
        ys = create("net.sf.openfx.ConstantPlugin", 3000, -600)
        ma = create("Merge", 3150, 0)
        gb = create("net.sf.openfx.GradePlugin", 3000, -300)
        gc = create("net.sf.openfx.GradePlugin", 3300, -300)
        sa = create("net.sf.openfx.GradePlugin", 3600, -300)
        sb = create("net.sf.openfx.GradePlugin", 3900, -300)
        sm = create("Merge", 3750, 0)
        for fed in (gb, gc, sa, sb):
            fed.connectInput(0, ys)
        gt.close_all_panels()
        app.clearSelection()
        frame(graph, [ys, ma, gb, gc, sa, sb, sm], max_zoom=1.0)

        # The "Merge node connect to A input" preference picks which of A and B is preferred.
        to_a = NatronEngine.natron.getSettings().getParam("mergeConnectToA").getValue()

        def merge_slots(merge):
            preferred = input_index(merge, "A" if to_a else "B")
            if preferred is None:
                preferred = 0
            return preferred, [i for i in range(merge.getMaxInputCount()) if i != preferred][0]

        def select_in_order(nodes):
            app.setSelection(nodes)
            QApplication.processEvents()

        y_pref, y_next = merge_slots(ma)
        for order, (first, second) in (("B then C", (gb, gc)), ("C then B", (gc, gb))):
            select_in_order([ma, first, second])
            graph_key(Qt.Key_Y)
            yield ("sleep", SETTLE_MS)
            check(same(input_of(ma, y_pref), first),
                  "Y (%s): the first later-selected node feeds the Merge's preferred input" % order)
            check(same(input_of(ma, y_next), second),
                  "Y (%s): the second later-selected node feeds its next free input" % order)
            check(same(input_of(gb, 0), ys) and same(input_of(gc, 0), ys),
                  "Y (%s): the later-selected nodes keep their own inputs" % order)
            text = undo_text()[0]
            check(text is not None and "Connect Later-Selected Nodes into First Selected" in text,
                  "Y (%s): the undo entry reads %r" % (order, text))
            graph_shot("y-01-merge-fed-%s" % order.replace(" ", "-"))
            trigger("undo")
            yield ("sleep", 200)
            check(all(input_of(ma, i) is None for i in range(ma.getMaxInputCount())),
                  "Y (%s): one undo removes both connections" % order)

        s_pref, s_next = merge_slots(sm)
        for order, (first, second) in (("A then B", (sa, sb)), ("B then A", (sb, sa))):
            select_in_order([first, second, sm])
            graph_key(Qt.Key_Y, Qt.ShiftModifier)
            yield ("sleep", SETTLE_MS)
            check(same(input_of(sm, s_pref), first),
                  "Shift+Y (%s): the first earlier-selected node feeds the Merge's preferred input" % order)
            check(same(input_of(sm, s_next), second),
                  "Shift+Y (%s): the second earlier-selected node feeds its next free input" % order)
            check(same(input_of(sa, 0), ys) and same(input_of(sb, 0), ys),
                  "Shift+Y (%s): the earlier-selected nodes keep their own inputs" % order)
            text = undo_text()[0]
            check(text is not None and "Connect Earlier-Selected Nodes into Last Selected" in text,
                  "Shift+Y (%s): the undo entry reads %r" % (order, text))
            graph_shot("y-02-shift-y-%s" % order.replace(" ", "-"))
            trigger("undo")
            yield ("sleep", 200)
            check(all(input_of(sm, i) is None for i in range(sm.getMaxInputCount())),
                  "Shift+Y (%s): one undo removes both connections" % order)

        def merge_fill_order(merge):
            main = [i for i in range(merge.getMaxInputCount()) if merge.getInputLabel(i).lower() != "mask"]
            preferred = input_index(merge, "A" if to_a else "B")
            if preferred in main:
                main.remove(preferred)
                main.insert(0, preferred)
            return main

        mask_slots = [i for i in range(ma.getMaxInputCount()) if ma.getInputLabel(i).lower() == "mask"]
        fill_order = merge_fill_order(ma)
        y_grades = [create("net.sf.openfx.GradePlugin", 3000 + 300 * k, -900) for k in range(5)]
        gt.close_all_panels()
        select_in_order([ma] + y_grades)
        graph_key(Qt.Key_Y)
        yield ("sleep", SETTLE_MS)
        for k, slot in enumerate(fill_order[:len(y_grades)]):
            check(same(input_of(ma, slot), y_grades[k]),
                  "Y: Grade %d of 5 feeds the Merge's input %s" % (k + 1, ma.getInputLabel(slot)))
        check(all(input_of(ma, i) is None for i in mask_slots), "Y: five Grades into a Merge never fill its mask")
        frame(graph, [ma] + y_grades, max_zoom=1.0)
        graph_shot("y-03-merge-five-grades")
        trigger("undo")
        yield ("sleep", 200)
        check(all(input_of(ma, i) is None for i in range(ma.getMaxInputCount())), "Y: one undo removes every Merge connection")

        select_in_order([ma] + y_grades)
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        check(all(same(input_of(g, 0), ma) for g in y_grades), "U: the first-selected Merge feeds each of the five Grades")
        check(all(input_of(ma, i) is None for i in range(ma.getMaxInputCount())), "U: the Merge's own inputs stay empty")
        frame(graph, [ma] + y_grades, max_zoom=1.0)
        graph_shot("u-07-merge-into-five-grades")
        trigger("undo")
        yield ("sleep", 200)
        check(all(input_of(g, 0) is None for g in y_grades), "U: one undo removes every Grade connection")

        mg = create("net.sf.openfx.GradePlugin", 3300, -600)
        g_mask = [i for i in range(mg.getMaxInputCount()) if mg.getInputLabel(i).lower() == "mask"]
        g_main = [i for i in range(mg.getMaxInputCount()) if i not in g_mask]
        gt.close_all_panels()
        select_in_order([mg, y_grades[0], y_grades[1]])
        graph_key(Qt.Key_Y)
        yield ("sleep", SETTLE_MS)
        check(len(g_main) == 1 and len(g_mask) == 1, "Y: a Grade has one main input and one mask (%r)" % ([mg.getInputLabel(i) for i in range(mg.getMaxInputCount())],))
        check(same(input_of(mg, g_main[0]), y_grades[0]), "Y: the first source feeds the Grade's Source")
        check(same(input_of(mg, g_mask[0]), y_grades[1]), "Y: the second source feeds the Grade's Mask")
        graph_shot("y-04-grade-source-then-mask")
        trigger("undo")
        yield ("sleep", 200)
        destroy_nodes([mg] + y_grades)
        app.clearSelection()
        destroy_nodes([sm, sb, sa, gc, gb, ma, ys])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("u-shift-u")
        graph = root_graph()
        ux = create("net.sf.openfx.ConstantPlugin", 4200, -600)
        ua = create("net.sf.openfx.GradePlugin", 4200, -300)
        ub = create("net.sf.openfx.GradePlugin", 4500, -300)
        uc = create("net.sf.openfx.GradePlugin", 4800, -300)
        um = create("Merge", 4500, 100)
        ud = create("net.sf.openfx.GradePlugin", 4800, 100)
        gt.close_all_panels()
        app.clearSelection()
        frame(graph, [ux, ua, ub, uc, um, ud], max_zoom=1.0)

        def free_main_input(node):
            for i in range(node.getMaxInputCount()):
                if node.getInput(i) is None and node.getInputLabel(i).lower() != "mask":
                    return i
            return None

        select_in_order([ua, ub, uc])
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ub, 0), ua) and same(input_of(uc, 0), ua), "U: the first-selected node feeds both later-selected nodes")
        text = undo_text()[0]
        check(text is not None and "Connect First Selected into Later-Selected Nodes" in text, "U: the undo entry reads %r" % text)
        graph_shot("u-01-first-into-later")
        trigger("undo")
        yield ("sleep", 200)
        check(input_of(ub, 0) is None and input_of(uc, 0) is None, "U: one undo reverts both connections")

        select_in_order([ua, ub, uc])
        graph_key(Qt.Key_U, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ua, 0), uc) and same(input_of(ub, 0), uc), "Shift+U: the last-selected node feeds both earlier-selected nodes")
        text = undo_text()[0]
        check(text is not None and "Connect Last Selected into Other Selected Nodes" in text, "Shift+U: the undo entry reads %r" % text)
        graph_shot("u-02-last-into-others")
        trigger("undo")
        yield ("sleep", 200)
        check(input_of(ua, 0) is None and input_of(ub, 0) is None, "Shift+U: one undo reverts both connections")

        um_b = input_index(um, "B")
        um.connectInput(0 if um_b is None else um_b, ux)
        ud.connectInput(0, ux)
        QApplication.processEvents()
        um_want = free_main_input(um)
        ud_mask = [i for i in range(ud.getMaxInputCount()) if ud.getInputLabel(i).lower() == "mask"]
        select_in_order([ua, um, ud])
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        check(um_want is not None and same(input_of(um, um_want), ua), "U: a Merge with one input taken is fed on its next free input (%r)" % um_want)
        check(same(input_of(ud, 0), ux) and len(ud_mask) == 1 and same(input_of(ud, ud_mask[0]), ua),
              "U: a single-input node with its main input taken takes the source on its mask")
        graph_shot("u-03-next-free-input")
        trigger("undo")
        yield ("sleep", 200)
        check(um_want is not None and input_of(um, um_want) is None, "U: one undo reverts the Merge's new connections")
        check(all(input_of(ud, i) is None for i in ud_mask), "U: that undo also clears the Grade's mask")
        um.disconnectInput(0 if um_b is None else um_b)
        ud.disconnectInput(0)

        um_masks = [i for i in range(um.getMaxInputCount()) if um.getInputLabel(i).lower() == "mask"]
        um_main = [i for i in range(um.getMaxInputCount()) if i not in um_masks]
        for i in um_main:
            um.connectInput(i, ux)
        QApplication.processEvents()
        select_in_order([ua, um])
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        check(all(same(input_of(um, i), ux) for i in um_main), "U: a Merge with every main input taken keeps them")
        check(all(input_of(um, i) is None for i in um_masks), "U: a Merge with every main input taken is skipped, its mask left empty")
        graph_shot("u-05-merge-full-skips-mask")
        for i in um_main:
            um.disconnectInput(i)

        ud.connectInput(0, ux)
        QApplication.processEvents()
        select_in_order([ua, ud])
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        check(same(input_of(ud, 0), ux), "U: a Grade whose Source is taken keeps it")
        check(len(ud_mask) == 1 and same(input_of(ud, ud_mask[0]), ua), "U: a Grade whose Source is taken takes the U source on its Mask")
        graph_shot("u-06-grade-mask")
        trigger("undo")
        yield ("sleep", 200)
        check(all(input_of(ud, i) is None for i in ud_mask), "U: one undo clears the Grade's Mask")
        ud.disconnectInput(0)

        ua.connectInput(0, ub)
        QApplication.processEvents()
        select_in_order([ua, ub, uc])
        graph_key(Qt.Key_U)
        yield ("sleep", SETTLE_MS)
        message = main_window().statusBar().currentMessage()
        check(input_of(ub, 0) is None, "U: a node upstream of the first-selected one is skipped")
        check(same(input_of(uc, 0), ua), "U: the other later-selected node is still connected")
        check(message.startswith("Skipped"), "U: the status bar reports the skipped node (%r)" % message)
        graph_shot("u-04-skipped-upstream")
        trigger("undo")
        yield ("sleep", 200)
        check(input_of(uc, 0) is None and same(input_of(ua, 0), ub), "U: one undo reverts only the new connection")
        ua.disconnectInput(0)

        app.clearSelection()
        key_event = QKeyEvent(QEvent.KeyPress, Qt.Key_U, Qt.NoModifier)
        key_event.ignore()
        QApplication.sendEvent(root_graph(), key_event)
        QApplication.processEvents()
        check(key_event.isAccepted(), "U in the node graph is consumed by the graph, so it does not reach the viewer's Refresh")
        report("NOTE u-shift-u: whether the viewer re-rendered has no Python hook, so only the graph taking the key is checked")
        destroy_nodes([ud, um, uc, ub, ua, ux])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("paste-attach")
        reader = app.createReader(os.path.join(gt.FIXTURES, "flat-rgba-only.exr"))
        gt.require(reader is not None, "create a Read of flat-rgba-only.exr")
        reader.setPosition(5000, 0)
        connect(viewer, 0, reader)
        copy_blur = create("net.sf.cimg.CImgBlur", 5400, 0)
        copy_grade = create("net.sf.openfx.GradePlugin", 5400, 200)
        connect(copy_grade, 0, copy_blur)
        gt.close_all_panels()
        graph = root_graph()
        frame(graph, [reader, copy_blur, copy_grade], max_zoom=1.0)

        def copy(nodes):
            app.setSelection(nodes)
            QApplication.processEvents()
            graph_key(Qt.Key_C, Qt.ControlModifier)

        def paste_onto(nodes):
            if nodes:
                app.setSelection(nodes)
            else:
                app.clearSelection()
            QApplication.processEvents()
            before = child_names()
            graph_key(Qt.Key_V, Qt.ControlModifier)
            QApplication.processEvents()
            return new_children(before)

        copy([copy_blur])
        pasted = paste_onto([reader])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 1, "paste a Blur onto the Read: one node is pasted (got %d)" % len(pasted)):
            pb_new = pasted[0]
            check(same(input_of(pb_new, 0), reader), "paste onto Read: the pasted Blur takes the Read")
            check(same(input_of(viewer, 0), pb_new), "paste onto Read: the Viewer now takes the pasted Blur")
            frame(graph, [reader, pb_new], max_zoom=1.0)
            graph_shot("paste-01-read-blur-viewer")
            name = pb_new.getScriptName()
            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(app.getNode(name) is None, "paste onto Read: one undo removes the pasted Blur")
            check(same(input_of(viewer, 0), reader), "paste onto Read: one undo restores Read -> Viewer")

        copy([copy_blur, copy_grade])
        pasted = paste_onto([reader])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 2, "paste a Blur -> Grade chain onto the Read: two nodes (got %d)" % len(pasted)):
            pblur = [n for n in pasted if "Blur" in n.getPluginID()]
            pgrade = [n for n in pasted if "Grade" in n.getPluginID()]
            if check(len(pblur) == 1 and len(pgrade) == 1, "the chain pasted as one Blur and one Grade"):
                check(same(input_of(pblur[0], 0), reader), "chain: the pasted Blur (entry) takes the Read")
                check(same(input_of(pgrade[0], 0), pblur[0]), "chain: the pasted Grade still takes the pasted Blur")
                check(same(input_of(viewer, 0), pgrade[0]), "chain: the Viewer takes the pasted Grade (exit)")
                frame(graph, [reader, pblur[0], pgrade[0]], max_zoom=1.0)
                graph_shot("paste-02-chain")
            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(same(input_of(viewer, 0), reader), "chain: one undo restores Read -> Viewer")
            check(all(app.getNode(n.getScriptName()) is None for n in pasted), "chain: one undo removes both pasted nodes")

        split_src = create("net.sf.cimg.CImgBlur", 6400, 0)
        connect(split_src, 0, reader)
        split_merge = create("Merge", 6400, 300)
        split_a = input_index(split_merge, "A")
        split_b = input_index(split_merge, "B")
        gt.require(split_a is not None and split_b is not None, "the Merge has A and B inputs")
        connect(split_merge, split_a, split_src)
        connect(split_merge, split_b, split_src)
        gt.close_all_panels()
        copy([copy_blur, copy_grade])
        pasted = paste_onto([split_src])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 2, "paste a chain onto a node feeding a Merge's A and B: two nodes (got %d)" % len(pasted)):
            pgrade = [n for n in pasted if "Grade" in n.getPluginID()]
            if check(len(pgrade) == 1, "the chain pasted onto the Merge's source has one Grade"):
                check(same(input_of(split_merge, split_a), pgrade[0]), "A and B: the Merge's A takes the pasted Grade (exit)")
                check(same(input_of(split_merge, split_b), pgrade[0]), "A and B: the Merge's B takes the pasted Grade (exit)")
            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(same(input_of(split_merge, split_a), split_src) and same(input_of(split_merge, split_b), split_src),
                  "A and B: one undo restores both Merge inputs to the source")
            check(all(app.getNode(n.getScriptName()) is None for n in pasted), "A and B: one undo removes both pasted nodes")
        destroy_nodes([split_merge, split_src])

        copy([copy_blur])
        pasted = paste_onto([])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 1, "paste with nothing selected pastes one node"):
            check(input_of(pasted[0], 0) is None, "paste with nothing selected: the Blur is unconnected")

        pasted = paste_onto([viewer])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 1, "paste onto the Viewer pastes one node"):
            check(input_of(pasted[0], 0) is None, "paste onto the Viewer: the Blur is unconnected")
            check(same(input_of(viewer, 0), reader), "paste onto the Viewer: the Viewer still takes the Read")

        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        writer = app.createWriter(os.path.join(gt.OUT, "m69_paste.####.png"), None, props)
        gt.require(writer is not None, "create a Write")
        writer.setPosition(5800, 0)
        connect(writer, 0, reader)
        gt.close_all_panels()
        pasted = paste_onto([writer])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 1, "paste onto the Write pastes one node"):
            check(input_of(pasted[0], 0) is None, "paste onto the Write: the Blur is unconnected")
            check(same(input_of(writer, 0), reader), "paste onto the Write: the Write still takes the Read")

        gen_target = create("net.sf.cimg.CImgBlur", 6100, 0)
        connect(gen_target, 0, reader)
        gen_src = create("net.sf.openfx.ConstantPlugin", 6100, -300)
        gt.close_all_panels()
        copy([gen_src])
        pasted = paste_onto([gen_target])
        yield ("sleep", SETTLE_MS)
        if check(len(pasted) == 1, "paste a Constant onto a Blur pastes one node"):
            outs = [n for n in app.getChildren() if input_connected_from(n, pasted[0]) is not None]
            check(not outs, "paste a generator: the pasted Constant feeds nothing")
            check(same(input_of(gen_target, 0), reader), "paste a generator: the Blur keeps its input")
        app.clearSelection()
        frame(graph, [reader, writer, gen_target, gen_src], max_zoom=1.0)
        graph_shot("paste-03-exceptions")

        # =====================================================================================
        gt.section("paste-focus")
        graph = root_graph()
        blur_one = create("net.sf.cimg.CImgBlur", 6500, 0)
        blur_two = create("net.sf.cimg.CImgBlur", 6500, 250)
        gt.close_all_panels()
        pasted_here = []
        frame(graph, [blur_one, blur_two], max_zoom=1.0)

        def script_error_count():
            total = 0
            for editor in gt.widgets_of("ScriptEditor"):
                for edit in editor.findChildren(QTextEdit):
                    text = edit.toPlainText()
                    total += text.count("Python exception") + text.count("SyntaxError")
            return total

        if not gt.widgets_of("ScriptEditor"):
            report("NOTE paste-focus: no Script Editor widget exists, so the paste is judged by the graph alone")

        def paste_through_main_window(target):
            frame(graph, [blur_one, blur_two], max_zoom=1.0)
            x0, y0, w0, h0 = box(blur_two)
            click(graph.viewport(), to_vp(graph, (x0 - 50.0, y0 + h0 / 2.0)))
            app.setSelection([target])
            QApplication.processEvents()
            graph.clearFocus()
            QApplication.processEvents()
            before = child_names()
            errors_before = script_error_count()
            send_key(graph.window(), Qt.Key_V, Qt.ControlModifier)
            return before, errors_before

        app.setSelection([blur_one])
        QApplication.processEvents()
        graph_key(Qt.Key_C, Qt.ControlModifier)

        before, errors_before = paste_through_main_window(blur_two)
        yield ("sleep", SETTLE_MS)
        pasted = new_children(before)
        pasted_here.extend(pasted)
        if check(len(pasted) == 1, "Ctrl+V sent to the main window pastes one node (got %d)" % len(pasted)):
            check("Blur" in pasted[0].getPluginID(), "the pasted node is a Blur")
            check(same(input_of(pasted[0], 0), blur_two), "the pasted Blur takes the selected Blur")
        check(script_error_count() == errors_before, "the Script Editor shows no new Python exception or SyntaxError")
        graph_shot("paste-focus-01-main-window")

        app.clearSelection()
        before_tab = child_names()
        gcentre = QPointF(graph.rect().center())
        QApplication.sendEvent(graph, QEnterEvent(gcentre, gcentre, QPointF(graph.mapToGlobal(graph.rect().center()))))
        QTest.keyClick(graph, Qt.Key_Tab)
        yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), 20.0)
        dialogs = [d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible()]
        tab_node = None
        if check(dialogs, "the Tab key opens the tab menu"):
            dialog = dialogs[0]
            edits = dialog.findChildren(QLineEdit)
            lists = dialog.findChildren(QListView)
            if check(edits and lists, "the tab menu has its line edit and result list"):
                edits[0].setText("Grade")
                QApplication.processEvents()
                model = lists[0].model()
                row = next((r for r in range(model.rowCount())
                            if str(model.index(r, 0).data()).split("[")[0].strip() == "Grade"), -1)
                if check(row >= 0, "the tab menu lists the Grade plugin"):
                    lists[0].setCurrentIndex(model.index(row, 0))
                    QTest.keyClick(edits[0], Qt.Key_Return)
                else:
                    QTest.keyClick(dialog, Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        made = new_children(before_tab)
        pasted_here.extend(made)
        if check(len(made) == 1, "the popup created one node (got %d)" % len(made)):
            tab_node = made[0]
        gt.close_all_panels()
        if tab_node is not None:
            before, errors_before = paste_through_main_window(tab_node)
            yield ("sleep", SETTLE_MS)
            pasted = new_children(before)
            pasted_here.extend(pasted)
            if check(len(pasted) == 1, "after the popup, Ctrl+V sent to the main window pastes one node (got %d)" % len(pasted)):
                check(same(input_of(pasted[0], 0), tab_node), "after the popup, the pasted Blur takes the node the popup created")
            check(script_error_count() == errors_before, "after the popup, the Script Editor shows no new Python exception or SyntaxError")
            graph_shot("paste-focus-02-after-tab-popup")
        app.clearSelection()
        gt.close_all_panels()
        destroy_nodes(pasted_here + [blur_two, blur_one])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("pane-header-menu")
        graph = root_graph()
        pane = ancestor_of_class(graph, "TabWidget")
        header = None
        if pane is not None:
            headers = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabWidgetHeader"]
            header = headers[0] if headers else None

        def menu_at_cursor(seen, glob_pos):
            """The menu's left edge is at the cursor and it spans the cursor's row: Qt lifts a menu
            that would run off the bottom of the screen."""
            top = seen["pos"].y()
            return (abs(seen["pos"].x() - glob_pos.x()) <= 2
                    and top - 2 <= glob_pos.y() <= top + seen["height"] + 2)

        if check(header is not None, "found the graph pane's tab header"):
            empty = None
            for x in range(header.width() - 5, 0, -5):
                p = QPoint(x, header.height() // 2)
                if header.childAt(p) is None:
                    empty = p
                    break
            corner_texts = []
            if check(empty is not None, "the header has empty space beside the tabs"):
                glob = header.mapToGlobal(empty)
                popups.arm()
                QApplication.sendEvent(header, QContextMenuEvent(QContextMenuEvent.Mouse, empty, glob))
                QApplication.processEvents()
                popups.disarm()
                if check(len(popups.seen) == 1, "a right-click on the empty header opens one menu (got %d)" % len(popups.seen)):
                    seen = popups.seen[0]
                    report("NOTE header menu at %r, cursor at %r: %r" % (seen["pos"], glob, seen["texts"]))
                    check(menu_at_cursor(seen, glob), "the header menu opens at the cursor")
                    header_texts = seen["texts"]
                    popups.arm()
                    QMetaObject.invokeMethod(pane, "onPaneMenuButtonClicked")
                    QApplication.processEvents()
                    popups.disarm()
                    if check(len(popups.seen) == 1, "the corner icon still opens its menu"):
                        corner = popups.seen[0]
                        corner_texts = corner["texts"]
                        check(corner_texts == header_texts, "the header menu has the corner menu's entries")
                        check(abs(corner["pos"].x() - glob.x()) > 20,
                              "the corner menu opens at the icon, not at the header cursor (%r)" % corner["pos"])
            def pane_menu_after(widget, local_pos, label):
                glob_pos = widget.mapToGlobal(local_pos)
                popups.arm()
                QApplication.sendEvent(widget, QContextMenuEvent(QContextMenuEvent.Mouse, local_pos, glob_pos))
                QApplication.processEvents()
                popups.disarm()
                if not check(len(popups.seen) == 1, "a right-click on %s opens one menu (got %d)" % (label, len(popups.seen))):
                    return
                seen = popups.seen[0]
                check(corner_texts and seen["texts"] == corner_texts, "the menu from %s is the pane menu" % label)
                check(menu_at_cursor(seen, glob_pos), "the menu from %s opens at the cursor" % label)

            bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
            if check(bars, "found the pane's tab bar"):
                bar = bars[0]
                tab_count = bar.count()
                original = bar.currentIndex()
                if check(tab_count >= 2, "the graph pane has two or more tabs (got %d)" % tab_count):
                    target = [i for i in range(tab_count) if i != original][0]
                    original_widget = pane.currentWidget() if hasattr(pane, "currentWidget") else None
                    tab_center = bar.tabRect(target).center()
                    send_mouse(bar, QEvent.MouseButtonPress, tab_center, Qt.RightButton, Qt.RightButton)
                    send_mouse(bar, QEvent.MouseButtonRelease, tab_center, Qt.RightButton, Qt.NoButton)
                    check(bar.currentIndex() == original, "a right-press on a non-current tab leaves the current tab alone")
                    pane_menu_after(bar, tab_center, "a non-current tab")
                    check(bar.currentIndex() == original, "the right-clicked tab did not become current")

                    def shoot_tab_menu():
                        pop = QApplication.activePopupWidget()
                        if pop is not None:
                            gt.shot("pane-menu-02-non-current-tab.png")
                            pop.close()
                    QTimer.singleShot(300, shoot_tab_menu)
                    QApplication.sendEvent(bar, QContextMenuEvent(QContextMenuEvent.Mouse, tab_center, bar.mapToGlobal(tab_center)))
                    QApplication.processEvents()
                    check(bar.currentIndex() == original, "the tab is still not current after the captured menu closes")
                    if original_widget is not None:
                        check(pane.currentWidget() is original_widget, "the pane's current widget is unchanged by the right-click")
                buttons = header.findChildren(QPushButton)
                buttons.sort(key=lambda b: b.mapTo(header, QPoint(0, 0)).x())
                float_buttons = [b for b in buttons if "Float pane" in b.toolTip()]
                close_buttons = [b for b in buttons if "Close pane" in b.toolTip()]
                if check(buttons and float_buttons and close_buttons, "found the header's corner, float and close buttons"):
                    for button, label in ((buttons[0], "the left corner button"),
                                          (float_buttons[0], "the float button"),
                                          (close_buttons[0], "the close button")):
                        pane_menu_after(button, button.rect().center(), label)
                    check(pane.isVisible() and bar.count() == tab_count and root_graph() is not None
                          and root_graph().window() is main_window(),
                          "right-clicking the float and close buttons neither floated nor closed the pane")
            # A capture of the open menu itself, taken from inside its event loop.
            def shoot_menu():
                pop = QApplication.activePopupWidget()
                if pop is not None:
                    gt.shot("pane-menu-01-header-right-click.png")
                    pop.close()
            if empty is not None:
                QTimer.singleShot(300, shoot_menu)
                QApplication.sendEvent(header, QContextMenuEvent(QContextMenuEvent.Mouse, empty, header.mapToGlobal(empty)))
                QApplication.processEvents()

        # =====================================================================================
        gt.section("curve-join")
        gt.close_all_panels()
        cg = create("net.sf.openfx.GradePlugin", 7000, 0)
        mix = cg.getParam("mix")
        smooth = NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeSmooth
        for t, v in ((1, 0.0), (11, 0.5), (21, 1.0)):
            mix.setValueAtTime(v, t)
            mix.setInterpolationAtTime(t, smooth)
        yield ("sleep", SETTLE_MS)
        editors = gt.widgets_of("CurveEditor")
        cw = None
        if check(editors, "found the curve editor"):
            check(make_tab_current(editors[0]), "brought the curve editor's tab to the front")
            yield ("sleep", SETTLE_MS)
            trees = editors[0].findChildren(QTreeWidget)
            for tree in trees:
                for i in range(tree.topLevelItemCount()):
                    item = tree.topLevelItem(i)
                    if item.text(0) == cg.getLabel():
                        item.setSelected(True)
            QApplication.processEvents()
            cws = [w for w in gt.widgets_of("CurveWidget") if w.isVisible()]
            cw = cws[0] if cws else None
        if check(cw is not None, "the curve widget is visible"):
            send_key(cw, Qt.Key_A)
            yield ("sleep", SETTLE_MS)
            W, H = float(cw.width()), float(cw.height())
            y0, y1 = 0.0 - 0.1, 1.0 + 0.1
            sy = H / (y1 - y0)

            pyviewer = app.getViewer(viewer.getScriptName())
            pyviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            xa = time_cursor_x(cw)
            pyviewer.seek(21)
            yield ("sleep", SETTLE_MS)
            xb = time_cursor_x(cw)
            pyviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            report("NOTE curve widget %dx%d, time line at frame 1: %r, frame 21: %r" % (W, H, xa, xb))
            gt.require(xa is not None and xb is not None and xb > xa, "found the curve widget's time line at frames 1 and 21")
            sx = (xb - xa) / 20.0

            def to_w(t, v):
                return QPointF(xa + (t - 1) * sx, H - (v - y0) * sy)

            def slopes():
                e = 0.01
                v = mix.getValueAtTime(11.0)
                return ((v - mix.getValueAtTime(11.0 - e)) / e, (mix.getValueAtTime(11.0 + e) - v) / e)

            def handle_clamp():
                xmax = min(W / 8.0, (to_w(21, 0).x() - to_w(11, 0.5).x()) / 3.0)
                return xmax, min(H / 8.0, xmax)

            free_or_broken = (NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeFree,
                              NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeBroken)

            def right_handle(slope):
                key = to_w(11, 0.5)
                xmax, ymax = handle_clamp()
                pix_slope = slope * sy / sx
                interp, _left, _right, _lw, right_weight = mix.getKeyTangents(11.0)
                if interp in free_or_broken:
                    # A Free or Broken key's handle sits on its Bezier control point.
                    extent = right_weight * (to_w(21, 0).x() - key.x())
                    return key + QPointF(extent, -extent * pix_slope)
                if xmax * abs(pix_slope) < ymax:
                    return key + QPointF(xmax, -xmax * pix_slope)
                return key + QPointF(ymax / abs(pix_slope), -ymax * (1 if pix_slope > 0 else -1))

            click(cw, to_w(11, 0.5))
            yield ("sleep", 150)
            left0, right0 = slopes()
            report("NOTE slopes before: %.4f %.4f" % (left0, right0))
            check(abs(left0 - right0) < 1e-3, "the middle key starts smooth (equal slopes)")
            handle = right_handle(right0)
            drag(cw, handle, handle + QPointF(0, -40), Qt.LeftButton, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            left1, right1 = slopes()
            report("NOTE slopes after Ctrl-drag: %.4f %.4f" % (left1, right1))
            check(abs(left1 - left0) < 1e-3, "Ctrl-drag of the right handle leaves the left slope alone")
            check(right1 > right0 + 1e-3, "Ctrl-drag of the right handle steepens the right slope (broken)")
            widget_shot(cw, "curve-01-broken")
            send_key(cw, Qt.Key_X, Qt.ShiftModifier)
            yield ("sleep", SETTLE_MS)
            left2, right2 = slopes()
            want = math.tan((math.atan(left1) + math.atan(right1)) / 2.0)
            report("NOTE slopes after Join: %.4f %.4f (want %.4f)" % (left2, right2, want))
            check(abs(left2 - right2) < 1e-3, "Join makes both slopes equal")
            check(abs(left2 - want) < 2e-3, "Join takes the mean of the two tangent angles")
            widget_shot(cw, "curve-02-joined")
            handle = right_handle(right2)
            drag(cw, handle, handle + QPointF(0, 30))
            yield ("sleep", SETTLE_MS)
            left3, right3 = slopes()
            report("NOTE slopes after a plain drag: %.4f %.4f" % (left3, right3))
            check(abs(left3 - right3) < 1e-3 and abs(right3 - right2) > 1e-3,
                  "after Join a plain drag moves both slopes together")
            widget_shot(cw, "curve-03-plain-drag")

            xmax, ymax = handle_clamp()
            key_pt = to_w(11, 0.5)
            handle = right_handle(right3)
            # Twice the default extent at most, so the weight stays inside its clamp and the
            # handle can follow the cursor exactly.
            far = key_pt + QPointF(2.0 * xmax, -1.0 * xmax)
            cursor_length = math.hypot(far.x() - key_pt.x(), far.y() - key_pt.y())
            press(cw, handle, Qt.LeftButton, Qt.ControlModifier)
            for i in range(1, DRAG_STEPS + 1):
                move(cw, handle + (far - handle) * (i / float(DRAG_STEPS)), Qt.LeftButton, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            held_img = widget_image(cw)
            widget_shot(cw, "curve-04-handle-held-far")
            release(cw, far, Qt.LeftButton, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            released_img = widget_image(cw)
            click(cw, QPointF(10, 10))
            yield ("sleep", SETTLE_MS)
            bare_img = widget_image(cw)
            bounds = (key_pt.x() - 10, key_pt.y() - 3 * xmax, key_pt.x() + 4 * xmax, key_pt.y() + 3 * xmax)

            def handle_extent(img):
                """Count and farthest distance from the key of the handle-coloured pixels that
                selecting the key adds to the picture."""
                dists = [math.hypot(x - key_pt.x(), y - key_pt.y())
                         for x, y, c in differing_pixels(img, bare_img, bounds)
                         if c.red() > 200 and 50 < c.green() < 140 and 50 < c.blue() < 140]
                return len(dists), (max(dists) if dists else 0.0)

            n_held, far_held = handle_extent(held_img)
            n_released, far_released = handle_extent(released_img)
            report("NOTE tangent handle while held: %d px out to %.1f; after release: %d px out to %.1f; cursor %.1f"
                   % (n_held, far_held, n_released, far_released, cursor_length))
            check(n_held >= 10, "the dragged tangent handle is drawn while the button is held")
            check(abs(far_held - cursor_length) <= 6.0, "a held tangent handle reaches the cursor: the drag sets its length")
            check(n_released >= 10 and abs(far_held - far_released) <= 3.0,
                  "the tangent handle does not change length when the button is released")
        graphs = gt.widgets_of("NodeGraph")
        if graphs:
            make_tab_current(graphs[0])
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        cg.getParam("mix").removeAnimation(0)
        destroy_nodes([cg])

        # =====================================================================================
        gt.section("folded-curve")
        gt.close_all_panels()
        fg = create("net.sf.openfx.GradePlugin", 11200, 0)
        gain_name = "gain" if fg.getParam("gain") is not None else "multiply"
        gain = fg.getParam(gain_name)
        n_dims = gain.getNumDimensions()
        gain_label = gain.getLabel().strip().lower()
        report("NOTE folded-curve: using the Grade's %r knob, %d components" % (gain_name, n_dims))
        smooth = NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeSmooth
        for t, v in ((1, 1.0), (11, 1.5), (21, 2.0)):
            for d in range(n_dims):
                gain.setValueAtTime(v, t, d)
                gain.setInterpolationAtTime(t, smooth, d)
        yield ("sleep", SETTLE_MS)

        def gain_children():
            """Labels of the visible curve-editor rows under the knob's row, or None."""
            for tree in editors[0].findChildren(QTreeWidget):
                for i in range(tree.topLevelItemCount()):
                    node_item = tree.topLevelItem(i)
                    if node_item.text(0) != fg.getLabel():
                        continue
                    for j in range(node_item.childCount()):
                        knob_item = node_item.child(j)
                        if knob_item.text(0).strip().lower() == gain_label:
                            return [knob_item.child(k).text(0).strip().lower()
                                    for k in range(knob_item.childCount()) if not knob_item.child(k).isHidden()]
            return None

        def dimension_switch():
            for w in QApplication.allWidgets():
                try:
                    if not (isinstance(w, QLabel) and w.isVisible() and ancestor_of_class(w, "NodeSettingsPanel") is not None
                            and strip_tags(w.text()).rstrip(":").strip().lower() == gain_label):
                        continue
                    y = w.mapToGlobal(w.rect().center()).y()
                    for b in QApplication.allWidgets():
                        if isinstance(b, QPushButton) and b.isVisible() and "Switch between a single value" in b.toolTip():
                            top = b.mapToGlobal(QPoint(0, 0)).y()
                            if top <= y <= top + b.height():
                                return b
                except RuntimeError:
                    continue
            return None

        editors = gt.widgets_of("CurveEditor")
        switch = dimension_switch()
        if check(editors and switch is not None, "found the curve editor and the knob's dimension switch"):
            check(not switch.isChecked(), "the knob starts folded")
            rows = gain_children()
            check(rows == [gain_label], "folded: the curve editor shows one curve under the knob, named like it (got %r)" % (rows,))
            switch.click()
            yield ("sleep", SETTLE_MS)
            rows = gain_children()
            check(rows is not None and len(rows) == n_dims, "expanded: one curve per component (got %r)" % (rows,))
            switch.click()
            yield ("sleep", SETTLE_MS)
            rows = gain_children()
            check(rows == [gain_label], "folded again: one curve named like the knob (got %r)" % (rows,))
            for d in range(n_dims):
                gain.setValueAtTime(2.5, 31, d)
                gain.setInterpolationAtTime(31, smooth, d)
            yield ("sleep", SETTLE_MS)
            rows = gain_children()
            check(rows == [gain_label], "a key set while folded keeps it at one curve (got %r)" % (rows,))
            check(not switch.isChecked(), "the knob is still folded after the key")

            check(make_tab_current(editors[0]), "brought the curve editor's tab to the front")
            yield ("sleep", SETTLE_MS)
            for tree in editors[0].findChildren(QTreeWidget):
                for i in range(tree.topLevelItemCount()):
                    if tree.topLevelItem(i).text(0) == fg.getLabel():
                        tree.topLevelItem(i).setSelected(True)
            QApplication.processEvents()
            visible_cws = [w for w in gt.widgets_of("CurveWidget") if w.isVisible()]
            fcw = visible_cws[0] if visible_cws else None
        else:
            fcw = None
        if check(fcw is not None, "the curve widget is visible"):
            send_key(fcw, Qt.Key_A)
            yield ("sleep", SETTLE_MS)
            FW, FH = float(fcw.width()), float(fcw.height())
            lo, hi = 1.0, 2.5
            fy0, fy1 = lo - 0.1 * (hi - lo), hi + 0.1 * (hi - lo)
            fsy = FH / (fy1 - fy0)
            fviewer = app.getViewer(viewer.getScriptName())
            fviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            fxa = time_cursor_x(fcw)
            fviewer.seek(31)
            yield ("sleep", SETTLE_MS)
            fxb = time_cursor_x(fcw)
            fviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            gt.require(fxa is not None and fxb is not None and fxb > fxa, "found the curve widget's time line at frames 1 and 31")
            fsx = (fxb - fxa) / 30.0

            def to_f(t, v):
                return QPointF(fxa + (t - 1) * fsx, FH - (v - fy0) * fsy)

            samples = (1, 6, 11, 16, 21, 26, 31)

            def profile(d):
                e = 0.01
                out = []
                for t in samples:
                    v = gain.getValueAtTime(t, d)
                    out.append((v, (v - gain.getValueAtTime(t - e, d)) / e, (gain.getValueAtTime(t + e, d) - v) / e))
                return out

            def agree(a, b):
                return all(abs(x - y) <= 1e-4 for ta, tb in zip(a, b) for x, y in zip(ta, tb))

            def components_follow_first(label):
                base = profile(0)
                same_as_first = all(agree(base, profile(d)) for d in range(1, n_dims))
                check(same_as_first, "%s: every component has the first component's values and slopes at every key" % label)
                check(gain_children() == [gain_label] and not switch.isChecked(),
                      "%s: the knob is still folded, with one curve row" % label)

            start = [profile(d) for d in range(n_dims)]
            components_follow_first("before editing")
            key_start = to_f(11, 1.5)
            # Where the curve crosses the key's column on screen: the value axis need not follow
            # frameAll()'s padding once the folded curve is framed, and the curve's colour is the
            # knob's own, so it is found as the only saturated colour in the column.
            fimg = widget_image(fcw)
            kx = int(round(key_start.x()))
            curve_ys = sorted(y for y in range(fimg.height()) for x in (kx - 1, kx, kx + 1)
                              if 0 <= x < fimg.width()
                              and max(fimg.pixelColor(x, y).red(), fimg.pixelColor(x, y).green(), fimg.pixelColor(x, y).blue())
                              - min(fimg.pixelColor(x, y).red(), fimg.pixelColor(x, y).green(), fimg.pixelColor(x, y).blue()) > 150)
            report("NOTE folded-curve: key column %d, value-axis guess y=%.1f, curve pixels at %r"
                   % (kx, key_start.y(), curve_ys[:12]))
            if curve_ys:
                key_start = QPointF(key_start.x(), curve_ys[len(curve_ys) // 2])
            drag(fcw, key_start, key_start + QPointF(0, -40))
            yield ("sleep", SETTLE_MS)
            moved = gain.getValueAtTime(11, 0)
            check(abs(moved - 1.5) > 1e-3, "dragging the key on the single curve moved it (value at frame 11 now %.4f)" % moved)
            components_follow_first("after the drag")
            widget_shot(fcw, "folded-01-after-drag")
            send_key(fcw, Qt.Key_X)
            yield ("sleep", SETTLE_MS)
            components_follow_first("after Break")
            send_key(fcw, Qt.Key_X, Qt.ShiftModifier)
            yield ("sleep", SETTLE_MS)
            components_follow_first("after Join")
            for _ in range(3):
                trigger("undo")
                yield ("sleep", 200)
            check(all(agree(start[d], profile(d)) for d in range(n_dims)),
                  "three undos return every component to its starting curve")
            components_follow_first("after the undos")
            report("NOTE folded-curve: the undo index and the keys' interpolation types have no Python hook; "
                   "three undos restoring the start state stands for the drag being one step, and equal slopes for equal interpolation")
        graphs = gt.widgets_of("NodeGraph")
        if graphs:
            make_tab_current(graphs[0])
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        destroy_nodes([fg])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("expanded-curve")
        gt.close_all_panels()
        # The keys go in before any panel exists: keying X alone first makes X and Y differ for a
        # moment, which an open panel answers by expanding the knob.
        eb = app.createNode("net.sf.cimg.CImgBlur", -1, None,
                            {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)})
        gt.require(eb is not None, "create net.sf.cimg.CImgBlur", gt.plugin_problem("net.sf.cimg.CImgBlur"))
        eb.setPosition(11600, 0)
        size = eb.getParam("size")
        gt.require(size is not None and size.getNumDimensions() == 2, "the Blur has a two-component size knob")
        size_label = size.getLabel().strip().lower()
        for t, v in ((1, 2.0), (21, 6.0)):
            for d in range(2):
                size.setValueAtTime(v, t, d)
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        app.clearSelection()
        app.selectNode(eb, True)
        QMetaObject.invokeMethod(root_graph(), "showSelectedNodeSettingsPanel")
        yield ("until", lambda: gt.widgets_of("CurveEditor") and any(
            isinstance(b, QPushButton) and b.isVisible() and "Switch between a single value" in b.toolTip()
            for b in QApplication.allWidgets()), 10.0)
        yield ("sleep", SETTLE_MS)

        # Rows are read through the tree's model: QTreeWidgetItem is no QObject, so PySide can
        # hand back the wrapper of an item deleted with an earlier node when a new item reuses
        # its address.
        def size_knob_index():
            for tree in gt.widgets_of("CurveEditor")[0].findChildren(QTreeWidget):
                model = tree.model()
                for i in range(model.rowCount()):
                    node_index = model.index(i, 0)
                    if node_index.data() != eb.getLabel():
                        continue
                    for j in range(model.rowCount(node_index)):
                        knob_index = model.index(j, 0, node_index)
                        if str(knob_index.data()).strip().lower() == size_label:
                            return tree, knob_index
            return None, None

        def size_rows():
            """Labels of the visible curve-editor rows under the size knob's row, or None."""
            tree, knob_index = size_knob_index()
            if knob_index is None:
                return None
            model = tree.model()
            return [str(model.index(k, 0, knob_index).data()).strip().lower()
                    for k in range(model.rowCount(knob_index)) if not tree.isRowHidden(k, knob_index)]

        def size_switch():
            for w in QApplication.allWidgets():
                try:
                    if not (isinstance(w, QLabel) and w.isVisible() and ancestor_of_class(w, "NodeSettingsPanel") is not None
                            and strip_tags(w.text()).rstrip(":").strip().lower() == size_label):
                        continue
                    y = w.mapToGlobal(w.rect().center()).y()
                    for b in QApplication.allWidgets():
                        if isinstance(b, QPushButton) and b.isVisible() and "Switch between a single value" in b.toolTip():
                            top = b.mapToGlobal(QPoint(0, 0)).y()
                            if top <= y <= top + b.height():
                                return b
                except RuntimeError:
                    continue
            return None

        def size_at(t):
            return (size.getValueAtTime(t, 0), size.getValueAtTime(t, 1))

        editors = gt.widgets_of("CurveEditor")
        eswitch = size_switch()
        ecw = None
        if check(editors and eswitch is not None, "found the curve editor and the size knob's dimension switch"):
            check(not eswitch.isChecked(), "size starts folded")
            eswitch.click()
            yield ("sleep", SETTLE_MS)
            check(eswitch.isChecked(), "the switch expands size")
            rows = size_rows()
            check(rows is not None and len(rows) == 2, "expanded: one curve per component (got %r)" % (rows,))
            report("NOTE expanded-curve: undo action after the expand reads %r" % (undo_text(),))

            check(make_tab_current(editors[0]), "brought the curve editor's tab to the front")
            yield ("sleep", SETTLE_MS)
            tree, knob_index = size_knob_index()
            if check(knob_index is not None and tree.model().rowCount(knob_index) == 2, "found the size knob's two component rows"):
                # Only X is selected, so the curve widget shows X's curve alone and the drag cannot
                # land on Y's key, which sits on the same spot.
                tree.clearSelection()
                tree.selectionModel().select(tree.model().index(0, 0, knob_index),
                                             QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
                QApplication.processEvents()
                visible_cws = [w for w in gt.widgets_of("CurveWidget") if w.isVisible()]
                ecw = visible_cws[0] if visible_cws else None
        if check(ecw is not None, "the curve widget is visible"):
            send_key(ecw, Qt.Key_A)
            yield ("sleep", SETTLE_MS)
            EW, EH = float(ecw.width()), float(ecw.height())
            ey0, ey1 = 2.0 - 0.4, 6.0 + 0.4
            esy = EH / (ey1 - ey0)
            eviewer = app.getViewer(viewer.getScriptName())
            eviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            exa = time_cursor_x(ecw)
            # The drag happens with the time line where both components keep the same value, so a
            # refresh there would find nothing to keep the knob expanded for but the user's click.
            eviewer.seek(21)
            yield ("sleep", SETTLE_MS)
            exb = time_cursor_x(ecw)
            gt.require(exa is not None and exb is not None and exb > exa, "found the curve widget's time line at frames 1 and 21")
            esx = (exb - exa) / 20.0
            key_pt = QPointF(exa, EH - (2.0 - ey0) * esy)
            img = widget_image(ecw)
            kx = int(round(key_pt.x()))
            # The key sits where X's curve crosses frame 1's column; the value axis need not
            # follow frameAll()'s padding, so the row is found on screen as the only saturated
            # colour left of the key, where the curve is flat and the frame 1 time line is not in
            # the way (the curve's colour is the knob's own).
            marked = sorted(y for y in range(img.height()) for x in (kx - 8, kx - 7, kx - 6)
                            if 0 <= x < img.width()
                            and max(img.pixelColor(x, y).red(), img.pixelColor(x, y).green(), img.pixelColor(x, y).blue())
                            - min(img.pixelColor(x, y).red(), img.pixelColor(x, y).green(), img.pixelColor(x, y).blue()) > 150)
            report("NOTE expanded-curve: %dx%d, frames 1/21 at x=%r/%r, key guess y=%.1f, curve rows in its column %r"
                   % (EW, EH, exa, exb, key_pt.y(), marked[:12]))
            if marked:
                key_pt = QPointF(key_pt.x(), marked[len(marked) // 2])
            before = size_at(1)
            drag(ecw, key_pt, key_pt + QPointF(0, -40))
            yield ("sleep", SETTLE_MS)
            after = size_at(1)
            report("NOTE expanded-curve: size at frame 1 before %r, after the drag %r; at frame 21 %r" % (before, after, size_at(21)))
            check(abs(after[0] - before[0]) > 1e-3, "dragging X's key changed X at frame 1")
            check(abs(after[1] - before[1]) <= 1e-6, "dragging X's key left Y at frame 1 alone")
            check(eswitch.isChecked(), "size is still expanded after the drag")
            rows = size_rows()
            check(rows is not None and len(rows) == 2, "Y's curve is still listed in the curve editor (got %r)" % (rows,))
            widget_shot(ecw, "expanded-01-after-drag")

            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(abs(size_at(1)[0] - before[0]) <= 1e-6, "undo restores X at frame 1 (now %r)" % (size_at(1),))
            check(eswitch.isChecked(), "size is still expanded after the undo, with both components equal again")
            rows = size_rows()
            check(rows is not None and len(rows) == 2, "both curves are still listed after the undo (got %r)" % (rows,))

            # Equal values and equal keys in every component are what a new panel folds a knob for.
            for d in range(2):
                size.setValueAtTime(4.0, 11, d)
            yield ("sleep", SETTLE_MS)
            for t in (11, 1, 21):
                eviewer.seek(t)
                yield ("sleep", SETTLE_MS)
            check(all(abs(v - 4.0) <= 1e-6 for v in size_at(11)),
                  "a script gave X and Y the same key at frame 11 (got %r)" % (size_at(11),))
            check(eswitch.isChecked(), "size is still expanded after a script made X and Y equal and the time line moved")

            graphs = gt.widgets_of("NodeGraph")
            if graphs:
                make_tab_current(graphs[0])
            yield ("sleep", SETTLE_MS)
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
            check(size_switch() is None, "closed the Blur's properties panel")
            egraph = root_graph()
            gt.require(egraph is not None, "the node graph is on screen")
            app.clearSelection()
            app.selectNode(eb, True)
            QMetaObject.invokeMethod(egraph, "showSelectedNodeSettingsPanel")
            reopened = yield ("until", lambda: size_switch() is not None, 10.0)
            gt.require(reopened, "reopened the Blur's properties panel")
            yield ("sleep", SETTLE_MS)
            eswitch = size_switch()
            check(eswitch.isChecked(), "size is still expanded after its properties panel closes and reopens")
            rows = size_rows()
            check(rows is not None and len(rows) == 2, "after reopening: one curve per component (got %r)" % (rows,))

            eswitch.click()
            yield ("sleep", SETTLE_MS)
            check(not eswitch.isChecked(), "the switch folds size")
            check(size_rows() == [size_label], "folded: one curve named like the knob (got %r)" % (size_rows(),))
            undo_label = undo_text()[0] or ""
            check("fold" in undo_label.lower(), "the fold is on the undo stack (undo action reads %r)" % (undo_label,))
            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(eswitch.isChecked(), "undoing the fold expands size again")
            rows = size_rows()
            check(rows is not None and len(rows) == 2, "after undoing the fold: one curve per component (got %r)" % (rows,))
            trigger("redo")
            yield ("sleep", SETTLE_MS)
            check(not eswitch.isChecked(), "redoing the fold folds size again")
            check(size_rows() == [size_label], "after redoing the fold: one curve (got %r)" % (size_rows(),))
            check(all(abs(v - 2.0) <= 1e-6 for v in size_at(1)) and all(abs(v - 6.0) <= 1e-6 for v in size_at(21)),
                  "folding, undoing and redoing left the keys as they were (got %r, %r)" % (size_at(1), size_at(21)))
        graphs = gt.widgets_of("NodeGraph")
        if graphs:
            make_tab_current(graphs[0])
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        destroy_nodes([eb])
        # The roto sections key their shapes at frame 1 and drag at the current frame.
        app.getViewer(viewer.getScriptName()).seek(1)
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("picker-in-out")
        c25 = create("net.sf.openfx.ConstantPlugin", 8000, -300)
        set_rgba(c25.getParam("color"), (0.25, 0.25, 0.25, 1.0))
        g2 = create("net.sf.openfx.GradePlugin", 8000, 0)
        set_all(g2.getParam("multiply"), 2.0)
        connect(g2, 0, c25)
        gt.close_all_panels()
        view(g2)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        gt.require(gl is not None, "the viewer is visible")
        vmap = ViewerMap(gl)
        check(vmap.ok, "calibrated the viewer from the info bar's mouse coordinates")
        mid = QPointF(gl.width() / 2.0, gl.height() / 2.0)
        move(gl, mid)
        yield ("sleep", RENDER_MS)
        move(gl, mid + QPointF(1, 0))
        QApplication.processEvents()
        rgba = info_rgba()
        report("NOTE hover: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out", "hover reads the Grade's output, 0.5, marked out")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out", "Ctrl-click reads the output, 0.5, marked out")
        widget_shot(info_bar(), "picker-01-ctrl-click-out")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Alt-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "Ctrl+Alt-click reads the input, 0.25, marked in")
        widget_shot(info_bar(), "picker-02-ctrl-alt-click-in")
        drag(gl, mid - QPointF(20, 20), mid + QPointF(20, 20), Qt.LeftButton, Qt.ControlModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Shift rect: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out", "the Ctrl+Shift rectangle reads the output, 0.5, marked out")
        drag(gl, mid - QPointF(20, 20), mid + QPointF(20, 20), Qt.LeftButton,
             Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Shift+Alt rect: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in",
              "the Ctrl+Shift+Alt rectangle reads the input, 0.25, marked in")
        gt.shot("picker-03-rect-alt-in.png")

        gt.close_all_panels()
        eg = create("net.sf.openfx.GradePlugin", 8000, 300)
        set_all(eg.getParam("multiply"), 2.0)
        connect(eg, 0, c25)
        yield ("sleep", SETTLE_MS)
        view(eg)
        yield ("sleep", RENDER_MS)
        eye_labels = [w for w in gt.widgets_of("ColorPickerLabel") if w.isVisible()]
        if check(eye_labels, "the Grade's panel shows a colour picker"):
            eye_names = ("blackPoint", "whitePoint", "black", "white", "multiply", "offset", "gamma")
            eye_before = {}
            for n in eye_names:
                p = eg.getParam(n)
                if p is not None:
                    eye_before[n] = [p.getValue(d) for d in range(p.getNumDimensions())]
            eye_label = eye_labels[0]
            eye_centre = QPointF(eye_label.width() / 2.0, eye_label.height() / 2.0)
            click(eye_label, eye_centre)
            yield ("sleep", SETTLE_MS)
            move(gl, mid)
            move(gl, mid + QPointF(1, 0))
            QApplication.processEvents()
            report("NOTE eyedropper hover: %r %r" % (info_rgba(), info_source()))
            check(rgb_is(info_rgba(), (0.25, 0.25, 0.25)) and info_source() == "in",
                  "with the eyedropper on, hover reads the input, 0.25, marked in")
            click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            rgba = info_rgba()
            report("NOTE eyedropper Ctrl-click: %r %r" % (rgba, info_source()))
            check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in",
                  "with the eyedropper on, Ctrl-click reads the input, 0.25, marked in")
            widget_shot(info_bar(), "picker-04-eyedropper-ctrl-click-in")
            filled = None
            for n in eye_names:
                p = eg.getParam(n)
                if p is None:
                    continue
                now = [p.getValue(d) for d in range(p.getNumDimensions())]
                if now != eye_before.get(n):
                    filled = (n, now)
            report("NOTE eyedropper Grade knob that changed: %r" % (filled,))
            check(filled is not None and all(close(filled[1][i], 0.25) for i in range(3)),
                  "the Ctrl-click filled the colour knob with the input, 0.25")
            if filled is not None:
                restore = eg.getParam(filled[0])
                for d, v in enumerate(eye_before[filled[0]]):
                    restore.setValue(v, d)
                yield ("sleep", RENDER_MS)
            click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
            yield ("sleep", SETTLE_MS)
            rgba = info_rgba()
            report("NOTE eyedropper Ctrl+Alt-click: %r %r" % (rgba, info_source()))
            check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out",
                  "with the eyedropper on, Ctrl+Alt-click reads the output, 0.5, marked out")
            widget_shot(info_bar(), "picker-05-eyedropper-ctrl-alt-click-out")
            view(reader)
            yield ("sleep", RENDER_MS)
            click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            report("NOTE eyedropper Read Ctrl-click: %r %r" % (info_rgba(), info_source()))
            check(info_source() == "out", "on a Read with the eyedropper on, the pick reads the output, marked out")
            widget_shot(info_bar(), "picker-06-eyedropper-read-out")
            click(eye_label, eye_centre)
            yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        view(g2)
        yield ("sleep", RENDER_MS)

        mb = create("net.sf.openfx.ConstantPlugin", 8400, -300)
        set_rgba(mb.getParam("color"), (0.25, 0.25, 0.25, 1.0))
        ma = create("net.sf.openfx.ConstantPlugin", 8700, -300)
        set_rgba(ma.getParam("color"), (0.75, 0.75, 0.75, 1.0))
        mm = create("Merge", 8550, 0)
        connect(mm, input_index(mm, "B"), mb)
        connect(mm, input_index(mm, "A"), ma)
        gt.close_all_panels()
        view(mm)
        yield ("sleep", RENDER_MS)
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Merge Ctrl-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.75, 0.75, 0.75)) and info_source() == "out", "on a Merge, Ctrl-click reads A over B (0.75), marked out")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "on a Merge, Ctrl+Alt-click reads B (0.25), marked in")

        view(reader)
        yield ("sleep", RENDER_MS)
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        report("NOTE Read Ctrl-click: %r %r" % (info_rgba(), info_source()))
        check(info_source() == "out", "on a Read, Ctrl-click falls back to the output, marked out")
        widget_shot(info_bar(), "picker-07-read-out")

        # =====================================================================================
        gt.section("picker-scene-referred")
        settings = NatronEngine.natron.getSettings()
        check(settings.getParam("linearPickers") is None, "the 'Linear color pickers' preference is gone")
        cs = create("net.sf.openfx.ConstantPlugin", 9200, -300)
        want = (0.18, 2.0, 0.5, 1.0)
        set_rgba(cs.getParam("color"), want)
        gt.close_all_panels()
        pick_grade = create("net.sf.openfx.GradePlugin", 9500, -300)
        yield ("sleep", SETTLE_MS)
        view(cs)
        yield ("sleep", RENDER_MS)
        sample = mid + QPointF(-60, 40)

        def screen_colour():
            QApplication.processEvents()
            g = gl.mapToGlobal(sample.toPoint())
            c = QGuiApplication.primaryScreen().grabWindow(0).toImage().pixelColor(g.x(), g.y())
            return (c.red(), c.green(), c.blue())

        def viewer_box(tip_text):
            tab = viewer_tab()
            if tab is None:
                return None
            for w in QApplication.allWidgets():
                try:
                    if gt.class_name(w) == "SpinBox" and tip_text in w.toolTip() and ancestor_of_class(w, "ViewerTab") is tab:
                        return w
                except RuntimeError:
                    continue
            return None

        def set_gain_gamma(gain, gamma):
            gain_box, gamma_box = viewer_box("Gain:"), viewer_box("gamma correction level")
            if check(gain_box is not None and gamma_box is not None, "found the viewer's gain and gamma boxes"):
                gain_box.setText(str(gain))
                send_key(gain_box, Qt.Key_Return)
                gamma_box.setText(str(gamma))
                send_key(gamma_box, Qt.Key_Return)

        before = screen_colour()
        set_gain_gamma(2.0, 2.2)
        yield ("sleep", RENDER_MS)
        after = screen_colour()
        report("NOTE viewer centre on screen before %r, after gain/gamma %r" % (before, after))
        check(before != after, "viewer gain and gamma changed what is on screen")
        labels = [w for w in gt.widgets_of("ColorPickerLabel") if w.isVisible()]
        picked_knob = None
        names = ("blackPoint", "whitePoint", "black", "white", "multiply", "offset", "gamma")
        if check(labels, "the picking Grade's panel shows a colour picker"):
            values_before = {}
            for n in names:
                p = pick_grade.getParam(n)
                if p is not None:
                    values_before[n] = [p.getValue(d) for d in range(p.getNumDimensions())]
            click(labels[0], QPointF(labels[0].width() / 2.0, labels[0].height() / 2.0))
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE scene-referred Ctrl+Alt-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, want) and len(rgba) >= 4 and close(rgba[3], 1.0),
              "through gain and gamma, Ctrl+Alt-click reads exactly (0.18, 2.0, 0.5, 1)")
        gt.shot("picker-05-scene-referred.png")
        if labels:
            for n in names:
                p = pick_grade.getParam(n)
                if p is None:
                    continue
                now = [p.getValue(d) for d in range(p.getNumDimensions())]
                if now != values_before.get(n):
                    picked_knob = (n, now)
            report("NOTE picking Grade knob that changed: %r" % (picked_knob,))
            check(picked_knob is not None and all(close(picked_knob[1][i], want[i]) for i in range(3)),
                  "the picking Grade's colour knob received (0.18, 2.0, 0.5)")
            click(labels[0], QPointF(labels[0].width() / 2.0, labels[0].height() / 2.0))
        drag(gl, mid - QPointF(20, 20), mid + QPointF(20, 20), Qt.LeftButton, Qt.ControlModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE scene-referred rect: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, want), "the rectangle pick returns the same scene-referred values")
        set_gain_gamma(0.0, 1.0)
        gt.close_all_panels()

        # =====================================================================================
        gt.section("roto-multi-feather")
        roto = create("fr.inria.built-in.Roto", 10000, 0)
        ctx = roto.getRotoContext()
        gt.require(ctx is not None, "the Roto node has a roto context")
        t = 1.0
        view(roto)
        yield ("sleep", RENDER_MS)
        vmap = ViewerMap(gl)
        gt.require(vmap.ok, "calibrated the viewer for the roto drag")
        # The Roto viewer toolbar starts on the Bezier drawing tool; Q switches to selection.
        send_key(gl, Qt.Key_Q)
        # The first plain click after a colour pick only turns the picker off.
        click(gl, QPointF(5, 5))
        cx, cy = vmap.to_canonical(QPointF(gl.width() / 2.0, gl.height() / 2.0))
        r = 90.0 * vmap.pixel_scale()
        angles = [math.radians(90 - 72 * i) for i in range(5)]
        pts = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a in angles]
        bez = ctx.createBezier(pts[0][0], pts[0][1], t)
        gt.require(bez is not None, "created a Bezier")
        for p in pts[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        tl = 0.25 * r
        for i, a in enumerate(angles):
            tx, ty = math.sin(a), -math.cos(a)
            x, y = pts[i]
            bez.setPointAtIndex(i, t, x, y, x - tx * tl, y - ty * tl, x + tx * tl, y + ty * tl)
            bez.setFeatherPointAtIndex(i, t, x, y, x - tx * tl, y - ty * tl, x + tx * tl, y + ty * tl)
        yield ("sleep", RENDER_MS)
        # Shift is read from key events by the roto interact, not from the mouse event.
        click(gl, vmap.to_widget(pts[0][0], pts[0][1]))
        QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyPress, Qt.Key_Shift, Qt.ShiftModifier))
        for i in (1, 2):
            click(gl, vmap.to_widget(pts[i][0], pts[i][1]), Qt.LeftButton, Qt.ShiftModifier)
        QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyRelease, Qt.Key_Shift, Qt.NoModifier))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        gt.shot("roto-01-three-selected.png")

        def feather_offsets():
            out = []
            for i in range(5):
                c = bez.getControlPointPosition(i, t)
                f = bez.getFeatherPointPosition(i, t)
                out.append(((f[0] - c[0]), (f[1] - c[1])))
            return out

        before_offsets = feather_offsets()
        bar_start = vmap.to_widget(pts[0][0], pts[0][1] + 14 * vmap.pixel_scale())
        drag_px = 20.0
        drag(gl, bar_start, bar_start + QPointF(0, -drag_px))
        yield ("sleep", SETTLE_MS)
        after_offsets = feather_offsets()
        report("NOTE feather offsets before %r after %r" % (before_offsets, after_offsets))
        s = drag_px * vmap.pixel_scale()
        dist = []
        for i, a in enumerate(angles):
            ox, oy = after_offsets[i]
            dist.append(ox * math.cos(a) + oy * math.sin(a))
        report("NOTE feather distances along each normal: %r (drag %.2f)" % (dist, s))
        check(all(abs(dist[i] - dist[0]) <= 0.05 * abs(dist[0]) + 0.5 for i in (1, 2)) and dist[0] > 0.5 * s,
              "the three selected feathers moved outward by the same distance")
        check(all(abs(after_offsets[i][0]) < 1e-6 and abs(after_offsets[i][1]) < 1e-6 for i in (3, 4)),
              "the two unselected feathers did not move")
        gt.shot("roto-02-after-feather-drag.png")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        restored = feather_offsets()
        check(all(abs(o[0]) < 1e-6 and abs(o[1]) < 1e-6 for o in restored), "one undo restores all three feathers")
        gt.shot("roto-03-after-undo.png")
        gt.close_all_panels()

        # =====================================================================================
        gt.section("roto-broken-tangent")
        t = 1.0
        roto2 = create("fr.inria.built-in.Roto", 10600, 0)
        ctx2 = roto2.getRotoContext()
        gt.require(ctx2 is not None, "the second Roto node has a roto context")
        gl = viewer_gl()
        gt.require(gl is not None, "the viewer is visible")
        view(roto2)
        yield ("sleep", RENDER_MS)
        vmap = ViewerMap(gl)
        gt.require(vmap.ok, "calibrated the viewer for the broken-tangent drag")
        send_key(gl, Qt.Key_Q)
        click(gl, QPointF(5, 5))
        cx, cy = vmap.to_canonical(QPointF(gl.width() / 2.0, gl.height() / 2.0))
        r = 90.0 * vmap.pixel_scale()
        angles = [math.radians(90 - 72 * i) for i in range(5)]
        pts = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a in angles]
        bez = ctx2.createBezier(pts[0][0], pts[0][1], t)
        gt.require(bez is not None, "created a Bezier")
        for p in pts[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        tl = 0.25 * r
        for i, a in enumerate(angles):
            tx, ty = math.sin(a), -math.cos(a)
            x, y = pts[i]
            bez.setPointAtIndex(i, t, x, y, x - tx * tl, y - ty * tl, x + tx * tl, y + ty * tl)
            bez.setFeatherPointAtIndex(i, t, x, y, x - tx * tl, y - ty * tl, x + tx * tl, y + ty * tl)
        yield ("sleep", RENDER_MS)
        click(gl, vmap.to_widget(pts[0][0], pts[0][1]))
        yield ("sleep", SETTLE_MS)

        def top_point():
            return bez.getControlPointPosition(0, t)

        def left_of(pt):
            return (pt[2], pt[3])

        def right_of(pt):
            return (pt[4], pt[5])

        def moved(a, b):
            return math.hypot(a[0] - b[0], a[1] - b[1]) > 1e-4

        def same_point(a, b):
            return all(abs(a[i] - b[i]) < 1e-6 for i in range(6))

        def drag_right_tangent(pt, dy_px, modifiers):
            handle = vmap.to_widget(pt[4], pt[5])
            if modifiers & Qt.ControlModifier:
                QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
            drag(gl, handle, handle + QPointF(0, dy_px), Qt.LeftButton, modifiers)
            if modifiers & Qt.ControlModifier:
                QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
            QApplication.processEvents()

        def undo_until(want, limit):
            for _ in range(limit):
                if same_point(top_point(), want):
                    return True
                trigger("undo")
                QApplication.processEvents()
            return same_point(top_point(), want)

        start = top_point()
        drag_right_tangent(start, 30, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        after_ctrl = top_point()
        check(moved(right_of(after_ctrl), right_of(start)), "Ctrl-drag of the right tangent moved it")
        check(not moved(left_of(after_ctrl), left_of(start)), "Ctrl-drag of the right tangent left the left tangent alone")
        gt.shot("roto-04-broken.png")

        drag_right_tangent(after_ctrl, -25, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        after_plain = top_point()
        check(moved(right_of(after_plain), right_of(after_ctrl)), "a plain drag of the broken right tangent moved it")
        check(not moved(left_of(after_plain), left_of(start)),
              "a plain drag of a broken tangent leaves the left tangent alone")
        gt.shot("roto-04b-broken-after-plain-drag.png")

        smooth_button = roto2.getParam("SmoothItemsAction")
        if check(smooth_button is not None, "the Roto node has its Smooth action"):
            smooth_button.trigger()
            yield ("sleep", SETTLE_MS)
        after_smooth = top_point()
        check(moved(left_of(after_smooth), left_of(after_plain)) or moved(right_of(after_smooth), right_of(after_plain)),
              "Smooth changed the broken point's tangents")

        drag_right_tangent(after_smooth, 25, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        after_joined = top_point()
        check(moved(right_of(after_joined), right_of(after_smooth)), "a plain drag after Smooth moved the right tangent")
        check(moved(left_of(after_joined), left_of(after_smooth)), "a plain drag after Smooth moved the left tangent too")
        gt.shot("roto-05-smoothed-drag.png")

        check(undo_until(after_smooth, 3), "one undo reverts the plain drag after Smooth")
        check(undo_until(after_plain, 3), "undoing the Smooth restores the broken point's tangents")
        yield ("sleep", SETTLE_MS)
        drag_right_tangent(after_plain, 25, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        after_redrag = top_point()
        check(moved(right_of(after_redrag), right_of(after_plain)), "a plain drag after undoing the Smooth moved the right tangent")
        check(not moved(left_of(after_redrag), left_of(after_plain)),
              "after undoing the Smooth the point is broken again: a plain drag leaves the left tangent alone")

        check(undo_until(start, 8), "undoing the drags and the Ctrl-drag restores both tangents to where they started")
        gt.shot("roto-06-after-undo.png")
        gt.close_all_panels()
        destroy_nodes([roto2])
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        # Run last: resetProject() wipes every node made above.
        gt.section("fold-state-saved")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        def fold_switch(label):
            """The dimension switch on the row of the open properties panel's knob labelled `label`."""
            for w in QApplication.allWidgets():
                try:
                    if not (isinstance(w, QLabel) and w.isVisible() and ancestor_of_class(w, "NodeSettingsPanel") is not None
                            and strip_tags(w.text()).rstrip(":").strip().lower() == label):
                        continue
                    y = w.mapToGlobal(w.rect().center()).y()
                    for b in QApplication.allWidgets():
                        if isinstance(b, QPushButton) and b.isVisible() and "Switch between a single value" in b.toolTip():
                            top = b.mapToGlobal(QPoint(0, 0)).y()
                            if top <= y <= top + b.height():
                                return b
                except RuntimeError:
                    continue
            return None

        # loadProject() hands back the engine App, which has no selection calls; the window's
        # GuiApp is still `app`.
        def open_panel(an_app, node, label):
            graph = root_graph()
            gt.require(graph is not None, "the node graph is on screen")
            an_app.clearSelection()
            an_app.selectNode(node, True)
            QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
            opened = yield ("until", lambda: fold_switch(label) is not None, 10.0)
            yield ("sleep", SETTLE_MS)
            return opened

        fb = create("net.sf.cimg.CImgBlur", 12400, 0)
        fsize = fb.getParam("size")
        gt.require(fsize is not None and fsize.getNumDimensions() == 2, "the Blur has a two-component size knob")
        fsize_label = fsize.getLabel().strip().lower()
        for d in range(2):
            fsize.setValue(4.0, d)
        yield ("sleep", SETTLE_MS)
        fswitch = fold_switch(fsize_label)
        if check(fswitch is not None, "found the size knob's dimension switch"):
            if not fswitch.isChecked():
                fswitch.click()
                yield ("sleep", SETTLE_MS)
            check(fswitch.isChecked(), "size is expanded, with both components equal")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        fg = create("net.sf.openfx.GradePlugin", 12400, 300)
        fgain = fg.getParam("gain") if fg.getParam("gain") is not None else fg.getParam("multiply")
        gt.require(fgain is not None and fgain.getNumDimensions() > 1, "the Grade has a multi-component gain knob")
        fgain_label = fgain.getLabel().strip().lower()
        set_all(fgain, 1.5)
        yield ("sleep", SETTLE_MS)
        gswitch = fold_switch(fgain_label)
        if check(gswitch is not None, "found the gain knob's dimension switch"):
            if gswitch.isChecked():
                gswitch.click()
                yield ("sleep", SETTLE_MS)
            check(not gswitch.isChecked(), "gain is folded")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        fb_name = fb.getScriptName()
        fg_name = fg.getScriptName()
        fold_path = os.path.join(gt.OUT, "fold-state.ntpz")
        if check(app.saveProjectAs(fold_path), "save the project to " + fold_path):
            if check(app.resetProject(), "reset the project (close, keep the window)"):
                check(app.getNode(fb_name) is None and app.getNode(fg_name) is None, "the reset removed the Blur and the Grade")
                reloaded = app.loadProject(fold_path)
                yield ("sleep", SETTLE_MS)
                if check(reloaded is not None, "reload " + fold_path):
                    rb = reloaded.getNode(fb_name)
                    rg = reloaded.getNode(fg_name)
                    if check(rb is not None and rg is not None, "found the reloaded Blur and Grade"):
                        gt.close_all_panels()
                        yield ("sleep", SETTLE_MS)
                        opened = yield from open_panel(app, rb, fsize_label)
                        if check(opened, "opened the reloaded Blur's properties panel"):
                            rswitch = fold_switch(fsize_label)
                            check(rswitch is not None and rswitch.isChecked(), "size is still expanded after the reload")
                            gt.shot("fold-01-reloaded-blur-size-expanded.png")
                            check(all(abs(rb.getParam("size").getValue(d) - 4.0) <= TOL for d in range(2)),
                                  "both size components came back as 4")
                        gt.close_all_panels()
                        yield ("sleep", SETTLE_MS)
                        opened = yield from open_panel(app, rg, fgain_label)
                        if check(opened, "opened the reloaded Grade's properties panel"):
                            rswitch = fold_switch(fgain_label)
                            check(rswitch is not None and not rswitch.isChecked(), "gain is still folded after the reload")
                            gt.shot("fold-02-reloaded-grade-gain-folded.png")
                            rgain = rg.getParam("gain") if rg.getParam("gain") is not None else rg.getParam("multiply")
                            check(all(abs(rgain.getValue(d) - 1.5) <= TOL for d in range(rgain.getNumDimensions())),
                                  "every gain component came back as 1.5")
                        gt.close_all_panels()
                        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        gt.section("header-button-sizes")
        gt.close_all_panels()
        sized = create("net.sf.openfx.GradePlugin", 0, -2000)
        yield ("sleep", SETTLE_MS)

        def named_buttons(name):
            found = []
            for w in QApplication.allWidgets():
                try:
                    if w.objectName() == name and w.isVisible():
                        found.append(w)
                except RuntimeError:
                    continue
            return found

        panel_floats = named_buttons("DockablePanelFloatButton")
        if check(len(panel_floats) > 0, "the Grade's panel header has a float button"):
            ref = panel_floats[0]
            ref_size = (ref.width(), ref.height())
            ref_icon = (ref.iconSize().width(), ref.iconSize().height())
            report("NOTE panel header button: size=%r icon=%r" % (ref_size, ref_icon))
            for name in ("TabWidgetFloatButton", "TabWidgetCloseButton",
                         "PropertiesBinClearAllButton", "PropertiesBinMinimizeAllButton"):
                buttons = named_buttons(name)
                if check(len(buttons) > 0, "%s is on screen" % name):
                    b = buttons[0]
                    check((b.width(), b.height()) == ref_size,
                          "%s is the same size as a panel header button (%dx%d vs %dx%d)"
                          % (name, b.width(), b.height(), ref_size[0], ref_size[1]))
                    check((b.iconSize().width(), b.iconSize().height()) == ref_icon,
                          "%s has the same icon size as a panel header button" % name)
        gt.shot("header-button-sizes.png")
        sized.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
