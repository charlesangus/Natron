"""Checks that a node-graph pipe carrying geometry takes the Native 3D node colour.

Usage, from the repository root: Tests/gui/run-gui-test.sh Tests/gui/geometry_edge_colour.py

Graph: GeoPrimitive -> GeoTransform, stacked, nothing selected and nothing rendering.

Asserts:
  - the Geometry edges preference equals the Native 3D colour preference;
  - the pipe between the two nodes is drawn in that colour: across a strip at the pipe's middle,
    the drawn pixel closest to the preference is within a few levels of it.

Shot: geometry-edge-colour.png (the node graph around the pipe).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
WATCHDOG_S = 120
NODE_GAP = 300
STRIP_HALF_WIDTH = 8
# Antialiasing blends the pipe's edges with the background; its middle keeps the pen colour.
MAX_CHANNEL_ERROR = 12

try:
    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QColor

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        if not views:
            return None
        _held.append(views[0])
        return views[0]

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def colour_of(param):
        value = param.get()
        return QColor.fromRgbF(min(max(value.r, 0.0), 1.0), min(max(value.g, 0.0), 1.0),
                               min(max(value.b, 0.0), 1.0))

    def channel_error(a, b):
        return max(abs(a.red() - b.red()), abs(a.green() - b.green()), abs(a.blue() - b.blue()))

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("geometry edge colour")
        settings = NatronGui.natron.getSettings()
        native = settings.getParam("native3DNodesColor")
        edge_pref = settings.getParam("edgeGeometryColor")
        require(native is not None, "the Native 3D colour preference exists")
        require(edge_pref is not None, "the Geometry edges preference exists")
        expected = colour_of(native)
        check(colour_of(edge_pref).name() == expected.name(),
              "the Geometry edges preference equals the Native 3D colour %s (got %s)"
              % (expected.name(), colour_of(edge_pref).name()))

        source = create("GeoPrimitive", 0, 0)
        target = create("GeoTransform", 0, NODE_GAP)
        require(target.connectInput(0, source), "connect the GeoTransform to the GeoPrimitive")
        app.clearSelection()
        yield ("sleep", SETTLE_MS)

        sx, sy = source.getPosition()
        sw, sh = source.getSize()
        tx, ty = target.getPosition()
        tw, _ = target.getSize()
        middle = QPointF((sx + sw / 2.0 + tx + tw / 2.0) / 2.0, (sy + sh + ty) / 2.0)
        view = graph_view()
        view.centerOn(middle)
        yield ("sleep", SETTLE_MS)
        gt.shot("geometry-edge-colour.png")

        image = view.viewport().grab().toImage()
        centre = view.mapFromScene(middle)
        best = None
        for dy in (-4, 0, 4):
            for dx in range(-STRIP_HALF_WIDTH, STRIP_HALF_WIDTH + 1):
                x, y = centre.x() + dx, centre.y() + dy
                if not (0 <= x < image.width() and 0 <= y < image.height()):
                    continue
                pixel = image.pixelColor(x, y)
                if best is None or channel_error(pixel, expected) < channel_error(best, expected):
                    best = pixel
        require(best is not None, "the pipe's middle is inside the node graph view")
        check(channel_error(best, expected) <= MAX_CHANNEL_ERROR,
              "the GeoPrimitive -> GeoTransform pipe is drawn in the Native 3D colour %s (closest pixel %s)"
              % (expected.name(), best.name()))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.report("FAIL script error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
