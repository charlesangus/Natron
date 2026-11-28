"""A GUI test that shader nodes inside a USD Material Builder are drawn as port lists. It is driven
by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m351-gui/nodes GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_nodes.py <AppImage or build/fast/App/Natron>'

The script creates a builder, shows its graph, and creates a Principled BSDF and a
DwaBaseMaterial in it. For each shader node, the port items in the scene must match its ports
(shown and port-only) and its manifest outputs, one item each. A screenshot is taken per node.

Then, on the BSDF:
- title: a long label is elided with "..." inside the header, the full label in the tool tip, at
  three zoom levels; a short label is drawn whole, and renaming back to it shrinks the node;
- selection: the selection outline clears every port, unless NATRON_PORT_SELECTION_STYLE picks a
  look drawn on the body's edge; screenshots at 1x and zoomed out (run the script on an earlier
  build for the "before" shots: it takes them before checking);
- cursor: hovering an input port shows the crosshair, the node's body the open hand;
- port order: the input ports are drawn in manifest attribute order through show, hide and show
  again, whatever order the inputs are in.

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
LONG_LABEL = "Principled_BSDF_with_a_name_much_longer_than_any_header_could_hold"
SHORT_LABEL = "Skin"
ZOOMS = (1.0, 0.5, 2.0)
SELECTION_ZOOMS = (1.0, 0.4)
PORT_RADIUS = 5.0
DIAMOND_REACH = PORT_RADIUS * 1.3
SHOWN_PORT = "diffuse_roughness"

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

    def manifest_attributes(plugin_id, manifest):
        with open(os.path.join(MANIFESTS, manifest)) as f:
            nodes = json.load(f)["nodes"]
        for node in nodes:
            if node["id"] == plugin_id:
                return [a["name"] for a in node.get("attributes", [])]
        return None

    def items_of(graph, side, node_name):
        return [item for item in graph.scene().items()
                if item.data(0) == side and item.data(2) == node_name]

    def drawn_input_order(graph, node_name):
        """The input port names of node_name, top to bottom as drawn."""
        found = [item for item in items_of(graph, "input", node_name) if item.isVisible()]
        found.sort(key=lambda item: item.scenePos().y())
        return [item.data(1) for item in found]

    def node_rect(node):
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        return x, y, w, h

    def set_zoom(graph, zoom):
        graph.resetTransform()
        graph.scale(zoom, zoom)
        QApplication.processEvents()

    def hover(graph, scene_pos):
        """Moves the mouse over scene_pos with no button down and returns the graph's cursor."""
        pos = QPointF(graph.mapFromScene(scene_pos))
        event = QMouseEvent(QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton, Qt.NoModifier)
        QApplication.sendEvent(graph.viewport(), event)
        QApplication.processEvents()
        return graph.cursor().shape()

    def header_bottom(graph, node_name):
        """The scene y the node's header ends at: half a row above its first input row."""
        ys = sorted(item.scenePos().y() for item in items_of(graph, "input", node_name) if item.isVisible())
        if len(ys) < 2:
            return None
        return ys[0] - (ys[1] - ys[0]) / 2.0

    def check_title(graph, node, name, label, zoom):
        node.setLabel(label)
        QApplication.processEvents()
        titles = items_of(graph, "title", name)
        if not check(len(titles) == 1, "zoom %g: the node has one title item" % zoom):
            return None
        title = titles[0]
        x, y, w, h = node_rect(node)
        bottom = header_bottom(graph, name)
        r = title.sceneBoundingRect()
        text = title.data(1)
        check(bottom is not None and r.left() >= x and r.right() <= x + w and r.top() >= y and r.bottom() <= bottom + 0.5,
              "zoom %g: the title %r stays inside the header (title %r, node %r, header bottom %r)"
              % (zoom, label, (r.left(), r.top(), r.width(), r.height()), (x, y, w, h), bottom))
        check(title.toolTip() == label, "zoom %g: the tool tip is the full label (got %r)" % (zoom, title.toolTip()))
        return text

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
                  "%s draws one input port per shown or port-only port (%d drawn, %d ports)"
                  % (plugin_id, len(items["input"]), len(ports)))
            check(sorted(items["output"]) == sorted(outputs),
                  "%s draws one output port per output (drawn %r)" % (plugin_id, items["output"]))
            for i in range(node.getMaxInputCount()):
                check(node.getInputLabel(i) == ports[i] if i < len(ports) else False,
                      "%s input %d is the port %r" % (plugin_id, i, ports[i] if i < len(ports) else None))
            frame(graph, node)
            yield ("sleep", SETTLE_MS)
            gt.shot("shader_graph_nodes_%s.png" % plugin_id)

        graph = graph_of(builder)
        bsdf, bsdf_id, bsdf_manifest = shaders[0]
        bsdf_name = "%s.%s" % (builder.getScriptName(), bsdf.getScriptName())
        original_label = bsdf.getLabel()

        gt.section("title")
        for zoom in ZOOMS:
            set_zoom(graph, zoom)
            frame(graph, bsdf)
            text = check_title(graph, bsdf, bsdf_name, LONG_LABEL, zoom)
            yield ("sleep", SETTLE_MS)
            gt.shot("shader_graph_nodes_long_title_%g.png" % zoom)
            if text is not None:
                check(text.endswith("\u2026") and len(text) < len(LONG_LABEL),
                      "zoom %g: the long label is elided (drawn %r)" % (zoom, text))
        set_zoom(graph, 1.0)
        long_width = node_rect(bsdf)[2]
        text = check_title(graph, bsdf, bsdf_name, SHORT_LABEL, 1.0)
        check(text == SHORT_LABEL, "a short label is drawn whole (drawn %r)" % text)
        check(node_rect(bsdf)[2] <= long_width, "renaming to a short label does not widen the node")
        bsdf.setLabel(original_label)
        QApplication.processEvents()

        gt.section("selection")
        app.selectNode(bsdf, True)
        frame(graph, bsdf)
        yield ("sleep", SETTLE_MS)
        for zoom in SELECTION_ZOOMS:
            set_zoom(graph, zoom)
            frame(graph, bsdf)
            yield ("sleep", SETTLE_MS)
            gt.shot("shader_graph_nodes_selected_%g.png" % zoom)
        set_zoom(graph, 1.0)
        outlines = [item for item in items_of(graph, "selection", bsdf_name) if item.isVisible()]
        # The broken and header looks draw on the body's edge, through the ports' column.
        on_edge = os.environ.get("NATRON_PORT_SELECTION_STYLE", "").strip().lower() in ("2", "broken", "3", "header")
        if check(len(outlines) == 1, "the selected node shows one selection outline") and not on_edge:
            outline = outlines[0].sceneBoundingRect()
            ports = items_of(graph, "input", bsdf_name) + items_of(graph, "output", bsdf_name)
            xs = [item.scenePos().x() for item in ports]
            check(bool(xs) and outline.left() < min(xs) - DIAMOND_REACH and outline.right() > max(xs) + DIAMOND_REACH,
                  "the outline runs outside the ports (outline x %.1f..%.1f, ports x %.1f..%.1f)"
                  % (outline.left(), outline.right(), min(xs) if xs else 0, max(xs) if xs else 0))
        app.clearSelection(builder)
        QApplication.processEvents()
        check(not any(item.isVisible() for item in items_of(graph, "selection", bsdf_name)),
              "deselecting hides the outline")

        gt.section("cursor")
        frame(graph, bsdf)
        inputs = items_of(graph, "input", bsdf_name)
        if check(bool(inputs), "the BSDF has input ports to hover"):
            shape = hover(graph, inputs[0].scenePos())
            check(shape == Qt.CrossCursor, "the cursor over a port is the crosshair (got %r)" % shape)
            x, y, w, h = node_rect(bsdf)
            shape = hover(graph, QPointF(x + w / 2.0, y + h / 2.0))
            check(shape == Qt.OpenHandCursor, "the cursor over the node's body is the open hand (got %r)" % shape)

        gt.section("port order")
        schema = manifest_attributes(bsdf_id, bsdf_manifest)
        gt.require(schema is not None, "%s is in %s" % (bsdf_id, bsdf_manifest))

        def in_schema_order(order):
            return order == sorted(order, key=schema.index)

        first = drawn_input_order(graph, bsdf_name)
        check(in_schema_order(first), "the default ports are drawn in attribute order (%r)" % first)
        gt.require(SHOWN_PORT not in list(bsdf.getPorts()), "%s starts hidden" % SHOWN_PORT)
        gt.require(bsdf.showPort(SHOWN_PORT), "show %s" % SHOWN_PORT)
        QApplication.processEvents()
        shown = drawn_input_order(graph, bsdf_name)
        check(in_schema_order(shown), "after showing %s the ports are drawn in attribute order (%r)" % (SHOWN_PORT, shown))
        gt.require(bsdf.hidePort(SHOWN_PORT), "hide %s" % SHOWN_PORT)
        QApplication.processEvents()
        check(drawn_input_order(graph, bsdf_name) == first, "hiding %s restores the first order" % SHOWN_PORT)
        gt.require(bsdf.showPort(SHOWN_PORT), "show %s again" % SHOWN_PORT)
        QApplication.processEvents()
        check(drawn_input_order(graph, bsdf_name) == shown, "showing %s again draws the same order" % SHOWN_PORT)
        default = next((port for port in first if bsdf.hidePort(port)), None)
        gt.require(default is not None, "one of the default ports can be hidden")
        gt.require(bsdf.showPort(default), "show %s again" % default)
        QApplication.processEvents()
        report("NOTE input order after re-showing %s: %r" % (default, list(bsdf.getPorts())))
        check(drawn_input_order(graph, bsdf_name) == shown,
              "re-showing %s keeps the drawn order although it is now the last input" % default)
        frame(graph, bsdf)
        yield ("sleep", SETTLE_MS)
        gt.shot("shader_graph_nodes_port_order.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(main())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
