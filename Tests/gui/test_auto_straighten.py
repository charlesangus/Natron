"""Checks the native AutoStraighten node in the running GUI and takes the screenshots for sign-off.

The source is a CheckerBoard rotated by a Transform, standing in for a tilted architecture plate,
or keystoned by a CornerPin, standing in for converging verticals.
Sections (all by default; AUTOSTRAIGHTEN_GUI_SECTIONS selects some, comma-separated):
    panel       the settings panel in Drawn Lines mode; Fit Both with no guide lines says there are
                too few and leaves the knobs alone
    lines       on a keystoned checker, picks the Draw Line tool in the viewer toolbar, drags two
                guide lines along the converging sides, and Fit Vertical moves the vertical lens
                shift and straightens the picture: before and after screenshots
    rectangle   Rectangle mode with the corners on the rotated checker's inner square: Fit Both
                moves the rotation knob to undo the tilt, before and after screenshots
    drag        drags the top left corner handle in the viewer and checks that its knob follows

Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py. The run fits
in a GUI_TEST_TIMEOUT of 180 seconds on a 1920x1080 screen.
"""
import math
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 170
SETTLE_MS = 700
FALLBACK_SIZE = (1920.0, 1080.0)
ALL_SECTIONS = ("panel", "lines", "rectangle", "drag")

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"
PLUGIN_TRANSFORM = "net.sf.openfx.TransformPlugin"
PLUGIN_STRAIGHTEN = "AutoStraighten"
PLUGIN_CORNERPIN = "CornerPin"

TILT_DEGREES = 4.0
ROTATION_TOLERANCE = 0.3
PIXEL_DIFF_MIN = 60
CHANGED_PIXELS_MIN = 150
DRAG_PIXELS = (60, 40)
# The CornerPin pulls each top corner in by this fraction of the width.
KEYSTONE = 0.12
LENS_SHIFT_MIN = 0.05

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("AUTOSTRAIGHTEN_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in AUTOSTRAIGHTEN_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

    # ---- graph helpers -------------------------------------------------------------------

    def create(plugin_id, x=0, y=0, panel=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(panel)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        gt.require(p is not None, "%s has a parameter %r" % (node.getPluginID(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        return gt.require(node.connectInput(input_nb, source),
                          "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def destroy_all(nodes):
        for n in nodes:
            try:
                n.destroy(False)
            except Exception:
                report("NOTE could not destroy %r: %s" % (n, traceback.format_exc().splitlines()[-1]))
        QApplication.processEvents()

    def rod_size(node):
        try:
            rod = node.getRegionOfDefinition(1, 0)
            w, h = rod.x2 - rod.x1, rod.y2 - rod.y1
            if w > 0 and h > 0:
                return w, h
        except Exception:
            pass
        report("NOTE no region of definition from %s; assuming %dx%d" % (node.getLabel(), FALLBACK_SIZE[0], FALLBACK_SIZE[1]))
        return FALLBACK_SIZE

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 600)
        return v

    def persistent_message(node):
        try:
            return node.getPersistentMessage() or ""
        except Exception:
            return ""

    def tilted_plate():
        """A CheckerBoard of large boxes rotated by TILT_DEGREES about its centre, and its size."""
        checker = create(PLUGIN_CHECKER, 0, 0)
        w, h = rod_size(checker)
        param(checker, "boxSize").set(w / 8.0, h / 6.0)
        transform = create(PLUGIN_TRANSFORM, 0, 120)
        connect(transform, 0, checker)
        param(transform, "rotate").set(TILT_DEGREES)
        center = param(transform, "center")
        center.setValue(w / 2.0, 0)
        center.setValue(h / 2.0, 1)
        return checker, transform, w, h

    def keystoned_plate():
        """A CheckerBoard whose top corners a CornerPin pulls in, its CornerPin and its size."""
        checker = create(PLUGIN_CHECKER, 0, 0)
        w, h = rod_size(checker)
        param(checker, "boxSize").set(w / 8.0, h / 6.0)
        pin = create(PLUGIN_CORNERPIN, 0, 120)
        connect(pin, 0, checker)
        # to1..to4: bottom left, bottom right, top right, top left.
        for name, (x, y) in (("to1", (0.0, 0.0)), ("to2", (w, 0.0)), ("to3", (w * (1.0 - KEYSTONE), h)), ("to4", (w * KEYSTONE, h))):
            p = param(pin, name)
            p.setValue(x, 0)
            p.setValue(y, 1)
        for name, (x, y) in (("from1", (0.0, 0.0)), ("from2", (w, 0.0)), ("from3", (w, h)), ("from4", (0.0, h))):
            p = param(pin, name)
            p.setValue(x, 0)
            p.setValue(y, 1)
        return checker, pin, w, h

    def tool_action(tool_id):
        """The viewer toolbar's action for tool_id, from the visible tool buttons."""
        for button in gt.widgets_of("ViewerToolButton"):
            try:
                if not button.isVisible():
                    continue
                for act in button.actions():
                    data = act.data()
                    if data and len(data) > 1 and data[1] == tool_id:
                        return act
            except RuntimeError:
                continue
        return None

    def set_corners_on_tilted_square(node, w, h):
        """Puts the corners on the inner square of the source, as the Transform rotated it."""
        a = math.radians(TILT_DEGREES)
        cx, cy = w / 2.0, h / 2.0
        corners = {
            "topLeft": (0.25 * w, 0.75 * h),
            "topRight": (0.75 * w, 0.75 * h),
            "bottomLeft": (0.25 * w, 0.25 * h),
            "bottomRight": (0.75 * w, 0.25 * h),
        }
        for name, (x, y) in corners.items():
            dx, dy = x - cx, y - cy
            p = param(node, name)
            p.setValue(cx + dx * math.cos(a) - dy * math.sin(a), 0)
            p.setValue(cy + dx * math.sin(a) + dy * math.cos(a), 1)

    # ---- viewer pixels and events --------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def frame_all():
        done = False
        for tab in gt.widgets_of("ViewerTab"):
            try:
                if tab.isVisible() and tab.metaObject().indexOfMethod("centerViewer()") >= 0:
                    QMetaObject.invokeMethod(tab, "centerViewer")
                    done = True
                    break
            except RuntimeError:
                continue
        if not done:
            report("NOTE could not frame the viewer: no centerViewer slot found")
        QApplication.processEvents()

    def viewer_grab():
        widget = visible_viewer()
        if widget is None:
            return None
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        QApplication.processEvents()
        img = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return img.copy(top_left.x(), top_left.y(), widget.width(), widget.height())

    def image_rect(img, stride=2):
        """(x, y, w, h), in viewer widget pixels, of what differs from the viewer's background."""
        if img is None or img.width() < 2 or img.height() < 2:
            return None
        bg = img.pixelColor(0, 0)
        xs, ys = [], []
        for y in range(0, img.height(), stride):
            for x in range(0, img.width(), stride):
                c = img.pixelColor(x, y)
                if abs(c.red() - bg.red()) + abs(c.green() - bg.green()) + abs(c.blue() - bg.blue()) > PIXEL_DIFF_MIN:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1

    def changed_pixels(a, b, stride=2):
        if a is None or b is None or a.size() != b.size():
            return -1, 0
        count = total = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                total += 1
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count, total

    def show(viewer, node):
        connect(viewer, 0, node)
        yield ("sleep", SETTLE_MS)
        frame_all()
        yield ("sleep", SETTLE_MS)

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def drag(widget, start, end, steps=6):
        start, end = QPointF(start), QPointF(end)
        send_mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * (i / float(steps)), Qt.NoButton, Qt.LeftButton)
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    # ---- sections ------------------------------------------------------------------------

    def section_panel():
        gt.section("panel")
        viewer = viewer_node()
        checker, transform, _w, _h = tilted_plate()
        straighten = create(PLUGIN_STRAIGHTEN, 0, 240, panel=True)
        connect(straighten, 0, transform)
        yield from show(viewer, straighten)
        check(gt.any_panel_open(), "the AutoStraighten settings panel is open")
        gt.shot("autostraighten-panel-lines.png")

        rotation = param(straighten, "rotation")
        param(straighten, "fitBoth").trigger()
        yield ("sleep", SETTLE_MS)
        message = persistent_message(straighten)
        report("NOTE Fit Both with no guide lines: rotation %g, message %r" % (rotation.get(), message))
        check("not enough guide lines" in message, "Fit Both with no guide lines says there are too few")
        check(rotation.get() == 0.0, "Fit Both with no guide lines leaves the rotation at 0")
        gt.shot("autostraighten-panel-lines-fit.png")

        param(straighten, "lensModel").set("Specific")
        yield ("sleep", SETTLE_MS)
        gt.shot("autostraighten-panel-specific-lens.png")

        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([straighten, transform, checker])

    def section_lines():
        gt.section("lines")
        viewer = viewer_node()
        checker, pin, w, h = keystoned_plate()
        # The checker fills the format, so its on-screen rectangle maps canonical to widget pixels.
        yield from show(viewer, checker)
        rect = image_rect(viewer_grab())
        gt.require(rect is not None, "the source image can be located in the viewer")
        rx, ry, rw, rh = rect

        def to_widget(x, y):
            return QPointF(rx + x / w * rw, ry + (h - y) / h * rh)

        straighten = create(PLUGIN_STRAIGHTEN, 0, 240, panel=True)
        connect(straighten, 0, pin)
        check(param(straighten, "structure").get() == 0, "the structure defaults to Drawn Lines")
        yield from show(viewer, straighten)
        gl = visible_viewer()
        gt.require(gl is not None, "the viewer is visible")
        before = viewer_grab()
        gt.shot("autostraighten-lines-before.png")

        action = tool_action("drawLineTool")
        gt.require(action is not None, "the viewer toolbar has the Draw Line tool")
        action.trigger()
        yield ("sleep", SETTLE_MS)

        # Along the keystoned left and right sides of the checker, which were vertical, from 10%
        # to 90% of the height.
        for x_bottom, x_top in ((0.0, w * KEYSTONE), (w, w * (1.0 - KEYSTONE))):
            start = to_widget(x_bottom + 0.1 * (x_top - x_bottom) + (8.0 if x_bottom == 0.0 else -8.0), 0.1 * h)
            end = to_widget(x_bottom + 0.9 * (x_top - x_bottom) + (8.0 if x_bottom == 0.0 else -8.0), 0.9 * h)
            drag(gl, start, end, steps=10)
            yield ("sleep", SETTLE_MS)
        gt.shot("autostraighten-lines-drawn.png")

        lens_shift_v = param(straighten, "lensShiftV")
        param(straighten, "fitVertical").trigger()
        yield ("sleep", SETTLE_MS)
        message = persistent_message(straighten)
        report("NOTE Fit Vertical on two drawn lines: lensShiftV %g, rotation %g, message %r"
               % (lens_shift_v.get(), param(straighten, "rotation").get(), message))
        check(abs(lens_shift_v.get()) >= LENS_SHIFT_MIN,
              "Fit Vertical on the drawn lines corrects the keystone (lensShiftV %g, message %r)" % (lens_shift_v.get(), message))
        check(abs(param(straighten, "rotation").get()) <= ROTATION_TOLERANCE,
              "a symmetric keystone needs no rotation (rotation %g)" % param(straighten, "rotation").get())
        yield from show(viewer, straighten)
        after = viewer_grab()
        moved, total = changed_pixels(before, after)
        check(moved >= CHANGED_PIXELS_MIN, "the fit changes the picture (%d of %d sampled pixels)" % (moved, total))
        gt.shot("autostraighten-lines-after.png")

        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        gt.shot("autostraighten-lines-after-no-overlay.png")
        connect(viewer, 0, checker)
        destroy_all([straighten, pin, checker])

    def section_rectangle():
        gt.section("rectangle")
        viewer = viewer_node()
        checker, transform, w, h = tilted_plate()
        straighten = create(PLUGIN_STRAIGHTEN, 0, 240, panel=True)
        connect(straighten, 0, transform)
        param(straighten, "structure").set("Rectangle")
        param(straighten, "fitParameters").set("Rotation Only")
        set_corners_on_tilted_square(straighten, w, h)
        yield from show(viewer, straighten)
        before = viewer_grab()
        gt.shot("autostraighten-rectangle-before.png")

        param(straighten, "fitBoth").trigger()
        yield ("sleep", SETTLE_MS)
        rotation = param(straighten, "rotation").get()
        check(abs(abs(rotation) - TILT_DEGREES) <= ROTATION_TOLERANCE,
              "Fit Both on the rectangle undoes the %g degree tilt (rotation %g, message %r)"
              % (TILT_DEGREES, rotation, persistent_message(straighten)))
        check(param(straighten, "lensShiftV").get() == 0.0 and param(straighten, "lensShiftH").get() == 0.0,
              "Rotation Only leaves the lens shifts at 0")
        yield from show(viewer, straighten)
        after = viewer_grab()
        moved, total = changed_pixels(before, after)
        check(moved >= CHANGED_PIXELS_MIN, "the fit changes the picture (%d of %d sampled pixels)" % (moved, total))
        gt.shot("autostraighten-rectangle-after.png")

        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        gt.shot("autostraighten-rectangle-after-no-overlay.png")
        connect(viewer, 0, checker)
        destroy_all([straighten, transform, checker])

    def section_drag():
        gt.section("drag")
        viewer = viewer_node()
        checker, transform, w, h = tilted_plate()
        # The checker fills the format, so its on-screen rectangle maps canonical to widget pixels.
        yield from show(viewer, checker)
        rect = image_rect(viewer_grab())
        gt.require(rect is not None, "the source image can be located in the viewer")
        rx, ry, rw, rh = rect

        straighten = create(PLUGIN_STRAIGHTEN, 0, 240, panel=True)
        connect(straighten, 0, transform)
        param(straighten, "structure").set("Rectangle")
        yield from show(viewer, straighten)
        gl = visible_viewer()
        gt.require(gl is not None, "the viewer is visible")
        corner = param(straighten, "topLeft")
        x0, y0 = corner.getValue(0), corner.getValue(1)
        start = QPointF(rx + x0 / w * rw, ry + (h - y0) / h * rh)
        end = start + QPointF(*DRAG_PIXELS)
        gt.shot("autostraighten-drag-before.png")
        drag(gl, start, end)
        yield ("sleep", SETTLE_MS)
        x1, y1 = corner.getValue(0), corner.getValue(1)
        want_dx, want_dy = DRAG_PIXELS[0] * w / rw, -DRAG_PIXELS[1] * h / rh
        report("NOTE topLeft moved from (%g, %g) to (%g, %g); expected about (%+g, %+g)" % (x0, y0, x1, y1, want_dx, want_dy))
        check(abs((x1 - x0) - want_dx) <= 0.25 * abs(want_dx) and abs((y1 - y0) - want_dy) <= 0.25 * abs(want_dy),
              "dragging the top left handle moves its knob with the mouse")
        gt.shot("autostraighten-drag-after.png")

        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([straighten, transform, checker])

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {"panel": section_panel, "lines": section_lines, "rectangle": section_rectangle, "drag": section_drag}
        for name in SECTIONS:
            report("NOTE running section " + name)
            try:
                yield from runners[name]()
            except SystemExit:
                raise
            except Exception:
                check(False, "section %s raised: %s" % (name, traceback.format_exc()))
                gt.shot("error-%s.png" % name)
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
