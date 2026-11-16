"""Ctrl+wheel over the node graph zooms the graph exactly as the plain wheel does and never scales
the selected node.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m374-shots/out GUI_TEST_TIMEOUT=120 \
        Tests/gui/run-gui-test.sh Tests/gui/m374_ctrl_wheel_zoom.py build/fast/App/Natron'

Wheel events are synthetic QWheelEvents sent to the NodeGraph viewport, as in
Tests/gui/m55_node_graph.py. A node's scale lives on its graphics item, not in getSize(), so the
check reads the scene transforms of the items the view finds over the node.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 120
SETTLE_MS = 400
WHEEL_DELTA = 120
ZOOM_TOLERANCE = 1e-9

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, QPointF, Qt
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def send_wheel(viewport, pos, modifiers):
        from PySide6.QtGui import QWheelEvent
        QApplication.sendEvent(viewport, QWheelEvent(QPointF(pos), QPointF(viewport.mapToGlobal(pos)), QPoint(0, 0),
                                                     QPoint(0, WHEEL_DELTA), Qt.NoButton, modifiers,
                                                     Qt.NoScrollPhase, False))
        QApplication.processEvents()

    def node_item_scales(graph, node):
        """The scene scale of every item over node's centre, the node's own items included.

        Scaling a node scales all of its child items' scene transforms, so a magnified node changes
        this list. The view lists the items itself: NodeGraph.scene() raises "already deleted" here."""
        x, y = node.getPosition()
        w, h = node.getSize()
        scales = []
        try:
            for item in graph.items(graph.mapFromScene(x + w / 2.0, y + h / 2.0)):
                t = item.sceneTransform()
                scales.append((t.m11(), t.m22()))
        except RuntimeError:
            return []
        return sorted(scales)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode("Blur", -1, None, props)
        gt.require(node is not None, "create a node", gt.plugin_problem("Blur"))
        node.setPosition(0, 0)
        gt.close_all_panels()
        app.clearSelection()
        app.selectNode(node, False)
        yield ("sleep", SETTLE_MS)

        graph = root_graph()
        vp = graph.viewport()
        x, y = node.getPosition()
        w, h = node.getSize()
        graph.resetTransform()
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()
        over_node = graph.mapFromScene(x + w / 2.0, y + h / 2.0)
        start_scales = node_item_scales(graph, node)
        gt.require(len(start_scales) > 0, "the view finds the node's items over its centre")
        check((1.0, 1.0) in start_scales, "the node's items start unscaled (%r)" % (start_scales,))

        gt.section("ctrl-wheel")
        start_zoom = graph.transform().m11()
        send_wheel(vp, over_node, Qt.ControlModifier)
        ctrl_zoom = graph.transform().m11()
        check(ctrl_zoom != start_zoom, "ctrl-wheel: Ctrl+wheel over a selected node changes the graph zoom (%.4f -> %.4f)"
              % (start_zoom, ctrl_zoom))
        ctrl_scales = node_item_scales(graph, node)
        check(ctrl_scales == start_scales,
              "ctrl-wheel: Ctrl+wheel leaves the selected node's item scales at %r (got %r)" % (start_scales, ctrl_scales))
        ctrl_ratio = ctrl_zoom / start_zoom

        gt.section("plain-wheel")
        graph.resetTransform()
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()
        start_zoom = graph.transform().m11()
        send_wheel(vp, over_node, Qt.NoModifier)
        plain_zoom = graph.transform().m11()
        plain_ratio = plain_zoom / start_zoom
        check(plain_ratio != 1.0, "plain-wheel: the wheel changes the graph zoom (%.4f -> %.4f)" % (start_zoom, plain_zoom))
        check(abs(plain_ratio - ctrl_ratio) <= ZOOM_TOLERANCE,
              "plain-wheel: the plain wheel zoom change %.6f equals the Ctrl+wheel change %.6f" % (plain_ratio, ctrl_ratio))
        plain_scales = node_item_scales(graph, node)
        check(plain_scales == start_scales,
              "plain-wheel: the node's item scales stay at %r (got %r)" % (start_scales, plain_scales))

        del vp
        del graph
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
