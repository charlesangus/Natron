"""Pixel goldens for the classic node graph connectors (M351.P4.T1).

Each case builds a small graph at fixed positions, renders the NodeGraph's QGraphicsScene (nodes and
edges, no viewport chrome) at a fixed scale into a PNG crop of at most 256 px a side, and compares it
exactly against Tests/fixtures/graph_connectors/<case>.png. A mismatch fails naming the case and
writes <case>-actual.png next to results.txt. M351.P4.T2 moves the edge anchor maths into a layout
class and must leave these images pixel-identical.

Recording: a case whose golden is missing is recorded instead of compared (NOTE RECORDED). Setting
GUI_TEST_RECORD_GOLDENS=1 re-records every case; NATRON_GOLDEN_RECORD_DIR, when set, redirects the
recorded files to $NATRON_GOLDEN_RECORD_DIR/graph_connectors. Recorded files are always copied to
$NATRON_GUI_TEST_OUT/graph_connectors-recorded too, for a run that cannot write the checkout. No
other GUI script records goldens, so there is no shared helper to reuse.

Cases whose state Python cannot set directly are driven through the real input path: selection
through app.selectNode, bend points through the Control key press, and the highlight through the
Ctrl-drag cut line (which highlights the pipes it crosses). The rendering-coloured edge is not
covered: it is a transient state latched from a finished render's input execution counts, so it
cannot be held still for an exact comparison.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 420
SETTLE_MS = 500
RENDER_SCALE = 0.75
CROP_PX = 256
BACKGROUND = (50, 50, 50)
COL_STEP = 110.0
ROW_STEP = 130.0
BOTTOM_PAD = 30.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
    from PySide6.QtGui import QColor, QImage, QKeyEvent, QMouseEvent, QPainter
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    GOLDEN_DIR = os.path.join(gt.FIXTURES, "graph_connectors")
    RECORD_ALL = bool(os.environ.get("GUI_TEST_RECORD_GOLDENS"))
    RECORD_DIR = os.path.join(os.environ["NATRON_GOLDEN_RECORD_DIR"], "graph_connectors") \
        if os.environ.get("NATRON_GOLDEN_RECORD_DIR") else GOLDEN_DIR
    RECORDED_COPY_DIR = os.path.join(gt.OUT, "graph_connectors-recorded")

    # Wrappers of widgets looked up here die with a temporary wrapper of their parent.
    _held = []

    def create(plugin_id, name, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setScriptName(name)
        node.setLabel(name)
        node.setPosition(x, y)
        return node

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getScriptName(), input_nb, source.getScriptName()))

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def box(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def case_rect(nodes):
        xs = [box(n)[0] for n in nodes]
        ys = [box(n)[1] for n in nodes]
        xe = [box(n)[0] + box(n)[2] for n in nodes]
        ye = [box(n)[1] + box(n)[3] for n in nodes]
        side = CROP_PX / RENDER_SCALE
        cx = (min(xs) + max(xe)) / 2.0
        cy = (min(ys) + max(ye) + BOTTOM_PAD) / 2.0
        width = max(xe) - min(xs)
        height = max(ye) - min(ys) + BOTTOM_PAD
        fits = width <= side and height <= side
        return QRectF(cx - side / 2.0, cy - side / 2.0, side, side), fits

    def render_scene(graph, nodes):
        rect, fits = case_rect(nodes)
        QApplication.processEvents()
        image = QImage(CROP_PX, CROP_PX, QImage.Format_ARGB32)
        image.fill(QColor(*BACKGROUND))
        painter = QPainter(image)
        painter.setRenderHint(QPainter.Antialiasing, True)
        graph.scene().render(painter, QRectF(0, 0, CROP_PX, CROP_PX), rect, Qt.IgnoreAspectRatio)
        painter.end()
        return image, fits

    def centre_view(graph, nodes):
        rect, _fits = case_rect(nodes)
        graph.resetTransform()
        graph.centerOn(rect.center())
        QApplication.processEvents()

    def save_png(image, directory, name):
        if not os.path.isdir(directory):
            os.makedirs(directory)
        return image.save(os.path.join(directory, name + ".png"), "PNG")

    def compare_or_record(case, image):
        golden_path = os.path.join(GOLDEN_DIR, case + ".png")
        if RECORD_ALL or not os.path.isfile(golden_path):
            wrote = save_png(image, RECORD_DIR, case)
            save_png(image, RECORDED_COPY_DIR, case)
            report("NOTE RECORDED golden for case %s to %s" % (case, RECORD_DIR))
            check(wrote, "case %s: golden recorded" % case)
            return
        golden = QImage(golden_path).convertToFormat(QImage.Format_ARGB32)
        same = (golden.size() == image.size()) and golden == image
        if not same:
            save_png(image, gt.OUT, case + "-actual")
        check(same, "case %s: scene render matches Tests/fixtures/graph_connectors/%s.png" % (case, case))

    def key_event(kind, key, mods):
        return QKeyEvent(kind, key, mods)

    def mouse(graph, kind, scene_pos, buttons, mods):
        pos = graph.mapFromScene(scene_pos)
        button = Qt.LeftButton if (kind != QEvent.MouseMove or buttons) else Qt.NoButton
        ev = QMouseEvent(kind, QPointF(pos), button, buttons, mods)
        QApplication.sendEvent(graph.viewport(), ev)
        QApplication.processEvents()

    def clear_all(nodes):
        app.clearSelection()
        for n in nodes:
            n.destroy(False)
        QApplication.processEvents()

    def sources(count, y=0.0):
        return [create("Constant", "src%d" % i, i * COL_STEP, y) for i in range(count)]

    # Each builder returns (nodes, after) where after is an optional generator step run once the
    # graph has settled and before the capture, and a cleanup to run after it.

    def build_merge_abm():
        srcs = sources(3)
        merge = create("Merge", "merge", COL_STEP, ROW_STEP)
        for i, s in enumerate(srcs):
            connect(merge, i, s)
        return srcs + [merge], None

    def build_side_inputs():
        srcs = sources(3)
        roto = create("RotoPaint", "roto", COL_STEP, ROW_STEP)
        for i in range(min(roto.getMaxInputCount(), len(srcs))):
            connect(roto, i, srcs[i])
        return srcs + [roto], None

    def build_viewer_inspector():
        srcs = sources(3)
        viewer = create("Viewer", "viewer", COL_STEP, ROW_STEP)
        for i, s in enumerate(srcs):
            connect(viewer, i, s)
        return srcs + [viewer], None

    def build_switch_inspector():
        srcs = sources(3)
        switch = create("Switch", "switch", COL_STEP, ROW_STEP)
        for i, s in enumerate(srcs):
            connect(switch, i, s)
        return srcs + [switch], None

    def build_output_edge():
        src = create("Constant", "src", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        blur = create("Blur", "blur", 0, 2 * ROW_STEP)
        connect(grade, 0, src)
        connect(blur, 0, grade)
        return [src, grade, blur], None

    def build_bend_point():
        src = create("Constant", "src", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        connect(grade, 0, src)
        return [src, grade], "bend"

    def build_dot_chain():
        src = create("Constant", "src", 0, 0)
        dot1 = create("Dot", "dot1", 0, 70)
        dot2 = create("Dot", "dot2", 60, 140)
        grade = create("Grade", "grade", 60, 210)
        connect(dot1, 0, src)
        connect(dot2, 0, dot1)
        connect(grade, 0, dot2)
        return [src, dot1, dot2, grade], None

    def build_edge_selected():
        src = create("Constant", "src", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        connect(grade, 0, src)
        return [src, grade], ("select", grade)

    def build_edge_highlighted():
        src = create("Constant", "src", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        connect(grade, 0, src)
        return [src, grade], "cut"

    def build_kind_image():
        src = create("Constant", "src", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        connect(grade, 0, src)
        return [src, grade], None

    def build_kind_deep():
        src = create("Constant", "src", 0, 0)
        deep = create("DeepFromImage", "deepSrc", 0, ROW_STEP)
        crop = create("DeepCrop", "deepCrop", 0, 2 * ROW_STEP)
        connect(deep, 0, src)
        connect(crop, 0, deep)
        return [src, deep, crop], None

    def build_kind_scene():
        cube = create("USDCube", "cube", 0, 0)
        merge = create("USDMerge", "sceneMerge", 0, ROW_STEP)
        connect(merge, 0, cube)
        return [cube, merge], None

    def build_kind_raw():
        prep = create("RawPrepare", "rawPrep", 0, 0)
        temp = create("RawTemperature", "rawTemp", 0, ROW_STEP)
        connect(temp, 0, prep)
        return [prep, temp], None

    def build_kind_polymorphic():
        dot = create("Dot", "dot", 0, 0)
        grade = create("Grade", "grade", 0, ROW_STEP)
        connect(grade, 0, dot)
        return [dot, grade], None

    CASES = [
        ("merge_a_b_mask", build_merge_abm),
        ("side_inputs", build_side_inputs),
        ("viewer_inspector", build_viewer_inspector),
        ("switch_inspector", build_switch_inspector),
        ("output_edge", build_output_edge),
        ("bend_point", build_bend_point),
        ("dot_chain", build_dot_chain),
        ("edge_selected", build_edge_selected),
        ("edge_highlighted", build_edge_highlighted),
        ("kind_image", build_kind_image),
        ("kind_deep", build_kind_deep),
        ("kind_scene", build_kind_scene),
        ("kind_raw", build_kind_raw),
        ("kind_polymorphic", build_kind_polymorphic),
    ]

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        _held.append(graph)
        gt.require(graph.scene() is not None, "the node graph has a scene")

        gt.section("connector-goldens")
        for case, build in CASES:
            nodes, mode = build()
            app.clearSelection()
            centre_view(graph, nodes)
            yield ("sleep", SETTLE_MS)

            cleanup = None
            if isinstance(mode, tuple) and mode[0] == "select":
                app.selectNode(mode[1], True)
            elif mode == "bend":
                QApplication.sendEvent(graph, key_event(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
                cleanup = lambda: QApplication.sendEvent(graph, key_event(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
            elif mode == "cut":
                x, y, w, h = box(nodes[1])
                sx, sy, sw, sh = box(nodes[0])
                mid_y = (sy + sh + y) / 2.0
                mid_x = x + w / 2.0
                mouse(graph, QEvent.MouseButtonPress, QPointF(mid_x - 40.0, mid_y), Qt.LeftButton, Qt.ControlModifier)
                mouse(graph, QEvent.MouseMove, QPointF(mid_x - 10.0, mid_y), Qt.LeftButton, Qt.ControlModifier)
                mouse(graph, QEvent.MouseMove, QPointF(mid_x + 10.0, mid_y), Qt.LeftButton, Qt.ControlModifier)
                cleanup = lambda: mouse(graph, QEvent.MouseButtonRelease, QPointF(mid_x + 40.0, mid_y), Qt.NoButton, Qt.ControlModifier)
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)

            image, fits = render_scene(graph, nodes)
            check(fits, "case %s: the nodes fit the %d px crop" % (case, CROP_PX))
            compare_or_record(case, image)

            if cleanup is not None:
                cleanup()
            clear_all(nodes)
            yield ("sleep", 200)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
