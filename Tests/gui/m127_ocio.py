"""Checks the native OCIO nodes in the running GUI: the colour-space menus follow the project's
OpenColorIO config, an OCIOColorSpace -> OCIODisplay chain changes what the viewer shows, an
OCIOCDLTransform applies a .cc file, and the retired openfx-io OCIO plug-ins are gone from the
plug-in list while the native nodes are in it.

The project's two built-in configs stand in for "two different configs": the default (studio) config
and the CG config, which defines fewer colour spaces. Both define ACES2065-1 and ACEScg.

Sections (all by default; OCIO_GUI_SECTIONS selects some, comma-separated):
    config-switch, chain, cdl-file, retired-ids

The whole run needs GUI_TEST_TIMEOUT of about 300 seconds. Results and screenshots go where
Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 600
POLL_TIMEOUT_S = 25.0
ALL_SECTIONS = ("config-switch", "chain", "cdl-file", "retired-ids")

PLUGIN_CHECKER = "CheckerBoard"
PLUGIN_VIEWER = "Viewer"
PLUGIN_COLORSPACE = "OCIOColorSpace"
PLUGIN_DISPLAY = "OCIODisplay"
PLUGIN_CDL = "OCIOCDLTransform"
PLUGIN_FILE = "OCIOFileTransform"
PLUGIN_LOOK = "OCIOLookTransform"
NATIVE_IDS = (PLUGIN_COLORSPACE, PLUGIN_DISPLAY, PLUGIN_CDL, PLUGIN_FILE, PLUGIN_LOOK)
RETIRED_IDS = ("fr.inria.openfx.OCIOCDLTransform", "fr.inria.openfx.OCIOColorSpace",
               "fr.inria.openfx.OCIODisplay", "fr.inria.openfx.OCIOFileTransform",
               "fr.inria.openfx.OCIOLogConvert", "fr.inria.openfx.OCIOLookTransform")

CONFIG_PARAM = "ocioConfig"
IN_SPACE = "in_colorspace"
OUT_SPACE = "out_colorspace"
IN_SPACE_MENU = "in_colorspaceIndex"
SPACE_A = "ACES2065-1"
SPACE_B = "ACEScg"

PIXEL_DIFF_MIN = 40
CHANGED_PIXELS_MIN = 150

# The ASC CDL held by the test file, with values distinct per channel.
CDL_SLOPE = (1.25, 0.9, 0.6)
CDL_OFFSET = (0.05, -0.02, 0.01)
CDL_POWER = (1.1, 0.8, 0.95)
CDL_SATURATION = 0.6
CDL_TOLERANCE = 1e-4
CDL_FILE_TEXT = """<?xml version="1.0" encoding="UTF-8"?>
<ColorCorrection id="natron_test_cdl">
  <SOPNode>
    <Slope>%s</Slope>
    <Offset>%s</Offset>
    <Power>%s</Power>
  </SOPNode>
  <SATNode>
    <Saturation>%s</Saturation>
  </SATNode>
</ColorCorrection>
""" % (" ".join(map(str, CDL_SLOPE)), " ".join(map(str, CDL_OFFSET)), " ".join(map(str, CDL_POWER)), CDL_SATURATION)

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QMetaObject, QTimer
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("OCIO_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in OCIO_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

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

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 600)
        return v

    def project_param(name):
        p = app.getProjectParam(name)
        gt.require(p is not None, "the project has a parameter %r" % name)
        return p

    def options_of(choice):
        return [str(choice.getOption(i)) for i in range(choice.getNumOptions())]

    def menu_options(node, name):
        return options_of(param(node, name))

    def has_option(options, name):
        return any(name in o for o in options)

    def set_config(label_part):
        """Selects the project config whose menu label contains label_part (case-insensitive)."""
        choice = project_param(CONFIG_PARAM)
        labels = options_of(choice)
        match = [l for l in labels if label_part.lower() in l.lower()]
        gt.require(bool(match), "the project config menu has an entry with %r" % label_part, repr(labels))
        choice.set(match[0])
        QApplication.processEvents()
        return match[0]

    def current_config_label():
        choice = project_param(CONFIG_PARAM)
        return str(choice.getOption(choice.get()))

    # ---- widgets and pixels --------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def fit_viewer():
        """Fits the image to the viewer, so screenshots compare whole frames."""
        for tab in gt.widgets_of("ViewerTab"):
            try:
                if tab.isVisible():
                    QMetaObject.invokeMethod(tab, "centerViewer")
                    break
            except RuntimeError:
                continue
        QApplication.processEvents()

    def screen_image():
        QApplication.processEvents()
        return QGuiApplication.primaryScreen().grabWindow(0).toImage()

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return screen_image().copy(top_left.x(), top_left.y(), widget.width(), widget.height())

    def changed_pixels(a, b, stride=2):
        """How many sampled pixels differ clearly between two same-sized viewer grabs."""
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def combo_items(combo):
        try:
            return [str(i) for i in (combo.property("natronItems") or [])]
        except RuntimeError:
            return []

    def space_combo():
        """The topmost visible knob menu that lists ACEScg, which is a colour-space menu."""
        best = None
        for w in gt.widgets_of("ComboBox"):
            try:
                if not w.isVisible() or not has_option(combo_items(w), SPACE_B):
                    continue
                y = w.mapToGlobal(w.rect().topLeft()).y()
                if best is None or y < best[0]:
                    best = (y, w)
            except RuntimeError:
                continue
        return best[1] if best else None

    def open_menu_and_shoot(name):
        """Opens the first colour-space menu as a popup and saves the screen while it is open.
        The press blocks in the popup's event loop, so the grab runs from a timer."""
        from PySide6.QtCore import QEvent, QPointF, Qt
        from PySide6.QtGui import QMouseEvent
        seen = {}

        def grab():
            popup = QApplication.activePopupWidget()
            seen["popup"] = popup is not None
            gt.shot(name)
            if popup is not None:
                popup.close()
            seen["done"] = True

        combo = space_combo()
        if combo is None:
            return None
        QTimer.singleShot(1500, grab)
        centre = combo.rect().center()
        event = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(combo.mapToGlobal(centre)),
                            Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(combo, event)
        return seen

    # ---- sections ------------------------------------------------------------------------

    def section_config_switch():
        gt.section("config-switch")
        default_label = current_config_label()
        report("NOTE default config %r" % default_label)
        node = create(PLUGIN_COLORSPACE, 0, 0, panel=True)
        yield ("sleep", SETTLE_MS * 2)
        before = menu_options(node, IN_SPACE_MENU)
        check(len(before) > 10, "the default config fills the in_colorspace menu (%d entries)" % len(before))
        check(has_option(before, SPACE_A) and has_option(before, SPACE_B),
              "the default config's menu lists %s and %s" % (SPACE_A, SPACE_B))
        gt.shot("ocio-menu-default.png")

        other = set_config("cg")
        report("NOTE switched to config %r" % other)
        check(current_config_label() != default_label, "the project config changed (%r)" % current_config_label())
        yield ("sleep", SETTLE_MS * 2)
        after = menu_options(node, IN_SPACE_MENU)
        check(after != before, "the in_colorspace menu changed with the config (%d entries before, %d after)" %
              (len(before), len(after)))
        check(len(after) > 10, "the new config fills the menu (%d entries)" % len(after))
        check(has_option(after, SPACE_A) and has_option(after, SPACE_B),
              "the new config's menu lists %s and %s" % (SPACE_A, SPACE_B))
        out_after = menu_options(node, OUT_SPACE + "Index")
        check(out_after == after, "the out_colorspace menu holds the same entries as in_colorspace")

        seen = open_menu_and_shoot("ocio-menu-aces.png")
        check(seen is not None, "a colour-space menu is on the settings panel")
        if seen is not None:
            check(seen.get("done", False) and seen.get("popup", False), "the colour-space menu opened as a popup")
        yield ("sleep", SETTLE_MS)

        set_config(default_label.replace(" (recommended)", ""))
        yield ("sleep", SETTLE_MS * 2)
        restored = menu_options(node, IN_SPACE_MENU)
        check(restored == before, "switching back restores the first menu (%d entries)" % len(restored))
        gt.close_all_panels()
        destroy_all([node])

    def section_chain():
        gt.section("chain")
        viewer = viewer_node()
        checker = create(PLUGIN_CHECKER, 0, 0)
        space = create(PLUGIN_COLORSPACE, 0, 150, panel=True)
        display = create(PLUGIN_DISPLAY, 0, 300, panel=True)
        connect(space, 0, checker)
        connect(display, 0, space)

        param(space, IN_SPACE).set(SPACE_A)
        param(space, OUT_SPACE).set(SPACE_B)
        check(str(param(space, IN_SPACE).get()) == SPACE_A and str(param(space, OUT_SPACE).get()) == SPACE_B,
              "OCIOColorSpace converts %s to %s (got %r, %r)" %
              (SPACE_A, SPACE_B, param(space, IN_SPACE).get(), param(space, OUT_SPACE).get()))
        displays = menu_options(display, "display")
        views = menu_options(display, "view")
        check(len(displays) > 0 and len(views) > 0,
              "OCIODisplay lists displays and views (%d, %d)" % (len(displays), len(views)))
        report("NOTE display %r view %r" % (str(param(display, "display").getOption(param(display, "display").get())),
                                            str(param(display, "view").getOption(param(display, "view").get()))))
        yield ("sleep", SETTLE_MS)

        connect(viewer, 0, checker)
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        check(ok, "a viewer widget is visible")
        yield ("sleep", SETTLE_MS * 2)
        fit_viewer()
        yield ("sleep", SETTLE_MS)
        source = viewer_crop()
        gt.shot("ocio-source.png")

        connect(viewer, 0, display)
        yield ("sleep", SETTLE_MS * 3)
        fit_viewer()
        yield ("sleep", SETTLE_MS)
        shown = viewer_crop()
        diff = changed_pixels(source, shown)
        check(diff >= CHANGED_PIXELS_MIN,
              "the viewer on OCIODisplay differs from the source (%d sampled pixels differ)" % diff)
        gt.shot("ocio-chain.png")

        panels = [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]
        check(len(panels) >= 2, "the OCIOColorSpace and OCIODisplay panels are open (%d)" % len(panels))
        gt.shot("ocio-panels.png")

        # A different view must change the picture.
        view = param(display, "view")
        if view.getNumOptions() > 1:
            view.set((view.get() + 1) % view.getNumOptions())
            yield ("sleep", SETTLE_MS * 3)
            other = viewer_crop()
            diff = changed_pixels(shown, other)
            check(diff >= CHANGED_PIXELS_MIN,
                  "choosing another view changes the picture (%d sampled pixels differ)" % diff)
        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([display, space, checker])

    def section_cdl_file():
        gt.section("cdl-file")
        viewer = viewer_node()
        path = os.path.join(gt.OUT, "natron_test_cdl.cc")
        with open(path, "w") as f:
            f.write(CDL_FILE_TEXT)
        checker = create(PLUGIN_CHECKER, 0, 0)
        cdl = create(PLUGIN_CDL, 0, 150, panel=True)
        connect(cdl, 0, checker)

        slope = param(cdl, "slope")
        defaults = [slope.getValue(i) for i in range(3)]
        check(all(abs(v - 1.0) < 1e-9 for v in defaults), "slope starts at 1 (%s)" % defaults)

        param(cdl, "file").set(path)
        param(cdl, "cccid").set("natron_test_cdl")
        param(cdl, "read_from_file").set(True)
        yield ("sleep", SETTLE_MS * 2)

        # In file mode the knobs hold the file's values, which are shown and locked.
        def knobs_match():
            got = ([param(cdl, "slope").getValue(i) for i in range(3)] +
                   [param(cdl, "offset").getValue(i) for i in range(3)] +
                   [param(cdl, "power").getValue(i) for i in range(3)] +
                   [param(cdl, "saturation").getValue()])
            want = list(CDL_SLOPE) + list(CDL_OFFSET) + list(CDL_POWER) + [CDL_SATURATION]
            return all(abs(g - w) < CDL_TOLERANCE for g, w in zip(got, want))

        ok = yield ("until", knobs_match, POLL_TIMEOUT_S)
        check(ok, "the slope, offset, power and saturation knobs show the file's values")
        check(str(param(cdl, "cccid").get()) == "natron_test_cdl", "the cccid knob holds the id")

        connect(viewer, 0, checker)
        yield ("sleep", SETTLE_MS * 2)
        fit_viewer()
        yield ("sleep", SETTLE_MS)
        source = viewer_crop()
        connect(viewer, 0, cdl)
        yield ("sleep", SETTLE_MS * 3)
        fit_viewer()
        yield ("sleep", SETTLE_MS)
        graded = viewer_crop()
        diff = changed_pixels(source, graded)
        check(diff >= CHANGED_PIXELS_MIN,
              "the viewer on OCIOCDLTransform differs from the source (%d sampled pixels differ)" % diff)
        gt.shot("ocio-cdl.png")

        # With the file off, the node applies its own knobs, which are the identity again or the
        # values just shown; either way the picture must not be the file's grade unless the knobs equal it.
        param(cdl, "read_from_file").set(False)
        yield ("sleep", SETTLE_MS * 2)
        param(cdl, "slope").set(1.0, 1.0, 1.0, 1.0)
        param(cdl, "offset").set(0.0, 0.0, 0.0, 1.0)
        param(cdl, "power").set(1.0, 1.0, 1.0, 1.0)
        param(cdl, "saturation").set(1.0)
        yield ("sleep", SETTLE_MS * 3)
        identity = viewer_crop()
        diff = changed_pixels(source, identity)
        check(0 <= diff < CHANGED_PIXELS_MIN,
              "with the file off and identity knobs the picture matches the source (%d sampled pixels differ)" % diff)
        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([cdl, checker])

    def section_retired_ids():
        gt.section("retired-ids")
        ids = set(str(i) for i in NatronEngine.natron.getPluginIDs())
        for rid in RETIRED_IDS:
            check(rid not in ids, "the retired plug-in %s is not in the plug-in list" % rid)
            marker = os.path.join(gt.ROOT, "Engine", "OfxRetired", rid)
            if os.path.isdir(os.path.dirname(marker)):
                check(os.path.exists(marker), "the source tree keeps a retirement marker for %s" % rid)
        for nid in NATIVE_IDS:
            check(nid in ids, "the native node %s is in the plug-in list" % nid)
        return
        yield

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {
            "config-switch": section_config_switch,
            "chain": section_chain,
            "cdl-file": section_cdl_file,
            "retired-ids": section_retired_ids,
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
