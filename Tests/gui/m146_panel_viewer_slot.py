"""The curve list panel of a paint curve host is unchanged, and curve hosts share one viewer
tool slot.

Opens a Roto node's settings panel, then a RotoPaint node's with the Roto panel still open. Checks
that each settings panel holds the curve list (RotoPanel) with the Operator (column 3) and Color
(column 5) columns shown and no extra host columns, and that after the second panel opens only
the RotoPaint node's viewer interface (its label in the viewer's buttons bar) is on screen: the
two nodes are different plug-ins but one tool slot. Closing the RotoPaint panel hands the slot back
to the Roto node. Saves screenshots of both panels and of the viewer after each step.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m146-shots/slot -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m146_panel_viewer_slot.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
COL_OPERATOR = 3
COL_COLOR = 5
MIN_CURVE_INK = 200  # a box outline across the viewer is thousands of pixels
PAINT_COLUMNS = (6, 7)  # MAX_COLS without/with NATRON_ROTO_INVERTIBLE

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QPoint, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

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

    def curve_tree(panel):
        """The curve list of a settings panel, or None if it has no RotoPanel."""
        for roto_panel in panel.findChildren(QTreeWidget):
            parent = roto_panel.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return roto_panel
                parent = parent.parentWidget()
        return None

    def viewer_interface_shown(node):
        """Whether the node's buttons bar (a ColoredFrame holding a Label with the node's label)
        is on screen in a viewer."""
        label = node.getLabel()
        for frame in gt.widgets_of("ColoredFrame"):
            try:
                if not frame.isVisible():
                    continue
                for w in frame.findChildren(QLabel):
                    if w.isVisible() and w.text() == label:
                        return True
            except RuntimeError:
                continue
        return False

    def open_panel(view, node):
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")

    def check_paint_columns(name, panel):
        tree = curve_tree(panel)
        check(tree is not None, "%s's settings panel holds the curve list" % name)
        if tree is None:
            return
        report("NOTE %s curve list has %d columns" % (name, tree.columnCount()))
        check(tree.columnCount() in PAINT_COLUMNS, "%s's curve list has no host columns" % name)
        check(not tree.isColumnHidden(COL_OPERATOR), "%s's curve list shows the Operator column" % name)
        check(not tree.isColumnHidden(COL_COLOR), "%s's curve list shows the Color column" % name)

    def visible_viewer():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    def grab_viewer():
        """A screen grab of the first visible viewer (a widget grab of the GL viewer can come out
        blank under llvmpipe), or None if there is none."""
        w = visible_viewer()
        if w is None:
            return None
        QApplication.processEvents()
        origin = w.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(
            0, origin.x(), origin.y(), w.width(), w.height()).toImage()

    def curve_pixels(img):
        """The (x, y) positions of the red curve overlay in a viewer grab. The viewer changes size
        when a host's buttons bar appears, so grabs are compared by colour, not position."""
        if img is None:
            return []
        points = []
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.red() > 150 and c.green() < 90 and c.blue() < 90:
                    points.append((x, y))
        return points

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
        view = node_graph_view()
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 2400)

        gt.section("roto-panel")
        roto = app.createNode("fr.inria.built-in.Roto")
        require(roto is not None, "created a Roto node", gt.plugin_problem("fr.inria.built-in.Roto"))
        roto.setPosition(0, 0)
        open_panel(view, roto)
        ok = yield ("until", lambda: panel_of(roto) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the Roto node's settings panel")
        yield ("sleep", SETTLE_MS)
        check_paint_columns("Roto", panel_of(roto))
        ok = yield ("until", lambda: viewer_interface_shown(roto), POLL_TIMEOUT_S)
        check(ok, "the Roto node's viewer interface is shown")
        gt.shot("m146-slot-roto.png")

        gt.section("rotopaint-takes-the-slot")
        paint = app.createNode("fr.inria.built-in.RotoPaint")
        require(paint is not None, "created a RotoPaint node", gt.plugin_problem("fr.inria.built-in.RotoPaint"))
        paint.setPosition(300, 0)
        open_panel(view, paint)
        ok = yield ("until", lambda: panel_of(paint) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the RotoPaint node's settings panel")
        yield ("sleep", SETTLE_MS)
        check(panel_of(roto) is not None, "the Roto node's settings panel is still open")
        check_paint_columns("RotoPaint", panel_of(paint))
        ok = yield ("until", lambda: viewer_interface_shown(paint), POLL_TIMEOUT_S)
        check(ok, "the RotoPaint node's viewer interface is shown")
        check(not viewer_interface_shown(roto), "the Roto node's viewer interface is no longer shown")
        gt.shot("m146-slot-rotopaint.png")

        gt.section("slot-handed-back")
        QMetaObject.invokeMethod(panel_of(paint), "closePanel")
        ok = yield ("until", lambda: viewer_interface_shown(roto), POLL_TIMEOUT_S)
        check(ok, "closing the RotoPaint panel hands the viewer slot back to the Roto node")
        check(not viewer_interface_shown(paint), "the RotoPaint node's viewer interface is gone")
        gt.shot("m146-slot-handback.png")

        gt.section("inactive-host-curves-drawn-without-input")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        ctx = roto.getRotoContext()
        require(ctx is not None, "the Roto node has a roto context")
        bez = ctx.createBezier(300.0, 200.0, 1.0)
        require(bez is not None, "created a Bezier on the Roto node")
        for x, y in ((1500.0, 200.0), (1500.0, 800.0), (300.0, 800.0)):
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        # Overlays are only drawn for nodes on the path feeding the viewer.
        require(paint.connectInput(0, roto), "connected Roto to RotoPaint")
        require(viewer.connectInput(0, paint), "connected RotoPaint to the viewer")
        app.selectNode(viewer, True)
        yield ("sleep", 2 * SETTLE_MS)
        baseline = curve_pixels(grab_viewer())
        report("NOTE no-panel grab has %d curve pixels" % len(baseline))
        check(len(baseline) < MIN_CURVE_INK, "with no panel open the curve is not drawn in the viewer")
        gt.shot("m146-slot-curve-no-panels.png")

        open_panel(view, roto)
        ok = yield ("until", lambda: viewer_interface_shown(roto), POLL_TIMEOUT_S)
        require(ok, "the Roto node owns the viewer slot")
        yield ("sleep", SETTLE_MS)
        owner = grab_viewer()
        owner_px = curve_pixels(owner)
        report("NOTE owner grab has %d curve pixels" % len(owner_px))
        check(len(owner_px) > MIN_CURVE_INK, "the owning host's curve is drawn in the viewer")
        gt.shot("m146-slot-curve-owner.png")

        open_panel(view, paint)
        ok = yield ("until", lambda: viewer_interface_shown(paint), POLL_TIMEOUT_S)
        require(ok, "the RotoPaint node took the viewer slot")
        check(panel_of(roto) is not None, "the Roto node's settings panel is still open")
        yield ("sleep", SETTLE_MS)
        inactive = grab_viewer()
        inactive_px = curve_pixels(inactive)
        report("NOTE inactive grab has %d curve pixels" % len(inactive_px))
        check(len(inactive_px) > MIN_CURVE_INK,
              "the inactive host's curve is still drawn in the viewer")
        gt.shot("m146-slot-curve-inactive.png")

        curve_points = sorted(inactive_px, key=lambda p: (p[1], p[0]))
        check(len(curve_points) > MIN_CURVE_INK, "the curve's pixels are found in the inactive grab")
        if curve_points:
            x, y = curve_points[len(curve_points) // 2]
            report("NOTE clicking the inactive host's curve at viewer pixel (%d, %d)" % (x, y))
            QTest.mouseClick(visible_viewer(), Qt.LeftButton, Qt.NoModifier, QPoint(x, y))
            yield ("sleep", SETTLE_MS)
            # The Python API has no query for the selected curves of a roto context, so that the
            # click selected nothing is judged from the picture and the slot owner alone.
            check(viewer_interface_shown(paint), "clicking the inactive host's curve leaves the slot with RotoPaint")
            gt.shot("m146-slot-curve-inactive-clicked.png")

        QMetaObject.invokeMethod(panel_of(roto), "closePanel")
        yield ("sleep", SETTLE_MS)
        after_close = curve_pixels(grab_viewer())
        report("NOTE after closing Roto's panel the grab has %d curve pixels" % len(after_close))
        check(len(after_close) < MIN_CURVE_INK,
              "closing the Roto panel removes its curve overlay from the viewer")
        gt.shot("m146-slot-curve-closed.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
