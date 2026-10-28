"""The native Defocus and ZDefocus work in the app (M131 - Native Defocus Family).

Sections (the source is a bright Constant cropped to a small square at the format centre, so a
defocused point shows as a disc; the Viewer is set to the Un-tone-mapped view; every comparison is
relative, between two renders of the same viewer region):
  - defocus: the panel fits 1920x1080 without scrolling (height reported against the screen); the bright
    point spreads into a disc, and a larger Size lights more viewer pixels (counts at two sizes); the
    Blades kernel (3 blades) changes the output against the disc;
  - zdefocus: the panel fits the screen; the Depth channel is the source's green channel, which holds 1
    inside the box. With the Focal Plane at 1 the box is in focus (few lit pixels), moving the Focal
    Plane to 0.1 defocuses it (more lit pixels); Lens mode and the focal-plane output also change the output.
Screenshots: m131-defocus-panel.png, m131-defocus.png, m131-defocus-blades.png, m131-zdefocus-panel.png,
m131-zdefocus.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m131-defocus/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m131_defocus.py build/release/App/Natron
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
PLUGIN_DEFOCUS = "Defocus"
PLUGIN_ZDEFOCUS = "ZDefocus"
VIEW_NAME = "Un-tone-mapped"
SCREEN_HEIGHT = 1080
LIT_LEVEL = 128
PIXEL_DIFF_MIN = 24
GROWTH_MIN = 2.0
CHANGED_PIXELS_MIN = 100
POINT_LEFT, POINT_BOTTOM, POINT_SIZE = 952, 532, 16

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

    def section_defocus():
        gt.section("defocus")
        source = point_source()
        node = create(PLUGIN_DEFOCUS, 0, 250)
        connect(node, 0, source)
        set_checked(node, "size", 0.0)
        show(node)
        yield ("sleep", RENDER_MS)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        yield from panel_fit(node, "m131-defocus-panel.png")
        base = yield from render_and_count()
        report("NOTE size 0 lit pixels: %d" % base)
        set_checked(node, "size", 40.0)
        small = yield from render_and_count()
        gt.shot("m131-defocus.png")
        set_checked(node, "size", 100.0)
        large = yield from render_and_count()
        report("NOTE lit pixels: size 0 -> %d, size 40 -> %d, size 100 -> %d" % (base, small, large))
        check(small > base * GROWTH_MIN, "Size 40 spreads the point into a disc (%d lit vs %d unblurred)" % (small, base))
        check(large > small * GROWTH_MIN, "Size 100 lights more than twice the pixels of Size 40 (%d vs %d)" % (large, small))

        set_checked(node, "size", 40.0)
        yield ("sleep", RENDER_MS)
        disc = viewer_crop()
        kind = param(node, "kernelType")
        i = find_option(kind, "blades")
        require(i >= 0, "the Kernel Type menu has Blades (%r)" % ([str(o) for o in kind.getOptions()],))
        kind.setValue(i)
        set_checked(node, "blades", 3)
        yield ("sleep", RENDER_MS)
        blades = viewer_crop()
        gt.shot("m131-defocus-blades.png")
        n = changed_pixels(disc, blades)
        check(n >= CHANGED_PIXELS_MIN, "a 3-blade kernel changes the output against the disc (%d sampled pixels differ)" % n)
        set_checked(node, "rotation", 60.0)
        yield ("sleep", RENDER_MS)
        n = changed_pixels(blades, viewer_crop())
        check(n >= CHANGED_PIXELS_MIN, "rotating the blades by 60 degrees changes the output (%d sampled pixels differ)" % n)

    def set_depth_channel(node):
        depth = param(node, "depth")
        depth.set("rgba.G")
        got = depth.get()
        require(got == "rgba.G", "%s's Depth channel is rgba.G (read back %r)" % (node.getLabel(), got))

    def section_zdefocus():
        gt.section("zdefocus")
        source = point_source()
        node = create(PLUGIN_ZDEFOCUS, 300, 250)
        connect(node, 0, source)
        set_depth_channel(node)
        set_checked(node, "size", 40.0)
        set_checked(node, "maxSize", 50.0)
        set_checked(node, "focalPlane", 1.0)
        show(node)
        yield ("sleep", RENDER_MS)
        yield from panel_fit(node, "m131-zdefocus-panel.png")
        sharp = yield from render_and_count()
        gt.shot("m131-zdefocus.png")
        set_checked(node, "focalPlane", 0.1)
        blurred = yield from render_and_count()
        report("NOTE lit pixels: Focal Plane 1 -> %d, Focal Plane 0.1 -> %d" % (sharp, blurred))
        check(blurred > sharp * GROWTH_MIN,
              "moving the Focal Plane off the box's depth defocuses it (%d lit vs %d in focus)" % (blurred, sharp))
        before = viewer_crop()
        mode = param(node, "mode")
        lens, manual = find_option(mode, "lens"), find_option(mode, "manual")
        require(lens >= 0 and manual >= 0, "the Mode menu has Manual and Lens (%r)" % ([str(o) for o in mode.getOptions()],))
        mode.setValue(lens)
        yield ("sleep", RENDER_MS)
        n = changed_pixels(before, viewer_crop())
        check(n >= CHANGED_PIXELS_MIN, "Lens mode changes the output against Manual (%d sampled pixels differ)" % n)
        mode.setValue(manual)
        out = param(node, "output")
        plane = find_option(out, "focalPlane")
        require(plane >= 0, "the Output menu has the focal plane (%r)" % ([str(o) for o in out.getOptions()],))
        before = viewer_crop()
        out.setValue(plane)
        yield ("sleep", RENDER_MS)
        n = changed_pixels(before, viewer_crop())
        check(n >= CHANGED_PIXELS_MIN, "the focal-plane output differs from the result (%d sampled pixels differ)" % n)

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from section_defocus()
        yield from section_zdefocus()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
