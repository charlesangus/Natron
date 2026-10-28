"""Checks the native LensDistortion node in the running GUI and takes the screenshots for the gate.

The source is a CheckerBoard with small boxes and default lines, so bending of the lines shows the
distortion. Sections (all by default; LENSDIST_GUI_SECTIONS selects some, comma-separated):
    plugin-ids       the retired OFX LensDistortion is not in the plug-in list, the native one creates
    models           one screenshot per lens model family, each checked to differ from the source
    round-trip       Distort then Undistort with the same parameters, compared with the source
    stmap            the STMap output mode
    settings-panel   the settings panel, for the Nuke model and for the model with the most knobs

PFBarrel normally takes its coefficients from a file; no fixture is shipped, so its coefficient knobs
are set by hand and the file knob is left empty.

The whole run needs GUI_TEST_TIMEOUT of about 300 seconds. Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import glob
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 700
FALLBACK_SIZE = (1920.0, 1080.0)
ALL_SECTIONS = ("plugin-ids", "models", "round-trip", "stmap", "settings-panel")

PLUGIN_CHECKER = "CheckerBoard"
PLUGIN_VIEWER = "Viewer"
PLUGIN_LENS = "LensDistortion"

PIXEL_DIFF_MIN = 60
CHANGED_PIXELS_MIN = 150
ROUND_TRIP_MAX_FRACTION = 0.06

# (screenshot name, model label, [(knob, value), ...]) in the order of the model choice.
MODELS = (
    ("nuke", "Nuke", [("k1", 0.2), ("k2", 0.05)]),
    ("pfbarrel", "PFBarrel", [("pfC3", 0.2), ("pfC5", 0.05)]),
    ("3de-classic", "3DE Classic", [("tde4_Distortion", 0.2), ("tde4_Quartic_Distortion", 0.05)]),
    ("3de-anamorphic6", "3DE4 Anamorphic Degree 6",
     [("tde4_Cx02_Degree_2", 0.1), ("tde4_Cy02_Degree_2", 0.1), ("tde4_Cx04_Degree_4", 0.05),
      ("tde4_Cx06_Degree_6", 0.03)]),
    ("3de-fisheye8", "3DE4 Radial Fisheye Degree 8", [("tde4_Distortion_Degree_2", 0.1)]),
    ("3de-radial4", "3DE4 Radial Standard Degree 4",
     [("tde4_Distortion_Degree_2", 0.2), ("tde4_Quartic_Distortion_Degree_4", 0.05)]),
    ("3de-anamorphic4", "3DE4 Anamorphic Standard Degree 4",
     [("tde4_Cx02_Degree_2", 0.1), ("tde4_Cy02_Degree_2", 0.1), ("tde4_Cx04_Degree_4", 0.05)]),
    ("panotools", "PanoTools", [("pt_a", 0.05), ("pt_b", 0.05)]),
)

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

    wanted = os.environ.get("LENSDIST_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in LENSDIST_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

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

    def make_checker(x, y):
        checker = create(PLUGIN_CHECKER, x, y)
        w, h = rod_size(checker)
        param(checker, "boxSize").set(w / 8.0, h / 8.0)
        return checker

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 600)
        return v

    def set_model(lens, label):
        choice = param(lens, "model")
        choice.set(label)
        return check(choice.get() == [m[1] for m in MODELS].index(label),
                     "model %r selected (index %s)" % (label, choice.get()))

    # ---- viewer pixels -------------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def frame_all():
        """Fits the image in the viewer, so each screenshot and each comparison sees the same framing."""
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
            gt_viewer = visible_viewer()
            if gt_viewer is not None and gt_viewer.metaObject().indexOfMethod("fitImageToFormat()") >= 0:
                QMetaObject.invokeMethod(gt_viewer, "fitImageToFormat")
                done = True
        if not done:
            report("NOTE could not frame the viewer: no centerViewer or fitImageToFormat slot found")
        QApplication.processEvents()

    def viewer_crop(inner=1.0):
        """The visible viewer's pixels on screen, or just its central inner fraction; None without a viewer."""
        widget = visible_viewer()
        if widget is None:
            return None
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        x0, y0, w, h = top_left.x(), top_left.y(), widget.width(), widget.height()
        QApplication.processEvents()
        img = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        mx, my = int(w * (1.0 - inner) / 2.0), int(h * (1.0 - inner) / 2.0)
        return img.copy(x0 + mx, y0 + my, w - 2 * mx, h - 2 * my)

    def image_rect(img, stride=2):
        """(x, y, w, h) bounding box of the pixels that differ from the viewer's background, taken from the
        top-left corner pixel of a full viewer grab; None if the grab is unusable or shows no image."""
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

    def central_of(img, rect, inner=0.5):
        """The central inner fraction, per axis, of rect within img."""
        x, y, w, h = rect
        mx, my = int(w * (1.0 - inner) / 2.0), int(h * (1.0 - inner) / 2.0)
        return img.copy(x + mx, y + my, w - 2 * mx, h - 2 * my)

    def changed_pixels(a, b, stride=2):
        """(differing sampled pixels, sampled pixels) between two same-sized viewer grabs; (-1, 0) if unusable."""
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
        """Connects node to the viewer, frames the image and waits for the render to land."""
        connect(viewer, 0, node)
        yield ("sleep", SETTLE_MS)
        frame_all()
        yield ("sleep", SETTLE_MS)

    def retired_marker():
        pattern = os.path.join(ROOT, "Engine", "OfxRetired", "*LensDistortion*")
        found = [p for p in glob.glob(pattern) if os.path.isfile(p)]
        return found[0] if len(found) == 1 else None

    # ---- sections ------------------------------------------------------------------------

    def section_plugin_ids():
        gt.section("plugin-ids")
        marker = retired_marker()
        check(marker is not None, "exactly one retired-OFX marker names LensDistortion under Engine/OfxRetired")
        if marker is None:
            return
        retired_id = os.path.basename(marker)
        with open(marker) as f:
            replacement = f.read().strip()
        ids = set(NatronEngine.natron.getPluginIDs())
        check(retired_id not in ids, "the retired OFX plug-in %s is not in the plug-in list" % retired_id)
        check(replacement == PLUGIN_LENS, "the marker's replacement is %r (got %r)" % (PLUGIN_LENS, replacement))
        check(PLUGIN_LENS in ids, "the native plug-in %s is in the plug-in list" % PLUGIN_LENS)
        lens = create(PLUGIN_LENS, 0, 100)
        check(lens.getPluginID() == PLUGIN_LENS, "creating %s gives a node with that plug-in ID (%s)" %
              (PLUGIN_LENS, lens.getPluginID()))
        destroy_all([lens])
        yield ("sleep", 100)

    def section_models():
        gt.section("models")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        lens = create(PLUGIN_LENS, 0, 200)
        connect(lens, 0, checker)
        gt.close_all_panels()

        yield from show(viewer, checker)
        source = viewer_crop(0.6)
        gt.shot("lens-source.png")

        connect(viewer, 0, lens)
        for shot_name, label, coefficients in MODELS:
            if not set_model(lens, label):
                continue
            for knob, value in coefficients:
                param(lens, knob).set(value)
            yield from show(viewer, lens)
            moved, total = changed_pixels(source, viewer_crop(0.6))
            check(moved >= CHANGED_PIXELS_MIN,
                  "%s with %s changes the picture (%d of %d sampled pixels differ from the source)" %
                  (label, ", ".join("%s=%g" % kv for kv in coefficients), moved, total))
            gt.shot("lens-model-%s.png" % shot_name)
            for knob, _value in coefficients:
                p = param(lens, knob)
                p.set(p.getDefaultValue())

        connect(viewer, 0, checker)
        destroy_all([lens, checker])

    def section_round_trip():
        gt.section("round-trip")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        distort = create(PLUGIN_LENS, 0, 200)
        undistort = create(PLUGIN_LENS, 0, 350)
        connect(distort, 0, checker)
        connect(undistort, 0, distort)
        gt.close_all_panels()
        for node in (distort, undistort):
            param(node, "k1").set(0.2)
            param(node, "k2").set(0.05)
        param(distort, "direction").set("Distort")
        param(undistort, "direction").set("Undistort")
        check(param(distort, "direction").get() == 0 and param(undistort, "direction").get() == 1,
              "the first node distorts and the second undistorts")

        yield from show(viewer, checker)
        source_full = viewer_crop()
        rect = image_rect(source_full)
        check(rect is not None, "the source image can be located in the viewer")
        source = central_of(source_full, rect) if rect is not None else None
        gt.shot("lens-roundtrip-source.png")

        yield from show(viewer, distort)
        distorted = central_of(viewer_crop(), rect) if rect is not None else None
        moved, total = changed_pixels(source, distorted)
        check(moved >= CHANGED_PIXELS_MIN, "the Distort node alone changes the picture (%d of %d sampled pixels)" % (moved, total))
        gt.shot("lens-roundtrip-distorted.png")

        yield from show(viewer, undistort)
        # Distort pushes the source's outer ring out of frame, so Undistort cannot restore it and its
        # result has a black border by design. Only the interior of the image is compared: the central
        # half of the image's on-screen rectangle, not of the viewer widget.
        result = central_of(viewer_crop(), rect) if rect is not None else None
        diff, total = changed_pixels(source, result)
        check(0 <= diff <= ROUND_TRIP_MAX_FRACTION * total,
              "Distort then Undistort returns the source (%d of %d sampled interior pixels differ, limit %d)" %
              (diff, total, int(ROUND_TRIP_MAX_FRACTION * total)))
        gt.shot("lens-roundtrip-result.png")

        connect(viewer, 0, checker)
        destroy_all([undistort, distort, checker])

    def section_stmap():
        gt.section("stmap")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        lens = create(PLUGIN_LENS, 0, 200)
        connect(lens, 0, checker)
        gt.close_all_panels()
        param(lens, "k1").set(0.2)
        param(lens, "k2").set(0.05)

        yield from show(viewer, lens)
        image = viewer_crop(0.6)
        param(lens, "outputMode").set("STMap")
        check(param(lens, "outputMode").get() == 1, "the output mode is STMap (index %s)" % param(lens, "outputMode").get())
        yield from show(viewer, lens)
        stmap = viewer_crop(0.6)
        moved, total = changed_pixels(image, stmap)
        check(moved >= CHANGED_PIXELS_MIN,
              "the STMap output differs from the image output (%d of %d sampled pixels)" % (moved, total))
        # An STMap is a smooth ramp in red and green: nearly every sampled pixel is coloured, none is the checker's two-tone pattern.
        gt.shot("lens-stmap.png")

        connect(viewer, 0, checker)
        destroy_all([lens, checker])

    def section_settings_panel():
        gt.section("settings-panel")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        lens = create(PLUGIN_LENS, 0, 200, panel=True)
        connect(lens, 0, checker)
        yield from show(viewer, lens)
        check(gt.any_panel_open(), "the LensDistortion settings panel is open")
        gt.shot("lens-panel-nuke.png")

        set_model(lens, "3DE4 Anamorphic Degree 6")
        yield ("sleep", SETTLE_MS * 2)
        check(gt.any_panel_open(), "the settings panel is still open on the 3DE4 Anamorphic Degree 6 model")
        gt.shot("lens-panel-3de-anamorphic6.png")

        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([lens, checker])

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {
            "plugin-ids": section_plugin_ids,
            "models": section_models,
            "round-trip": section_round_trip,
            "stmap": section_stmap,
            "settings-panel": section_settings_panel,
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
