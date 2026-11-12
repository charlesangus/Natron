"""Sign-off screenshots of the node graph's level-of-detail painting, indicators, pan and selection.

Usage (natron-dev container; Tests/gui/run-gui-test.sh sets up Xvfb and the AppImage):
    docker exec -e NATRON_GUI_TEST_OUT=build/m118-shots -w <tree> natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m118_shots.py <AppImage>

Builds a ~300 node comp graph with tools/bench/gui/gen_graph.py plus a hand-placed cluster for the
indicators, then writes lod-{near,mid,far}.png, indicators-*.png, pan.png and select-all-far.png
to $NATRON_GUI_TEST_OUT. Each file is a screen grab of the whole 1920x1080 window.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench", "gui"))

try:
    import time

    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent, QWheelEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QSplitter

    import gen_graph
    import guitest as gt

    SETTLE_MS = 400
    WHEEL_STEP = 30

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def activate():
        win = main_window()
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)
        return win.isActiveWindow()

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if not w.objectName()]
        return graphs[0] if graphs else None

    def enlarge_graph_pane():
        """Gives the graph's pane nearly all of the window: the default layout leaves it a strip."""
        win = main_window()
        win.resize(1920, 1080)
        QApplication.processEvents()
        child = root_graph()
        while child is not None and child is not win:
            parent = child.parentWidget()
            if isinstance(parent, QSplitter):
                index = parent.indexOf(child)
                parent.setSizes([4000 if i == index else 1 for i in range(parent.count())])
            child = parent
        QApplication.processEvents()

    def scale():
        return root_graph().transform().m22()

    def wheel(steps):
        """Hovers, then wheels, at an off-centre point, the way a user zooms towards the cursor."""
        vp = root_graph().viewport()
        pos = QPointF(vp.width() * 0.4, vp.height() * 0.45)
        QApplication.sendEvent(vp, QMouseEvent(QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton, Qt.NoModifier))
        glob = QPointF(vp.mapToGlobal(QPoint(int(pos.x()), int(pos.y()))))
        QApplication.sendEvent(vp, QWheelEvent(pos, glob, QPoint(0, 0), QPoint(0, WHEEL_STEP * steps), Qt.NoButton,
                                               Qt.NoModifier, Qt.NoScrollPhase, False))

    def zoom_to(target, tolerance):
        """Wheel-zooms about an off-centre cursor until the scale is within tolerance of target."""
        for _ in range(400):
            s = scale()
            if abs(s / target - 1.0) <= tolerance:
                return True
            wheel(1 if s < target else -1)
            QApplication.processEvents()
        return False

    def invoke(slot):
        QMetaObject.invokeMethod(root_graph(), slot)

    def node_box(name):
        node = app.getNode(name)
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def centre_on(x, y):
        root_graph().centerOn(QPointF(x, y))
        QApplication.processEvents()

    def steps():
        gt.section("m118 shots")
        gt.require(activate(), "main window active")
        graph = root_graph()
        gt.require(graph is not None, "root node graph found")
        gt.close_all_panels()
        enlarge_graph_pane()
        yield ("sleep", SETTLE_MS)

        summary = gen_graph.build(app, "comp", 300, 1, groups=False)
        names = summary["root_nodes"]
        gt.check(summary["nodes"] == 300, "generated %d processing nodes" % summary["nodes"])
        gt.check(summary["backdrops"] > 0, "generated %d backdrops" % summary["backdrops"])
        gt.check(summary["expressions"] > 0, "generated %d expressions" % summary["expressions"])

        boxes = [node_box(n) for n in names]
        min_x = min(b[0] for b in boxes)
        min_y = min(b[1] for b in boxes)

        # A node placed on top of an existing one, and a disabled one, inside the generated graph.
        mid_name = names[len(names) // 2]
        mx, my, mw, mh = node_box(mid_name)
        stacked = app.createNode("Grade")
        stacked.setPosition(mx + 25, my + 12)
        disabled = app.createNode("Blur")
        disabled.setPosition(mx + 150, my + 60)
        disabled.getParam("disableNode").setValue(True)

        # Indicator cluster, left of the generated graph: a source with a persistent message,
        # a Grade with an expression, a disabled Blur, and two Grades joined by a knob link.
        cx, cy = min_x - 900.0, min_y + 200.0
        bad = app.createNode("Read")
        bad.setScriptName("MissingRead")
        bad.setPosition(cx, cy)
        bad.getParam("filename").setValue("/nonexistent/m118_missing.####.exr")
        expr = app.createNode("Grade")
        expr.setScriptName("ExprGrade")
        expr.setPosition(cx, cy + 120)
        expr.connectInput(0, bad)
        off = app.createNode("Blur")
        off.setScriptName("DisabledBlur")
        off.setPosition(cx, cy + 240)
        off.connectInput(0, expr)
        off.getParam("disableNode").setValue(True)
        src = app.createNode("Grade")
        src.setScriptName("LinkSource")
        src.setPosition(cx + 260, cy + 120)
        dst = app.createNode("Grade")
        dst.setScriptName("LinkTarget")
        dst.setPosition(cx + 260, cy + 280)
        dst.connectInput(0, src)
        gt.check(expr.getParam("multiply").setExpression("1 + 0.01 * frame", False, 0), "expression on ExprGrade.multiply")
        gt.check(dst.getParam("multiply").setExpression("LinkSource.multiply.getValue(0)", False, 0),
                 "LinkTarget.multiply linked to LinkSource.multiply")
        yield ("sleep", SETTLE_MS * 2)

        graph = root_graph()
        app.clearSelection(app)
        invoke("centerOnAllNodes")
        yield ("sleep", SETTLE_MS)
        fit_scale = scale()
        gt.report("NOTE fit scale %.4f" % fit_scale)
        font_height = graph.fontMetrics().height()
        threshold = 4.0 / font_height
        gt.report("NOTE font height %d, label-hide threshold scale %.4f" % (font_height, threshold))

        # lod-far: fit all, below the threshold.
        gt.check(fit_scale < threshold, "fit-all scale %.4f is below the threshold %.4f" % (fit_scale, threshold))
        yield ("sleep", SETTLE_MS)
        gt.shot("lod-far.png")

        # pan.png is taken further down, from the 1:1 view; select-all-far.png at the fit.
        app.selectAllNodes(app)
        yield ("sleep", SETTLE_MS)
        gt.shot("select-all-far.png")
        app.clearSelection(app)
        yield ("sleep", SETTLE_MS)

        # lod-mid: just above the threshold, centred on the stacked nodes.
        zoom_to(threshold * 1.15, 0.05)
        centre_on(mx + mw, my + mh)
        yield ("sleep", SETTLE_MS)
        gt.report("NOTE mid scale %.4f" % scale())
        gt.check(scale() >= threshold, "mid scale %.4f is above the threshold %.4f" % (scale(), threshold))
        gt.shot("lod-mid.png")

        # lod-near: 1:1.
        zoom_to(1.0, 0.03)
        centre_on(mx + mw, my + mh)
        yield ("sleep", SETTLE_MS)
        gt.report("NOTE near scale %.4f" % scale())
        gt.shot("lod-near.png")

        # Pan by dragging the middle button, from the 1:1 view.
        vp = graph.viewport()
        x, y = vp.width() / 2.0, vp.height() / 2.0
        QApplication.sendEvent(vp, QMouseEvent(QEvent.MouseButtonPress, QPointF(x, y), Qt.MiddleButton, Qt.MiddleButton, Qt.NoModifier))
        for i in range(1, 11):
            QApplication.sendEvent(vp, QMouseEvent(QEvent.MouseMove, QPointF(x - 40 * i, y - 15 * i), Qt.NoButton, Qt.MiddleButton, Qt.NoModifier))
            QApplication.processEvents()
        QApplication.sendEvent(vp, QMouseEvent(QEvent.MouseButtonRelease, QPointF(x - 400, y - 150), Qt.MiddleButton, Qt.NoButton, Qt.NoModifier))
        yield ("sleep", SETTLE_MS)
        gt.shot("pan.png")

        # Indicators at 1:1 on the cluster.
        centre_on(cx + 130, cy + 200)
        yield ("sleep", SETTLE_MS)
        gt.shot("indicators-overview.png")

        centre_on(cx, cy + 60)
        yield ("sleep", SETTLE_MS)
        gt.shot("indicators-message.png")

        centre_on(cx + 130, cy + 200)
        app.setSelection([bad, expr, off])
        yield ("sleep", SETTLE_MS)
        gt.shot("indicators-selected.png")
        app.clearSelection(app)

        centre_on(cx + 260, cy + 220)
        yield ("sleep", SETTLE_MS)
        gt.shot("indicators-link.png")

        centre_on(cx, cy + 180)
        yield ("sleep", SETTLE_MS)
        gt.shot("indicators-expression-disabled.png")

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
            f.write("FAIL script error: " + traceback.format_exc() + "\n")
        os._exit(1)
