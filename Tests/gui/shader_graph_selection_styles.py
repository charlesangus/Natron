"""A GUI script that screenshots each selection look of shader nodes, for comparing them side by
side. For every value of NATRON_PORT_SELECTION_STYLE it builds, inside a USD Material Builder, an
Image Texture whose color output feeds a Principled BSDF's base_color, selects the BSDF, and saves
selection-<n>-<name>-<zoom>x.png at 1x and zoomed out to $NATRON_GUI_TEST_OUT. The nodes are built
after the variable is set, as the port positions follow the look. It is driven by
Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/selection GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_selection_styles.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
SETTLE_MS = 500
STYLE_VARIABLE = "NATRON_PORT_SELECTION_STYLE"
STYLES = ((1, "inset"), (2, "broken"), (3, "header"), (4, "halo"), (5, "outside"))
ZOOMS = (1.0, 0.4)
TEXTURE = "cycles_image_texture"
BSDF = "cycles_principled_bsdf"
TEXTURE_X = 0
BSDF_X = 350
INSET_MIN = 4.0
EDGE_TOLERANCE = 1.0

try:
    import faulthandler

    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def items_of(graph, side, node_name):
        return [item for item in graph.scene().items() if item.data(0) == side and item.data(2) == node_name]

    def set_zoom(graph, zoom):
        graph.resetTransform()
        graph.scale(zoom, zoom)
        QApplication.processEvents()

    def frame_pair(graph, left, right):
        lx, ly = left.getPosition()[0:2]
        rx, ry = right.getPosition()[0:2]
        rw, rh = right.getSize()[0:2]
        graph.centerOn((lx + rx + rw) / 2.0, (min(ly, ry) + max(ly, ry) + rh) / 2.0)
        QApplication.processEvents()

    def check_ports(graph, node, node_name, inset, label):
        x = node.getPosition()[0]
        w = node.getSize()[0]
        inputs = [item.scenePos().x() - x for item in items_of(graph, "input", node_name) if item.isVisible()]
        outputs = [x + w - item.scenePos().x() for item in items_of(graph, "output", node_name) if item.isVisible()]
        offsets = inputs + outputs
        if not check(bool(offsets), "%s: %s draws its ports" % (label, node_name)):
            return
        if inset:
            check(min(offsets) >= INSET_MIN, "%s: the ports of %s sit inside the body (offsets %r)"
                  % (label, node_name, sorted(set(round(o, 1) for o in offsets))))
        else:
            check(max(abs(o) for o in offsets) <= EDGE_TOLERANCE, "%s: the ports of %s sit on the body's sides (offsets %r)"
                  % (label, node_name, sorted(set(round(o, 1) for o in offsets))))

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, 60.0)
        sg.park_viewer(app)
        require(ok, "the main window is up")

        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        graph = sg.graph_of(builder)

        for number, name in STYLES:
            label = "style %d (%s)" % (number, name)
            gt.section(label)
            os.environ[STYLE_VARIABLE] = str(number)
            texture = sg.create(app, TEXTURE, TEXTURE_X, 0, builder)
            bsdf = sg.create(app, BSDF, BSDF_X, 0, builder)
            sg.connect(texture, "color", bsdf, "base_color")
            app.clearSelection(builder)
            app.selectNode(bsdf, True)
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)

            bsdf_name = "%s.%s" % (builder.getScriptName(), bsdf.getScriptName())
            texture_name = "%s.%s" % (builder.getScriptName(), texture.getScriptName())
            outlines = [item for item in items_of(graph, "selection", bsdf_name) if item.isVisible()]
            check(len(outlines) == 1, "%s: the selected node shows its selection look" % label)
            for node, node_name in ((bsdf, bsdf_name), (texture, texture_name)):
                check_ports(graph, node, node_name, number == 1, label)

            for zoom in ZOOMS:
                set_zoom(graph, zoom)
                frame_pair(graph, texture, bsdf)
                yield ("sleep", SETTLE_MS)
                gt.shot("selection-%d-%s-%gx.png" % (number, name, zoom))
            set_zoom(graph, 1.0)

            app.clearSelection(builder)
            bsdf.destroy(False)
            texture.destroy(False)
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)

        del os.environ[STYLE_VARIABLE]
        report("NOTE the screenshots are in %s" % gt.OUT)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
