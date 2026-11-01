"""Shape creation stays under the cursor and the Bezier tool always starts a new shape, in Roto and
RotoPaint.

Per node type:
  - Repeated drawing: several rounds, each first dragging a point of the previous shape (a live
    render runs for the edit), then V and a four-point shape drawn with clicks and click-drags at
    varied intervals, so renders start and end between the points. Every new point stays where it
    was placed (none snaps to 0,0), and each round adds exactly one closed shape.
  - After an ellipse: picking the Bezier tool from the toolbar menu and clicking starts a new shape
    and keeps the Bezier tool (the finished ellipse used to be taken for the shape in progress, so
    the click only switched back to the selection tool).
  - Leaving the tool mid-shape: two points, then Q, then V and a click start a new shape; the
    abandoned one keeps its two points.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m224-shots/shape-creation -e GUI_TEST_TIMEOUT=400 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m224_shape_creation.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 400
SETTLE_MS = 300
RENDER_MS = 1500
ROUNDS = 5
GAPS_MS = (0, 15, 40, 90)
NODES = (("Roto", "fr.inria.built-in.Roto"), ("RotoPaint", "fr.inria.built-in.RotoPaint"))

try:
    import NatronEngine
    from PySide6.QtCore import QPointF, Qt
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = {}

    def shape_outline(gl, k):
        # Four corners of a small quad, shifted per round so no click lands on an earlier shape.
        w, h = float(gl.width()), float(gl.height())
        col, row = k % 3, k // 3
        x0, y0 = w * (0.15 + 0.27 * col), h * (0.15 + 0.4 * row)
        s = min(w, h) * 0.12
        return [QPointF(x0, y0), QPointF(x0 + s, y0 + 0.1 * s), QPointF(x0 + 1.1 * s, y0 + s), QPointF(x0 - 0.1 * s, y0 + 0.9 * s)]

    def draw_shape(gl, outline, k):
        """Clicks, then click-drags, the outline with varied pauses, and closes on the first point."""
        for i, p in enumerate(outline):
            if i % 2 == 1:
                rg.drag(gl, p, p + QPointF(18, 6), steps=4)
            else:
                rg.click(gl, p)
            yield ("sleep", GAPS_MS[(i + k) % len(GAPS_MS)])
        rg.click(gl, outline[0])
        yield ("sleep", SETTLE_MS)

    def node_sections(gl, label, ctx, vmap):
        ps = vmap.pixel_scale()
        tol = 1.5 * ps

        gt.section("%s-repeated-drawing" % label)
        rg.clear_shapes(ctx)
        rg.select_tool(gl)
        made = []
        for k in range(ROUNDS):
            if made:
                prev = made[-1]
                p = rg.point(prev, 2)
                handle = vmap.to_widget(p[0], p[1])
                rg.select_tool(gl)
                rg.drag(gl, handle, handle + QPointF(6, 4), steps=6)
                yield ("sleep", GAPS_MS[k % len(GAPS_MS)])
            rg.send_key(gl, Qt.Key_V)
            yield ("sleep", 50)
            tool = rg.drawing_tool()
            check(tool == (True, "DrawBezierTool"), "%s round %d: V picks the Bezier tool (%s)" % (label, k, tool))
            outline = shape_outline(gl, k)
            for cmd in draw_shape(gl, outline, k):
                yield cmd
            now = rg.shapes(ctx)
            check(len(now) == k + 1, "%s round %d: the round adds one shape (%d shapes)" % (label, k, len(now)))
            if len(now) != k + 1:
                gt.shot("m224_shape_creation_%s_round%d.png" % (label, k))
                return
            known = set(b.getScriptName() for b in made)
            bez = [b for b in now if b.getScriptName() not in known][0]
            made = now
            check(bez.isCurveFinished(), "%s round %d: clicking the first point closed the shape" % (label, k))
            n = bez.getNumControlPoints()
            check(n == len(outline), "%s round %d: the shape has %d points (%d)" % (label, k, len(outline), n))
            worst = 0.0
            at_origin = 0
            for i in range(min(n, len(outline))):
                x, y = rg.point(bez, i)[:2]
                ex, ey = vmap.to_canonical(outline[i])
                worst = max(worst, abs(x - ex), abs(y - ey))
                if abs(x) < tol and abs(y) < tol:
                    at_origin += 1
            check(at_origin == 0, "%s round %d: no point snapped to 0,0" % (label, k))
            check(worst < tol, "%s round %d: every point stays where it was placed (worst %.2f px)" % (label, k, worst / ps))
        yield ("sleep", RENDER_MS)
        for b in made:
            for i in range(b.getNumControlPoints()):
                x, y = rg.point(b, i)[:2]
                if abs(x) < tol and abs(y) < tol:
                    check(False, "%s: after the renders settle a point of %s sits at 0,0" % (label, b.getScriptName()))
        gt.shot("m224_shape_creation_%s.png" % label)

        gt.section("%s-bezier-after-ellipse" % label)
        rg.clear_shapes(ctx)
        rg.select_tool(gl)
        require(rg.pick_drawing_tool("DrawEllipseTool"), "%s: picked the ellipse tool" % label)
        w, h = float(gl.width()), float(gl.height())
        a = QPointF(w * 0.3, h * 0.3)
        rg.drag(gl, a, a + QPointF(90, 60))
        yield ("sleep", SETTLE_MS)
        check(len(rg.shapes(ctx)) == 1, "%s: the ellipse tool drew one shape" % label)
        require(rg.pick_drawing_tool("DrawBezierTool"), "%s: picked the Bezier tool" % label)
        yield ("sleep", 50)
        rg.click(gl, QPointF(w * 0.6, h * 0.6))
        yield ("sleep", SETTLE_MS)
        tool = rg.drawing_tool()
        check(tool == (True, "DrawBezierTool"), "%s: a click after the ellipse keeps the Bezier tool (%s)" % (label, tool))
        now = rg.shapes(ctx)
        check(len(now) == 2, "%s: a click after the ellipse starts a new shape (%d shapes)" % (label, len(now)))
        rg.send_key(gl, Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)

        gt.section("%s-leave-tool-mid-shape" % label)
        rg.clear_shapes(ctx)
        rg.select_tool(gl)
        rg.send_key(gl, Qt.Key_V)
        yield ("sleep", 50)
        rg.click(gl, QPointF(w * 0.2, h * 0.7))
        rg.click(gl, QPointF(w * 0.3, h * 0.75))
        yield ("sleep", SETTLE_MS)
        rg.send_key(gl, Qt.Key_Q)
        yield ("sleep", SETTLE_MS)
        rg.send_key(gl, Qt.Key_V)
        yield ("sleep", 50)
        tool = rg.drawing_tool()
        check(tool == (True, "DrawBezierTool"), "%s: V after Q picks the Bezier tool (%s)" % (label, tool))
        rg.click(gl, QPointF(w * 0.7, h * 0.3))
        yield ("sleep", SETTLE_MS)
        now = rg.shapes(ctx)
        counts = sorted(b.getNumControlPoints() for b in now)
        check(counts == [1, 2], "%s: the click starts a new shape and the left one keeps two points (%s)" % (label, counts))
        rg.send_key(gl, Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        rg.clear_shapes(ctx)

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")

        for label, plugin_id in NODES:
            gt.close_all_panels()
            rg.activate()
            node, ctx = rg.open_roto(app, plugin_id)
            require(node is not None, "create %s" % label, gt.plugin_problem(plugin_id))
            require(ctx is not None, "%s has a roto context" % label)
            _keep[label] = node
            yield ("sleep", RENDER_MS)
            gl = rg.viewer_gl()
            require(gl is not None, "the viewer is visible")
            _keep["gl"] = gl
            rg.activate()
            rg.untone_view()
            yield ("sleep", RENDER_MS)
            vmap = rg.ViewerMap(gl)
            require(vmap.ok, "calibrated the viewer for %s" % label)
            for cmd in node_sections(gl, label, ctx, vmap):
                yield cmd
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
