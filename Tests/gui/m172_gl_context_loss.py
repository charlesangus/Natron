"""GUI checks that every GL widget keeps drawing its text after its pane moves to another top-level
window. QOpenGLWidget destroys its context on such a move and builds a new one, so anything the
widget kept from the old context (glyph atlases, textures, shaders) has to be released and rebuilt.

Sections (all run by default; M172_SECTIONS=viewer,timeline,... picks a subset):
  viewer          the ViewerGL of Viewer1.
  timeline        the TimeLineGui under Viewer1, which moves with the viewer tab.
  histogram       a Histogram added to the node graph's pane.
  curve           the Curve Editor's CurveWidget.
  dopesheet       the Dope Sheet's DopeSheetView.
  param-interact  a CustomParamInteract; skipped, see PARAM_INTERACT_SKIP.
  host-overlay    a Position node's on-viewer handle, whose label one HostOverlay draws in Viewer1 and
                  Viewer2; Viewer2 moves while both viewers are measured.

Each section splits the tab into a pane of its own, then measures text pixels in the widget's
framebuffer at each step:
  docked          the baseline, in the main window.
  floated         the pane floated out (PyTabWidget.floatPane()).
  redocked        the tab moved back to the pane it came from (GuiApp.moveTab()).
  floated-twice   floated, then moved straight into a second floating pane.
  closed          that floating window closed, which moves its tabs to the anchor pane.
A step passes when the widget still exists, its text-pixel count is at least TEXT_KEEP_FRACTION of
the baseline, at most SOLID_MAX_FRACTION of those pixels are solid, and natron.log has no new
GL_ERROR line. A glyph atlas name left over from a dead context names no texture in the new one, so
binding it makes an incomplete texture; fixed-function texturing then turns off and every glyph quad
fills with the text colour. That raises the text-pixel count rather than lowering it, so only the
solid fraction catches it. An atlas that is released but not rebuilt draws blank instead, which the
count catches.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m172-out -e GUI_TEST_TIMEOUT=400 \
        [-e M172_SECTIONS=histogram,timeline] -w /home/bosley/git/Natron natron-dev \
        Tests/gui/run-gui-test.sh Tests/gui/m172_gl_context_loss.py <Natron AppImage or binary>

Screenshots: m172-<section>-{docked,floated,redocked,floated-twice,closed}.png, each the moved
widget's framebuffer (host-overlay also saves m172-host-overlay-viewer1-<step>.png).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 380
SETTLE_MS = 400
RENDER_MS = 1500
# Text pixels: near-grey (channel spread at most TEXT_MAX_SPREAD) and at least TEXT_MIN_CONTRAST
# brighter than the most common luminance, which is the widget's background.
TEXT_MAX_SPREAD = 40
TEXT_MIN_CONTRAST = 70
# The viewer's "Overlays off" notice is red and the histogram's labels are green or red, while the
# project frame's outline and the histogram's curves are grey or yellow. For those widgets text is
# a pixel whose red and green differ by at least COLOURED_TEXT_SPREAD and whose luminance is at
# least COLOURED_MIN_LUM. The absolute floor stands in for the background estimate, which a viewer
# image filling most of the widget would skew. Rows and columns that are LINE_FRACTION hits or
# more are grid lines and do not count.
COLOURED_TEXT_LABELS = ("viewer", "histogram")
COLOURED_TEXT_SPREAD = 25
COLOURED_MIN_LUM = 52
LINE_FRACTION = 0.6
# Floating resizes the widget, so the number of tick labels changes; the bound only has to tell
# drawn text from none.
TEXT_KEEP_FRACTION = 0.3
MIN_TEXT_PX = 15
# A text pixel is solid when its whole 3x3 neighbourhood is text. Antialiased glyph strokes are one
# or two pixels wide, so drawn text keeps this near 0 (at most about 0.01 seen), while glyph quads
# filled with the text colour reach 0.4 to 0.8.
SOLID_MAX_FRACTION = 0.15
ALL_SECTIONS = ("viewer", "timeline", "histogram", "curve", "dopesheet", "param-interact", "host-overlay")
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
GRADE_ID = "net.sf.openfx.GradePlugin"
POSITION_ID = "net.sf.openfx.Position"
VIEWER_ID = "fr.inria.built-in.Viewer"
# Natron gives a parametric parameter's interact to its curve widget, not to a CustomParamInteract,
# and the bundled plugins put interacts only on parametric parameters (ColorLookup, HueCorrect).
PARAM_INTERACT_SKIP = ("no bundled or test plugin has a non-parametric parameter with a custom OFX "
                       "interact, so no CustomParamInteract can be created")

try:
    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtGui import QGuiApplication, QImage
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("M172_SECTIONS")
    selected = wanted.split(",") if wanted else ALL_SECTIONS
    LOG_PATH = os.path.join(gt.OUT, "natron.log")

    # ---- helpers --------------------------------------------------------------------------

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def find_by_ptr(name, address):
        """The live widget of class name at address, or None once it is gone. Looked up again after
        every yield instead of holding a wrapper across one."""
        for w in gt.widgets_of(name):
            try:
                if ptr(w) == address:
                    return w
            except RuntimeError:
                continue
        return None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def pane_name_of(widget):
        pane = ancestor_of_class(widget, "TabWidget")
        return pane.objectName() if pane is not None else None

    def is_floating(widget):
        return widget is not None and ancestor_of_class(widget, "FloatingWidget") is not None

    def close_floating_pane(pane_name):
        """Closes the floating window that holds pane_name, if there still is one."""
        for w in gt.widgets_of("TabWidget"):
            try:
                if w.objectName() != pane_name:
                    continue
                window = ancestor_of_class(w, "FloatingWidget")
                if window is not None and window.isVisible():
                    window.close()
            except RuntimeError:
                continue
        QApplication.processEvents()

    def gl_error_count():
        try:
            with open(LOG_PATH, "r", errors="replace") as f:
                return sum(1 for line in f if "GL_ERROR" in line)
        except OSError:
            return None

    def framebuffer(widget):
        """What the widget draws, rendered through its paintGL into its own framebuffer by
        QWidget.grab (the bundle has no PySide6.QtOpenGLWidgets for grabFramebuffer). Falls back to
        a screen grab of its area if that comes back empty."""
        image = widget.grab().toImage()
        if image.isNull():
            origin = widget.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(),
                                                                widget.width(), widget.height()).toImage()
        return image

    def solid_fraction(hits, width, height):
        """The share of hits whose 3x3 neighbourhood is all hits."""
        rows = [hits[y * width:(y + 1) * width] for y in range(height)]
        across = [[a and b and c for a, b, c in zip(r, r[1:], r[2:])] for r in rows]
        solid = sum(sum(1 for a, b, c in zip(up, mid, down) if a and b and c)
                    for up, mid, down in zip(across, across[1:], across[2:]))
        total = sum(hits)
        return solid / total if total else 0.0

    def text_pixels(image, coloured=False):
        """The number of text pixels in image and the fraction of them that are solid."""
        image = image.convertToFormat(QImage.Format_RGB32)
        if image.isNull() or image.width() == 0 or image.height() == 0:
            return 0, 0.0
        data = bytes(image.constBits())[:image.width() * image.height() * 4]
        blue, green, red = data[0::4], data[1::4], data[2::4]
        lum = [(r * 299 + g * 587 + b * 114) // 1000 for r, g, b in zip(red, green, blue)]
        buckets = [0] * 32
        for v in lum:
            buckets[v >> 3] += 1
        background = (buckets.index(max(buckets)) << 3) + 4
        threshold = COLOURED_MIN_LUM if coloured else background + TEXT_MIN_CONTRAST
        width, height = image.width(), image.height()
        hits = []
        for r, g, b, v in zip(red, green, blue, lum):
            if v < threshold:
                hits.append(False)
            elif coloured:
                hits.append(abs(r - g) >= COLOURED_TEXT_SPREAD)
            else:
                hits.append(max(r, g, b) - min(r, g, b) <= TEXT_MAX_SPREAD)
        if coloured:
            # A grid line is a row or column that is mostly hits; text never is.
            rows = [sum(hits[y * width:(y + 1) * width]) for y in range(height)]
            cols = [sum(hits[x::width]) for x in range(width)]
            hits = [hit and rows[i // width] < LINE_FRACTION * width and cols[i % width] < LINE_FRACTION * height
                    for i, hit in enumerate(hits)]
        return sum(hits), solid_fraction(hits, width, height)

    def save_image(image, name):
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if image.save(path) else "NOTE could not save ") + path)

    def create(plugin_id, x, y, opened=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(opened)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def view(viewer, node):
        current = viewer.getInput(0)
        if current is not None and current.getScriptName() == node.getScriptName():
            return
        if current is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), viewer.getScriptName() + " shows " + node.getLabel())

    # ---- the graph ------------------------------------------------------------------------

    graph = {}

    def build_graph():
        """Constant -> Grade -> Viewer1, with the Grade's white animated so the Curve Editor, the
        Dope Sheet and the timeline have keys to draw. The Constant is a dark blue, which neither
        text test counts (grey text, or red-green difference) as text in the viewer."""
        if graph:
            return
        constant = create(CONSTANT_ID, 0, 0)
        colour = constant.getParam("color")
        for d, v in enumerate((0.05, 0.05, 0.35, 1.0)[:colour.getNumDimensions()]):
            colour.setValue(v, d)
        grade = create(GRADE_ID, 0, 150, opened=True)
        require(grade.connectInput(0, constant), "connect the Grade to the Constant")
        white = grade.getParam("white")
        for d in range(white.getNumDimensions()):
            white.setValueAtTime(1.0, 1, d)
            white.setValueAtTime(1.5, 25, d)
        viewer = app.getNode("Viewer1") or app.createNode(VIEWER_ID)
        require(viewer is not None, "a Viewer exists")
        view(viewer, grade)
        graph.update(constant=constant, grade=grade, viewer=viewer)

    def viewer_gls():
        return [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]

    # ---- the float-out-and-back cycle -----------------------------------------------------

    def measure(step, targets, baselines, errors_before):
        """Counts text pixels in each (label, class, address) target and checks them against the
        baselines, which are filled in on the first call. Returns False if a target is gone."""
        yield ("sleep", RENDER_MS)
        QApplication.processEvents()
        survived = True
        for i, (label, cls, address) in enumerate(targets):
            w = find_by_ptr(cls, address)
            if not check(w is not None, "%s: the %s still exists" % (step, label)):
                survived = False
                continue
            image = framebuffer(w)
            count, solid = text_pixels(image, label in COLOURED_TEXT_LABELS)
            save_image(image, "m172-%s-%s" % (label.replace(" ", "-"), step))
            if baselines[i] is None:
                baselines[i] = count
                check(count >= MIN_TEXT_PX,
                      "%s: the %s draws text (%d text pixels)" % (step, label, count))
            else:
                bound = max(MIN_TEXT_PX, int(TEXT_KEEP_FRACTION * baselines[i]))
                check(count >= bound, "%s: the %s still draws text (%d text pixels, docked %d, need %d)"
                      % (step, label, count, baselines[i], bound))
            check(solid <= SOLID_MAX_FRACTION, "%s: the %s draws glyphs, not filled boxes (%.3f of text pixels "
                  "solid, at most %.2f)" % (step, label, solid, SOLID_MAX_FRACTION))
        errors = gl_error_count()
        if errors is None or errors_before is None:
            report("NOTE %s: %s unreadable, GL errors not checked" % (step, LOG_PATH))
        else:
            check(errors == errors_before, "%s: no new GL_ERROR in natron.log (%d new)" % (step, errors - errors_before))
        return survived

    def float_cycle(section_name, tab, moved, others=()):
        """Runs the docked/floated/redocked/floated-twice/closed steps on the tab whose script name
        is tab. moved is the (class, address) of the GL widget inside it; others are further
        (label, class, address) widgets measured at each step but not moved."""
        cls, address = moved
        targets = [(section_name, cls, address)] + list(others)
        baselines = [None] * len(targets)
        errors_before = gl_error_count()

        def widget():
            return find_by_ptr(cls, address)

        home = pane_name_of(widget())
        require(home is not None, "the %s is in a pane" % section_name)

        # A pane of its own, so floating it takes nothing else along and the home pane stays docked.
        split = app.getTabWidget(home).splitHorizontally()
        require(split is not None, "split the %s pane" % home)
        split_name = split.getScriptName()
        require(app.moveTab(tab, split), "move %s into its own pane" % tab)
        split = None
        yield ("sleep", SETTLE_MS)
        require((yield from measure("docked", targets, baselines, errors_before)), "the %s is up" % section_name)

        app.getTabWidget(split_name).floatPane()
        ok = yield ("until", lambda: is_floating(widget()) and widget().isVisible(), POLL_TIMEOUT_S)
        check(ok, "floated: the %s is in a floating window" % section_name)
        if not (yield from measure("floated", targets, baselines, errors_before)):
            return

        check(app.moveTab(tab, app.getTabWidget(home)), "redocked: moveTab put %s back in %s" % (tab, home))
        ok = yield ("until", lambda: widget() is not None and not is_floating(widget()) and widget().isVisible(),
                    POLL_TIMEOUT_S)
        check(ok, "redocked: the %s is back in the main window" % section_name)
        close_floating_pane(split_name)
        if not (yield from measure("redocked", targets, baselines, errors_before)):
            return

        first = app.getTabWidget(home).splitHorizontally()
        require(first is not None, "split the %s pane again" % home)
        first_name = first.getScriptName()
        require(app.moveTab(tab, first), "move %s into a pane to float" % tab)
        first.floatPane()
        first = None
        ok = yield ("until", lambda: is_floating(widget()) and widget().isVisible(), POLL_TIMEOUT_S)
        check(ok, "floated-twice: the %s is in the first floating window" % section_name)
        yield ("sleep", SETTLE_MS)
        second = app.getTabWidget(home).splitHorizontally()
        require(second is not None, "split the %s pane a third time" % home)
        second_name = second.getScriptName()
        second.floatPane()
        second = None
        QApplication.processEvents()
        check(app.moveTab(tab, app.getTabWidget(second_name)), "floated-twice: moveTab between floating windows")
        ok = yield ("until", lambda: pane_name_of(widget()) == second_name and widget().isVisible(), POLL_TIMEOUT_S)
        check(ok, "floated-twice: the %s is in the second floating window" % section_name)
        close_floating_pane(first_name)
        if not (yield from measure("floated-twice", targets, baselines, errors_before)):
            return

        close_floating_pane(second_name)
        ok = yield ("until", lambda: widget() is not None and not is_floating(widget()) and widget().isVisible(),
                    POLL_TIMEOUT_S)
        check(ok, "closed: closing the floating window put the %s back in the main window" % section_name)
        if not (yield from measure("closed", targets, baselines, errors_before)):
            return
        if pane_name_of(widget()) != home:
            app.moveTab(tab, app.getTabWidget(home))
        yield ("sleep", SETTLE_MS)

    # ---- sections -------------------------------------------------------------------------

    def viewer1_gl():
        build_graph()
        found = viewer_gls()
        require(bool(found), "Viewer1's ViewerGL is on screen")
        return found[0]

    def viewer_section():
        gt.section("viewer")
        gl = viewer1_gl()
        address = ptr(gl)
        # The "Overlays off" notice sits at a fixed spot of the widget whatever its size, unlike the
        # format label at the image's corner, which a small pane clips.
        QTest.keyClick(gl, Qt.Key_O)
        gl = None
        yield from float_cycle("viewer", "Viewer1", ("ViewerGL", address))
        QTest.keyClick(find_by_ptr("ViewerGL", address), Qt.Key_O)
        yield ("sleep", SETTLE_MS)

    def timeline_section():
        gt.section("timeline")
        gl = viewer1_gl()
        tab = ancestor_of_class(gl, "ViewerTab")
        require(tab is not None, "Viewer1's ViewerTab")
        timelines = [t for t in gt.widgets_of("TimeLineGui") if tab.isAncestorOf(t)]
        require(bool(timelines), "Viewer1 has a timeline")
        yield from float_cycle("timeline", "Viewer1", ("TimeLineGui", ptr(timelines[0])))

    def histogram_section():
        gt.section("histogram")
        gl = viewer1_gl()
        graph_view = next((v for v in gt.widgets_of("NodeGraph") if v.isVisible()), None)
        require(graph_view is not None, "the node graph is on screen")
        home = pane_name_of(graph_view)
        before = set(ptr(h) for h in gt.widgets_of("Histogram"))
        pane = next(w for w in gt.widgets_of("TabWidget") if w.objectName() == home)
        from PySide6.QtCore import QMetaObject
        QMetaObject.invokeMethod(pane, "newHistogramHere")
        pane = None
        gl = None
        ok = yield ("until", lambda: any(ptr(h) not in before and h.isVisible() for h in gt.widgets_of("Histogram")),
                    POLL_TIMEOUT_S)
        require(ok, "a new Histogram is on screen")
        histogram = next(h for h in gt.widgets_of("Histogram") if ptr(h) not in before)
        py_pane = app.getTabWidget(home)
        label = py_pane.getTabLabel(py_pane.count() - 1)
        require(label.startswith("Histogram"), "the new tab is the histogram (%s)" % label)
        # The tab's script name is its label with a lower-case first letter ("histogram1").
        yield from float_cycle("histogram", label[0].lower() + label[1:], ("Histogram", ptr(histogram)))

    def curve_section():
        gt.section("curve")
        build_graph()
        widgets = [w for w in gt.widgets_of("CurveWidget") if ancestor_of_class(w, "CurveEditor") is not None]
        require(bool(widgets), "the Curve Editor's CurveWidget exists")
        address = ptr(widgets[0])
        widgets = None
        yield from float_cycle("curve", "curveEditor", ("CurveWidget", address))

    def dopesheet_section():
        gt.section("dopesheet")
        build_graph()
        views = gt.widgets_of("DopeSheetView")
        require(bool(views), "the Dope Sheet's DopeSheetView exists")
        address = ptr(views[0])
        views = None
        yield from float_cycle("dopesheet", "dopeSheetEditor", ("DopeSheetView", address))

    def param_interact_section():
        gt.section("param-interact")
        report("NOTE [param-interact] skipped: " + PARAM_INTERACT_SKIP)
        yield ("sleep", 0)

    def host_overlay_section():
        gt.section("host-overlay")
        build_graph()
        position = create(POSITION_ID, 200, 150, opened=True)
        require(position.connectInput(0, graph["grade"]), "connect the Position to the Grade")
        view(graph["viewer"], position)
        before = set(ptr(w) for w in gt.widgets_of("ViewerGL"))
        viewer2 = app.createNode(VIEWER_ID)
        require(viewer2 is not None, "create a second Viewer")
        view(viewer2, position)
        ok = yield ("until", lambda: any(ptr(w) not in before and w.isVisible() for w in gt.widgets_of("ViewerGL")),
                    POLL_TIMEOUT_S)
        require(ok, "the second viewer is on screen")
        second = next(w for w in gt.widgets_of("ViewerGL") if ptr(w) not in before)
        first = next(w for w in gt.widgets_of("ViewerGL") if ptr(w) in before)
        moved, other = ptr(second), ptr(first)
        second = first = None
        yield from float_cycle("host-overlay", viewer2.getScriptName(), ("ViewerGL", moved),
                               others=[("host-overlay-viewer1", "ViewerGL", other)])
        view(graph["viewer"], graph["grade"])

    # ---- steps ----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: any(v.isVisible() for v in gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        win = None
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        for name in selected:
            if name == "viewer":
                yield from viewer_section()
            elif name == "timeline":
                yield from timeline_section()
            elif name == "histogram":
                yield from histogram_section()
            elif name == "curve":
                yield from curve_section()
            elif name == "dopesheet":
                yield from dopesheet_section()
            elif name == "param-interact":
                yield from param_interact_section()
            elif name == "host-overlay":
                yield from host_overlay_section()
            else:
                check(False, "unknown section " + name)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        sys.stderr.flush()
        os._exit(1)
