"""RotoPaint brushes: no white shape under the pen, and Multi-Stroke off paints stable strokes.

  - Clone and eraser under a paused viewer: no render can reach the screen, so anything drawn under
    the pen comes from the overlay. Their result cannot be previewed, so after each pen move every
    sample of the stroke must stay dark (no white footprint).
  - Multi-Stroke off: a red solid brush paints several strokes in quick succession, some with no
    pause between them. After every pen move, each sample of the strokes painted so far must be red
    on screen: an earlier stroke that vanishes, flickers, turns black or shows half-drawn fails.
    Once the renders settle every stroke is its own item and is fully on screen.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m224-shots/brush-strokes -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m224_brush_strokes.py build/fast/App/Natron
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 300
RENDER_MS = 1500
CONVERGE_S = 5.0
PEN_INTERVAL_MS = 8
STROKE_GAPS_MS = (0, 30, 0, 80, 0, 10)
SAMPLES = 10
BRUSH_PX = 40.0

try:
    import NatronEngine
    from PySide6.QtCore import QPointF, Qt
    from PySide6.QtWidgets import QAbstractButton, QApplication

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = {}

    def pause_button(gl):
        tab = rg.ancestor_of_class(gl, "ViewerTab")
        if tab is None:
            return None
        # findChildren wrappers die with the parent's wrapper; hold the tab while the button is used.
        _keep["tab"] = tab
        for b in tab.findChildren(QAbstractButton):
            try:
                if b.isCheckable() and "Pause Updates" in b.toolTip():
                    return b
            except RuntimeError:
                continue
        return None

    def is_red(c):
        return c is not None and c[0] >= 200 and c[1] <= 60 and c[2] <= 60

    def is_dark(c):
        return c is not None and max(c) <= 60

    def line(gl, y_frac, x0_frac=0.3, x1_frac=0.7, n=SAMPLES):
        w, h = float(gl.width()), float(gl.height())
        return [QPointF(w * (x0_frac + (x1_frac - x0_frac) * i / float(n - 1)), h * y_frac) for i in range(n)]

    def interior(pts):
        # The viewer labels the RoD corners, which sit on the strokes' ends.
        return pts[2:-2]

    def path(pts):
        # Every pixel along the inset stroke, so the gaps between pen moves are covered.
        inner = interior(pts)
        out = []
        for a, b in zip(inner, inner[1:]):
            n = max(1, int(math.ceil(max(abs(b.x() - a.x()), abs(b.y() - a.y())))))
            out.extend(QPointF(a.x() + (b.x() - a.x()) * t / float(n), a.y() + (b.y() - a.y()) * t / float(n)) for t in range(n))
        out.extend(inner[-1:])
        return out

    def clear(rp):
        rg.clear_shapes(rp.getRotoContext())

    def no_white_phase(gl, rp, label, group, tool_id):
        gt.section("%s-no-white" % label)
        clear(rp)
        require(rg.pick_tool(group, tool_id), "picked the %s tool" % label)
        button = pause_button(gl)
        require(button is not None, "found the viewer's Pause Updates button")
        _keep["pause"] = button
        if not button.isChecked():
            button.click()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        pts = line(gl, 0.5)
        check(all(is_dark(rg.Grab(gl).colour(p.x(), p.y())) for p in pts), "%s: the viewer is dark before the stroke" % label)
        rg.press(gl, pts[0])
        lit = []
        for i in range(1, len(pts)):
            yield ("sleep", PEN_INTERVAL_MS)
            rg.move(gl, pts[i], Qt.LeftButton)
            gl.repaint()
            grab = rg.Grab(gl)
            for j in range(i + 1):
                c = grab.colour(pts[j].x(), pts[j].y())
                if not is_dark(c):
                    lit.append((i, j, c))
        rg.release(gl, pts[-1])
        check(not lit, "%s: nothing is drawn under the pen (lit samples: %s)" % (label, lit[:5]))
        if lit:
            gt.shot("m224_brush_%s_white.png" % label)
        yield ("sleep", SETTLE_MS)
        button.click()
        QApplication.processEvents()
        check(not button.isChecked(), "the viewer is unpaused")
        yield ("sleep", RENDER_MS)

    def single_stroke_phase(gl, rp):
        gt.section("multi-stroke-off")
        clear(rp)
        require(rg.pick_tool("PaintBrushToolButton", "PaintSolidTool"), "picked the solid brush")
        rp.getParam("multiStrokeEnabledButton").setValue(False)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

        strokes = [line(gl, 0.2 + 0.11 * k) for k in range(len(STROKE_GAPS_MS))]
        gaps = []
        for k, pts in enumerate(strokes):
            yield ("sleep", STROKE_GAPS_MS[k])
            rg.press(gl, pts[0])
            for i in range(1, len(pts)):
                yield ("sleep", PEN_INTERVAL_MS)
                rg.move(gl, pts[i], Qt.LeftButton)
                gl.repaint()
                grab = rg.Grab(gl)
                for s in range(k):
                    for p in interior(strokes[s]):
                        if not is_red(grab.colour(p.x(), p.y())):
                            gaps.append((k, i, s, grab.colour(p.x(), p.y())))
                            break
            rg.release(gl, pts[-1])
        check(not gaps, "earlier strokes stay on screen while later ones are painted (gaps: %s)" % gaps[:5])
        if gaps:
            gt.shot("m224_brush_single_stroke_gap.png")

        def all_red():
            grab = rg.Grab(gl)
            return all(is_red(grab.colour(p.x(), p.y())) for pts in strokes for p in path(pts))

        ok = yield ("until", all_red, CONVERGE_S)
        check(ok, "every stroke is fully on screen once the renders land")
        between = QPointF(gl.width() * 0.5, gl.height() * (0.2 + 0.055))
        check(is_dark(rg.Grab(gl).colour(between.x(), between.y())), "the strokes stay apart")
        items = rp.getRotoContext().getBaseLayer().getChildren()
        check(len(items) == len(strokes), "each stroke is its own item (%d items for %d strokes)" % (len(items), len(strokes)))
        gt.shot("m224_brush_single_stroke.png")
        rp.getParam("multiStrokeEnabledButton").setValue(True)

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        rg.activate()

        rp, ctx = rg.open_roto(app, "RotoPaint")
        require(rp is not None, "create RotoPaint", gt.plugin_problem("RotoPaint"))
        require(ctx is not None, "the RotoPaint node has a roto context")
        _keep["rp"] = rp
        rp.getParam("sizeSpinbox").setValue(BRUSH_PX)
        rp.getParam("hardnessSpinbox").setValue(1.0)
        rp.getParam("opacitySpinbox").setValue(1.0)
        colour = rp.getParam("strokeColorButton")
        for dim, v in enumerate((1.0, 0.0, 0.0)):
            colour.setValue(v, dim)
        yield ("sleep", RENDER_MS)

        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        _keep["gl"] = gl
        rg.activate()
        require(rg.untone_view(), "picked the Un-tone-mapped view")
        yield ("sleep", RENDER_MS)

        yield from no_white_phase(gl, rp, "clone", "CloneToolButton", "CloneTool")
        yield from no_white_phase(gl, rp, "eraser", "PaintBrushToolButton", "EraserTool")
        yield from single_stroke_phase(gl, rp)
        clear(rp)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
