"""ColorWarp's Colour Space viewer mode in the real application: the viewer swaps the image for a 3D
view of the ColorWarp's colours, with its pins as arrows, and swaps back with its pan and zoom.

A CheckerBoard with four known colours (one of them above 1) feeds a ColorWarp feeding the viewer,
with the ColorWarp's settings panel open. Two pins are added through Python: one maps the red box
to another colour, one holds the blue box. The script turns the mode on and checks, through
ViewerGL::colorSpaceViewState() and colorSpaceViewProbe(), that the cloud and the pins are drawn and
that each box colour has a cloud point on it; it orbits, toggles the cloud between input and
output, clicks a pin's target to select it and checks the panel follows, drags the selected target
along one axis handle and reads the knob back, undoes the drag, and turns the mode off and checks
the 2D pan and zoom came back. Last, viewing the CheckerBoard instead turns the mode off and
disables the toggle. Saves a screenshot of every state.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m459-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m459_colorwarp_3d_view.py build/fast/App/Natron
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
DRAG_STEPS = 12
DRAG_PIXELS = 40.0
POINT_BUDGET = 20000
# Box colours in the working space; the third is above 1 so the view has to reach past the unit cube.
BOXES = [
    (0.8, 0.1, 0.1),
    (0.1, 0.6, 0.2),
    (2.5, 1.5, 0.4),
    (0.2, 0.2, 0.7),
]
MOVED_TARGET = (0.3, 0.5, 0.9)
ON_POINT = 1e-3  # warp-space distance under which a colour counts as a point of the cloud

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, Q_ARG, Q_RETURN_ARG
    from PySide6.QtGui import QCursor, QMouseEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QPushButton, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Python wrappers die with their parent wrapper, so every one used later is held here.
    held = []

    def hold(obj):
        held.append(obj)
        return obj

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def visible_viewer():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    def button(name):
        win = main_window()
        if win is None:
            return None
        for b in win.findChildren(QPushButton, name):
            if b.isVisible() or name == "colorSpaceViewOutput":
                return hold(b)
        return None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def pin_list(node):
        """The pin list, found again on every call; the panel wrapper is held so the list's
        wrapper outlives this call."""
        panel = panel_of(node)
        if panel is None:
            return None
        hold(panel)
        for tree in panel.findChildren(QTreeWidget):
            labels = [tree.headerItem().text(c) for c in range(tree.columnCount())]
            if "Target" in labels and "Source" in labels:
                return tree
        return None

    def listed_selection(node):
        tree = pin_list(node)
        if tree is None:
            return None
        return sorted(tree.indexOfTopLevelItem(item) for item in tree.selectedItems())

    def open_panel(node):
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def state():
        gl = visible_viewer()
        if gl is None:
            return {}
        return QMetaObject.invokeMethod(gl, "colorSpaceViewState", Qt.DirectConnection, Q_RETURN_ARG("QVariantMap")) or {}

    def probe(rgb):
        gl = visible_viewer()
        return QMetaObject.invokeMethod(gl, "colorSpaceViewProbe", Qt.DirectConnection, Q_RETURN_ARG("QVariantMap"),
                                        Q_ARG(float, float(rgb[0])), Q_ARG(float, float(rgb[1])),
                                        Q_ARG(float, float(rgb[2]))) or {}

    def projection():
        gl = visible_viewer()
        return [float(v) for v in QMetaObject.invokeMethod(gl, "viewProjection", Qt.DirectConnection,
                                                           Q_RETURN_ARG("QVariantList"))]

    def cloud_ready():
        s = state()
        return bool(s) and not s.get("fetching", True) and s.get("pointCount", 0) > 0

    def pin_entry(index):
        """The view lists the enabled pins in the pins parameter's order; every pin here is enabled."""
        entries = state().get("pins", [])
        return entries[index] if 0 <= index < len(entries) else None

    def send_mouse(widget, kind, pos, button_, buttons, mods=Qt.NoModifier):
        pos = QPointF(pos)
        QApplication.sendEvent(widget, QMouseEvent(kind, pos, QPointF(widget.mapToGlobal(pos.toPoint())),
                                                   button_, buttons, mods))
        QApplication.processEvents()

    def drag(widget, start, end, button_=Qt.LeftButton, mods=Qt.NoModifier):
        start = QPointF(start)
        end = QPointF(end)
        send_mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton, mods)
        send_mouse(widget, QEvent.MouseButtonPress, start, button_, button_, mods)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * f, Qt.NoButton, button_, mods)
        send_mouse(widget, QEvent.MouseButtonRelease, end, button_, Qt.NoButton, mods)

    def click(widget, pos):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)

    def wheel(widget, steps):
        pos = QPointF(widget.width() / 2.0, widget.height() / 2.0)
        QApplication.sendEvent(widget, QWheelEvent(pos, QPointF(widget.mapToGlobal(pos.toPoint())), QPoint(0, 0),
                                                   QPoint(0, 120 * steps), Qt.NoButton, Qt.NoModifier,
                                                   Qt.NoScrollPhase, False))
        QApplication.processEvents()

    def undo_redo(redo):
        """Ctrl+Z or Ctrl+Shift+Z with the pointer over the node graph."""
        view = node_graph_view()
        QCursor.setPos(view.mapToGlobal(view.rect().center()))
        win = main_window()
        win.activateWindow()
        QApplication.processEvents()
        QTest.keyClick(win, Qt.Key_Z, Qt.ControlModifier | (Qt.ShiftModifier if redo else Qt.NoModifier))
        QApplication.processEvents()

    def target_of(pins, index):
        t = pins.getPinTarget(index, app.timelineGetTime())
        return (t.r, t.g, t.b)

    def close(a, b, tolerance=1e-4):
        return a is not None and b is not None and all(abs(x - y) <= tolerance for x, y in zip(a, b))

    def nearest(rgb):
        return float(probe(rgb).get("nearest", -1.0))

    def on_cloud(rgb):
        d = nearest(rgb)
        return 0.0 <= d <= ON_POINT

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 600)
        chart = app.createNode("CheckerBoard")
        require(chart is not None, "created a CheckerBoard", gt.plugin_problem("CheckerBoard"))
        chart.setPosition(0, 0)
        for i, rgb in enumerate(BOXES):
            chart.getParam("color%d" % i).set(rgb[0], rgb[1], rgb[2], 1.0)
        for name in ("lineWidth", "centerlineWidth"):
            param = chart.getParam(name)
            if param is not None:
                param.set(0.0)
        warp = app.createNode("ColorWarp")
        require(warp is not None, "created a ColorWarp", gt.plugin_problem("ColorWarp"))
        warp.setPosition(0, 300)
        require(warp.connectInput(0, chart), "connected the CheckerBoard to the ColorWarp")
        require(viewer.connectInput(0, warp), "connected the ColorWarp to the viewer")
        pins = hold(warp.getParam("pins"))
        require(pins is not None and hasattr(pins, "getPinCount"), "the ColorWarp has a pins parameter with the pin API")
        time = app.timelineGetTime()
        moved = pins.addPin(BOXES[0][0], BOXES[0][1], BOXES[0][2], 100.0, 100.0, time)
        pins.setPinTarget(moved, MOVED_TARGET[0], MOVED_TARGET[1], MOVED_TARGET[2], time)
        held_pin = pins.addPin(BOXES[3][0], BOXES[3][1], BOXES[3][2], 300.0, 100.0, time)
        check(pins.getPinCount() == 2 and (moved, held_pin) == (0, 1), "added two pins (%d)" % pins.getPinCount())

        open_panel(warp)
        ok = yield ("until", lambda: panel_of(warp) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the ColorWarp's settings panel")
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        yield ("sleep", RENDER_MS)

        gt.section("toggle")
        toggle = button("colorSpaceView")
        require(toggle is not None, "the viewer has a Colour Space toggle")
        check(toggle.text() == "Colour Space", "the toggle is labelled Colour Space (%r)" % toggle.text())
        ok = yield ("until", lambda: toggle.isEnabled(), POLL_TIMEOUT_S)
        require(ok, "the toggle is enabled while the viewer shows a ColorWarp")
        gl = visible_viewer()
        wheel(gl, 2)
        yield ("sleep", SETTLE_MS)
        before_2d = projection()
        report("NOTE 2D projection before the mode: %r" % (before_2d,))
        gt.shot("m459-image.png")

        toggle.click()
        QApplication.processEvents()
        check(toggle.isChecked(), "clicking the toggle checks it")
        ok = yield ("until", cloud_ready, POLL_TIMEOUT_S)
        require(ok, "the cloud of the input arrives (%r)" % ({k: v for k, v in state().items() if k != "pins"},))
        s = state()
        report("NOTE state: %r" % ({k: v for k, v in s.items() if k != "pins"},))
        count = s.get("pointCount", 0)
        check(0 < count <= POINT_BUDGET * 11 // 10, "the cloud holds a bounded subsample (%d points)" % count)
        check(s.get("output") is False, "the cloud starts on the input")
        check(s.get("arrowCount") == 1 and s.get("holdCount") == 1,
              "one arrow and one hold marker are drawn (%r, %r)" % (s.get("arrowCount"), s.get("holdCount")))
        check(len(s.get("axes", [])) == 3, "the axes are named (%r)" % (s.get("axes"),))
        for rgb in BOXES:
            d = nearest(rgb)
            check(0.0 <= d <= ON_POINT, "the box colour %r is a point of the input cloud (%.2g)" % (rgb, d))
        hdr = probe(BOXES[2]).get("warp", [])
        lo = s.get("boundsMin", [])
        hi = s.get("boundsMax", [])
        check(len(hdr) == 3 and all(lo[k] - 1e-6 * abs(lo[k]) <= hdr[k] <= hi[k] + 1e-6 * abs(hi[k]) for k in range(3)),
              "the axes reach the colour above 1 (%r in %r..%r)" % (hdr, lo, hi))
        output_toggle = button("colorSpaceViewOutput")
        check(output_toggle is not None and output_toggle.isVisible(), "the Output toggle shows with the mode")
        gt.shot("m459-input-cloud.png")

        gt.section("orbit")
        camera = state().get("camera")
        gl = visible_viewer()
        centre = QPointF(gl.width() / 2.0, gl.height() / 2.0)
        drag(gl, centre + QPointF(0, 120), centre + QPointF(120, 80), Qt.LeftButton, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        orbited = state().get("camera")
        check(orbited != camera, "a tumble drag orbits the camera (%r -> %r)" % (camera, orbited))
        check(pins.getPinCount() == 2 and close(target_of(pins, moved), MOVED_TARGET),
              "orbiting edits no pin")
        wheel(gl, 1)
        yield ("sleep", SETTLE_MS)
        check(state().get("camera") != orbited, "the wheel dollies the camera")
        gt.shot("m459-orbited.png")

        gt.section("output")
        require(output_toggle is not None, "found the Output toggle")
        output_toggle.click()
        QApplication.processEvents()
        ok = yield ("until", lambda: state().get("output") is True and cloud_ready(), POLL_TIMEOUT_S)
        require(ok, "the cloud switches to the output")
        check(on_cloud(MOVED_TARGET), "the output cloud holds the moved pin's target (%.2g)" % nearest(MOVED_TARGET))
        check(not on_cloud(BOXES[0]), "and no longer the colour it moved (%.2g)" % nearest(BOXES[0]))
        check(on_cloud(BOXES[3]), "the held colour stays (%.2g)" % nearest(BOXES[3]))
        gt.shot("m459-output-cloud.png")
        output_toggle.click()
        QApplication.processEvents()
        ok = yield ("until", lambda: state().get("output") is False and cloud_ready(), POLL_TIMEOUT_S)
        check(ok, "the cloud switches back to the input")
        check(state().get("pointCount") == count, "the input cloud has the same points again (%r)" % state().get("pointCount"))
        check(on_cloud(BOXES[0]), "with the moved colour back on it")

        gt.section("select")
        entry = pin_entry(held_pin)
        require(entry is not None, "the hold pin is listed in the view's state")
        gl = visible_viewer()
        click(gl, QPointF(entry["target"][0], entry["target"][1]))
        ok = yield ("until", lambda: pins.getSelectedPin() == held_pin, POLL_TIMEOUT_S)
        check(ok, "clicking the hold pin selects it (%d)" % pins.getSelectedPin())
        ok = yield ("until", lambda: listed_selection(warp) == [held_pin], POLL_TIMEOUT_S)
        check(ok, "the panel's pin list follows (%r)" % (listed_selection(warp),))
        check(close(target_of(pins, held_pin), BOXES[3]), "a click without a move leaves the target")
        gt.shot("m459-selected-hold.png")

        entry = pin_entry(moved)
        moved_id = entry["id"] if entry else None
        require(entry is not None, "the moved pin is listed in the view's state")
        click(gl, QPointF(entry["target"][0], entry["target"][1]))
        ok = yield ("until", lambda: pins.getSelectedPin() == moved, POLL_TIMEOUT_S)
        check(ok, "clicking the moved pin's target selects it (%d)" % pins.getSelectedPin())
        ok = yield ("until", lambda: listed_selection(warp) == [moved], POLL_TIMEOUT_S)
        check(ok, "the panel's pin list follows (%r)" % (listed_selection(warp),))
        yield ("sleep", SETTLE_MS)
        check(state().get("selectedPin") == moved_id, "the view draws the selection")
        gt.shot("m459-selected.png")

        gt.section("drag-axis")
        entry = pin_entry(moved)
        require(entry is not None and "handles" in entry, "the selected target has axis handles")
        tx, ty = entry["target"]
        lengths = [math.hypot(h[0] - tx, h[1] - ty) for h in entry["handles"]]
        axis = max(range(3), key=lambda k: lengths[k])
        hx, hy = entry["handles"][axis]
        require(lengths[axis] > 30.0, "a handle is long enough on screen to grab (%r)" % (lengths,))
        grab = QPointF(tx + (hx - tx) * 0.6, ty + (hy - ty) * 0.6)
        unit = QPointF((hx - tx) / lengths[axis], (hy - ty) / lengths[axis])
        warp_before = list(entry["targetWarp"])
        knob_before = target_of(pins, moved)
        drag(gl, grab, grab + unit * DRAG_PIXELS)
        yield ("sleep", RENDER_MS)
        entry = pin_entry(moved)
        warp_after = list(entry["targetWarp"]) if entry else warp_before
        knob_after = target_of(pins, moved)
        report("NOTE axis %d: warp %r -> %r, knob %r -> %r" % (axis, warp_before, warp_after, knob_before, knob_after))
        check(warp_after[axis] > warp_before[axis] + 1e-4, "the drag moves the target along the handle's axis")
        check(all(abs(warp_after[k] - warp_before[k]) < 1e-4 for k in range(3) if k != axis),
              "and only along it")
        check(not close(knob_after, knob_before), "the pins parameter holds the moved target (%r)" % (knob_after,))
        source = pins.getPinSource(moved)
        check(close((source.r, source.g, source.b), BOXES[0]), "the source stays")
        gt.shot("m459-dragged.png")

        undo_redo(False)
        ok = yield ("until", lambda: close(target_of(pins, moved), knob_before), POLL_TIMEOUT_S)
        check(ok, "one undo puts the target back (%r)" % (target_of(pins, moved),))
        undo_redo(True)
        ok = yield ("until", lambda: close(target_of(pins, moved), knob_after), POLL_TIMEOUT_S)
        check(ok, "redo moves it again (%r)" % (target_of(pins, moved),))
        undo_redo(False)
        ok = yield ("until", lambda: close(target_of(pins, moved), knob_before), POLL_TIMEOUT_S)
        check(ok, "and undo takes it back for the rest of the check")

        gt.section("off")
        toggle = button("colorSpaceView")
        toggle.click()
        QApplication.processEvents()
        yield ("sleep", RENDER_MS)
        check(not toggle.isChecked(), "clicking the toggle again unchecks it")
        check(state() == {}, "the view is off")
        after_2d = projection()
        check(close(after_2d, before_2d, 1e-6), "the 2D pan and zoom are restored (%r -> %r)" % (before_2d, after_2d))
        output_toggle = button("colorSpaceViewOutput")
        check(output_toggle is None or not output_toggle.isVisible(), "the Output toggle hides with the mode")
        gt.shot("m459-off.png")

        gt.section("not-a-colorwarp")
        toggle.click()
        QApplication.processEvents()
        ok = yield ("until", cloud_ready, POLL_TIMEOUT_S)
        check(ok, "the mode comes back on")
        app.clearSelection()
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, chart), "the viewer views the CheckerBoard")
        ok = yield ("until", lambda: not toggle.isEnabled(), POLL_TIMEOUT_S)
        check(ok, "the toggle is disabled while the viewer shows no ColorWarp")
        check(not toggle.isChecked() and state() == {}, "and the mode is off")
        check(close(projection(), before_2d, 1e-6), "with the 2D pan and zoom restored (%r)" % (projection(),))
        gt.shot("m459-not-a-colorwarp.png")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, warp), "the viewer views the ColorWarp again")
        ok = yield ("until", lambda: toggle.isEnabled(), POLL_TIMEOUT_S)
        check(ok, "the toggle is enabled again")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
