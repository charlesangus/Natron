"""A GUI test of dragging pipes between the ports of shader nodes inside a USD Material Builder. It
is driven by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m351-gui/drag GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_drag.py <AppImage or build/fast/App/Natron>'

A texture's colour output is dragged onto a BSDF's base colour, a compatible port, which must get
a halo mid-drag. The BSDF's closure output is dragged onto a math node's float input, which must
be dimmed mid-drag and refuse the drop, with the reason in the status bar. The pipe is then picked
up by its destination end and moved to another BSDF, by its source end and moved to the math
node's output, an output is clicked and then an input to connect them, and a pipe is dropped on
empty space to disconnect it.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
SETTLE_MS = 500
MAX_PANES = 40
DRAG_STEPS = 8
# How far along a pipe from its port a press lands to pick the pipe up by that end: beyond the
# port's reach, where the pipe still runs level.
PIPE_END_OFFSET = 24
BUILDER = "USDMaterialBuilder"
TEXTURE = "cycles_image_texture"
MATH = "cycles_math"
BSDF = "cycles_principled_bsdf"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QFocusEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x, y, group=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def graph_of(group):
        found = [g for g in gt.widgets_of("NodeGraph") if g.objectName() == group.getLabel()]
        return found[-1] if found else None

    def show_graph_tab(label):
        for i in range(1, MAX_PANES + 1):
            pane = app.getTabWidget("pane%d" % i)
            if pane is None:
                continue
            for t in range(pane.count()):
                if pane.getTabLabel(t) == label:
                    pane.setCurrentIndex(t)
                    QApplication.processEvents()
                    return True
        return False

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if not w.objectName() and w.isVisible()]
        return graphs[0] if graphs else None

    def open_graph(group):
        """Shows group's graph tab, diving into the group from the root graph (Shift+double-click)
        when it has none."""
        if show_graph_tab(group.getLabel()):
            return True
        root = root_graph()
        if root is None:
            return False
        # The dive-in appends the tab to the pane of the last focused graph.
        QApplication.sendEvent(root, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
        x, y = group.getPosition()[0:2]
        w, h = group.getSize()[0:2]
        centre = QPointF(x + w / 2.0, y + h / 2.0)
        root.centerOn(centre)
        QApplication.processEvents()
        pos = QPointF(root.mapFromScene(centre))
        event = QMouseEvent(QEvent.MouseButtonDblClick, pos, Qt.LeftButton, Qt.LeftButton, Qt.ShiftModifier)
        QApplication.sendEvent(root.viewport(), event)
        QApplication.processEvents()
        return show_graph_tab(group.getLabel())

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def status_text():
        win = main_window()
        return win.statusBar().currentMessage() if win is not None else ""

    def input_index(node, port):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == port:
                return i
        return -1

    def show(node, port):
        if port not in list(node.getPorts()):
            gt.require(node.showPort(port), "show %s.%s" % (node.getScriptName(), port))

    def input_source(dest, port):
        index = input_index(dest, port)
        src = dest.getInput(index) if index >= 0 else None
        return src.getScriptName() if src is not None else None

    def expect_connection(stage, dest, port, source, output):
        got = input_source(dest, port)
        check(got == (source.getScriptName() if source is not None else None),
              "%s: %s.%s is connected to %r (got %r)" % (stage, dest.getScriptName(), port,
                                                       source.getScriptName() if source else None, got))
        if source is not None:
            check(dest.getOutputForInput(port) == output,
                  "%s: %s.%s reads output %r (got %r)" % (stage, dest.getScriptName(), port, output,
                                                         dest.getOutputForInput(port)))

    def port_item(graph, builder, node, side, name):
        full = "%s.%s" % (builder.getScriptName(), node.getScriptName())
        for item in graph.scene().items():
            if item.data(0) == side and item.data(1) == name and item.data(2) == full and item.isVisible():
                return item
        return None

    def port_point(graph, builder, node, side, name):
        item = port_item(graph, builder, node, side, name)
        gt.require(item is not None, "%s has a %s port %s" % (node.getScriptName(), side, name))
        point = QPointF(graph.mapFromScene(item.scenePos()))
        gt.require(graph.viewport().rect().contains(point.toPoint()),
                   "%s.%s is in view (at %r)" % (node.getScriptName(), name, point))
        return point

    def highlight_of(graph, builder, node, side, name):
        item = port_item(graph, builder, node, side, name)
        return (item.data(3) or "") if item is not None else None

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def press(widget, pos):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)

    def move_to(widget, start, end, buttons=Qt.LeftButton):
        start, end = QPointF(start), QPointF(end)
        for i in range(1, DRAG_STEPS + 1):
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * (i / float(DRAG_STEPS)), Qt.NoButton, buttons)

    def release(widget, pos):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)

    def drag(widget, start, end):
        press(widget, start)
        move_to(widget, start, end)
        release(widget, end)

    def click(widget, pos):
        press(widget, pos)
        release(widget, pos)

    def main():
        gt.section("build")
        builder = create(BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        gt.require(open_graph(builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: graph_of(builder) is not None and graph_of(builder).isVisible(), 10.0)
        gt.require(ok, "the builder's graph is shown")
        texture = create(TEXTURE, 0, 0, builder)
        math = create(MATH, 0, 260, builder)
        bsdf = create(BSDF, 400, 0, builder)
        tinted = create(BSDF, 400, 300, builder)
        for port in ("base_color", "alpha", "roughness"):
            show(bsdf, port)
        show(tinted, "base_color")
        show(math, "value1")
        yield ("sleep", SETTLE_MS)
        graph = graph_of(builder)
        gt.require(graph is not None, "the builder's graph is still up")
        graph.centerOn(300, 220)
        yield ("sleep", SETTLE_MS)
        viewport = graph.viewport()

        def point(node, side, name):
            return port_point(graph, builder, node, side, name)

        def lit(node, side, name):
            return highlight_of(graph, builder, node, side, name)

        gt.section("drag onto a compatible port")
        start = point(texture, "output", "color")
        end = point(bsdf, "input", "base_color")
        press(viewport, start)
        move_to(viewport, start, end - QPointF(40, 0))
        yield ("sleep", SETTLE_MS)
        check(lit(bsdf, "input", "base_color") == "halo",
              "mid-drag, base_color has a halo (got %r)" % lit(bsdf, "input", "base_color"))
        check(lit(math, "output", "value") == "dimmed",
              "mid-drag, an output is dimmed for a pipe carried from an output (got %r)" % lit(math, "output", "value"))
        gt.shot("shader_graph_drag_halo.png")
        move_to(viewport, end - QPointF(40, 0), end)
        release(viewport, end)
        yield ("sleep", SETTLE_MS)
        expect_connection("compatible drop", bsdf, "base_color", texture, "color")
        check(lit(bsdf, "input", "base_color") == "", "the highlights are cleared after the drop")

        gt.section("drag onto an incompatible port")
        start = point(bsdf, "output", "BSDF")
        end = point(math, "input", "value1")
        press(viewport, start)
        move_to(viewport, start, end + QPointF(0, -30))
        yield ("sleep", SETTLE_MS)
        item = port_item(graph, builder, math, "input", "value1")
        check(lit(math, "input", "value1") == "dimmed",
              "mid-drag, value1 is dimmed (got %r)" % lit(math, "input", "value1"))
        check(item is not None and abs(item.opacity() - 0.3) < 0.05,
              "mid-drag, value1 is drawn at 30%% opacity (got %r)" % (item.opacity() if item else None))
        gt.shot("shader_graph_drag_dimmed.png")
        move_to(viewport, end + QPointF(0, -30), end)
        release(viewport, end)
        yield ("sleep", SETTLE_MS)
        message = status_text()
        report("NOTE status bar after the refused drop: %r" % message)
        check("closure cannot feed float" in message,
              "the status bar gives the reason (got %r)" % message)
        expect_connection("refused drop", math, "value1", None, None)
        expect_connection("refused drop", bsdf, "base_color", texture, "color")

        gt.section("pick a pipe up by its destination end")
        start = point(bsdf, "input", "base_color") - QPointF(PIPE_END_OFFSET, 0)
        end = point(tinted, "input", "base_color")
        drag(viewport, start, end)
        yield ("sleep", SETTLE_MS)
        expect_connection("moved destination", bsdf, "base_color", None, None)
        expect_connection("moved destination", tinted, "base_color", texture, "color")

        gt.section("pick a pipe up by its source end")
        start = point(texture, "output", "color") + QPointF(PIPE_END_OFFSET, 0)
        end = point(math, "output", "value")
        drag(viewport, start, end)
        yield ("sleep", SETTLE_MS)
        expect_connection("moved source", tinted, "base_color", math, "value")

        gt.section("click to pick up, click to drop")
        click(viewport, point(texture, "output", "alpha"))
        yield ("sleep", SETTLE_MS)
        expect_connection("held", bsdf, "alpha", None, None)
        target = point(bsdf, "input", "alpha")
        move_to(viewport, point(texture, "output", "alpha"), target, Qt.NoButton)
        click(viewport, target)
        yield ("sleep", SETTLE_MS)
        expect_connection("clicked", bsdf, "alpha", texture, "alpha")

        gt.section("drop on empty space")
        start = point(tinted, "input", "base_color")
        end = start + QPointF(-160, 160)
        drag(viewport, start, end)
        yield ("sleep", SETTLE_MS)
        expect_connection("dropped on empty space", tinted, "base_color", None, None)
        expect_connection("dropped on empty space", bsdf, "alpha", texture, "alpha")
        gt.shot("shader_graph_drag_end.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(main())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
