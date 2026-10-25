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
PAINT_COLUMNS = (6, 7)  # MAX_COLS without/with NATRON_ROTO_INVERTIBLE

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject
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

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
