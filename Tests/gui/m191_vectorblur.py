"""The native VectorBlur works in the app (M191 - Native VectorBlur).

The source is a small bright box at the format centre (Constant + Crop) fed through a ShuffleCopy
whose second input is a Constant holding the motion vector in its red/green; the ShuffleCopy writes
that into a project layer named "motion", which the node's Auto layer entry reads. The Viewer is set
to the Un-tone-mapped view; every comparison is relative, between renders of the same viewer region
at 100% zoom (lit-pixel bounding box of the viewer crop).
Checks: the panel fits 1920x1080 without scrolling; a horizontal vector smears the box horizontally
(bbox wider than tall, wider than the unblurred box); a vertical vector smears vertically; Scale 2
lengthens the streak; Offset 0 and 1 streak to opposite sides. Occlusion: a bright near static box
(vector 0, depth 1 in rgba.G) over a dim far bar moving 60 px (depth 10), both built with Merge; with
Occlusion Off the near box's light is dragged sideways over the bar (lit bbox much wider), with
Depth-aware it stays as wide as the unblurred box, and the two renders differ.
Screenshots: m191-vectorblur-panel.png, m191-horizontal.png, m191-vertical.png, m191-scale2.png,
m191-occlusion.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m191-vectorblur/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m191_vectorblur.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 2000
POLL_TIMEOUT_S = 20.0
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
PLUGIN_CROP = "net.sf.openfx.CropPlugin"
PLUGIN_VECTORBLUR = "VectorBlur"
PLUGIN_SHUFFLECOPY = "fr.natron.ShuffleCopy"
PLUGIN_MERGE = "Merge"
VIEW_NAME = "Un-tone-mapped"
SCREEN_HEIGHT = 1080
LIT_LEVEL = 128
PIXEL_DIFF_MIN = 24
GROWTH_MIN = 2.0
CHANGED_PIXELS_MIN = 10
FOCAL_PLANE_CHANGED_MIN = 100
CHANGED_FRACTION_MIN = 0.05
POINT_LEFT, POINT_BOTTOM, POINT_SIZE = 952, 532, 16
VECTOR_PX = 60.0
NEAR_LEFT, NEAR_BOTTOM, NEAR_SIZE = 940, 520, 40
OCCLUSION_SHARP_TOL = 10
MOTION_LAYER = "motion"
EXTENT_MIN = 20  # sampled viewer pixels (stride 2 => 40 px)

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up in a panel die with a temporary wrapper of the panel.
    _held = []
    _viewer = []

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def node_panel(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        _held.append(panel)
                        return panel
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                _held.append(panel)
                return panel
        return None

    def open_panel(node):
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            activate_main_window()
            app.selectNode(node, True)
            QApplication.processEvents()
            view = graph_view()
            invoked = view is not None and QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            if not invoked:
                report("NOTE showSelectedNodeSettingsPanel was not invoked (attempt %d)" % (attempt + 1))
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def constant(rgb, x=0, y=0):
        node = create(PLUGIN_CONSTANT, x, y)
        param(node, "color").set(rgb[0], rgb[1], rgb[2], 1.0)
        return node

    def crop_box(source, left, bottom, width, height, x=0, y=0):
        """Crops the source to a box (the native Constant always fills the format); returns the Crop."""
        crop = create(PLUGIN_CROP, x, y)
        connect(crop, 0, source)
        extent = param(crop, "extent")
        options = [str(o) for o in extent.getOptions()]
        require("size" in options, "the Crop's extent menu has a Size entry (%r)" % (options,))
        extent.setValue(options.index("size"))
        param(crop, "reformat").setValue(False)
        param(crop, "bottomLeft").set(left, bottom)
        param(crop, "size").set(width, height)
        got = param(crop, "size").get()
        require((got.x, got.y) == (width, height), "%s's size is %rx%r (%rx%r)" % (crop.getLabel(), width, height, got.x, got.y))
        got = param(crop, "bottomLeft").get()
        require((got.x, got.y) == (left, bottom), "%s's bottom left is %r,%r (%r,%r)" % (crop.getLabel(), left, bottom, got.x, got.y))
        return crop

    def set_checked(node, name, value):
        """Sets a single-value knob and fails loudly if the value read back differs."""
        p = param(node, name)
        p.setValue(value)
        got = p.get()
        require(abs(float(got) - float(value)) < 1e-6, "%s's %s is %r (read back %r)" % (node.getLabel(), name, value, got))

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def visible_viewer():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return views[0] if views else None

    def choose_view():
        """Switches the viewer's View menu to Un-tone-mapped; returns whether it was found."""
        for w in gt.widgets_of("ComboBox"):
            try:
                if not w.isVisible():
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
                if VIEW_NAME in items:
                    _held.append(w)
                    QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(VIEW_NAME)))
                    QApplication.processEvents()
                    return str(w.property("natronCurrentText")) == VIEW_NAME
            except RuntimeError:
                continue
        return False

    def colour():
        return gt.viewer_centre_colour()

    def is_colour(c, name):
        if c is None:
            return False
        r, g, b = c
        if name == "red":
            return r > 120 and g < 50 and b < 50
        if name == "blue":
            return b > 120 and r < 50 and g < 50
        if name == "bright":
            return min(c) > 140
        if name == "dark":
            return max(c) < 40
        return False

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(
            top_left.x(), top_left.y(), widget.width(), widget.height())

    def changed_pixels(a, b, stride=2):
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def set_zoom(text):
        """Picks text ("100%", "Fit"...) in the viewer's zoom menu."""
        for combo in gt.widgets_of("ComboBox"):
            try:
                if not combo.isVisible() or "Zoom:" not in (combo.toolTip() or ""):
                    continue
                items = [str(i).strip() for i in (combo.property("natronItems") or [])]
            except RuntimeError:
                continue
            if text in items:
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(text)))
                return True
            return check(False, "the zoom menu offers %s (%r)" % (text, items))
        return check(False, "found the viewer's zoom menu")

    def check_changed(reference, other, what):
        """Passes when at least CHANGED_FRACTION_MIN of the reference's lit pixels (and CHANGED_PIXELS_MIN) differ."""
        n = changed_pixels(reference, other)
        lit = lit_pixels(reference)
        need = max(CHANGED_PIXELS_MIN, int(CHANGED_FRACTION_MIN * max(lit, 0)))
        check(n >= need, "%s (%d sampled pixels differ, %d required: %d%% of %d lit, at least %d)"
              % (what, n, need, int(CHANGED_FRACTION_MIN * 100), lit, CHANGED_PIXELS_MIN))

    def panel_shot(node, name):
        ok = yield from open_panel(node)
        check(ok, "%s's panel opens" % node.getLabel())
        yield ("sleep", SETTLE_MS)
        gt.shot(name)
        gt.close_all_panels()
        yield ("sleep", 300)

    def expect_colour(name, label):
        ok = yield ("until", lambda: is_colour(colour(), name), POLL_TIMEOUT_S)
        check(ok, "%s (viewer centre %r)" % (label, colour()))
        return ok

    def find_option(choice, *names):
        """Index of the menu entry whose label or ID matches one of names (case and spaces ignored), or -1."""
        wanted = [n.lower().replace(" ", "") for n in names]
        for i in range(choice.getNumOptions()):
            if choice.getOption(i).lower().replace(" ", "") in wanted:
                return i
        options = [str(o).lower().replace(" ", "") for o in choice.getOptions()]
        for i, o in enumerate(options):
            if o in wanted:
                return i
        return -1

    def lit_pixels(image, stride=2):
        if image is None:
            return -1
        count = 0
        for y in range(0, image.height(), stride):
            for x in range(0, image.width(), stride):
                c = image.pixelColor(x, y)
                if max(c.red(), c.green(), c.blue()) > LIT_LEVEL:
                    count += 1
        return count

    def render_and_count():
        yield ("sleep", RENDER_MS)
        return lit_pixels(viewer_crop())

    def panel_fit(node, name):
        """Opens node's panel, screenshots it and checks it fits the screen without scrolling."""
        ok = yield from open_panel(node)
        check(ok, "%s's panel opens" % node.getLabel())
        yield ("sleep", SETTLE_MS)
        panel = node_panel(node)
        if panel is not None:
            from PySide6.QtWidgets import QAbstractScrollArea
            top = panel.mapToGlobal(panel.rect().topLeft()).y()
            report("NOTE %s panel: height %d, sizeHint height %d, top %d, screen height %d" % (
                node.getLabel(), panel.height(), panel.sizeHint().height(), top, SCREEN_HEIGHT))
            check(panel.height() <= SCREEN_HEIGHT, "%s's panel (%d px) fits the %d px screen" % (
                node.getLabel(), panel.height(), SCREEN_HEIGHT))
            scrolling = []
            for area in panel.findChildren(QAbstractScrollArea):
                try:
                    bar = area.verticalScrollBar()
                    if area.isVisible() and bar.isVisible() and bar.maximum() > 0:
                        scrolling.append((area.objectName() or "scroll area", bar.maximum()))
                except RuntimeError:
                    continue
            check(not scrolling, "%s's panel needs no vertical scrolling (%r)" % (node.getLabel(), scrolling))
        gt.shot(name)
        gt.close_all_panels()
        yield ("sleep", 300)

    def point_source():
        """A bright (value 100) square of POINT_SIZE px at the format centre; green is 1 inside it (the depth)."""
        plate = create(PLUGIN_CONSTANT, 0, 0)
        param(plate, "color").set(100.0, 1.0, 1.0, 1.0)
        return crop_box(plate, POINT_LEFT, POINT_BOTTOM, POINT_SIZE, POINT_SIZE, 0, 100)

    def set_vector(vec, vx, vy):
        param(vec, "color").set(vx, vy, 0.0, 1.0)
        got = param(vec, "color").get()
        require(abs(got.x - vx) < 1e-4 and abs(got.y - vy) < 1e-4,
                "the vector constant is (%r, %r) (read back %r, %r)" % (vx, vy, got.x, got.y))

    def lit_extent(image, stride=2):
        """(min x, max x, min y, max y, count, sum x) of the lit pixels, or None."""
        if image is None:
            return None
        x0 = y0 = 10 ** 9
        x1 = y1 = -1
        n = sx = 0
        for y in range(0, image.height(), stride):
            for x in range(0, image.width(), stride):
                c = image.pixelColor(x, y)
                if max(c.red(), c.green(), c.blue()) > LIT_LEVEL:
                    x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
                    n += 1
                    sx += x
        if n == 0:
            return None
        return (x0, x1, y0, y1, n, sx / float(n))

    def size_of(ext):
        return (0, 0) if ext is None else (ext[1] - ext[0], ext[3] - ext[2])

    def measure():
        yield ("sleep", RENDER_MS)
        ext = lit_extent(viewer_crop())
        report("NOTE lit extent (w,h)=%r count=%r" % (size_of(ext), None if ext is None else ext[4]))
        return ext

    def section_vectorblur():
        gt.section("vectorblur")
        try:
            if app.getProjectLayer(MOTION_LAYER) is None:
                app.addProjectLayer(MOTION_LAYER, ["U", "V"])
        except Exception:
            report("NOTE could not register the motion layer: " + traceback.format_exc().splitlines()[-1])
        box = point_source()
        vec = create(PLUGIN_CONSTANT, 300, 0)
        set_vector(vec, VECTOR_PX, 0.0)
        shuffle = create(PLUGIN_SHUFFLECOPY, 150, 250)
        connect(shuffle, 0, box)
        connect(shuffle, 1, vec)
        shuffle.getParam("out1").setLayer(MOTION_LAYER)
        node = create(PLUGIN_VECTORBLUR, 150, 400)
        connect(node, 0, shuffle)
        set_checked(node, "scale", 0.0)
        show(node)
        yield ("sleep", RENDER_MS)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        yield from panel_fit(node, "m191-vectorblur-panel.png")
        set_zoom("100%")
        sharp = yield from measure()
        require(sharp is not None, "the unblurred box is lit in the viewer")
        sw, sh = size_of(sharp)

        set_checked(node, "scale", 1.0)
        horizontal = yield from measure()
        gt.shot("m191-horizontal.png")
        hw, hh = size_of(horizontal)
        check(horizontal is not None and hw > hh and hw > sw + EXTENT_MIN,
              "a horizontal vector smears the box horizontally (%dx%d lit vs %dx%d unblurred)" % (hw, hh, sw, sh))

        set_vector(vec, 0.0, VECTOR_PX)
        vertical = yield from measure()
        gt.shot("m191-vertical.png")
        vw, vh = size_of(vertical)
        check(vertical is not None and vh > vw and vh > sh + EXTENT_MIN,
              "a vertical vector smears the box vertically (%dx%d lit vs %dx%d unblurred)" % (vw, vh, sw, sh))
        check(vertical is not None and vh > hh + EXTENT_MIN and hw > vw + EXTENT_MIN,
              "changing the vector direction changes the smear direction (h %dx%d, v %dx%d)" % (hw, hh, vw, vh))

        set_vector(vec, VECTOR_PX, 0.0)
        set_checked(node, "scale", 2.0)
        longer = yield from measure()
        gt.shot("m191-scale2.png")
        lw, _lh = size_of(longer)
        check(longer is not None and lw > hw + EXTENT_MIN,
              "Scale 2 lengthens the streak (%d px wide vs %d at Scale 1)" % (lw, hw))
        set_checked(node, "scale", 1.0)

        set_checked(node, "offset", 0.0)
        start = yield from measure()
        set_checked(node, "offset", 1.0)
        end = yield from measure()
        check(start is not None and end is not None and abs(start[5] - end[5]) > EXTENT_MIN,
              "Offset 0 and 1 streak to opposite sides of the box (centroid x %r vs %r)"
              % (None if start is None else round(start[5], 1), None if end is None else round(end[5], 1)))
        set_checked(node, "offset", 0.5)

        set_checked(node, "samples", 2)
        few = yield from measure()
        report("NOTE Samples 2 lit extent %r vs default %r" % (size_of(few), (hw, hh)))
        set_checked(node, "samples", 16)

        set_zoom("Fit")

    def merge_over(top, bottom, x, y):
        m = create(PLUGIN_MERGE, x, y)
        connect(m, 0, bottom)
        connect(m, 1, top)
        return m

    def section_occlusion():
        gt.section("occlusion")
        # A dim, far, moving bar (vector 60 px, depth 10) under a bright, near, static box (vector 0, depth 1).
        # rgba.G carries the depth; the vector plate is built the same way with Merge.
        bar_plate = create(PLUGIN_CONSTANT, 600, 0)
        param(bar_plate, "color").set(0.05, 10.0, 10.0, 1.0)
        bar = crop_box(bar_plate, 760, 500, 400, 80, 600, 100)
        near_plate = create(PLUGIN_CONSTANT, 800, 0)
        param(near_plate, "color").set(100.0, 1.0, 1.0, 1.0)
        near = crop_box(near_plate, NEAR_LEFT, NEAR_BOTTOM, NEAR_SIZE, NEAR_SIZE, 800, 100)
        scene = merge_over(near, bar, 700, 250)
        move_plate = create(PLUGIN_CONSTANT, 1000, 0)
        param(move_plate, "color").set(VECTOR_PX, 0.0, 0.0, 1.0)
        moving = crop_box(move_plate, 760, 500, 400, 80, 1000, 100)
        still_plate = create(PLUGIN_CONSTANT, 1200, 0)
        param(still_plate, "color").set(0.0, 0.0, 0.0, 1.0)
        still = crop_box(still_plate, NEAR_LEFT, NEAR_BOTTOM, NEAR_SIZE, NEAR_SIZE, 1200, 100)
        vectors = merge_over(still, moving, 1100, 250)
        shuffle = create(PLUGIN_SHUFFLECOPY, 900, 400)
        connect(shuffle, 0, scene)
        connect(shuffle, 1, vectors)
        shuffle.getParam("out1").setLayer(MOTION_LAYER)
        node = create(PLUGIN_VECTORBLUR, 900, 550)
        connect(node, 0, shuffle)
        depth = param(node, "depth")
        depth.set("rgba.G")
        require(depth.get() == "rgba.G", "the Depth channel is rgba.G (read back %r)" % (depth.get(),))
        occ = param(node, "occlusion")
        depth_aware = find_option(occ, "depth-aware", "depthaware", "depth")
        require(depth_aware >= 0, "the Occlusion menu has Depth-aware (%r)" % ([str(o) for o in occ.getOptions()],))
        set_checked(node, "scale", 0.0)
        show(node)
        yield ("sleep", RENDER_MS)
        set_zoom("100%")
        sharp = yield from measure()
        require(sharp is not None, "the near box is lit without blur")
        sw, sh = size_of(sharp)

        set_checked(node, "scale", 1.0)
        occ.setValue(0)
        off = yield from measure()
        gt.shot("m191-occlusion-off.png")
        off_crop = viewer_crop()
        occ.setValue(depth_aware)
        on = yield from measure()
        gt.shot("m191-occlusion-on.png")
        on_crop = viewer_crop()
        ow, oh = size_of(off)
        nw, nh = size_of(on)
        n = changed_pixels(off_crop, on_crop)
        report("NOTE near box lit extent: unblurred %dx%d, Occlusion Off %dx%d, Depth-aware %dx%d; %d sampled pixels differ"
               % (sw, sh, ow, oh, nw, nh, n))
        check(off is not None and ow > sw + EXTENT_MIN,
              "with Occlusion Off the moving background drags the near box's light sideways (%d px wide vs %d unblurred)" % (ow, sw))
        check(on is not None and nw <= sw + OCCLUSION_SHARP_TOL,
              "with Depth-aware occlusion the near box stays sharp (%d px wide vs %d unblurred)" % (nw, sw))
        check(ow > nw + EXTENT_MIN and n >= CHANGED_PIXELS_MIN,
              "Occlusion On differs measurably from Off around the box (%d vs %d px wide, %d sampled pixels differ)" % (nw, ow, n))
        set_zoom("Fit")

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from section_vectorblur()
        yield from section_occlusion()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
