"""A GUI test of the pipes between shader nodes inside a USD Material Builder. It is driven by
Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m351-gui/edges GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_edges.py <AppImage or build/fast/App/Natron>'

One image texture feeds a Principled BSDF from both of its outputs: color into base_color and
alpha into alpha. A math node's float value feeds a second BSDF's base_color, a colour port. The
nodes are moved, the project is saved, reset and reloaded, and every connection must come back
with the output it reads. A screenshot of the reloaded graph shows the pipes.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
SETTLE_MS = 500
LOAD_TIMEOUT_S = 30.0
MAX_PANES = 40
BUILDER = "USDMaterialBuilder"
TEXTURE = "cycles_image_texture"
MATH = "cycles_math"
BSDF = "cycles_principled_bsdf"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QFocusEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication

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
        when it has none: a group made without a focused graph, or loaded from a project, gets its
        graph without a tab."""
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

    def input_index(node, port):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == port:
                return i
        return -1

    def connect(source, output, dest, port):
        """Connects output of source to the port of dest, promoting the port first if needed."""
        if port not in list(dest.getPorts()):
            gt.require(dest.promotePort(port), "promote %s.%s" % (dest.getScriptName(), port))
        index = input_index(dest, port)
        gt.require(index >= 0, "%s has an input for %s" % (dest.getScriptName(), port))
        gt.require(dest.connectInput(index, source), "connect %s to %s.%s"
                   % (source.getScriptName(), dest.getScriptName(), port))
        gt.require(dest.setOutputForInput(port, output), "%s.%s reads %s.%s"
                   % (dest.getScriptName(), port, source.getScriptName(), output))

    def input_source(dest, port):
        index = input_index(dest, port)
        src = dest.getInput(index) if index >= 0 else None
        return src.getScriptName() if src is not None else None

    def check_connections(stage, connections):
        for source, output, dest, port in connections:
            check(input_source(dest, port) == source,
                  "%s: %s.%s is connected to %s (got %r)" % (stage, dest.getScriptName(), port, source,
                                                           input_source(dest, port)))
            check(dest.getOutputForInput(port) == output,
                  "%s: %s.%s reads output %r (got %r)" % (stage, dest.getScriptName(), port, output,
                                                         dest.getOutputForInput(port)))

    def main():
        gt.section("build")
        builder = create(BUILDER, 0, 0)
        builder_name = builder.getScriptName()
        yield ("sleep", SETTLE_MS)
        gt.require(open_graph(builder), "the builder's graph has a tab")
        texture = create(TEXTURE, 0, 0, builder)
        math = create(MATH, 0, 300, builder)
        bsdf = create(BSDF, 450, 0, builder)
        tinted = create(BSDF, 450, 400, builder)
        yield ("sleep", SETTLE_MS)
        connect(texture, "color", bsdf, "base_color")
        connect(texture, "alpha", bsdf, "alpha")
        connect(math, "value", tinted, "base_color")
        yield ("sleep", SETTLE_MS)
        names = (texture.getScriptName(), math.getScriptName(), bsdf.getScriptName(), tinted.getScriptName())
        connections = ((names[0], "color", bsdf, "base_color"),
                       (names[0], "alpha", bsdf, "alpha"),
                       (names[1], "value", tinted, "base_color"))
        check_connections("connected", connections)

        gt.section("move")
        texture.setPosition(-150, 120)
        math.setPosition(-100, 450)
        bsdf.setPosition(500, -60)
        yield ("sleep", SETTLE_MS)
        check_connections("moved", connections)
        gt.shot("shader_graph_edges_before_save.png")

        gt.section("reload")
        path = os.path.join(gt.OUT, "shader_graph_edges.ntpz")
        gt.require(app.saveProjectAs(path), "saved the project to " + path)
        gt.require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        host = app.loadProject(path)
        gt.require(host is not None, "reloaded " + path)
        ok = yield ("until", lambda: host.getNode(builder_name) is not None, LOAD_TIMEOUT_S)
        gt.require(ok, "the builder is back after reloading")
        reloaded = host.getNode(builder_name)
        yield ("sleep", SETTLE_MS)
        nodes = [reloaded.getNode(n) for n in names]
        gt.require(all(n is not None for n in nodes), "every shader node is back in the builder")
        check_connections("reloaded", ((names[0], "color", nodes[2], "base_color"),
                                       (names[0], "alpha", nodes[2], "alpha"),
                                       (names[1], "value", nodes[3], "base_color")))
        x, y = nodes[0].getPosition()[0:2]
        check(abs(x + 150) < 1 and abs(y - 120) < 1, "the texture kept its moved position (got %r, %r)" % (x, y))
        gt.require(open_graph(reloaded), "the reloaded builder's graph has a tab")
        ok = yield ("until", lambda: graph_of(reloaded) is not None and graph_of(reloaded).isVisible(), 10.0)
        gt.require(ok, "the reloaded builder's graph is shown")
        graph = graph_of(reloaded)
        graph.centerOn(200, 200)
        yield ("sleep", SETTLE_MS)
        report("NOTE pipes are checked by eye in the screenshot")
        gt.shot("shader_graph_edges_reloaded.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(main())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
