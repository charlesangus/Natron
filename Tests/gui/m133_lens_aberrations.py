"""Checks the lens aberrations of the native Defocus and ZDefocus in the running GUI and takes the
screenshots for the gate.

The source is a grid of point lights: the product of a CheckerBoard with horizontal lines only and
one with vertical lines only, each line bright, inset by a Rectangle so that the format's edges
carry no line. One light sits at the format's centre. Sections (all by default; LENSAB_GUI_SECTIONS
selects some, comma-separated):
    cat-eye      the centre light keeps its disc while the edge lights clip into cat-eyes
    swirl        the tangential cat-eye differs from the radial one
    chromatic    chromatic aberration gives the lights coloured fringes
    aspect       aspect ratio 0.5 squeezes the bokeh, with and without the cat-eye
    zdefocus     ZDefocus has the same lens knobs, and its settings panel shows them

Only differences between renders and colour fringes are checked, never absolute brightness, so the
viewer's tone-mapped view is kept. Results and screenshots go where Tests/gui/run-gui-test.sh puts
them; see guitest.py. GUI_TEST_TIMEOUT of about 240 seconds covers the run.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 220
SETTLE_MS = 1500
FALLBACK_SIZE = (1920.0, 1080.0)
ALL_SECTIONS = ("cat-eye", "swirl", "chromatic", "aspect", "zdefocus")

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_RECTANGLE = "net.sf.openfx.Rectangle"
PLUGIN_MERGE = "Merge"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"
PLUGIN_DEFOCUS = "Defocus"
PLUGIN_ZDEFOCUS = "ZDefocus"

LENS_KNOBS = ("catEye", "catEyeOrientation", "chromaticAberration", "chromaticOffset", "sphericalAberration",
              "filterShape", "innerSize", "innerFeather", "innerBrightness", "catadioptric", "catadioptricSize")

# Lights per axis across the format, so that the centre falls on a line of each CheckerBoard.
LIGHTS_X = 16
LIGHTS_Y = 10
LIGHT_WIDTH = 4.0
# Each line's value; a light is the product of two, spread over the defocus disc.
LIGHT_VALUE = 10.0
DEFOCUS_SIZE = 40.0

PIXEL_DIFF_MIN = 40
CHANGED_PIXELS_MIN = 40
# The centre light's disc may move by a rounding of the display, never by a clip.
CENTRE_CHANGED_MAX_FRACTION = 0.02
FRINGE_MIN = 60
FRINGE_PIXELS_MIN = 40

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("LENSAB_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in LENSAB_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

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

    def set_choice(node, name, option):
        choice = param(node, name)
        choice.set(option)
        options = list(choice.getOptions())
        return check(option in options and choice.get() == options.index(option),
                     "%s.%s is %r (index %s of %r)" % (node.getLabel(), name, option, choice.get(), options))

    def line_checker(x, y, box_w, box_h):
        """A CheckerBoard whose boxes are black and whose lines, every box_w by box_h, are bright."""
        checker = create(PLUGIN_CHECKER, x, y)
        param(checker, "boxSize").set(box_w, box_h)
        for k in range(4):
            param(checker, "color%d" % k).set(0.0, 0.0, 0.0, 1.0)
        param(checker, "lineColor").set(LIGHT_VALUE, LIGHT_VALUE, LIGHT_VALUE, 1.0)
        param(checker, "lineWidth").set(LIGHT_WIDTH)
        param(checker, "centerlineWidth").set(0.0)
        return checker

    def make_lights():
        """The point-light grid: returns (output node, every node built, (width, height))."""
        probe = create(PLUGIN_CHECKER, -400, 0)
        w, h = rod_size(probe)
        destroy_all([probe])
        box_w, box_h = w / LIGHTS_X, h / LIGHTS_Y
        # Boxes larger than the format leave each CheckerBoard its lines along one axis only.
        rows = line_checker(-200, 0, 4.0 * w, box_h)
        columns = line_checker(0, 0, box_w, 4.0 * h)
        grid = create(PLUGIN_MERGE, -100, 150)
        set_choice(grid, "operation", "multiply")
        connect(grid, 0, columns)
        connect(grid, 1, rows)
        inset = create(PLUGIN_RECTANGLE, 200, 0)
        param(inset, "bottomLeft").setValue(box_w / 2.0, 0)
        param(inset, "bottomLeft").setValue(box_h / 2.0, 1)
        param(inset, "size").setValue(w - box_w, 0)
        param(inset, "size").setValue(h - box_h, 1)
        param(inset, "softness").setValue(0.0)
        param(inset, "color1").set(1.0, 1.0, 1.0, 1.0)
        param(inset, "color0").set(0.0, 0.0, 0.0, 1.0)
        lights = create(PLUGIN_MERGE, 0, 300)
        set_choice(lights, "operation", "multiply")
        connect(lights, 0, grid)
        connect(lights, 1, inset)
        return lights, [lights, inset, grid, columns, rows], (w, h)

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 600)
        return v

    # ---- viewer pixels -------------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def frame_all():
        """Fits the image in the viewer, so each screenshot and each comparison sees the same framing."""
        for tab in gt.widgets_of("ViewerTab"):
            try:
                if tab.isVisible() and tab.metaObject().indexOfMethod("centerViewer()") >= 0:
                    QMetaObject.invokeMethod(tab, "centerViewer")
                    QApplication.processEvents()
                    return
            except RuntimeError:
                continue
        widget = visible_viewer()
        if widget is not None and widget.metaObject().indexOfMethod("fitImageToFormat()") >= 0:
            QMetaObject.invokeMethod(widget, "fitImageToFormat")
        else:
            report("NOTE could not frame the viewer: no centerViewer or fitImageToFormat slot found")
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
        """(x, y, w, h) of the format's lights on screen: the bounding box of the pixels that differ from
        the viewer's background, the top-left pixel. None if the grab shows no light."""
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

    def crop(img, rect):
        if img is None or rect is None:
            return None
        return img.copy(*rect)

    def centre_rect(lights_rect):
        """The screen rect around the centre light, a third of the way to its neighbours."""
        x, y, w, h = lights_rect
        # The unblurred lights' bounding box spans LIGHTS - 2 spacings, the inset dropping the edge lines.
        half_w = max(4, int(w / (LIGHTS_X - 2) / 3.0))
        half_h = max(4, int(h / (LIGHTS_Y - 2) / 3.0))
        cx, cy = x + w // 2, y + h // 2
        return cx - half_w, cy - half_h, 2 * half_w, 2 * half_h

    def corner_rect(lights_rect):
        """The screen rect of the lights' bottom-left quarter of a quarter: the frame's edge lights."""
        x, y, w, h = lights_rect
        return x, y + h - h // 4, w // 4, h // 4

    def changed_pixels(a, b, stride=1):
        """(differing pixels, compared pixels) of two same-sized grabs; (-1, 0) if unusable."""
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

    def fringe_pixels(img, stride=2):
        """Pixels whose red and blue differ by FRINGE_MIN or more: coloured, where grey lights are not."""
        if img is None:
            return -1
        count = 0
        for y in range(0, img.height(), stride):
            for x in range(0, img.width(), stride):
                c = img.pixelColor(x, y)
                if abs(c.red() - c.blue()) >= FRINGE_MIN:
                    count += 1
        return count

    def show(viewer, node):
        """Connects node to the viewer, frames the image and waits for the render to land."""
        connect(viewer, 0, node)
        yield ("sleep", SETTLE_MS)
        frame_all()
        yield ("sleep", SETTLE_MS)

    def defocus_setup(x=0, y=450):
        """The lights, a Defocus of them and the viewer; returns (viewer, lights, defocus, nodes, rect)
        with rect the lights' screen rect, from the lights viewed unblurred."""
        viewer = viewer_node()
        lights, nodes, _size = make_lights()
        defocus = create(PLUGIN_DEFOCUS, x, y)
        connect(defocus, 0, lights)
        param(defocus, "size").set(DEFOCUS_SIZE)
        gt.close_all_panels()
        yield from show(viewer, lights)
        # Every pixel, so that a light a pixel or two wide on screen is not stepped over.
        rect = image_rect(viewer_grab(), stride=1)
        check(rect is not None, "the point lights can be located in the viewer")
        gt.shot("lens-lights.png")
        return viewer, lights, defocus, [defocus] + nodes, rect

    def finish_section(viewer, lights, nodes):
        connect(viewer, 0, lights)
        yield ("sleep", 200)
        viewer.disconnectInput(0)
        destroy_all(nodes)
        yield ("sleep", 200)

    # ---- sections ------------------------------------------------------------------------

    def section_cat_eye():
        gt.section("cat-eye")
        viewer, lights, defocus, nodes, rect = yield from defocus_setup()
        if rect is None:
            yield from finish_section(viewer, lights, nodes)
            return
        yield from show(viewer, defocus)
        discs = viewer_grab()
        gt.shot("lens-disc.png")

        param(defocus, "catEye").set(1.0)
        yield from show(viewer, defocus)
        eyes = viewer_grab()
        gt.shot("lens-cat-eye.png")

        moved, total = changed_pixels(crop(discs, corner_rect(rect)), crop(eyes, corner_rect(rect)))
        check(moved >= CHANGED_PIXELS_MIN,
              "the cat-eye clips the edge lights' discs (%d of %d corner pixels differ)" % (moved, total))
        moved, total = changed_pixels(crop(discs, centre_rect(rect)), crop(eyes, centre_rect(rect)))
        check(0 <= moved <= CENTRE_CHANGED_MAX_FRACTION * total,
              "the centre light keeps its disc (%d of %d centre pixels differ, limit %d)" %
              (moved, total, int(CENTRE_CHANGED_MAX_FRACTION * total)))
        yield from finish_section(viewer, lights, nodes)

    def section_swirl():
        gt.section("swirl")
        viewer, lights, defocus, nodes, rect = yield from defocus_setup()
        if rect is None:
            yield from finish_section(viewer, lights, nodes)
            return
        param(defocus, "catEye").set(0.6)
        set_choice(defocus, "catEyeOrientation", "radial")
        yield from show(viewer, defocus)
        radial = viewer_grab()

        set_choice(defocus, "catEyeOrientation", "tangential")
        yield from show(viewer, defocus)
        swirl = viewer_grab()
        gt.shot("lens-swirl.png")

        moved, total = changed_pixels(crop(radial, corner_rect(rect)), crop(swirl, corner_rect(rect)))
        check(moved >= CHANGED_PIXELS_MIN,
              "the swirl turns the edge lights' cat-eyes (%d of %d corner pixels differ from radial)" % (moved, total))
        yield from finish_section(viewer, lights, nodes)

    def section_chromatic():
        gt.section("chromatic")
        viewer, lights, defocus, nodes, rect = yield from defocus_setup()
        if rect is None:
            yield from finish_section(viewer, lights, nodes)
            return
        yield from show(viewer, defocus)
        grey = fringe_pixels(crop(viewer_grab(), rect))

        param(defocus, "chromaticAberration").set(0.5)
        yield from show(viewer, defocus)
        fringed = fringe_pixels(crop(viewer_grab(), rect))
        gt.shot("lens-chromatic.png")

        check(fringed >= grey + FRINGE_PIXELS_MIN,
              "chromatic aberration colours the lights' edges (%d coloured sampled pixels, %d without)" % (fringed, grey))

        param(defocus, "catEye").set(0.8)
        param(defocus, "sphericalAberration").set(0.5)
        yield from show(viewer, defocus)
        gt.shot("lens-chromatic-cat-eye-spherical.png")
        yield from finish_section(viewer, lights, nodes)

    def section_aspect():
        gt.section("aspect")
        viewer, lights, defocus, nodes, rect = yield from defocus_setup()
        if rect is None:
            yield from finish_section(viewer, lights, nodes)
            return
        yield from show(viewer, defocus)
        round_discs = viewer_grab()

        param(defocus, "aspectRatio").set(0.5)
        yield from show(viewer, defocus)
        ovals = viewer_grab()
        gt.shot("lens-aspect-0.5.png")
        moved, total = changed_pixels(crop(round_discs, centre_rect(rect)), crop(ovals, centre_rect(rect)))
        check(moved >= CHANGED_PIXELS_MIN // 4,
              "aspect ratio 0.5 squeezes the centre light's disc (%d of %d centre pixels differ)" % (moved, total))

        param(defocus, "catEye").set(1.0)
        yield from show(viewer, defocus)
        oval_eyes = viewer_grab()
        gt.shot("lens-aspect-0.5-cat-eye.png")
        moved, total = changed_pixels(crop(ovals, corner_rect(rect)), crop(oval_eyes, corner_rect(rect)))
        check(moved >= CHANGED_PIXELS_MIN,
              "the cat-eye clips the squeezed edge lights (%d of %d corner pixels differ)" % (moved, total))
        yield from finish_section(viewer, lights, nodes)

    def section_zdefocus():
        gt.section("zdefocus")
        viewer = viewer_node()
        lights, nodes, _size = make_lights()
        zdefocus = create(PLUGIN_ZDEFOCUS, 0, 450, panel=True)
        nodes = [zdefocus] + nodes
        connect(zdefocus, 0, lights)
        for name in LENS_KNOBS:
            check(zdefocus.getParam(name) is not None, "ZDefocus has the lens knob %r" % name)
        param(zdefocus, "catEye").set(1.0)
        param(zdefocus, "chromaticAberration").set(0.3)
        yield from show(viewer, zdefocus)
        check(gt.any_panel_open(), "the ZDefocus settings panel is open")
        gt.shot("lens-zdefocus-panel.png")
        gt.close_all_panels()
        yield from finish_section(viewer, lights, nodes)

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {
            "cat-eye": section_cat_eye,
            "swirl": section_swirl,
            "chromatic": section_chromatic,
            "aspect": section_aspect,
            "zdefocus": section_zdefocus,
        }
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
            yield ("sleep", 500)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
