"""Overlays of nodes off the viewer's render path are drawn and edited through the graph path.

A Constant feeds a Transform translating by (50, 0), which feeds a Roto holding a square. The
viewer shows the Constant, upstream of the Roto, so the square's overlay is drawn through the
inverse of the Transform: 50 units left of where the Roto holds it. The test:
  - drags the square's top-right point where it is drawn, by 20 units right, and checks the Roto
    point moved by 20, which it can only do if the press was mapped back through the Transform;
  - views a second Transform (30, 0) on the Constant instead, a sibling branch: the square is
    drawn through the shared Constant, 20 units left, and a drag there moves the point;
  - clicks the toolbar's "mapSiblingOverlays" button: the knob turns off and the square is drawn
    in its own space, where a drag moves the point again;
  - presses Shift+O in the viewer: the knob turns back on;
  - connects the Roto to the viewer's second pipe and shows the second Transform on A again: only
    the active pipe places the overlay, so the square is still drawn 20 units left;
  - wipes the Roto on B against A: the square is on the B side's render path, so it is drawn in
    its own space.
Saves a screenshot of each placement.

Run in the natron-dev container:
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m432-overlays/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m432_overlays_off_path.py <AppImage or build/fast/App/Natron>'

Canonical coordinates are mapped to viewer widget pixels through the ViewerGL's
natronCanonicalToWidget property [origin x, origin y, pixels per unit x, pixels per unit y].
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 200
SETTLE_MS = 500
RENDER_MS = 1000
SQUARE_X, SQUARE_Y, SQUARE_SIZE = 760.0, 340.0, 400.0
UPSTREAM_SHIFT = 50.0
SIBLING_SHIFT = 30.0
DRAG_UNITS = 20.0
DRAG_STEPS = 5
CORNER = 1
T = 1.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    held = {}

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def activate():
        win = main_window()
        if win is not None:
            win.activateWindow()
            win.raise_()

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        value = held["gl"].property("natronCanonicalToWidget")
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def to_widget(x, y):
        ox, oy, sx, sy = mapping()
        return QPointF(ox + sx * x, oy + sy * y)

    def mouse(kind, pos, button, buttons):
        QApplication.sendEvent(held["gl"], QMouseEvent(kind, pos, button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def key(k, mods=Qt.NoModifier):
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyPress, k, mods))
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyRelease, k, mods))
        QApplication.processEvents()

    def corner():
        p = held["shape"].getControlPointPosition(CORNER, T)
        return (p[0], p[1])

    def create(plugin_id, panel):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(panel)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create %s" % plugin_id, gt.plugin_problem(plugin_id))
        return node

    def translate(node, x):
        param = node.getParam("translate")
        gt.require(param is not None, "the Transform has a translate parameter")
        param.set(x, 0.)

    def map_siblings_knob():
        param = held["viewer"].getParam("mapSiblingOverlays")
        gt.require(param is not None, "the viewer has a mapSiblingOverlays parameter")
        return param

    def toolbar_button(name):
        for w in QApplication.allWidgets():
            if isinstance(w, QPushButton) and w.objectName() == name and w.isVisible():
                return w
        return None

    def drag_corner(label, shift):
        """Drags the square's corner where it is drawn, shift canonical units left of the Roto's
        own position, and checks the Roto moved it by the drag."""
        before = corner()
        start = to_widget(before[0] - shift, before[1])
        end = to_widget(before[0] - shift + DRAG_UNITS, before[1])
        report("NOTE %s: corner at %r, pressed at widget (%.1f, %.1f)" % (label, before, start.x(), start.y()))
        mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            p = QPointF(start.x() + (end.x() - start.x()) * i / DRAG_STEPS, start.y())
            mouse(QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        after = corner()
        tolerance = 1.0 / abs(mapping()[2]) + 0.01
        check(abs(after[0] - before[0] - DRAG_UNITS) <= tolerance and abs(after[1] - before[1]) <= tolerance,
              "%s: dragging the drawn corner by %g moves the Roto point by it (from %r to %r)"
              % (label, DRAG_UNITS, before, after))

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window is up")
        gt.close_all_panels()

        gt.section("setup")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        gt.require(viewer is not None, "the project has a viewer", gt.plugin_problem("Viewer"))
        held["viewer"] = viewer
        constant = create("Constant", False)
        left = create("Transform", False)
        gt.require(left.connectInput(0, constant), "the Transform reads the Constant")
        translate(left, UPSTREAM_SHIFT)
        roto = create("Roto", True)
        gt.require(roto.connectInput(0, left), "the Roto reads the Transform")
        held["roto"] = roto
        ctx = roto.getRotoContext()
        gt.require(ctx is not None, "the Roto node has a roto context")
        shape = ctx.createRectangle(SQUARE_X, SQUARE_Y, SQUARE_SIZE, T)
        gt.require(shape is not None, "created a square on the Roto node")
        held["shape"] = shape
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        gt.require(viewer.connectInput(0, constant), "the viewer shows the Constant")

        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "a viewer is up")
        held["gl"] = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()][0]
        activate()
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the viewer publishes natronCanonicalToWidget")
        yield ("sleep", RENDER_MS)
        check(map_siblings_knob().get(), "Map Sibling Overlays defaults on")
        # Roto starts in the Bezier drawing tool, where a press would start a new shape.
        key(Qt.Key_Escape)

        gt.section("upstream")
        gt.shot("m432_upstream.png")
        for cmd in drag_corner("upstream", UPSTREAM_SHIFT):
            yield cmd

        gt.section("sibling")
        right = create("Transform", False)
        gt.require(right.connectInput(0, constant), "a second Transform reads the Constant")
        translate(right, SIBLING_SHIFT)
        viewer.disconnectInput(0)
        gt.require(viewer.connectInput(0, right), "the viewer shows the second Transform")
        yield ("sleep", RENDER_MS)
        gt.shot("m432_sibling_mapped.png")
        for cmd in drag_corner("sibling", UPSTREAM_SHIFT - SIBLING_SHIFT):
            yield cmd

        gt.section("toggle")
        button = toolbar_button("mapSiblingOverlays")
        gt.require(button is not None, "the viewer toolbar has the mapSiblingOverlays button")
        check(button.isChecked(), "the button shows the knob is on")
        button.click()
        QApplication.processEvents()
        check(not map_siblings_knob().get(), "clicking the button turns Map Sibling Overlays off")
        check(not button.isChecked(), "the button shows the knob is off")
        yield ("sleep", SETTLE_MS)
        gt.shot("m432_sibling_unmapped.png")
        for cmd in drag_corner("sibling unmapped", 0.):
            yield cmd

        activate()
        key(Qt.Key_O, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        check(map_siblings_knob().get(), "Shift+O in the viewer turns Map Sibling Overlays back on")
        check(button.isChecked(), "the button follows the shortcut")

        gt.section("inactive pipe")
        py_viewer = app.getViewer(viewer.getScriptName())
        gt.require(py_viewer is not None, "the viewer has a Python viewer")
        gt.require(viewer.connectInput(1, roto), "the viewer's second pipe reads the Roto")
        py_viewer.setAInput(0)
        yield ("sleep", RENDER_MS)
        check(py_viewer.getAInput() == 0, "A shows the second Transform again")
        gt.shot("m432_inactive_pipe.png")
        for cmd in drag_corner("inactive pipe", UPSTREAM_SHIFT - SIBLING_SHIFT):
            yield cmd

        gt.section("wipe")
        py_viewer.setBInput(1)
        py_viewer.setCompositingOperator(NatronEngine.Natron.ViewerCompositingOperatorEnum.eViewerCompositingOperatorWipeUnder)
        yield ("sleep", RENDER_MS)
        check(py_viewer.getBInput() == 1, "B shows the Roto")
        gt.shot("m432_wipe_b.png")
        for cmd in drag_corner("wipe B", 0.):
            yield cmd
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
