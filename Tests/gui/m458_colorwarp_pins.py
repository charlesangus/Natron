"""ColorWarp pins in the real application: a click in the viewer adds a pin on the colour under
the pointer, a drag moves it, Delete removes the selected pin, and the global mode's C stroke
pushes the selected pin's target. Each of those is one undo step, and redo puts it back.

A Constant feeds a ColorWarp feeding the viewer, with the ColorWarp's settings panel open. One pin
is added with a viewer click and two through the pins parameter's Python API. The first is pushed
with Ctrl+E over the pin list and a C stroke, which has to change the viewer's colour; the second is
dragged in the viewer; the third is selected and deleted with Delete. Saves screenshots of every
step.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m458-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m458_colorwarp_pins.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
STROKE_MS = 300
# The X server delivers the move from QCursor.setPos() asynchronously; a buttonless real move
# arriving after a synthetic press would end the global mode stroke before it starts.
POINTER_SETTLE_MS = 300
DRAG_STEPS = 12
DRAG_BY = (120, -60)  # viewer pixels: right and up
INPUT_RGB = (0.45, 0.25, 0.15)

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QCursor, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QTreeWidget

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

    def open_panel(node):
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def send_mouse(widget, kind, pos, button, buttons):
        pos = QPointF(pos)
        QApplication.sendEvent(widget, QMouseEvent(kind, pos, QPointF(widget.mapToGlobal(pos.toPoint())),
                                                   button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def gmouse(kind, g, button, buttons):
        w = main_window()
        local = QPointF(w.mapFromGlobal(g))
        QApplication.sendEvent(w, QMouseEvent(kind, local, QPointF(w.mapToGlobal(local)), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def send_key(key, mods=Qt.NoModifier):
        target = main_window()
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, key, mods))
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyRelease, key, mods))
        QApplication.processEvents()

    def viewer_key(key):
        """key through the viewer's window to the viewer, as a key press from a person arrives."""
        gl = visible_viewer()
        gl.setFocus(Qt.OtherFocusReason)
        QTest.keyClick(gl.window().windowHandle(), key)
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

    def engaged():
        return QApplication.overrideCursor() is not None

    def engage_over(widget):
        """Ctrl+E with the real pointer over widget; falls back to the shortcut's action."""
        QCursor.setPos(widget.mapToGlobal(widget.rect().center()))
        win = main_window()
        win.activateWindow()
        QApplication.processEvents()
        QTest.keyClick(win, Qt.Key_E, Qt.ControlModifier)
        QApplication.processEvents()
        if engaged():
            return True
        for a in win.actions():
            if a.shortcut().toString() == "Ctrl+E":
                report("NOTE the Ctrl+E shortcut did not engage; the action was triggered instead")
                a.trigger()
                QApplication.processEvents()
                break
        return engaged()

    def hud_knob():
        for name in ("ColorTrackballHudWindow", "ColorTrackballHudBlock"):
            for w in gt.widgets_of(name):
                if w.isVisible():
                    return w.property("trackballHudKnob")
        return None

    def stroke(dx, dy, steps=10):
        """A press at the node graph's centre, steps moves to (dx, dy) away, and a release."""
        view = node_graph_view()
        start = view.mapToGlobal(view.rect().center())
        QCursor.setPos(start)
        yield ("sleep", POINTER_SETTLE_MS)
        gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        yield ("sleep", 60)
        for k in range(1, steps + 1):
            gmouse(QEvent.MouseMove, QPoint(start.x() + dx * k // steps, start.y() + dy * k // steps),
                   Qt.NoButton, Qt.LeftButton)
        gmouse(QEvent.MouseButtonRelease, QPoint(start.x() + dx, start.y() + dy), Qt.LeftButton, Qt.NoButton)
        yield ("sleep", STROKE_MS)

    def target_of(pins, index):
        t = pins.getPinTarget(index, app.timelineGetTime())
        return (round(t.r, 4), round(t.g, 4), round(t.b, 4))

    def position_of(pins, index):
        p = pins.getPinPosition(index)
        return (round(p.x, 2), round(p.y, 2))

    def viewer_colour():
        # Off-centre: a pin added at the centre draws its handle swatch there, which would be read instead of the image.
        return gt.viewer_centre_colour((0, -60))

    def near(a, b, tolerance=2):
        return a is not None and b is not None and all(abs(x - y) <= tolerance for x, y in zip(a, b))

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
        constant = app.createNode("Constant")
        require(constant is not None, "created a Constant", gt.plugin_problem("Constant"))
        constant.setPosition(0, 0)
        constant.getParam("color").set(INPUT_RGB[0], INPUT_RGB[1], INPUT_RGB[2], 1.0)
        warp = app.createNode("ColorWarp")
        require(warp is not None, "created a ColorWarp", gt.plugin_problem("ColorWarp"))
        warp.setPosition(0, 300)
        require(warp.connectInput(0, constant), "connected the Constant to the ColorWarp")
        require(viewer.connectInput(0, warp), "connected the ColorWarp to the viewer")
        pins = hold(warp.getParam("pins"))
        require(pins is not None and hasattr(pins, "getPinCount"), "the ColorWarp has a pins parameter with the pin API")
        check(warp.getParam("hostUnPremultBy") is not None, "the ColorWarp has the (un)premult-by knob")

        open_panel(warp)
        ok = yield ("until", lambda: panel_of(warp) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the ColorWarp's settings panel")
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        require(pin_list(warp) is not None, "the settings panel holds the pin list")
        # The identity checks below compare against this, so it must be the rendered input, not the empty viewer.
        ok = yield ("until", lambda: (viewer_colour() or (0, 0, 0)) != (0, 0, 0), POLL_TIMEOUT_S)
        require(ok, "the viewer shows the input with no pins (%r)" % (viewer_colour(),))
        untouched = viewer_colour()
        report("NOTE viewer centre with no pins: %r" % (untouched,))
        gt.shot("m458-no-pins.png")

        gt.section("add-in-viewer")
        gl = visible_viewer()
        centre = QPoint(gl.width() // 2, gl.height() // 2)
        send_mouse(gl, QEvent.MouseMove, centre, Qt.NoButton, Qt.NoButton)
        send_mouse(gl, QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        send_mouse(gl, QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        ok = yield ("until", lambda: pins.getPinCount() == 1, POLL_TIMEOUT_S)
        require(ok, "a click in the viewer added a pin (%d pins)" % pins.getPinCount())
        src = pins.getPinSource(0)
        check(all(abs(a - b) < 1e-3 for a, b in zip((src.r, src.g, src.b), INPUT_RGB)),
              "the pin's source is the colour under the pointer (%r)" % ((src.r, src.g, src.b),))
        check(pins.getSelectedPin() == 0, "the new pin is selected (%d)" % pins.getSelectedPin())
        tree = pin_list(warp)
        check(tree is not None and tree.topLevelItemCount() == 1, "the pin list shows the pin")
        yield ("sleep", RENDER_MS)
        check(near(viewer_colour(), untouched), "a fresh pin maps its source to itself (%r)" % (viewer_colour(),))
        gt.shot("m458-added.png")
        undo_redo(False)
        ok = yield ("until", lambda: pins.getPinCount() == 0, POLL_TIMEOUT_S)
        check(ok, "one undo removes the added pin")
        undo_redo(True)
        ok = yield ("until", lambda: pins.getPinCount() == 1, POLL_TIMEOUT_S)
        check(ok, "redo adds it back")

        gt.section("add-through-python")
        second = pins.addPin(0.2, 0.3, 0.6, 400.0, 300.0, app.timelineGetTime())
        third = pins.addPin(0.7, 0.7, 0.2, 600.0, 300.0, app.timelineGetTime())
        check(pins.getPinCount() == 3 and (second, third) == (1, 2),
              "addPin appends two pins (%d pins, indices %r)" % (pins.getPinCount(), (second, third)))
        pins.setPinName(second, "Sky")
        check(pins.getPinName(second) == "Sky", "setPinName renames a pin")
        yield ("sleep", SETTLE_MS)
        tree = pin_list(warp)
        check(tree is not None and tree.topLevelItemCount() == 3, "the pin list shows three pins")

        gt.section("push-with-global-mode")
        pins.setSelectedPin(0)
        yield ("sleep", SETTLE_MS)
        before = target_of(pins, 0)
        tree = pin_list(warp)
        require(tree is not None, "found the pin list to hover")
        require(engage_over(tree.viewport()), "Ctrl+E over the pin list engages the global mode")
        report("NOTE the HUD names %r" % (hud_knob(),))
        send_key(Qt.Key_C)
        yield ("sleep", SETTLE_MS)
        gt.shot("m458-global-mode.png")
        yield from stroke(60, -30)
        pushed = target_of(pins, 0)
        check(pushed != before, "a C stroke pushes the selected pin's target (%r -> %r)" % (before, pushed))
        check(target_of(pins, 1) == (0.2, 0.3, 0.6), "the other pins keep their targets")
        send_key(Qt.Key_Escape)
        yield ("sleep", RENDER_MS)
        check(not engaged(), "Esc leaves the global mode")
        pushed_colour = viewer_colour()
        check(not near(pushed_colour, untouched), "the pushed target changes the viewer (%r -> %r)" % (untouched, pushed_colour))
        gt.shot("m458-pushed.png")
        undo_redo(False)
        ok = yield ("until", lambda: target_of(pins, 0) == before, POLL_TIMEOUT_S)
        check(ok, "one undo puts the target back (%r)" % (target_of(pins, 0),))
        yield ("sleep", RENDER_MS)
        check(near(viewer_colour(), untouched), "and the viewer with it (%r)" % (viewer_colour(),))
        undo_redo(True)
        ok = yield ("until", lambda: target_of(pins, 0) == pushed, POLL_TIMEOUT_S)
        check(ok, "redo pushes it again (%r)" % (target_of(pins, 0),))

        gt.section("drag-in-viewer")
        start_pos = position_of(pins, 0)
        gl = visible_viewer()
        send_mouse(gl, QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            send_mouse(gl, QEvent.MouseMove, QPointF(centre) + QPointF(DRAG_BY[0] * f, DRAG_BY[1] * f),
                       Qt.NoButton, Qt.LeftButton)
        send_mouse(gl, QEvent.MouseButtonRelease, centre + QPoint(*DRAG_BY), Qt.LeftButton, Qt.NoButton)
        # Drag samples land asynchronously; the move is recorded once the last one has.
        yield ("until", lambda: position_of(pins, 0) != start_pos, POLL_TIMEOUT_S)
        yield ("sleep", RENDER_MS)
        end_pos = position_of(pins, 0)
        check(pins.getPinCount() == 3, "a drag on a pin adds none (%d pins)" % pins.getPinCount())
        check(end_pos[0] > start_pos[0] and end_pos[1] > start_pos[1],
              "the drag moved the pin up and right (%r -> %r)" % (start_pos, end_pos))
        check(target_of(pins, 0) == pushed, "on a flat input the moved pin keeps its target (%r)" % (target_of(pins, 0),))
        gt.shot("m458-dragged.png")
        undo_redo(False)
        ok = yield ("until", lambda: position_of(pins, 0) == start_pos, POLL_TIMEOUT_S)
        check(ok, "one undo moves the pin back (%r)" % (position_of(pins, 0),))
        undo_redo(True)
        ok = yield ("until", lambda: position_of(pins, 0) == end_pos, POLL_TIMEOUT_S)
        check(ok, "redo moves it again (%r)" % (position_of(pins, 0),))

        gt.section("delete-in-viewer")
        pins.setSelectedPin(third)
        yield ("sleep", SETTLE_MS)
        gl = visible_viewer()
        QCursor.setPos(gl.mapToGlobal(centre))
        QApplication.processEvents()
        viewer_key(Qt.Key_Delete)
        ok = yield ("until", lambda: pins.getPinCount() == 2, POLL_TIMEOUT_S)
        check(ok, "Delete in the viewer removes the selected pin (%d pins)" % pins.getPinCount())
        check(pins.getPinCount() == 2 and pins.getPinName(1) == "Sky", "the other pins stay")
        gt.shot("m458-deleted.png")
        undo_redo(False)
        ok = yield ("until", lambda: pins.getPinCount() == 3, POLL_TIMEOUT_S)
        check(ok, "one undo brings the pin back")
        check(pins.getPinCount() == 3 and abs(pins.getPinSource(2).r - 0.7) < 1e-6, "with its source")
        undo_redo(True)
        ok = yield ("until", lambda: pins.getPinCount() == 2, POLL_TIMEOUT_S)
        check(ok, "redo deletes it again")

        gt.section("panel")
        open_panel(warp)
        yield ("sleep", RENDER_MS)
        gt.shot("m458-panel.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
