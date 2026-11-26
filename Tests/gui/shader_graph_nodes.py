"""A GUI test that shader nodes inside a USD Material Builder are drawn as port lists. It is driven
by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m351-gui/nodes GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_nodes.py <AppImage or build/fast/App/Natron>'

The script creates a builder, shows its graph, and creates a Principled BSDF and a
DwaBaseMaterial in it. For each shader node, the port items in the scene must match its ports
(promoted and port-only) and its manifest outputs, one item each. A screenshot is taken per node.

Port items carry their side, port name and node's fully qualified name in data() keys 0, 1 and 2.
Scene items are read through the graph's scene() and are never walked up with parentItem().
"""
import json
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
SETTLE_MS = 500
MAX_PANES = 40
BUILDER = "USDMaterialBuilder"
SHADERS = (("cycles_principled_bsdf", "cycles.json", 0, 0), ("DwaBaseMaterial", "moonray.json", 500, 0))

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

    MANIFESTS = os.path.join(gt.ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")

    def manifest_outputs(plugin_id, manifest):
        with open(os.path.join(MANIFESTS, manifest)) as f:
            nodes = json.load(f)["nodes"]
        for node in nodes:
            if node["id"] == plugin_id:
                return [o["name"] for o in node.get("outputs", [])]
        return None

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

    def port_items(graph, node_name):
        """{"input": [names], "output": [names]} of the visible port items of node_name."""
        found = {"input": [], "output": []}
        for item in graph.scene().items():
            side = item.data(0)
            if side not in found or item.data(2) != node_name or not item.isVisible():
                continue
            found[side].append(item.data(1))
        return found

    def frame(graph, node):
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()

    def main():
        gt.section("builder")
        builder = create(BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        gt.require(open_graph(builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: graph_of(builder) is not None and graph_of(builder).isVisible(), 10.0)
        gt.require(ok, "the builder's graph is shown")

        shaders = []
        for plugin_id, manifest, x, y in SHADERS:
            shaders.append((create(plugin_id, x, y, builder), plugin_id, manifest))
        yield ("sleep", SETTLE_MS)

        for node, plugin_id, manifest in shaders:
            gt.section(plugin_id)
            graph = graph_of(builder)
            gt.require(graph is not None, "the builder's graph is still up")
            name = "%s.%s" % (builder.getScriptName(), node.getScriptName())
            ports = list(node.getPorts())
            outputs = manifest_outputs(plugin_id, manifest)
            gt.require(outputs is not None, "%s is in %s" % (plugin_id, manifest))
            items = port_items(graph, name)
            report("NOTE %s ports %r outputs %r" % (plugin_id, ports, outputs))
            check(len(ports) > 0, "%s has ports" % plugin_id)
            check(sorted(items["input"]) == sorted(ports),
                  "%s draws one input port per promoted or port-only port (%d drawn, %d ports)"
                  % (plugin_id, len(items["input"]), len(ports)))
            check(sorted(items["output"]) == sorted(outputs),
                  "%s draws one output port per output (drawn %r)" % (plugin_id, items["output"]))
            for i in range(node.getMaxInputCount()):
                check(node.getInputLabel(i) == ports[i] if i < len(ports) else False,
                      "%s input %d is the port %r" % (plugin_id, i, ports[i] if i < len(ports) else None))
            frame(graph, node)
            yield ("sleep", SETTLE_MS)
            gt.shot("shader_graph_nodes_%s.png" % plugin_id)

        faulthandler.cancel_dump_traceback_later()

    gt.run(main())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
