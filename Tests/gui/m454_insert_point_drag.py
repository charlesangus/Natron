"""Inserting a point on a curve is a click-and-drag: the new point follows the pen with smooth handles."""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
FRAME = 1.0
# The curve, a rectangle in canonical coordinates: (x0, y0) to (x1, y1).
RECT = (700.0, 350.0, 1200.0, 750.0)
# Viewer pixels the pen moves after the press: right and up.
DRAG_BY = (40.0, -70.0)
DRAG_STEPS = 12
TOL = 1e-6
# Handle directions agree to within this, as the sine of the angle between them.
COLLINEAR_TOL = 1e-6

try:
    import NatronEngine
    from PySide6.QtCore import QPointF, Qt
    from PySide6.QtWidgets import QApplication, QToolButton

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = {}

    def hold(obj):
        _keep.setdefault("held", []).append(obj)
        return obj

    def select_tool(text):
        """Selects a viewer toolbar tool the way its button does; the tool knobs are checkable, so
        triggering the parameter from a script never changes the tool."""
        for button in QApplication.allWidgets():
            if isinstance(button, QToolButton) and button.isVisible():
                for act in button.actions():
                    if act.text() == text:
                        button.setDefaultAction(act)
                        button.triggered.emit(act)
                        QApplication.processEvents()
                        return True
        report("NOTE no viewer toolbar button offers %s" % text)
        return False

    def make_rect(ctx):
        x0, y0, x1, y1 = RECT
        bez = hold(ctx.createBezier(x0, y0, FRAME))
        for x, y in ((x1, y0), (x1, y1), (x0, y1)):
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        return bez

    def pose(bez):
        return [tuple(rg.point(bez, i, FRAME)) for i in range(bez.getNumControlPoints())]

    def same(a, b, tol=TOL):
        return len(a) == len(b) and rg.close(a, b, tol)

    def same_pose(a, b):
        return len(a) == len(b) and all(same(p, q) for p, q in zip(a, b))

    def smooth(p):
        left, right = rg.left_off(p), rg.right_off(p)
        ll, rl = rg.length(left), rg.length(right)
        if ll <= TOL or rl <= TOL:
            return False
        sine = (left[0] * right[1] - left[1] * right[0]) / (ll * rl)
        return abs(sine) <= COLLINEAR_TOL and (left[0] * right[0] + left[1] * right[1]) < 0.0

    def segment_middle():
        x0, y0, x1, _ = RECT
        return ((x0 + x1) / 2.0, y0)

    def drag_points(start):
        end = QPointF(start.x() + DRAG_BY[0], start.y() + DRAG_BY[1])
        return rg.segment(start, end, DRAG_STEPS)

    def gesture(gl, points, modifiers, mid_press):
        rg.hold(gl, modifiers)
        try:
            rg.press(gl, points[0], modifiers)
            mid_press()
            for p in points[1:]:
                rg.move(gl, p, Qt.LeftButton, modifiers)
            rg.release(gl, points[-1], modifiers)
        finally:
            rg.unhold(gl, modifiers)

    def insert_drag(tag, bez, gl, vmap, modifiers, what):
        before = pose(bez)
        mx, my = segment_middle()
        points = drag_points(vmap.to_widget(mx, my))
        seen = {}

        def mid_press():
            seen["count"] = bez.getNumControlPoints()
            seen["pose"] = pose(bez)

        gesture(gl, points, modifiers, mid_press)
        yield ("sleep", SETTLE_MS)

        n = len(before)
        require(seen.get("count") == n + 1, "%s: %s inserted one point at the press (%r points)" % (tag, what, seen.get("count")))
        require(bez.getNumControlPoints() == n + 1, "%s: the curve keeps the inserted point after the drag" % tag)
        split = seen["pose"]
        after = pose(bez)
        inserted = after[1]
        release = vmap.to_canonical(points[-1])
        press = vmap.to_canonical(points[0])
        tol = 2.0 * vmap.pixel_scale()
        expected = (split[1][0] + release[0] - press[0], split[1][1] + release[1] - press[1])
        check(math.hypot(inserted[0] - expected[0], inserted[1] - expected[1]) <= tol,
              "%s: %s dragged the new point to the release position: %r, expected %r" % (tag, what, inserted[:2], expected))
        check(math.hypot(inserted[0] - split[1][0], inserted[1] - split[1][1]) > 10.0 * tol,
              "%s: the new point left the insert position %r" % (tag, split[1][:2]))
        check(smooth(inserted), "%s: the new point's handles are smooth: %r" % (tag, inserted))
        check(same(after[0], split[0]) and same(after[2], split[2]),
              "%s: the neighbours keep the handles of the insert: %r %r, insert gave %r %r"
              % (tag, after[0], after[2], split[0], split[2]))
        _keep["before"] = before

    def undo_restores(tag, bez, what):
        before = _keep["before"]
        rg.trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(bez.getNumControlPoints() == len(before),
              "%s: one undo of %s removes the inserted point (%d points)" % (tag, what, bez.getNumControlPoints()))
        check(same_pose(pose(bez), before), "%s: one undo of %s gives back the curve from before the press" % (tag, what))

    def node_steps(tag, node, ctx):
        bez = make_rect(ctx)
        require(bez is not None, "%s: created the curve" % tag)
        yield ("sleep", RENDER_MS)
        gl = rg.viewer_gl()
        require(gl is not None, "%s: the viewer is visible" % tag)
        hold(gl)
        rg.activate()
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "%s: calibrated the viewer" % tag)
        ctrl_alt = Qt.ControlModifier | Qt.AltModifier

        gt.section("%s-ctrl-alt-drag" % tag)
        rg.select_tool(gl)
        for cmd in insert_drag(tag, bez, gl, vmap, ctrl_alt, "a Ctrl+Alt press"):
            yield cmd
        gt.shot("m454-%s-ctrl-alt-drag.png" % tag)
        for cmd in undo_restores(tag, bez, "the Ctrl+Alt drag"):
            yield cmd

        gt.section("%s-ctrl-alt-click" % tag)
        before = pose(bez)
        mx, my = segment_middle()
        pos = vmap.to_widget(mx, my)
        seen = {}

        def mid_press():
            seen["pose"] = pose(bez)

        gesture(gl, [pos], ctrl_alt, mid_press)
        yield ("sleep", SETTLE_MS)
        check(bez.getNumControlPoints() == len(before) + 1, "%s: a Ctrl+Alt click inserts one point" % tag)
        check(same_pose(pose(bez), seen.get("pose", [])),
              "%s: a click with no motion leaves the insert as it was at the press" % tag)
        rg.trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(same_pose(pose(bez), before), "%s: one undo removes the clicked point" % tag)

        gt.section("%s-add-points-tool" % tag)
        # A plain click on a side edge selects the curve, which the Add Points tool needs.
        x0, y0, x1, y1 = RECT
        rg.click(gl, vmap.to_widget(x1, (y0 + y1) / 2.0))
        yield ("sleep", SETTLE_MS)
        require(select_tool("Add Points Tool"), "%s: picked the Add Points tool" % tag)
        for cmd in insert_drag(tag, bez, gl, vmap, Qt.NoModifier, "an Add Points press"):
            yield cmd
        gt.shot("m454-%s-add-points-drag.png" % tag)
        for cmd in undo_restores(tag, bez, "the Add Points drag"):
            yield cmd
        rg.select_tool(gl)
        rg.clear_shapes(ctx)

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        pyviewer = app.getViewer(viewer.getScriptName())
        require(pyviewer is not None, "the viewer has a Python handle")
        hold(pyviewer)
        pyviewer.seek(int(FRAME))

        gt.close_all_panels()
        rg.activate()
        roto, ctx = rg.open_roto(app, "Roto")
        require(roto is not None, "create Roto", gt.plugin_problem("Roto"))
        require(ctx is not None, "Roto has a roto context")
        hold(roto)
        hold(ctx)
        yield ("sleep", RENDER_MS)
        for cmd in node_steps("roto", roto, ctx):
            yield cmd

        gt.close_all_panels()
        rg.activate()
        checker = app.createNode("CheckerBoard")
        require(checker is not None, "created a CheckerBoard", gt.plugin_problem("CheckerBoard"))
        hold(checker)
        warp, wctx = rg.open_roto(app, "SplineWarp")
        require(warp is not None, "create SplineWarp", gt.plugin_problem("SplineWarp"))
        require(wctx is not None, "SplineWarp has a roto context")
        hold(warp)
        hold(wctx)
        require(warp.connectInput(0, checker), "connected the CheckerBoard to the SplineWarp")
        yield ("sleep", RENDER_MS)
        for cmd in node_steps("splinewarp", warp, wctx):
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
