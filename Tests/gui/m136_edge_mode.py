"""GUI check of the Edge Mode knob: a small source box built from a Constant feeds a Crop larger than
the source, a Transform and a Merge whose A is smaller than B. Each Edge Mode choice is applied in
turn; the node's info readout must name the resolved mode on its "Edge Mode:" line, and the viewer
pixels past the source's edge must show the edge repeated under Stretch and black under Black.

Run through Tests/gui/run-gui-test.sh under Xvfb at 1920x1080.
"""
import math
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
POLL_TIMEOUT_S = 20.0
PLUGIN_CONSTANT = "Constant"
PLUGIN_CROP = "Crop"
PLUGIN_TRANSFORM = "Transform"
PLUGIN_MERGE = "Merge"
VIEW_NAME = "Un-tone-mapped"
FORMAT_NAME = "m136frame"
FORMAT_SIZE = (320, 180)
# The source box, left, bottom, width, height, in the frame above.
SOURCE_BOX = (120, 60, 80, 60)
BIG_CROP = (60, 30, 200, 120)
CHOICES = ("input", "black", "stretch")
RED = (0.9, 0.0, 0.0)
BLUE = (0.0, 0.0, 0.9)
# Under the Un-tone-mapped view a 0.9 channel reads well above this and a 0 channel well below.
BRIGHT_MIN = 120
DARK_MAX = 40
# A half-pixel shift reads the edge pixel half from outside the source: under Black that halves it.
EDGE_DIMMING_MIN = 25
EDGE_MATCH_MAX = 14

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, QPoint
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up here die with a temporary wrapper of their parent.
    _held = []
    _viewer = []

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def constant(rgb, x=0, y=0):
        node = create(PLUGIN_CONSTANT, x, y)
        param(node, "color").set(rgb[0], rgb[1], rgb[2], 1.0)
        return node

    def set_option(node, name, wanted):
        p = param(node, name)
        options = [str(o).lower() for o in p.getOptions()]
        index = options.index(wanted) if wanted in options else next(
            (i for i, o in enumerate(options) if o.startswith(wanted)), -1)
        require(index >= 0, "%s's %s menu has %r (%r)" % (node.getLabel(), name, wanted, options))
        p.setValue(index)

    def crop_to(source, box, edge_mode, x=0, y=0):
        crop = create(PLUGIN_CROP, x, y)
        connect(crop, 0, source)
        set_option(crop, "extent", "size")
        param(crop, "reformat").setValue(False)
        param(crop, "softness").setValue(0.0)
        param(crop, "bottomLeft").set(box[0], box[1])
        param(crop, "size").set(box[2], box[3])
        set_option(crop, "edgeMode", edge_mode)
        return crop

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def visible_viewer():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

    def choose_view():
        for w in gt.widgets_of("ComboBox"):
            try:
                if not w.isVisible():
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
                if VIEW_NAME in items:
                    _held.append(w)
                    QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(VIEW_NAME)))
                    QApplication.processEvents()
                    return str(w.property("natronCurrentText")) == VIEW_NAME
            except RuntimeError:
                continue
        return False

    def frame_all():
        for tab in gt.widgets_of("ViewerTab"):
            try:
                if tab.isVisible() and tab.metaObject().indexOfMethod("centerViewer()") >= 0:
                    QMetaObject.invokeMethod(tab, "centerViewer")
                    QApplication.processEvents()
                    return True
            except RuntimeError:
                continue
        return False

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        widget = visible_viewer()
        value = widget.property("natronCanonicalToWidget") if widget is not None else None
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def sample(x, y):
        """The on-screen colour of the viewer at canonical (x, y), or None."""
        m = mapping()
        widget = visible_viewer()
        if m is None or widget is None:
            return None
        QApplication.processEvents()
        g = widget.mapToGlobal(QPoint(int(round(m[0] + m[2] * x)), int(round(m[1] + m[3] * y))))
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        if not (0 <= g.x() < image.width() and 0 <= g.y() < image.height()):
            return None
        c = image.pixelColor(g.x(), g.y())
        return (c.red(), c.green(), c.blue())

    def brightest_red_in_pixel(px, y):
        """The largest red of the screen pixels wholly inside image pixel column px at canonical y,
        or None. The viewer outlines the region of definition with a grey dashed line, which can run
        through the middle of the column; the brightest screen pixel is one the line misses."""
        m = mapping()
        widget = visible_viewer()
        if m is None or widget is None:
            return None
        QApplication.processEvents()
        a, b = sorted((m[0] + m[2] * px, m[0] + m[2] * (px + 1)))
        columns = list(range(int(math.ceil(a)), int(math.floor(b))))
        if not columns:
            columns = [int(round(0.5 * (a + b)))]
        row = int(round(m[1] + m[3] * y))
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        reds = []
        for column in columns:
            g = widget.mapToGlobal(QPoint(column, row))
            if 0 <= g.x() < image.width() and 0 <= g.y() < image.height():
                reds.append(image.pixelColor(g.x(), g.y()).red())
        return max(reds) if reds else None

    def is_red(c):
        return c is not None and c[0] > BRIGHT_MIN and c[1] < DARK_MAX and c[2] < DARK_MAX

    def is_blue(c):
        return c is not None and c[2] > BRIGHT_MIN and c[0] < DARK_MAX and c[1] < DARK_MAX

    def is_dark(c):
        return c is not None and max(c) < DARK_MAX

    def edge_modes(node):
        """The info readout's "Edge Mode:" values by block, keyed by input label and "Output"."""
        param(node, "refreshButton").trigger()
        QApplication.processEvents()
        text = str(param(node, "nodeInfos").getValue())
        modes = {}
        blocks = re.split(r'<b><font color="orange">', text)
        for block in blocks[1:]:
            label = re.match(r"(.*?):</font>", block)
            mode = re.search(r"Edge Mode:</b>\s*<font[^>]*>(\w+)<", block)
            if label and mode:
                modes[label.group(1)] = mode.group(1).lower()
        return modes

    def resolved(choice, input_mode):
        return input_mode if choice == "input" else choice

    def shot_name(node_name, source_mode, choice):
        return "m136-%s-source-%s-%s.png" % (node_name, source_mode, choice)

    def expect_state(label, node, block, want_mode, pixels_ok, describe):
        ok = yield ("until", lambda: edge_modes(node).get(block) == want_mode, POLL_TIMEOUT_S)
        check(ok, "%s: the %s block of the info readout says Edge Mode %s (%r)"
              % (label, block, want_mode, edge_modes(node)))
        ok = yield ("until", pixels_ok, POLL_TIMEOUT_S)
        check(ok, "%s: %s" % (label, describe()))

    def cycle(node_name, node, source_crop, block, knob_node, pixels_for):
        """Each Edge Mode choice on knob_node over a Stretch source, then Input over a Black one."""
        states = [("stretch", c) for c in CHOICES] + [("black", "input")]
        for source_mode, choice in states:
            set_option(source_crop, "edgeMode", source_mode)
            set_option(knob_node, "edgeMode", choice)
            want = resolved(choice, source_mode)
            label = "%s, %s over a %s source" % (node_name, choice, source_mode)
            pixels_ok, describe = pixels_for(want)
            yield from expect_state(label, node, block, want, pixels_ok, describe)
            yield ("sleep", SETTLE_MS)
            gt.shot(shot_name(node_name, source_mode, choice))

    def past_the_edge(points, stretched_test, black_test, stretched_name, black_name):
        def pixels_for(mode):
            test = stretched_test if mode == "stretch" else black_test
            name = stretched_name if mode == "stretch" else black_name

            def ok():
                return all(test(sample(x, y)) for x, y in points)

            def describe():
                return "past the source's edge the viewer shows %s (%r)" % (name, [sample(x, y) for x, y in points])

            return ok, describe

        return pixels_for

    def section_crop(source_crop):
        gt.section("crop larger than the source")
        crop = crop_to(source_crop, BIG_CROP, "input", 0, 200)
        show(crop)
        frame_all()
        yield ("sleep", SETTLE_MS)
        left, bottom, width, height = SOURCE_BOX
        points = [(left - 30.5, bottom + 30.5), (left + width + 40.5, bottom + height + 20.5)]
        yield from cycle("crop", crop, source_crop, "Output", crop,
                         past_the_edge(points, is_red, is_dark, "the edge repeated, red", "black"))

    def section_transform(source_crop):
        gt.section("transform")
        transform = create(PLUGIN_TRANSFORM, 200, 200)
        connect(transform, 0, source_crop)
        # Shifting by half a pixel makes the first column read half from past the source.
        param(transform, "translate").set(0.5, 0.0)
        show(transform)
        frame_all()
        yield ("sleep", SETTLE_MS)
        left, bottom, width, height = SOURCE_BOX
        row = bottom + height / 2.0 + 0.5
        edge = (left, row)
        inside = (left + width // 2, row)

        def pixels_for(mode):
            def ok():
                e, i = brightest_red_in_pixel(*edge), brightest_red_in_pixel(*inside)
                if e is None or i is None or i <= BRIGHT_MIN:
                    return False
                return (abs(i - e) <= EDGE_MATCH_MAX) if mode == "stretch" else (i - e >= EDGE_DIMMING_MIN)

            def describe():
                return ("the first column is %s (edge red %r, inside red %r, edge %r)"
                        % ("as bright as the inside" if mode == "stretch" else "dimmed by the black outside",
                           brightest_red_in_pixel(*edge), brightest_red_in_pixel(*inside), sample(left + 0.5, row)))

            return ok, describe

        yield from cycle("transform", transform, source_crop, "Output", transform, pixels_for)

    def section_merge(source_crop, plate):
        gt.section("merge A smaller than B")
        merge = create(PLUGIN_MERGE, 400, 200)
        connect(merge, 0, plate)
        connect(merge, 1, source_crop)
        set_option(merge, "operation", "over")
        show(merge)
        frame_all()
        yield ("sleep", SETTLE_MS)
        left, bottom, width, height = SOURCE_BOX
        points = [(left - 60.5, bottom - 30.5), (left + width + 60.5, bottom + height + 30.5)]
        pixels_for = past_the_edge(points, is_red, is_blue, "A's edge repeated over B, red", "B, blue")

        # Merge has no Edge Mode knob: A's own mode, set on the Crop feeding it, governs the reads past A.
        for source_mode in ("stretch", "black"):
            set_option(source_crop, "edgeMode", source_mode)
            label = "merge, A %s" % source_mode
            pixels_ok, describe = pixels_for(source_mode)
            yield from expect_state(label, merge, "A", source_mode, pixels_ok, describe)
            yield ("sleep", SETTLE_MS)
            gt.shot("m136-merge-a-%s.png" % source_mode)
        check(merge.getParam("edgeMode") is None, "Merge has no Edge Mode knob of its own")

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        app.addFormat("%s %dx%d" % (FORMAT_NAME, FORMAT_SIZE[0], FORMAT_SIZE[1]))
        fmt = app.getProjectParam("outputFormat")
        options = [str(o) for o in fmt.getOptions()]
        index = next((i for i, o in enumerate(options) if FORMAT_NAME in o), -1)
        require(index >= 0, "the project offers the %s format (%r)" % (FORMAT_NAME, options))
        fmt.setValue(index)
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        red = constant(RED, 0, 0)
        plate = constant(BLUE, 400, 0)
        source_crop = crop_to(red, SOURCE_BOX, "stretch", 0, 100)
        show(source_crop)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        check(frame_all(), "the viewer frames the format")
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer publishes natronCanonicalToWidget")
        m = mapping()
        check(m[2] >= 1.0, "the viewer shows at least one screen pixel per image pixel (scale %r)" % (m[2],))
        report("NOTE the Constant's own edge mode is %r" % (edge_modes(red).get("Output"),))

        yield from section_crop(source_crop)
        yield from section_transform(source_crop)
        yield from section_merge(source_crop, plate)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
