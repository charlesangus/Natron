"""GUI check: IDefocus in the GUI.

Fixture: two bright 16 px squares over a full-format black Constant, one left of the format centre and one right
of it, built from Constants, two Crops and two Merges. The Map is a Roto ellipse around the right square, so the
Map is 1 there and 0 at the left square. Size At Black is 0 and Size 40, so the left square must come out exactly
as it went in and the right one must spread into the bokeh.

The viewer shows the Un-tone-mapped view at 100% around the format centre. The black background keeps the
source's region of definition at the format, so the region-of-definition box (which IDefocus grows by half the
Size) and its corner labels stay outside the viewer and every sampled difference comes from the image.

For each kernel (the default disc, a 6-blade aperture, and an Image kernel from a small square into the Kernel
input) the script compares the viewer against the source: the left half must not change, the right half must, and
the three results must differ from one another on the right.
Screenshots: the viewer of each kernel, and the IDefocus panel.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must run Xvfb at
1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m186-idefocus/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m186_idefocus.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
RENDER_TIMEOUT_S = 90.0
WATCHDOG_S = 600
SETTLE_MS = 500
RENDER_MS = 2500
VIEW_NAME = "Un-tone-mapped"
LIT_LEVEL = 60
PLUGIN_CONSTANT = "Constant"
PLUGIN_CROP = "Crop"
PLUGIN_MERGE = "Merge"
PLUGIN_IDEFOCUS = "IDefocus"
INPUT_SOURCE, INPUT_KERNEL, INPUT_MAP = 0, 2, 3
SQUARE = 16
SQUARE_BOTTOM = 532
LEFT_SQUARE_X = 710
RIGHT_SQUARE_X = 1194
KERNEL_SQUARE = 48
ELLIPSE_X, ELLIPSE_Y, ELLIPSE_DIAMETER = 1202.0, 540.0, 200.0
SIZE = 40.0
PIXEL_DIFF_MIN = 24
UNCHANGED_MAX = 4
CHANGED_MIN = 200
KERNELS_DIFFER_MIN = 100

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

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name),
                "has: " + ", ".join(sorted(q.getScriptName() for q in node.getParams())))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def set_checked(node, name, value):
        p = param(node, name)
        p.setValue(value)
        got = p.get()
        require(abs(float(got) - float(value)) < 1e-6, "%s's %s is %r (read back %r)" % (node.getLabel(), name, value, got))

    def find_option(choice, *names):
        wanted = [n.lower().replace(" ", "") for n in names]
        for i in range(choice.getNumOptions()):
            if choice.getOption(i).lower().replace(" ", "") in wanted:
                return i
        return -1

    def select_kernel(node, name):
        kind = param(node, "kernelType")
        i = find_option(kind, name)
        require(i >= 0, "the Kernel Type menu has %s (%r)" % (name, [str(o) for o in kind.getOptions()]))
        kind.setValue(i)

    def crop_box(source, left, bottom, width, height, x, y):
        crop = create(PLUGIN_CROP, x, y)
        connect(crop, 0, source)
        extent = param(crop, "extent")
        options = [str(o) for o in extent.getOptions()]
        require("size" in options, "the Crop's extent menu has a Size entry (%r)" % (options,))
        extent.setValue(options.index("size"))
        param(crop, "reformat").setValue(False)
        param(crop, "bottomLeft").set(left, bottom)
        param(crop, "size").set(width, height)
        return crop

    def viewer_node():
        return app.getNode("Viewer1") or create("Viewer", 600, 700)

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def visible_viewer():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return views[0] if views else None

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(
            top_left.x(), top_left.y(), widget.width(), widget.height())

    def fit_viewer():
        views = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        if views:
            _held.append(views[0])
            QMetaObject.invokeMethod(views[0], "centerViewer")
        QApplication.processEvents()

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

    def set_zoom(text):
        """Picks text ("100%", "Fit"...) in the viewer's zoom menu; the viewer zooms about its centre."""
        for combo in gt.widgets_of("ComboBox"):
            try:
                if not combo.isVisible() or "Zoom:" not in (combo.toolTip() or ""):
                    continue
                items = [str(i).strip() for i in (combo.property("natronItems") or [])]
            except RuntimeError:
                continue
            if text in items:
                _held.append(combo)
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(text)))
                QApplication.processEvents()
                return True
            return False
        return False

    def lit_pixels(image, x0, x1, stride=2):
        if image is None:
            return -1
        count = 0
        for y in range(0, image.height(), stride):
            for x in range(x0, x1, stride):
                c = image.pixelColor(x, y)
                if max(c.red(), c.green(), c.blue()) > LIT_LEVEL:
                    count += 1
        return count

    def changed_pixels(a, b, x0, x1, stride=2):
        """Sampled pixels in columns [x0, x1) whose RGB differs between a and b by more than PIXEL_DIFF_MIN."""
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), stride):
            for x in range(x0, x1, stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def settled_crop(label):
        """Steps that wait for the viewer to stop changing, then return its crop."""
        last = [None]

        def stable():
            now = viewer_crop()
            same = last[0] is not None and now is not None and changed_pixels(last[0], now, 0, now.width(), 4) == 0
            last[0] = now
            return same

        ok = yield ("until", stable, RENDER_TIMEOUT_S)
        check(ok, label + " rendered and settled")
        return last[0]

    def save_viewer(image, name):
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if image is not None and image.save(path) else "NOTE could not save ") + path)

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
        """showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
            if guis:
                _held.append(guis[0])
                guis[0].activateWindow()
                guis[0].raise_()
            app.selectNode(node, True)
            QApplication.processEvents()
            views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
            if views:
                _held.append(views[0])
                QMetaObject.invokeMethod(views[0], "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def steps():
        ok = yield ("until", lambda: [w for w in gt.widgets_of("NodeGraph") if w.isVisible()], STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph is up")
        viewer_node()
        ok = yield ("until", lambda: visible_viewer() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("fixture")
        plate = create(PLUGIN_CONSTANT, 0, 0)
        param(plate, "color").set(30.0, 30.0, 30.0, 1.0)
        left_sq = crop_box(plate, LEFT_SQUARE_X, SQUARE_BOTTOM, SQUARE, SQUARE, 0, 100)
        right_sq = crop_box(plate, RIGHT_SQUARE_X, SQUARE_BOTTOM, SQUARE, SQUARE, 150, 100)
        background = create(PLUGIN_CONSTANT, -150, 0)
        param(background, "color").set(0.0, 0.0, 0.0, 1.0)
        with_left = create(PLUGIN_MERGE, 0, 200)
        connect(with_left, 0, background)
        connect(with_left, 1, left_sq)
        source = create(PLUGIN_MERGE, 75, 275)
        connect(source, 0, with_left)
        connect(source, 1, right_sq)

        roto = create("Roto", 400, 100)
        ctx = roto.getRotoContext()
        require(ctx is not None, "the Roto node has a roto context")
        ellipse = ctx.createEllipse(ELLIPSE_X, ELLIPSE_Y, ELLIPSE_DIAMETER, True, 1.0)
        require(ellipse is not None, "created a Roto ellipse")

        kernel_plate = create(PLUGIN_CONSTANT, 600, 0)
        param(kernel_plate, "color").set(1.0, 1.0, 1.0, 1.0)
        kernel_src = crop_box(kernel_plate, 960 - KERNEL_SQUARE // 2, 540 - KERNEL_SQUARE // 2,
                              KERNEL_SQUARE, KERNEL_SQUARE, 600, 100)

        gt.section("idefocus")
        node = create(PLUGIN_IDEFOCUS, 75, 350)
        connect(node, INPUT_SOURCE, source)
        connect(node, INPUT_MAP, roto)
        set_checked(node, "sizeAtBlack", 0.0)
        set_checked(node, "size", SIZE)

        show(source)
        fit_viewer()
        require(set_zoom("100%"), "the viewer's zoom menu offers 100%")
        yield ("sleep", RENDER_MS)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)

        def both_squares_lit():
            image = viewer_crop()
            if image is None:
                return False
            mid = image.width() // 2
            return lit_pixels(image, 0, mid) > 0 and lit_pixels(image, mid, image.width()) > 0

        ok = yield ("until", both_squares_lit, RENDER_TIMEOUT_S)
        require(ok, "the source shows a square on each side of the viewer centre")
        reference = yield from settled_crop("the source")
        require(reference is not None, "the viewer is visible")
        half = reference.width() // 2
        report("NOTE source lit samples left %d right %d"
               % (lit_pixels(reference, 0, half), lit_pixels(reference, half, reference.width())))
        save_viewer(reference, "m186-idefocus-source.png")

        show(node)
        results = {}
        for key, shot_name in (("disc", "m186-idefocus-disc.png"),
                               ("blades", "m186-idefocus-blades.png"),
                               ("image", "m186-idefocus-image.png")):
            if key == "blades":
                select_kernel(node, "blades")
                set_checked(node, "blades", 6)
            elif key == "image":
                select_kernel(node, "image")
                connect(node, INPUT_KERNEL, kernel_src)
            yield ("sleep", RENDER_MS)
            image = yield from settled_crop("IDefocus with the %s kernel" % key)
            results[key] = image
            left = changed_pixels(reference, image, 0, half)
            right = changed_pixels(reference, image, half, image.width())
            report("NOTE %s: changed samples left %d right %d" % (key, left, right))
            check(0 <= left <= UNCHANGED_MAX,
                  "%s kernel: the output equals the source where the Map is zero (%d samples differ)" % (key, left))
            check(right >= CHANGED_MIN,
                  "%s kernel: the output differs from the source where the Map is non-zero (%d samples differ, %d required)"
                  % (key, right, CHANGED_MIN))
            save_viewer(image, shot_name)

        for a, b in (("disc", "blades"), ("disc", "image"), ("blades", "image")):
            n = changed_pixels(results[a], results[b], half, results[a].width())
            check(n >= KERNELS_DIFFER_MIN,
                  "the %s and %s bokeh differ on the Map side (%d samples differ, %d required)" % (a, b, n, KERNELS_DIFFER_MIN))

        gt.section("panel")
        select_kernel(node, "blades")
        ok = yield from open_panel(node)
        check(ok, "%s's panel opens" % node.getLabel())
        yield ("sleep", SETTLE_MS)
        gt.shot("m186-idefocus-panel.png")
        gt.close_all_panels()
        yield ("sleep", 300)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
