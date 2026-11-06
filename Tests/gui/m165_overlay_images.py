"""A GUI test of native-node overlay images: the test-only Overlay Image Gui node (registered when
NATRON_GUI_TEST_NODES=1, which run-gui-test.sh sets) offers a red mask one canonical pixel wide and a
half-transparent green block when its Show Mask knob is on and its panel open. The viewer must draw
the mask over the image at 100% zoom, still draw the one-pixel mask at 25% zoom (max-downsampling,
not nearest picking), blend the block at its tint's opacity, and stop drawing both when the knob is
turned off.

Run in the natron-dev container:
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m165-overlay-images/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m165_overlay_images.py <AppImage or build/fast/App/Natron>'

Screenshots: m165-overlay-images-<100|25>.png (the mask on) and m165-overlay-images-off.png.
Canonical coordinates are mapped to viewer widget pixels through the ViewerGL's
natronCanonicalToWidget property; pixels are read from screen grabs. The geometry checked here is
the node's (Engine/Nodes/Test/OverlayImageGuiTestNode.h).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 200
SETTLE_MS = 600
NODE_ID = "test.natron.built-in.OverlayImageGui"
VIEWER_ID = "fr.inria.built-in.Viewer"
LINE_X = 200
LINE_Y = (20, 230)
BLOCK = (300, 100, 364, 164)
# Rows of the line sampled at 25% zoom, and the share of them that must show red within a pixel.
ROW_SAMPLES = 20
ROW_SHARE = 0.9

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QPoint, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # PySide wrappers of widgets used across steps; a wrapper that dies takes its children's with it.
    held = {}

    # ---- plumbing --------------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def choose(combo, text):
        """Selects the first item containing text (case-insensitive); False if there is none."""
        items = [str(i) for i in (combo.property("natronItems") or [])]
        for index, item in enumerate(items):
            if text.lower() in item.lower():
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
                QApplication.processEvents()
                return True
        return False

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        value = held["viewer"].property("natronCanonicalToWidget")
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def to_widget(x, y):
        ox, oy, sx, sy = mapping()
        return ox + sx * x, oy + sy * y

    def grab():
        QApplication.processEvents()
        return QGuiApplication.primaryScreen().grabWindow(0).toImage()

    def pixel(image, wx, wy):
        g = held["viewer"].mapToGlobal(QPoint(int(round(wx)), int(round(wy))))
        if not (0 <= g.x() < image.width() and 0 <= g.y() < image.height()):
            return (0, 0, 0)
        c = image.pixelColor(g.x(), g.y())
        return (c.red(), c.green(), c.blue())

    def box(image, wx, wy, radius):
        return [pixel(image, wx + dx, wy + dy) for dy in range(-radius, radius + 1) for dx in range(-radius, radius + 1)]

    def any_in_box(image, wx, wy, radius, predicate):
        return any(predicate(p) for p in box(image, wx, wy, radius))

    def is_red(p):
        return p[0] > 150 and p[1] < 90 and p[2] < 90

    def is_green_blend(p):
        return p[1] > p[0] + 40 and p[1] > p[2] + 40

    def line_rows_with_red(image, radius):
        """How many of the sampled rows of the line show red within radius widget pixels of it."""
        hits = 0
        for i in range(ROW_SAMPLES):
            y = LINE_Y[0] + (LINE_Y[1] - LINE_Y[0]) * i / float(ROW_SAMPLES - 1)
            wx, wy = to_widget(LINE_X + 0.5, y)
            if any(is_red(pixel(image, wx + dx, wy)) for dx in range(-radius, radius + 1)):
                hits += 1
        return hits

    def zoom_to(text):
        ok = choose(held["zoom_combo"], text)
        check(ok, "the viewer zooms to " + text)
        return ok

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        gt.require(NODE_ID in gt._plugin_ids, "the test Overlay Image Gui node is registered (NATRON_GUI_TEST_NODES=%r)"
                   % os.environ.get("NATRON_GUI_TEST_NODES"))
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode(VIEWER_ID)
        gt.require(viewer is not None, "the project has a viewer", gt.plugin_problem(VIEWER_ID))
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(NODE_ID, -1, None, props)
        gt.require(node is not None, "create the Overlay Image Gui node", gt.plugin_problem(NODE_ID))
        held["node"] = node
        held["viewer_node"] = viewer
        node.setPosition(0, 0)
        viewer.setPosition(0, 200)
        gt.require(viewer.connectInput(0, node), "the node feeds the viewer")
        show = node.getParam("showMask")
        gt.require(show is not None, "the node has a Show Mask parameter")
        held["show"] = show

        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "a viewer is up")
        held["viewer"] = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()][0]
        view_combo = widget_with_tip("ComboBox", "View:")
        zoom_combo = widget_with_tip("ComboBox", "Zoom:")
        held["view_combo"] = view_combo
        held["zoom_combo"] = zoom_combo
        gt.require(view_combo is not None and zoom_combo is not None, "found the viewer's View and Zoom menus")
        check(choose(view_combo, "Un-tone-mapped") or choose(view_combo, "Raw"),
              "the viewer shows the Un-tone-mapped (or Raw) view (items %r)" % (view_combo.property("natronItems"),))
        zoom_to("100%")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the viewer publishes natronCanonicalToWidget")

        gt.section("mask off")
        img = grab()
        wx, wy = to_widget(LINE_X + 0.5, 128)
        check(not any_in_box(img, wx, wy, 2, is_red), "no mask is drawn while Show Mask is off")

        gt.section("100% zoom")
        show.set(True)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: any_in_box(grab(), *to_widget(LINE_X + 0.5, 128), 2, is_red), POLL_TIMEOUT_S)
        gt.shot("m165-overlay-images-100.png")
        check(ok, "the mask line is drawn once Show Mask is on")
        img = grab()
        check(line_rows_with_red(img, 1) >= ROW_SAMPLES * ROW_SHARE, "the mask line is drawn along its length")
        bx, by = to_widget((BLOCK[0] + BLOCK[2]) / 2.0, (BLOCK[1] + BLOCK[3]) / 2.0)
        inside = pixel(img, bx, by)
        check(is_green_blend(inside), "the block is a green blended over the grey image (pixel %r)" % (inside,))
        check(inside[1] < 230, "the block's tint opacity leaves the image showing through (pixel %r)" % (inside,))
        ox, oy = to_widget(BLOCK[0] - 20, (BLOCK[1] + BLOCK[3]) / 2.0)
        check(not is_green_blend(pixel(img, ox, oy)) and not is_red(pixel(img, ox, oy)),
              "outside the block shows the plain image (pixel %r)" % (pixel(img, ox, oy),))

        gt.section("25% zoom")
        zoom_to("25%")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: mapping() is not None and abs(mapping()[2]) < 0.5, POLL_TIMEOUT_S)
        check(ok, "the viewer is zoomed out (mapping %r)" % (mapping(),))
        yield ("sleep", SETTLE_MS)
        gt.shot("m165-overlay-images-25.png")
        img = grab()
        rows = line_rows_with_red(img, 2)
        check(rows >= ROW_SAMPLES * ROW_SHARE,
              "the one-pixel mask line survives at 25%% zoom (%d of %d rows show red)" % (rows, ROW_SAMPLES))
        bx, by = to_widget((BLOCK[0] + BLOCK[2]) / 2.0, (BLOCK[1] + BLOCK[3]) / 2.0)
        check(any_in_box(img, bx, by, 1, is_green_blend), "the block is drawn at 25% zoom")

        gt.section("mask off again")
        show.set(False)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: line_rows_with_red(grab(), 2) == 0, POLL_TIMEOUT_S)
        gt.shot("m165-overlay-images-off.png")
        check(ok, "the mask is gone once Show Mask is off")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
