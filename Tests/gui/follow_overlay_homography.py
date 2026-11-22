"""A shape's overlay stays on its curve when the shape's transform is a homography, in the app.

A perspective follow keys a homography into a shape's extra matrix. This script keys one by hand,
so the shape is far from identity without a plate and a fit, then edits the shape in the viewer
where the curve is drawn:

  Roto        one shape with handles
  SplineWarp  a keyed source shape linked to a destination shape; the destination carries the
              homography

For each node:
  point    a press on where point 0 is drawn picks it, and the drag moves it by the mouse delta on
           screen
  handle   with point 1 picked, a press on where its right handle is drawn picks the handle, and
           the drag moves it by the mouse delta on screen
  box      a rubber band over the drawn shape selects its points, and a drag of the selection
           box's mid-right handle stretches the drawn points and handles in x about the box's
           centre by the ratio of the handle's distances from it

The box is drawn around the points, and their handles hit, where the viewer shows them; before
that, every press missed and started a rubber band. Screenshots of each node with its points
selected (follow-overlay-<node>.png in the output directory).

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/follow-overlay-homography/out -e GUI_TEST_TIMEOUT=300 \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/follow_overlay_homography.py <Natron AppImage>
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
T = 1.0
CENTRE_X, CENTRE_Y, HALF = 960.0, 540.0, 220.0
HANDLE = 70.0
SOURCE_X, SOURCE_Y, SOURCE_HALF = 260.0, 260.0, 80.0
SOURCE_KEY = 2.0
SOURCE_KEY_SHIFT = (12.0, -6.0)
# Row-major, as the extraMatrix knob stores it. Its last row moves every point by a different w,
# about a fifth across the shape.
HOMOGRAPHY = (1.05, 0.04, 40.0,
              -0.03, 0.97, -30.0,
              0.00018, -0.00012, 1.0)
POINT_DRAG = (24.0, -18.0)
HANDLE_DRAG_FRACTION = 0.4
BOX_DRAG = 60.0
RUBBER_MARGIN = 60.0

try:
    import NatronEngine
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = []

    def keep(obj):
        _keep.append(obj)
        return obj

    def project(x, y):
        m = HOMOGRAPHY
        w = m[6] * x + m[7] * y + m[8]
        return ((m[0] * x + m[1] * y + m[2]) / w, (m[3] * x + m[4] * y + m[5]) / w)

    def shown(bez, i):
        """(point, left handle, right handle) of point i where the viewer draws them."""
        p = rg.point(bez, i, T)
        return project(p[0], p[1]), project(p[2], p[3]), project(p[4], p[5])

    def shown_all(bez):
        return [shown(bez, i) for i in range(bez.getNumControlPoints())]

    def make_shape(ctx, cx, cy, half, handle):
        corners = [(cx - half, cy + half), (cx + half, cy + half), (cx + half, cy - half), (cx - half, cy - half)]
        bez = keep(ctx.createBezier(corners[0][0], corners[0][1], T))
        for x, y in corners[1:]:
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        # Handles along the clockwise tangent, so every point has two away from it.
        for i, (x, y) in enumerate(corners):
            nx, ny = corners[(i + 1) % 4]
            px, py = corners[(i - 1) % 4]
            dx, dy = nx - px, ny - py
            n = math.hypot(dx, dy)
            ux, uy = dx / n * handle, dy / n * handle
            bez.setPointAtIndex(i, T, x, y, x - ux, y - uy, x + ux, y + uy)
        return bez

    def key_homography(bez):
        extra = keep(bez.getParam("extraMatrix"))
        require(extra is not None, "the shape has an extraMatrix parameter")
        for d, v in enumerate(HOMOGRAPHY):
            extra.setValueAtTime(v, T, d)
        got = [extra.getValueAtTime(T, d) for d in range(9)]
        require(all(abs(a - b) <= 1e-9 for a, b in zip(got, HOMOGRAPHY)), "the homography is keyed (%r)" % (got,))

    def near(a, b, tol):
        return math.hypot(a[0] - b[0], a[1] - b[1]) <= tol

    def bounds(points):
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        return min(xs), min(ys), max(xs), max(ys)

    def edit_steps(label, bez, gl, vmap):
        tol = 2.0 * vmap.pixel_scale()
        rg.select_tool(gl)
        yield ("sleep", SETTLE_MS)

        gt.section(label + " point")
        before = shown_all(bez)
        start = before[0][0]
        end = (start[0] + POINT_DRAG[0], start[1] + POINT_DRAG[1])
        rg.drag(gl, vmap.to_widget(*start), vmap.to_widget(*end))
        yield ("sleep", SETTLE_MS)
        after = shown_all(bez)
        check(near(after[0][0], end, tol),
              "%s point: point 0 drawn at (%.1f, %.1f) was picked and dragged to (%.1f, %.1f) (now at (%.1f, %.1f), tolerance %.2f)"
              % (label, start[0], start[1], end[0], end[1], after[0][0][0], after[0][0][1], tol))
        moved = max(math.hypot(after[i][0][0] - before[i][0][0], after[i][0][1] - before[i][0][1]) for i in range(1, len(after)))
        check(moved <= 1e-6, "%s point: the other points stay where they were (largest move %.3g)" % (label, moved))

        gt.section(label + " handle")
        p, _, right = shown(bez, 1)
        rg.click(gl, vmap.to_widget(*p))
        yield ("sleep", SETTLE_MS)
        # Along the handle, which keeps it in line with the other handle.
        end = (right[0] + HANDLE_DRAG_FRACTION * (right[0] - p[0]), right[1] + HANDLE_DRAG_FRACTION * (right[1] - p[1]))
        rg.drag(gl, vmap.to_widget(*right), vmap.to_widget(*end))
        yield ("sleep", SETTLE_MS)
        p_after, _, right_after = shown(bez, 1)
        check(near(right_after, end, tol),
              "%s handle: the right handle of point 1 drawn at (%.1f, %.1f) was picked and dragged to (%.1f, %.1f) (now at (%.1f, %.1f), tolerance %.2f)"
              % (label, right[0], right[1], end[0], end[1], right_after[0], right_after[1], tol))
        check(near(p_after, p, 1e-6),
              "%s handle: point 1 stays where it was (moved by %.3g)" % (label, math.hypot(p_after[0] - p[0], p_after[1] - p[1])))

        gt.section(label + " box")
        before = shown_all(bez)
        everything = [q for item in before for q in item]
        l, b, r, t = bounds(everything)
        rg.drag(gl, vmap.to_widget(l - RUBBER_MARGIN, b - RUBBER_MARGIN), vmap.to_widget(r + RUBBER_MARGIN, t + RUBBER_MARGIN))
        yield ("sleep", SETTLE_MS)
        gt.shot("follow-overlay-%s.png" % label.lower())
        report("NOTE caption follow-overlay-%s.png: %s with every point selected; the box, points and handles sit on the drawn curve" % (label.lower(), label))
        cx, cy = (l + r) / 2.0, (b + t) / 2.0
        # Without Shift a mid handle scales about the box's centre; Shift pins the opposite edge.
        ratio = (r + BOX_DRAG - cx) / (r - cx)
        rg.drag(gl, vmap.to_widget(r, cy), vmap.to_widget(r + BOX_DRAG, cy))
        yield ("sleep", SETTLE_MS)
        after = shown_all(bez)
        worst = 0.0
        for i in range(len(before)):
            for k in range(3):
                q = before[i][k]
                want = (cx + ratio * (q[0] - cx), q[1])
                worst = max(worst, math.hypot(after[i][k][0] - want[0], after[i][k][1] - want[1]))
        check(worst <= 2.0 * tol,
              "%s box: the drawn points and handles stretched by %.3f in x about the box's centre x = %.1f (largest error %.2f, tolerance %.2f)"
              % (label, ratio, cx, worst, 2.0 * tol))

    def open_node(viewer, plugin_id, plate):
        gt.close_all_panels()
        rg.activate()
        node, ctx = rg.open_roto(app, plugin_id)
        require(node is not None and ctx is not None, "create %s with its panel open" % plugin_id, gt.plugin_problem(plugin_id))
        keep(node)
        keep(ctx)
        if plate is not None:
            require(node.connectInput(0, plate), "connect %s to the plate" % plugin_id)
        return node, ctx

    def calibrated():
        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        keep(gl)
        rg.activate()
        yield ("sleep", RENDER_MS)
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")
        return gl, vmap

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        pyviewer = keep(app.getViewer(viewer.getScriptName()))
        require(pyviewer is not None, "the viewer has a Python handle")
        pyviewer.seek(int(T))

        gt.section("setup Roto")
        node, ctx = open_node(viewer, "Roto", None)
        bez = make_shape(ctx, CENTRE_X, CENTRE_Y, HALF, HANDLE)
        key_homography(bez)
        gl, vmap = yield from calibrated()
        yield from edit_steps("Roto", bez, gl, vmap)
        viewer.disconnectInput(0)

        gt.section("setup SplineWarp")
        plate = keep(app.createNode("Roto"))
        require(plate is not None, "create the plate Roto")
        node, ctx = open_node(viewer, "SplineWarp", plate)
        warp = keep(node.getSplineWarp())
        require(warp is not None, "the SplineWarp has links")
        source = make_shape(ctx, SOURCE_X, SOURCE_Y, SOURCE_HALF, SOURCE_HALF / 3.0)
        # The source moves the way a per-point follow keys it.
        for i in range(source.getNumControlPoints()):
            p = rg.point(source, i, T)
            sx, sy = SOURCE_KEY_SHIFT
            source.setPointAtIndex(i, SOURCE_KEY, p[0] + sx, p[1] + sy, p[2] + sx, p[3] + sy, p[4] + sx, p[5] + sy)
        destination = make_shape(ctx, CENTRE_X, CENTRE_Y, HALF, HANDLE)
        require(keep(warp.link(source, destination, True)) is not None, "link the source to the destination")
        key_homography(destination)
        gl, vmap = yield from calibrated()
        yield from edit_steps("SplineWarp", destination, gl, vmap)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
