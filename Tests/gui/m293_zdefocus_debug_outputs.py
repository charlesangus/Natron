"""ZDefocus's four outputs in the app (M293 - ZDefocus Debug Outputs).

The source is Tests/fixtures/zdefocus-depth.exr, a CG frame (a checkered ground plane and red,
green and blue spheres at distances 3, 6 and 12) with an inverse-distance depth.Z pass. ZDefocus
reads depth.Z, focused on the green sphere (Focal Plane 1/6) with Size 20 and Max Size 20, so the
near ground and the red sphere blur in front and the blue sphere and the sky behind; the ground
nearest the camera passes Max Size. The Viewer is set to the Un-tone-mapped view.

Checks:
  - the Output menu offers Result, Focal Plane, Blur Radius and Slices, labelled as their script names;
  - every output changes the viewer against the one before it;
  - Blur Radius, shown 5 stops down so that diameters up to 32 pixels read below 1, lights the frame;
  - Slices shows white (the slice in focus) and at least three other hues.
Screenshots, one per output, each captioned in the log by a CAPTION line:
m293-zdefocus-result.png, m293-zdefocus-focal-plane.png, m293-zdefocus-blur-radius.png,
m293-zdefocus-slices.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m293-zdefocus/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m293_zdefocus_debug_outputs.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 2500
PLUGIN_ZDEFOCUS = "ZDefocus"
FIXTURE = "zdefocus-depth.exr"
VIEW_NAME = "Un-tone-mapped"
PIXEL_DIFF_MIN = 24
CHANGED_PIXELS_MIN = 200
LIT_LEVEL = 40
LIT_PIXELS_MIN = 200
HUE_PIXELS_MIN = 20
BLUR_RADIUS_STOPS = -5.0
OUTPUTS = [
    # Script name, label, screenshot, caption.
    ("result", "Result", "m293-zdefocus-result.png",
     "Result: the frame defocused around the green sphere; the near ground and red sphere blur in front, "
     "the blue sphere and sky behind."),
    ("focalPlane", "Focal Plane", "m293-zdefocus-focal-plane.png",
     "Focal Plane: the source tinted red in front of the focus, green in it and blue behind it."),
    ("blurRadius", "Blur Radius", "m293-zdefocus-blur-radius.png",
     "Blur Radius, viewer gain 5 stops down: red the signed diameter (negative, so black, in front), "
     "green its size, blue its size after Max Size; where Max Size clamps, on the ground at the bottom, "
     "green exceeds blue."),
    ("slices", "Slices", "m293-zdefocus-slices.png",
     "Slices: white for the slice in focus around the green sphere, the others in hues that cycle with "
     "their distance from it."),
]

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up by class die with a temporary wrapper of their parent.
    _held = []
    _viewer = []

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return p

    def set_checked(node, name, value):
        """Sets a single-value knob and fails loudly if the value read back differs."""
        p = param(node, name)
        p.setValue(value)
        got = p.get()
        require(abs(float(got) - float(value)) < 1e-6, "%s's %s is %r (read back %r)" % (node.getLabel(), name, value, got))

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

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

    def set_gain_stops(stops):
        box = widget_with_tip("SpinBox", "Gain is shown as f-stops")
        if not check(box is not None, "found the viewer's gain box"):
            return
        QMetaObject.invokeMethod(box, "setValue", Q_ARG(float, float(stops)))
        QApplication.processEvents()

    def viewer_crop():
        widgets = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        if not widgets:
            return None
        widget = widgets[0]
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

    def hue_counts(image, stride=2):
        """Sampled pixels per saturated colour, keyed by each channel rounded to 0, half or full."""
        counts = {}
        if image is None:
            return counts
        for y in range(0, image.height(), stride):
            for x in range(0, image.width(), stride):
                c = image.pixelColor(x, y)
                key = []
                for v in (c.red(), c.green(), c.blue()):
                    if v < 40:
                        key.append(0)
                    elif 90 < v < 170:
                        key.append(1)
                    elif v > 215:
                        key.append(2)
                    else:
                        key = None
                        break
                if key is not None and key != [0, 0, 0]:
                    counts[tuple(key)] = counts.get(tuple(key), 0) + 1
        return counts

    def option_names(choice):
        return [str(o) for o in choice.getOptions()]

    def find_option(choice, *names):
        """Index of the menu entry whose ID or label is one of names, or -1."""
        for i in range(choice.getNumOptions()):
            if str(choice.getOption(i)) in names:
                return i
        for i, o in enumerate(option_names(choice)):
            if o in names:
                return i
        return -1

    def section_outputs():
        gt.section("outputs")
        path = os.path.join(gt.FIXTURES, FIXTURE)
        require(os.path.isfile(path), "fixture exists: " + FIXTURE)
        read = app.createReader(path)
        require(read is not None, "create a Read for " + FIXTURE)
        read.setPosition(0, 0)

        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(PLUGIN_ZDEFOCUS, -1, None, props)
        require(node is not None, "create " + PLUGIN_ZDEFOCUS, gt.plugin_problem(PLUGIN_ZDEFOCUS))
        node.setPosition(0, 200)
        require(node.connectInput(0, read), "connect the Read to ZDefocus")
        depth = param(node, "depth")
        depth.set("depth.Z")
        require(depth.get() == "depth.Z", "ZDefocus's Depth channel is depth.Z (read back %r)" % (depth.get(),))
        set_checked(node, "focalPlane", 1.0 / 6.0)
        set_checked(node, "size", 20.0)
        set_checked(node, "maxSize", 20.0)

        output = param(node, "output")
        names = [str(output.getOption(i)) for i in range(output.getNumOptions())]
        report("NOTE Output options: %r %r" % (names, option_names(output)))
        check(output.getNumOptions() == len(OUTPUTS), "the Output menu has %d entries (%r)" % (len(OUTPUTS), names))
        for script, label, _, _ in OUTPUTS:
            check(find_option(output, script, label) >= 0, "the Output menu offers %s (%r)" % (label, names))

        show(node)
        yield ("sleep", RENDER_MS)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)

        previous = None
        for script, label, shot, caption in OUTPUTS:
            i = find_option(output, script, label)
            require(i >= 0, "the Output menu has %s" % label)
            output.setValue(i)
            set_gain_stops(BLUR_RADIUS_STOPS if script == "blurRadius" else 0.0)
            yield ("sleep", RENDER_MS)
            image = viewer_crop()
            gt.shot(shot)
            report("CAPTION %s: %s" % (shot, caption))
            if previous is not None:
                n = changed_pixels(previous, image)
                check(n >= CHANGED_PIXELS_MIN, "%s changes the viewer against the output before it (%d sampled pixels differ)" % (label, n))
            if script == "blurRadius":
                lit = lit_pixels(image)
                check(lit >= LIT_PIXELS_MIN, "Blur Radius lights the frame (%d sampled pixels lit)" % lit)
            if script == "slices":
                counts = hue_counts(image)
                report("NOTE Slices colours (0 none, 1 half, 2 full): %r" % (counts,))
                white = counts.get((2, 2, 2), 0)
                hues = [k for k, v in counts.items() if k != (2, 2, 2) and v >= HUE_PIXELS_MIN]
                check(white >= HUE_PIXELS_MIN, "Slices shows the slice in focus white (%d sampled pixels)" % white)
                check(len(hues) >= 3, "Slices shows at least three other hues (%r)" % (hues,))
            previous = image
        set_gain_stops(0.0)

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from section_outputs()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
