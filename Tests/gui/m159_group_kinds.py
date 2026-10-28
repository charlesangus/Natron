"""Checks that a group's external edges are drawn in the deep colour when it wraps a deep chain and
not when it wraps an image chain. Python exposes neither an Edge's pen nor the Preferences edge
colours, so the script samples screen pixels along each edge and compares them with the default
deep-edge colour; the image group's edges are the control that the probe is not seeing deep colour
everywhere.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 420
SETTLE_MS = 500
MARGIN = 120.0
MAX_ZOOM = 1.5
DEEP_RGB = (0, 114, 178)
COLOUR_TOLERANCE = 60
HIT_RATIO = 0.4

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
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

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def inner(group, plugin_id):
        return next((n for n in group.getChildren() if n.getPluginID() == plugin_id), None)

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def box(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def in_box(px, py, b, pad=3.0):
        return (b[0] - pad) <= px <= (b[0] + b[2] + pad) and (b[1] - pad) <= py <= (b[1] + b[3] + pad)

    def frame(graph, nodes):
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y, w, h = box(n)
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        bw = max(xe) - min(xs) + 2 * MARGIN
        bh = max(ye) - min(ys) + 2 * MARGIN
        vp = graph.viewport()
        want = min(MAX_ZOOM, vp.width() / bw, vp.height() / bh)
        graph.resetTransform()
        graph.scale(want, want)
        graph.centerOn((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0)
        QApplication.processEvents()

    def hover_away(graph):
        ev = QMouseEvent(QEvent.MouseMove, QPointF(2.0, 2.0), Qt.NoButton, Qt.NoButton, Qt.NoModifier)
        QApplication.sendEvent(graph.viewport(), ev)
        QApplication.processEvents()

    def viewport_image(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        return pix, pix.toImage()

    def near(img, px, py, rgb, radius=1):
        for dx in range(-radius, radius + 1):
            for dy in range(-radius, radius + 1):
                x, y = px + dx, py + dy
                if 0 <= x < img.width() and 0 <= y < img.height():
                    c = img.pixelColor(x, y)
                    if abs(c.red() - rgb[0]) + abs(c.green() - rgb[1]) + abs(c.blue() - rgb[2]) <= COLOUR_TOLERANCE:
                        return True
        return False

    def deep_ratio(graph, source, dest):
        """Fraction of the points on the segment between the boxes that are deep-coloured."""
        frame(graph, [source, dest])
        _pix, img = viewport_image(graph)
        sb, db = box(source), box(dest)
        scx, scy = sb[0] + sb[2] / 2.0, sb[1] + sb[3] / 2.0
        dcx, dcy = db[0] + db[2] / 2.0, db[1] + db[3] / 2.0
        hits = total = 0
        for i in range(1, 60):
            t = i / 60.0
            wx, wy = scx + (dcx - scx) * t, scy + (dcy - scy) * t
            if in_box(wx, wy, sb) or in_box(wx, wy, db):
                continue
            total += 1
            p = graph.mapFromScene(QPointF(wx, wy))
            if near(img, p.x(), p.y(), DEEP_RGB):
                hits += 1
        return hits / float(total) if total else -1.0

    def check_edge(graph, source, dest, label, deep):
        hover_away(graph)
        ratio = deep_ratio(graph, source, dest)
        ok = ratio >= HIT_RATIO if deep else (0.0 <= ratio < HIT_RATIO)
        return check(ok, "%s is %sdeep-coloured (%.2f of its points)" % (label, "" if deep else "not ", ratio))

    def graph_shot(graph, name):
        pix, _img = viewport_image(graph)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("deep-group")
        const = create("Constant", 0, 0)
        source = create("DeepFromImage", 250, 0)
        group = create("Group", 500, 0)
        consumer = create("DeepCrop", 800, 0)
        crop_in = create("DeepCrop", 250, 150, group=group)
        group_in = inner(group, "Input")
        group_out = inner(group, "Output")
        gt.require(group_in is not None and group_out is not None, "the group has its built-in Input and Output")
        group_out.disconnectInput(0)
        connect(crop_in, 0, group_in)
        connect(group_out, 0, crop_in)
        connect(source, 0, const)
        check(group.getMaxInputCount() >= 1, "the group exposes an input")
        connect(group, 0, source)
        connect(consumer, 0, group)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # The image control sits below, so it shares the screenshot.
        gt.section("image-group-control")
        const_img = create("Constant", 0, 400)
        group_img = create("Group", 500, 400)
        connect(group_img, 0, const_img)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # Its own band below the control keeps these edges clear of the earlier sections' edges.
        gt.section("multi-input")
        const_deep = create("Constant", 0, 800)
        source_multi = create("DeepFromImage", 250, 800)
        const_multi_img = create("Constant", 250, 1000)
        group_multi = create("Group", 600, 900)
        deep_in = inner(group_multi, "Input")
        multi_out = inner(group_multi, "Output")
        gt.require(deep_in is not None and multi_out is not None, "the multi-input group has its built-in Input and Output")
        multi_out.disconnectInput(0)
        image_in = create("Input", 450, 250, group=group_multi)
        deep_crop_multi = create("DeepCrop", 250, 150, group=group_multi)
        image_filter = create("Invert", 450, 150, group=group_multi)
        connect(deep_crop_multi, 0, deep_in)
        connect(image_filter, 0, image_in)
        connect(multi_out, 0, deep_crop_multi)
        check(group_multi.getMaxInputCount() >= 2, "the multi-input group exposes two inputs")
        connect(source_multi, 0, const_deep)
        connect(group_multi, 0, source_multi)
        connect(group_multi, 1, const_multi_img)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        graph = root_graph()
        gt.section("edge-colours")
        check_edge(graph, source, group, "DeepFromImage -> Group (external input edge)", True)
        check_edge(graph, group, consumer, "Group -> DeepCrop (external output edge)", True)
        check_edge(graph, const, source, "Constant -> DeepFromImage (image edge, control)", False)
        check_edge(graph, const_img, group_img, "Constant -> image Group (image edge, control)", False)

        gt.section("multi-input-edge-colours")
        check_edge(graph, source_multi, group_multi, "DeepFromImage -> multi-input Group input 1 (deep edge)", True)
        check_edge(graph, const_multi_img, group_multi, "Constant -> multi-input Group input 2 (image edge)", False)
        check_edge(graph, const_deep, source_multi, "Constant -> DeepFromImage (image edge, control)", False)

        frame(graph, [const, source, group, consumer, const_img, group_img])
        yield ("sleep", SETTLE_MS)
        graph_shot(graph, "m159-group-external-edges")
        frame(graph, [const_deep, source_multi, const_multi_img, group_multi])
        yield ("sleep", SETTLE_MS)
        graph_shot(graph, "m159-group-multi-input")
        gt.shot("m159-window.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
