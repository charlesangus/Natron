"""Interaction checks and screenshots for the minor-cleanup changes: no crash when a node dies with
its panel's undo stack active, easier-to-grab input stubs, pick-up connection mode, Y/Shift+Y
connection shortcuts, paste-onto-a-node, the pane menu from the empty tab header, scene-referred
viewer picking with input/output modifiers, the multi-point roto feather drag, and curve-editor
tangent break and join.

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
WATCHDOG_S = 290
SETTLE_MS = 400
RENDER_MS = 1500
DRAG_STEPS = 4
DISCONNECTED_ARROW_LENGTH = 30.0
TOL = 1e-3

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QKeyEvent, QMouseEvent, QWheelEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QMenu, QTreeWidget, QWidget

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
        merge = create("net.sf.openfx.MergePlugin", 0, 0)
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
        cancel_run("empty click", lambda p: click(vp, p))
        yield ("sleep", 100)

        app.clearSelection()
        frame(graph, [pb, pc, pk, pb2], max_zoom=1.0)
        x2, y2, w2, h2 = box(pb2)
        on_pipe = (x2 + w2 / 2.0, y2 - 20.0)
        pick_up(pb2, on_pipe)
        move(vp, empty_point())
        yield ("sleep", 100)
        graph_shot("pickup-05-connected-input-held")
        graph_key(Qt.Key_Escape)
        yield ("sleep", 100)
        check(same(input_of(pb2, 0), pk), "pick-up: a connected input picked up and cancelled keeps its source")
        graph_shot("pickup-06-connected-input-restored")

        # =====================================================================================
        gt.section("y-shift-y")
        yc = create("net.sf.openfx.ConstantPlugin", 3000, -300)
        yk = create("net.sf.openfx.CheckerBoardPlugin", 3300, -300)
        ym = create("net.sf.openfx.MergePlugin", 3150, 0)
        gt.close_all_panels()
        app.clearSelection()
        graph = root_graph()
        frame(graph, [yc, yk, ym], max_zoom=1.0)
        app.setSelection([yc, yk, ym])
        QApplication.processEvents()
        graph_key(Qt.Key_Y)
        yield ("sleep", SETTLE_MS)
        a_in, b_in = input_index(ym, "A"), input_index(ym, "B")
        feeds = set()
        for i in (a_in, b_in):
            if i is not None and ym.getInput(i) is not None:
                feeds.add(ym.getInput(i).getScriptName())
        check(feeds == {yc.getScriptName(), yk.getScriptName()},
              "Y: Merge's A and B are fed by the Constant and the CheckerBoard (got %r)" % sorted(feeds))
        graph_shot("y-01-merge-fed")
        trigger("undo")
        yield ("sleep", 200)
        check(all(ym.getInput(i) is None for i in range(ym.getMaxInputCount())), "Y: one undo removes both connections")

        sg = create("net.sf.openfx.GradePlugin", 3600, 0)
        sb = create("net.sf.cimg.CImgBlur", 3850, 0)
        sc = create("net.sf.openfx.ConstantPlugin", 3725, -300)
        gt.close_all_panels()
        frame(graph, [sg, sb, sc], max_zoom=1.0)
        app.setSelection([sg, sb, sc])
        QApplication.processEvents()
        graph_key(Qt.Key_Y, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(sg, sc) is not None, "Shift+Y: the Grade takes the Constant")
        check(input_connected_from(sb, sc) is not None, "Shift+Y: the Blur takes the Constant")
        graph_shot("y-02-shift-y")
        app.clearSelection()

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
        gt.section("pane-header-menu")
        graph = root_graph()
        pane = ancestor_of_class(graph, "TabWidget")
        header = None
        if pane is not None:
            headers = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabWidgetHeader"]
            header = headers[0] if headers else None
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
                    check(abs(seen["pos"].x() - glob.x()) <= 2 and abs(seen["pos"].y() - glob.y()) <= 2,
                          "the header menu opens with its top-left at the cursor")
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
            bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
            if check(bars, "found the pane's tab bar"):
                bar = bars[0]
                tab_pos = bar.tabRect(0).center()
                popups.arm()
                QApplication.sendEvent(bar, QContextMenuEvent(QContextMenuEvent.Mouse, tab_pos, bar.mapToGlobal(tab_pos)))
                QApplication.processEvents()
                popups.disarm()
                pane_menu = [s for s in popups.seen if corner_texts and s["texts"] == corner_texts]
                check(not pane_menu, "a right-click on a tab does not open the pane menu")
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

            def time_cursor_x():
                """The column of the curve widget's orange current-time line."""
                QApplication.processEvents()
                origin = cw.mapToGlobal(QPoint(0, 0))
                img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), cw.width(), cw.height()).toImage()
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

            pyviewer = app.getViewer(viewer.getScriptName())
            pyviewer.seek(1)
            yield ("sleep", SETTLE_MS)
            xa = time_cursor_x()
            pyviewer.seek(21)
            yield ("sleep", SETTLE_MS)
            xb = time_cursor_x()
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

            def right_handle(slope):
                key = to_w(11, 0.5)
                xmax = min(W / 8.0, (to_w(21, 0).x() - key.x()) / 3.0)
                ymax = min(H / 8.0, xmax)
                pix_slope = slope * sy / sx
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
        graphs = gt.widgets_of("NodeGraph")
        if graphs:
            make_tab_current(graphs[0])
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        cg.getParam("mix").removeAnimation(0)

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
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "hover reads the Grade's input, 0.25, marked in")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "Ctrl-click reads the input, 0.25, marked in")
        widget_shot(info_bar(), "picker-01-ctrl-click-in")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Alt-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out", "Ctrl+Alt-click reads the output, 0.5, marked out")
        widget_shot(info_bar(), "picker-02-ctrl-alt-click-out")
        drag(gl, mid - QPointF(20, 20), mid + QPointF(20, 20), Qt.LeftButton, Qt.ControlModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Shift rect: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "the Ctrl+Shift rectangle reads the input, 0.25, marked in")
        drag(gl, mid - QPointF(20, 20), mid + QPointF(20, 20), Qt.LeftButton,
             Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Ctrl+Shift+Alt rect: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.5, 0.5, 0.5)) and info_source() == "out",
              "the Ctrl+Shift+Alt rectangle reads the output, 0.5, marked out")
        gt.shot("picker-03-rect-out.png")

        mb = create("net.sf.openfx.ConstantPlugin", 8400, -300)
        set_rgba(mb.getParam("color"), (0.25, 0.25, 0.25, 1.0))
        ma = create("net.sf.openfx.ConstantPlugin", 8700, -300)
        set_rgba(ma.getParam("color"), (0.75, 0.75, 0.75, 1.0))
        mm = create("net.sf.openfx.MergePlugin", 8550, 0)
        connect(mm, input_index(mm, "B"), mb)
        connect(mm, input_index(mm, "A"), ma)
        gt.close_all_panels()
        view(mm)
        yield ("sleep", RENDER_MS)
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        report("NOTE Merge Ctrl-click: %r %r" % (rgba, info_source()))
        check(rgb_is(rgba, (0.25, 0.25, 0.25)) and info_source() == "in", "on a Merge, Ctrl-click reads B (0.25), marked in")
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        rgba = info_rgba()
        check(rgb_is(rgba, (0.75, 0.75, 0.75)) and info_source() == "out", "on a Merge, Ctrl+Alt-click reads A over B (0.75), marked out")

        view(reader)
        yield ("sleep", RENDER_MS)
        click(gl, mid, Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        report("NOTE Read Ctrl-click: %r %r" % (info_rgba(), info_source()))
        check(info_source() == "out", "on a Read, Ctrl-click falls back to the output, marked out")
        widget_shot(info_bar(), "picker-04-read-out")

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

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
