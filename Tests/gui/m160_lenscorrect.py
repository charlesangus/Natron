"""Checks the native LensCorrect node in the running GUI and takes the screenshots for the gate.

Sections (all by default; LENSCORRECT_GUI_SECTIONS selects some, comma-separated):
    plugin-id        the native plug-in is in the plug-in list and creates
    metadata         a CheckerBoard with lens EXIF keys set by ModifyMetadata (no JPEG fixture with lens
                     EXIF is shipped): the info knob reports the camera and lens it found and that the
                     focal length came from metadata; a typed focal length then wins; the manual
                     vignette and a fisheye reprojection change the picture
    raw              the Canon CR2 fixture through Develop RAW, with LensCorrect after the chain: the lens
                     is found from the file's EXIF (skipped without Git LFS fixtures)
    settings-panel   the settings panel, which must fit the screen

The whole run needs GUI_TEST_TIMEOUT of about 300 seconds. Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 700
FALLBACK_SIZE = (1920.0, 1080.0)
ALL_SECTIONS = ("plugin-id", "metadata", "raw", "settings-panel")

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"
PLUGIN_MODIFY = "fr.natron.ModifyMetadata"
PLUGIN_LENS = "LensCorrect"
READ_ID = "fr.inria.built-in.Read"
CHAIN_LAST_ID = "fr.natron.RawColorCalibration"

# ModifyMetadata's operation and type indices: Set, Double and String.
OPERATION_SET = 1
TYPE_DOUBLE = 1
TYPE_STRING = 2

PIXEL_DIFF_MIN = 60
CHANGED_PIXELS_MIN = 150

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QMetaObject, QMimeData, QPoint, QPointF, Qt, QUrl
    from PySide6.QtGui import QDragEnterEvent, QDropEvent, QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("LENSCORRECT_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in LENSCORRECT_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

    CR2 = os.path.join(gt.FIXTURES, "raw", "canon-eos-5d-mark-iii.cr2")

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

    def set_metadata(modify, slot, key, value):
        param(modify, "operation%d" % slot).set(OPERATION_SET)
        param(modify, "key%d" % slot).set(key)
        if isinstance(value, str):
            param(modify, "type%d" % slot).set(TYPE_STRING)
            param(modify, "stringValue%d" % slot).set(value)
        else:
            param(modify, "type%d" % slot).set(TYPE_DOUBLE)
            param(modify, "doubleValue%d" % slot).set(float(value))

    def info_of(lens):
        return param(lens, "info").getValue()

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible() and gt.class_name(w) == "Gui":
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def nodes_of(plugin_id):
        return [n for n in app.getChildren() if n.getPluginID() == plugin_id]

    def is_real_fixture(path):
        """A decodable fixture rather than a Git LFS pointer file."""
        return os.path.exists(path) and os.path.getsize(path) > (1 << 20)

    def drop_file(path, fx=0.5, fy=0.5):
        """Drags path onto the node graph; Qt 6 delivers a drop only to the widget the drag entered."""
        app.clearSelection()
        win = main_window()
        graph = root_graph()
        gt.require(win is not None and graph is not None, "the main window and its node graph are up")
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(path)])
        local = QPoint(int(graph.width() * fx), int(graph.height() * fy))
        at = QPointF(win.mapFromGlobal(graph.mapToGlobal(local)))
        QApplication.sendEvent(win, QDragEnterEvent(at.toPoint(), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier))
        QApplication.sendEvent(win, QDropEvent(at, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier))

    # ---- viewer pixels -------------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def frame_all():
        for tab in gt.widgets_of("ViewerTab"):
            try:
                if tab.isVisible() and tab.metaObject().indexOfMethod("centerViewer()") >= 0:
                    QMetaObject.invokeMethod(tab, "centerViewer")
                    break
            except RuntimeError:
                continue
        QApplication.processEvents()

    def viewer_crop(inner=1.0):
        widget = visible_viewer()
        if widget is None:
            return None
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        x0, y0, w, h = top_left.x(), top_left.y(), widget.width(), widget.height()
        QApplication.processEvents()
        img = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        mx, my = int(w * (1.0 - inner) / 2.0), int(h * (1.0 - inner) / 2.0)
        return img.copy(x0 + mx, y0 + my, w - 2 * mx, h - 2 * my)

    def changed_pixels(a, b, stride=2):
        if a is None or b is None or a.size() != b.size():
            return -1, 0
        count = total = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                total += 1
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count, total

    def show(viewer, node):
        connect(viewer, 0, node)
        yield ("sleep", SETTLE_MS)
        frame_all()
        yield ("sleep", SETTLE_MS)

    # ---- sections ------------------------------------------------------------------------

    def section_plugin_id():
        gt.section("plugin-id")
        ids = set(NatronEngine.natron.getPluginIDs())
        check(PLUGIN_LENS in ids, "the native plug-in %s is in the plug-in list" % PLUGIN_LENS)
        lens = create(PLUGIN_LENS, 0, 100)
        check(lens.getPluginID() == PLUGIN_LENS, "creating %s gives a node with that plug-in ID" % PLUGIN_LENS)
        destroy_all([lens])
        yield ("sleep", 100)

    def section_metadata():
        gt.section("metadata")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        modify = create(PLUGIN_MODIFY, 0, 100)
        lens = create(PLUGIN_LENS, 0, 200)
        connect(modify, 0, checker)
        set_metadata(modify, 1, "exif/Make", "Canon")
        set_metadata(modify, 2, "exif/Model", "Canon EOS 5D Mark III")
        set_metadata(modify, 3, "exif/LensModel", "Canon EF 70-200mm f/2.8L IS II USM (stabilised)")
        set_metadata(modify, 4, "exif/FocalLength", 135.0)
        set_metadata(modify, 5, "exif/FNumber", 4.0)
        connect(lens, 0, modify)
        gt.close_all_panels()

        yield from show(viewer, checker)
        source = viewer_crop(0.8)
        yield from show(viewer, lens)
        ok = yield ("until", lambda: "70-200" in info_of(lens), POLL_TIMEOUT_S)
        text = info_of(lens)
        check(ok, "the info knob reports the lens found from exif/LensModel (%r)" % text)
        check("5D Mark III" in text, "the info knob reports the camera found from exif/Make and exif/Model")
        check("135 mm (metadata)" in text, "the focal length comes from exif/FocalLength (%r)" % text)
        check("f/4 (metadata)" in text, "the aperture comes from exif/FNumber (%r)" % text)
        gt.shot("lenscorrect-metadata.png")

        param(lens, "focalLength").set(70.0)
        ok = yield ("until", lambda: "70 mm (knob)" in info_of(lens), POLL_TIMEOUT_S)
        check(ok, "a typed focal length overrides the metadata (%r)" % info_of(lens))

        param(lens, "vignetteStrength").set(0.8)
        yield from show(viewer, lens)
        moved, total = changed_pixels(source, viewer_crop(0.8))
        check(moved >= CHANGED_PIXELS_MIN,
              "the manual vignette brightens the corners (%d of %d sampled pixels differ)" % (moved, total))
        gt.shot("lenscorrect-vignette.png")
        param(lens, "vignetteStrength").set(0.0)

        param(lens, "camera").set("Canon EOS 7D")
        param(lens, "lens").set("Zenitar MC 16mm f/2.8")
        param(lens, "focalLength").set(16.0)
        param(lens, "targetGeometry").set(1)
        yield from show(viewer, lens)
        ok = yield ("until", lambda: "Zenitar" in info_of(lens), POLL_TIMEOUT_S)
        check(ok, "typed camera and lens replace the metadata (%r)" % info_of(lens))
        moved, total = changed_pixels(source, viewer_crop(0.8))
        check(moved >= CHANGED_PIXELS_MIN,
              "fisheye to rectilinear bends the checker (%d of %d sampled pixels differ)" % (moved, total))
        gt.shot("lenscorrect-fisheye-rectilinear.png")

        connect(viewer, 0, checker)
        destroy_all([lens, modify, checker])

    def section_raw():
        gt.section("raw")
        if not is_real_fixture(CR2):
            report("NOTE %s is not a real fixture (no Git LFS); raw section skipped" % CR2)
            return
        develop = NatronEngine.natron.getSettings().getParam("developRawOnRead")
        gt.require(develop is not None and develop.getValue(), "Develop RAW files on read is on")
        viewer = viewer_node()
        before = set(n.getScriptName() for n in app.getChildren())
        drop_file(CR2, 0.2, 0.15)
        ok = yield ("until", lambda: len(nodes_of(CHAIN_LAST_ID)) == 1, POLL_TIMEOUT_S)
        gt.require(ok, "dropping the CR2 builds the Develop RAW chain")
        yield ("sleep", SETTLE_MS)
        last = nodes_of(CHAIN_LAST_ID)[0]
        x, y = last.getPosition()
        lens = create(PLUGIN_LENS, x, y + 150)
        connect(lens, 0, last)
        gt.close_all_panels()
        yield from show(viewer, lens)
        ok = yield ("until", lambda: "70-200" in info_of(lens), POLL_TIMEOUT_S)
        text = info_of(lens)
        check(ok, "the CR2's EXIF finds the EF 70-200mm f/2.8L IS II through the Develop RAW chain (%r)" % text)
        check("5D Mark III" in text, "the CR2's camera is found (%r)" % text)
        check("70 mm (metadata)" in text, "the CR2's focal length comes from metadata (%r)" % text)
        check("not found" not in text, "nothing is reported missing (%r)" % text)
        gt.shot("lenscorrect-raw.png")

        connect(viewer, 0, last)
        added = [n for n in app.getChildren() if n.getScriptName() not in before]
        destroy_all([lens] + [n for n in added if n is not lens])

    def section_settings_panel():
        gt.section("settings-panel")
        viewer = viewer_node()
        checker = make_checker(0, 0)
        lens = create(PLUGIN_LENS, 0, 200, panel=True)
        connect(lens, 0, checker)
        param(lens, "camera").set("Canon EOS 5D Mark III")
        param(lens, "lens").set("Canon EF 70-200mm f/2.8L IS II USM")
        param(lens, "focalLength").set(100.0)
        yield from show(viewer, lens)
        check(gt.any_panel_open(), "the LensCorrect settings panel is open")
        screen = QGuiApplication.primaryScreen().geometry()
        panels = [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]
        for p in panels:
            r = p.geometry()
            check(r.height() <= screen.height() and r.width() <= screen.width(),
                  "the settings panel (%dx%d) fits the %dx%d screen" % (r.width(), r.height(), screen.width(), screen.height()))
        gt.shot("lenscorrect-panel.png")

        param(lens, "method").set(1)
        yield ("sleep", SETTLE_MS)
        gt.shot("lenscorrect-panel-vignette-only.png")

        gt.close_all_panels()
        connect(viewer, 0, checker)
        destroy_all([lens, checker])

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        win = main_window()
        if win is not None:
            win.activateWindow()
            win.raise_()
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {
            "plugin-id": section_plugin_id,
            "metadata": section_metadata,
            "raw": section_raw,
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
