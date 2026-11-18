"""LoopEnd's Map Blend mode from the GUI.

Sections:
  - eye blur: Constant -> LoopBegin -> Blur -> LoopEnd, the Blur's size driven from loop.t from 0 to
    20 and the LoopEnd in Map Blend mode (8 iterations, Linear) with a Roto ellipse as its Map input.
    Pixels well outside the ellipse (Map 0, iteration 0) equal the unblurred CheckerBoard exactly;
    the pixels inside the ellipse (Map 1, the full blur) differ from it.
  - map grade: the same loop with a Grade whose gain (multiply) is driven from 0 to 1 over 11
    iterations and a horizontal Ramp as the Map. The left end is dark, the right end is the input, and
    the middle matches a Grade with a static gain of 0.5.
  - panel: LoopEnd's panel open, showing the Map Blend controls.
All pixels are read from screen grabs of the viewer, so each comparison is between two grabs of
the same display pipeline. Screenshots: m183-eye-blur.png, m183-map-grade.png, m183-panel.png in
$NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m183_map_blend.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
RENDER_MS = 2500
CONSTANT_ID = "Constant"
CHECKER_ID = "CheckerBoard"
RAMP_ID = "Ramp"
ROTO_ID = "Roto"
BLUR_ID = "Blur"
GRADE_ID = "Grade"
BEGIN_ID = "LoopBegin"
END_ID = "LoopEnd"
VIEW_NAME = "Un-tone-mapped"
BLUR_ITERATIONS = 8
GRADE_ITERATIONS = 11
BLUR_MAX = 20.0
# Fractions of the image's width and height on screen.
FAR_POINTS = ((0.04, 0.04), (0.96, 0.04), (0.04, 0.96), (0.96, 0.96), (0.04, 0.5), (0.96, 0.5), (0.5, 0.04), (0.5, 0.96))
ELLIPSE_RADIUS = 0.2
GRADE_TOLERANCE = 6

try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, QPoint
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def visible_of(name):
        return [w for w in gt.widgets_of(name) if w.isVisible()]

    def hold(w):
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def viewer_gl():
        gls = visible_of("ViewerGL")
        return hold(gls[0]) if gls else None

    def choose_view():
        for w in gt.widgets_of("ComboBox"):
            try:
                if not w.isVisible():
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
                if VIEW_NAME in items:
                    hold(w)
                    QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(VIEW_NAME)))
                    QApplication.processEvents()
                    return str(w.property("natronCurrentText")) == VIEW_NAME
            except RuntimeError:
                continue
        return False

    def grab_viewer():
        QApplication.processEvents()
        gl = viewer_gl()
        origin = gl.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()

    def is_red(c):
        return c.red() > c.green() + 60 and c.red() > c.blue() + 60

    def image_span(img):
        """(x0, y0, x1, y1) of the red pixels of a grab: the frame of a red Constant on screen."""
        xs, ys = [], []
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                if is_red(img.pixelColor(x, y)):
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return min(xs), min(ys), max(xs) + 2, max(ys) + 2

    def at(img, span, fx, fy):
        """The grab's (r, g, b) at fraction (fx, fy) of the image, y down."""
        x0, y0, x1, y1 = span
        c = img.pixelColor(int(x0 + fx * (x1 - x0)), int(y0 + fy * (y1 - y0)))
        return (c.red(), c.green(), c.blue())

    def luma(rgb):
        return (rgb[0] + rgb[1] + rgb[2]) / 3.0

    def show(viewer, node):
        """Steps that point the viewer's input at node and let it render; returns the grab."""
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer shows %s" % node.getScriptName())
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        return img

    def build_loop(body, end_name, source):
        """LoopBegin -> body -> LoopEnd under source; returns (begin, end)."""
        begin = app.createNode(BEGIN_ID)
        end = app.createNode(END_ID)
        require(begin is not None and end is not None, "create the loop's nodes")
        end.getParam("name").setValue(end_name)
        require(begin.connectInput(0, source), "connect the LoopBegin")
        require(body.connectInput(0, begin), "connect the body")
        require(end.connectInput(0, body), "connect the LoopEnd")
        return begin, end

    def steps():
        gt.section("map blend")
        ok = yield ("until", lambda: bool(visible_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)

        # Calibrate: a red Constant fills the frame, which locates it on screen.
        calib = app.createNode(CONSTANT_ID)
        require(calib is not None, "create the calibration Constant", gt.plugin_problem(CONSTANT_ID))
        calib.getParam("color").set(0.8, 0.1, 0.1, 1.0)
        img = yield from show(viewer, calib)
        span = image_span(img)
        require(span is not None, "the Constant's frame is on screen")
        x0, y0, x1, y1 = span
        report("NOTE frame on screen at %r" % (span,))
        rod = calib.getRegionOfDefinition(1, 0)
        fw, fh = rod.x2 - rod.x1, rod.y2 - rod.y1
        require(fw > 0 and fh > 0, "the frame has a size (%r x %r)" % (fw, fh))
        calib.destroy()

        # eye blur
        gt.section("eye blur")
        checker = app.createNode(CHECKER_ID)
        require(checker is not None, "create a CheckerBoard", gt.plugin_problem(CHECKER_ID))
        checker.getParam("boxSize").set(20, 20)
        blur = app.createNode(BLUR_ID)
        roto = app.createNode(ROTO_ID)
        require(blur is not None and roto is not None, "create the Blur and the Roto", gt.plugin_problem(ROTO_ID))
        ctx = roto.getRotoContext()
        require(ctx is not None, "the Roto node has a roto context")
        shape = ctx.createEllipse(rod.x1 + 0.5 * fw, rod.y1 + 0.5 * fh, 2 * ELLIPSE_RADIUS * fh, True, 1.0)
        require(shape is not None, "created the Map's ellipse")
        begin, end = build_loop(blur, "eye", checker)
        require(end.connectInput(1, roto), "the Roto is the LoopEnd's Map input")
        end.getParam("mode").set("mapBlend")
        end.getParam("iterations").setValue(BLUR_ITERATIONS)
        end.getParam("interpolation").set("linear")
        size = blur.getParam("size")
        require(size.setDriver("loop.t", 0.0, BLUR_MAX), "the Blur's size is driven from loop.t")
        check(size.hasDriver(0) and size.getDriverEnd(0) == BLUR_MAX,
              "the driver goes to %g (got %s)" % (BLUR_MAX, size.getDriverEnd(0)))
        for i, node in enumerate((checker, begin, blur, end)):
            node.setPosition(0, i * 110)
        roto.setPosition(250, 220)

        source = yield from show(viewer, checker)
        blurred = yield from show(viewer, end)
        gt.shot("m183-eye-blur.png")
        same_far = sum(1 for p in FAR_POINTS if at(blurred, span, *p) == at(source, span, *p))
        check(same_far == len(FAR_POINTS),
              "pixels far outside the ellipse equal the input exactly (%d of %d)" % (same_far, len(FAR_POINTS)))
        inside = [(0.5 + dx * ELLIPSE_RADIUS * 0.6 * fh / fw, 0.5 + dy * ELLIPSE_RADIUS * 0.6)
                  for dx in (-1, -0.5, 0, 0.5, 1) for dy in (-1, -0.5, 0, 0.5, 1)]
        changed = sum(1 for p in inside if at(blurred, span, *p) != at(source, span, *p))
        report("NOTE %d of %d pixels inside the ellipse differ from the input" % (changed, len(inside)))
        check(changed >= len(inside) // 2, "most pixels inside the ellipse differ from the input (%d of %d)" % (changed, len(inside)))

        # map grade
        gt.section("map grade")
        constant = app.createNode(CONSTANT_ID)
        grade = app.createNode(GRADE_ID)
        ramp = app.createNode(RAMP_ID)
        ref = app.createNode(GRADE_ID)
        require(constant is not None and grade is not None and ramp is not None and ref is not None,
                "create the Constant, Grades and Ramp", gt.plugin_problem(RAMP_ID))
        constant.getParam("color").set(0.5, 0.5, 0.5, 1.0)
        ramp.getParam("point0").set(rod.x1, rod.y1 + 0.5 * fh)
        ramp.getParam("point1").set(rod.x2, rod.y1 + 0.5 * fh)
        ramp.getParam("color0").set(0.0, 0.0, 0.0, 0.0)
        ramp.getParam("color1").set(1.0, 1.0, 1.0, 1.0)
        begin2, end2 = build_loop(grade, "ramp", constant)
        require(end2.connectInput(1, ramp), "the Ramp is the second LoopEnd's Map input")
        end2.getParam("mode").set("mapBlend")
        end2.getParam("iterations").setValue(GRADE_ITERATIONS)
        end2.getParam("interpolation").set("linear")
        gain = grade.getParam("multiply")
        require(gain.setDriver("loop.t", 0.0, 1.0), "the Grade's gain is driven from loop.t")
        check(gain.hasDriver(0) and gain.getDriverEnd(0) == 1.0, "the gain driver goes to 1 (got %s)" % gain.getDriverEnd(0))
        require(ref.connectInput(0, constant), "connect the reference Grade")
        for d in range(ref.getParam("multiply").getNumDimensions()):
            ref.getParam("multiply").setValue(0.5, d)
        for i, node in enumerate((constant, begin2, grade, end2)):
            node.setPosition(500, i * 110)
        ramp.setPosition(750, 220)
        ref.setPosition(250, 110)

        plain = yield from show(viewer, constant)
        graded = yield from show(viewer, end2)
        gt.shot("m183-map-grade.png")
        half = yield from show(viewer, ref)
        base = luma(at(plain, span, 0.5, 0.5))
        left = luma(at(graded, span, 0.02, 0.5))
        middle = luma(at(graded, span, 0.5, 0.5))
        right = luma(at(graded, span, 0.98, 0.5))
        report("NOTE input %.1f; left %.1f, middle %.1f, right %.1f; gain 0.5 reference %.1f"
               % (base, left, middle, right, luma(at(half, span, 0.5, 0.5))))
        check(left < 0.5 * base, "the left end is dark (%.1f against the input's %.1f)" % (left, base))
        check(abs(right - base) <= GRADE_TOLERANCE * 2, "the right end is near the input (%.1f against %.1f)" % (right, base))
        check(abs(middle - luma(at(half, span, 0.5, 0.5))) <= GRADE_TOLERANCE,
              "the middle matches a gain of 0.5 (%.1f against %.1f)" % (middle, luma(at(half, span, 0.5, 0.5))))
        check(left < middle < right, "the grade rises from left to right (%.1f, %.1f, %.1f)" % (left, middle, right))

        # panel
        gt.section("panel")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, end), "the Viewer shows the eye blur again")
        yield ("sleep", RENDER_MS)
        gt.close_all_panels()
        yield ("sleep", 300)
        app.clearSelection()
        app.selectNode(end, True)
        QApplication.processEvents()
        views = visible_of("NodeGraph")
        require(bool(views), "the node graph is visible")
        QMetaObject.invokeMethod(hold(views[0]), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: gt.any_panel_open(), POLL_TIMEOUT_S)
        check(ok, "the LoopEnd's panel opened")
        yield ("sleep", 800)
        gt.shot("m183-panel.png")
        gt.close_all_panels()

    gt.run(steps())
except Exception:
    import traceback
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + traceback.format_exc() + "\n")
    os._exit(1)
