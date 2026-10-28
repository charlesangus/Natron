"""The native KeyMix, Difference and AdjustBBox work in the app, and the LightWrap PyPlug's Switch toggles.

Sections (each node is created from its plugin ID; sources are Constants; the Viewer is set to the
Un-tone-mapped view so screen colours follow the image):
  - keymix: B blue, A red. A white Mask Constant shows A (red), a black one shows B (blue), no Mask
    shows A everywhere (red);
  - difference: B bright grey, A dark grey, then Premult so the alpha shows as brightness. Different
    inputs give a bright viewer, equal inputs a black one (alpha 0);
  - adjustbbox: a Constant whose box is a small square in the bottom-left corner of the format does
    not reach the viewer centre; AdjustBBox with Nearest boundary and a large Num Pixels grows the
    region so the centre shows the Constant's colour. The Python API has no region-of-definition
    getter, so the viewer centre is the probe;
  - lightwrap: the LightWrap PyPlug (its inner Switches follow Use Constant Highlight and Disable
    luminance-Based Wrap) is fed a foreground box and a background. Turning Use Constant Highlight on
    with a blue constant, and turning Disable luminance-Based Wrap on, each change the viewer image.
Screenshots: m121-keymix-panel.png, m121-difference-panel.png, m121-adjustbbox-panel.png,
m121-keymix.png, m121-difference.png, m121-adjustbbox.png, m121-lightwrap-before.png and
m121-lightwrap-constant.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m121-routing-mix/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m121_routing_mix.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 240
SETTLE_MS = 500
POLL_TIMEOUT_S = 20.0
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
PLUGIN_PREMULT = "Premult"
PLUGIN_KEYMIX = "KeyMix"
PLUGIN_DIFFERENCE = "Difference"
PLUGIN_ADJUSTBBOX = "AdjustBBox"
PLUGIN_LIGHTWRAP = "fr.inria.LightWrap"
VIEW_NAME = "Un-tone-mapped"
PIXEL_DIFF_MIN = 24
CHANGED_PIXELS_MIN = 200

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

    def set_constant_box(node, left, bottom, width, height):
        """Gives the Constant a box instead of the project format; returns whether it took."""
        extent = node.getParam("extent")
        size = node.getParam("size")
        corner = node.getParam("btmLeft")
        if extent is None or size is None or corner is None:
            report("NOTE the Constant has no extent/size/btmLeft parameters")
            return False
        options = [str(o) for o in extent.getOptions()]
        index = next((i for i, o in enumerate(options) if o.lower().startswith("size")), -1)
        if index < 0:
            report("NOTE the Constant's extent menu has no Size entry: %r" % (options,))
            return False
        extent.setValue(index)
        corner.setValue(left, bottom)
        size.setValue(width, height)
        return True

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

    def section_keymix():
        gt.section("keymix")
        b = constant((0.0, 0.0, 0.9), 0, 0)
        a = constant((0.9, 0.0, 0.0), 200, 0)
        white = constant((1.0, 1.0, 1.0), 400, 0)
        keymix = create(PLUGIN_KEYMIX, 100, 200)
        connect(keymix, 0, b)
        connect(keymix, 1, a)
        show(keymix)
        yield from panel_shot(keymix, "m121-keymix-panel.png")
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        yield from expect_colour("red", "without a Mask A covers everywhere, red")
        connect(keymix, 2, white)
        yield from expect_colour("red", "a white Mask shows A, red")
        gt.shot("m121-keymix.png")
        param(white, "color").set(0.0, 0.0, 0.0, 1.0)
        yield from expect_colour("blue", "a black Mask shows B, blue")
        param(white, "color").set(1.0, 1.0, 1.0, 1.0)
        yield from expect_colour("red", "the Mask white again shows A, red")

    def section_difference():
        gt.section("difference")
        subject = constant((0.8, 0.8, 0.8), 0, 400)
        plate = constant((0.1, 0.1, 0.1), 200, 400)
        diff = create(PLUGIN_DIFFERENCE, 100, 600)
        premult = create(PLUGIN_PREMULT, 100, 700)
        connect(diff, 0, subject)
        connect(diff, 1, plate)
        connect(premult, 0, diff)
        show(premult)
        yield from panel_shot(diff, "m121-difference-panel.png")
        yield from expect_colour("bright", "differing B and A give a high alpha, so Premult shows a bright image")
        gt.shot("m121-difference.png")
        param(plate, "color").set(0.8, 0.8, 0.8, 1.0)
        yield from expect_colour("dark", "equal B and A give alpha 0, so Premult shows black")

    def section_adjustbbox():
        gt.section("adjustbbox")
        source = constant((0.9, 0.0, 0.0), 0, 800)
        boxed = set_constant_box(source, 0, 0, 200, 200)
        check(boxed, "the Constant is a 200x200 box in the bottom-left corner")
        show(source)
        yield ("sleep", SETTLE_MS * 2)
        before = colour()
        check(not is_colour(before, "red"), "the small box does not reach the viewer centre (%r)" % (before,))
        adjust = create(PLUGIN_ADJUSTBBOX, 0, 900)
        connect(adjust, 0, source)
        param(adjust, "boundary").setValue(1)
        param(adjust, "numpixels").setValue(1200, 800)
        show(adjust)
        yield from panel_shot(adjust, "m121-adjustbbox-panel.png")
        yield from expect_colour("red", "AdjustBBox grew the region of definition to the viewer centre, "
                                        "and Nearest fills it with the edge colour")
        gt.shot("m121-adjustbbox.png")
        param(adjust, "numpixels").setValue(0, 0)
        ok = yield ("until", lambda: not is_colour(colour(), "red"), POLL_TIMEOUT_S)
        check(ok, "Num Pixels 0 leaves the region as it was, so the centre is empty again (%r)" % (colour(),))

    def section_lightwrap():
        gt.section("lightwrap")
        foreground = constant((0.5, 0.5, 0.5), 600, 0)
        set_constant_box(foreground, 700, 380, 520, 320)
        background = constant((0.9, 0.5, 0.1), 800, 0)
        wrap = create(PLUGIN_LIGHTWRAP, 700, 200)
        connect(wrap, 0, foreground)
        connect(wrap, 1, background)
        show(wrap)
        use_constant = param(wrap, "useConstant")
        disable_luma = param(wrap, "disableLuma")
        yield ("sleep", SETTLE_MS * 4)
        base = viewer_crop()
        report("NOTE LightWrap defaults: useConstant=%r disableLuma=%r" % (use_constant.getValue(), disable_luma.getValue()))
        gt.shot("m121-lightwrap-before.png")
        check(base is not None, "the viewer shows the LightWrap output")

        param(wrap, "constantcolor").set(0.0, 0.0, 1.0, 1.0)
        use_constant.setValue(not use_constant.getValue())
        yield ("sleep", SETTLE_MS * 4)
        toggled = viewer_crop()
        n = changed_pixels(base, toggled)
        check(n >= CHANGED_PIXELS_MIN, "toggling Use Constant Highlight changes the output (%d sampled pixels differ)" % n)
        gt.shot("m121-lightwrap-constant.png")
        use_constant.setValue(not use_constant.getValue())
        yield ("sleep", SETTLE_MS * 4)
        n = changed_pixels(base, viewer_crop())
        check(n <= CHANGED_PIXELS_MIN // 4, "toggling it back restores the output (%d sampled pixels differ)" % n)

        disable_luma.setValue(not disable_luma.getValue())
        yield ("sleep", SETTLE_MS * 4)
        n = changed_pixels(base, viewer_crop())
        check(n >= CHANGED_PIXELS_MIN // 4, "toggling Disable luminance-Based Wrap changes the output (%d sampled pixels differ)" % n)
        disable_luma.setValue(not disable_luma.getValue())

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from section_keymix()
        yield from section_difference()
        yield from section_adjustbbox()
        yield from section_lightwrap()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
