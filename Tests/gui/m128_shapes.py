"""The native shape generators work in the app (M128 - Native Shape Generators).

Sections (the Viewer is created first and set to the Un-tone-mapped view; every comparison is relative,
between viewer pixels of the same run; the viewer frame is located by showing a white Constant first):
  - plugin-ids: Ramp, Radial, Rectangle and ColorWheel are registered; the retired OpenFX IDs
    (net.sf.openfx.Ramp, Radial, Rectangle, ColorWheel) are not in the plug-in list;
  - ramp: the panel fits 1920x1080; brightness increases from Point 0 to Point 1 and is constant across
    the ramp; the falloff changes the curve; moving Point 1 by Python moves the gradient; with the panel
    open, dragging each overlay point moves its knob by the pen's travel;
  - radial: centre bright, outside dark; Softness widens the falloff (fewer fully lit pixels along the
    middle row); moving the extent's box moves the ellipse; dragging the box's corner handle moves it;
  - rectangle: inside Color 1 against outside Color 0; Softness darkens the inside near the edge; Corner
    Radius darkens the corner; dragging the box's corner handle moves it;
  - colorwheel: hue varies around the centre, saturation grows outward, Rotate changes the hue at a
    fixed angle.
Screenshots: m128-<node>-panel.png, m128-<node>.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m128-shapes/out -w /home/bosley/git/Natron/.worktrees/m128 natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m128_shapes.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 400
SETTLE_MS = 500
RENDER_MS = 1500
POLL_TIMEOUT_S = 20.0
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
NATIVE_IDS = ("Ramp", "Radial", "Rectangle", "ColorWheel")
RETIRED_IDS = ("net.sf.openfx.Ramp", "net.sf.openfx.Radial", "net.sf.openfx.Rectangle", "net.sf.openfx.ColorWheel")
VIEW_NAME = "Un-tone-mapped"
SCREEN_HEIGHT = 1080
LIT_LEVEL = 128
# Margins on the 0..255 screen scale between two samples that must differ.
BRIGHT_MARGIN = 25
HUE_MARGIN = 60
SAT_MARGIN = 60
# Pen travel per drag, in viewer-widget pixels (y down).
RAMP_DRAGS = {"point0": (40, -30), "point1": (-35, 25)}
CORNER_DRAG = (30, -25)
# Rounding to the screen pixel plus a pixel of span measurement error, in screen pixels.
TOL_PX = 3.0

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Q_ARG, QMetaObject, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up in a panel die with a temporary wrapper of the panel.
    _held = []
    _viewer = []
    _frame = {}

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

    def set_checked(node, name, value):
        p = param(node, name)
        p.setValue(value)
        got = p.get()
        require(abs(float(got) - float(value)) < 1e-6, "%s's %s is %r (read back %r)" % (node.getLabel(), name, value, got))

    def set_pair(node, name, x, y):
        """Two-value knobs are set with set(x, y); reads the value back."""
        p = param(node, name)
        p.set(x, y)
        got = p.get()
        require(abs(got.x - x) < 1e-3 and abs(got.y - y) < 1e-3,
                "%s's %s is %r,%r (read back %r,%r)" % (node.getLabel(), name, x, y, got.x, got.y))

    def set_colours(node, c0, c1):
        param(node, "color0").set(c0[0], c0[1], c0[2], 1.0)
        param(node, "color1").set(c1[0], c1[1], c1[2], 1.0)

    def find_option(choice, *names):
        wanted = [n.lower().replace(" ", "") for n in names]
        options = [str(o).lower().replace(" ", "") for o in choice.getOptions()]
        for i, o in enumerate(options):
            if o in wanted:
                return i
        for i in range(choice.getNumOptions()):
            if choice.getOption(i).lower().replace(" ", "") in wanted:
                return i
        return -1

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

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(
            top_left.x(), top_left.y(), widget.width(), widget.height())

    # ---- frame geometry: canonical coordinates to viewer-widget pixels ---------------------------

    def measure_frame():
        """Shows a white Constant, finds its frame on screen and records the canonical-to-screen mapping.
        Call again whenever the viewer's size changes (a panel opening or closing)."""
        white = create(PLUGIN_CONSTANT, -300, 0)
        param(white, "color").set(1.0, 1.0, 1.0, 1.0)
        show(white)
        yield ("sleep", RENDER_MS)
        img = viewer_crop()
        xs, ys = [], []
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                c = img.pixelColor(x, y)
                if min(c.red(), c.green(), c.blue()) > 235:
                    xs.append(x)
                    ys.append(y)
        require(bool(xs), "the white frame is on screen in the Un-tone-mapped view")
        rod = white.getRegionOfDefinition(1, 0)
        _frame.update(rod=(rod.x1, rod.y1, rod.x2, rod.y2), span=(min(xs), min(ys), max(xs) + 2, max(ys) + 2))
        report("NOTE frame %r on screen at %r" % (_frame["rod"], _frame["span"]))
        white.destroy()

    def to_screen(cx, cy):
        x1, y1, x2, y2 = _frame["rod"]
        sx0, sy0, sx1, sy1 = _frame["span"]
        return QPointF(sx0 + (cx - x1) / float(x2 - x1) * (sx1 - sx0), sy1 - (cy - y1) / float(y2 - y1) * (sy1 - sy0))

    def units_per_pixel():
        x1, y1, x2, y2 = _frame["rod"]
        sx0, sy0, sx1, sy1 = _frame["span"]
        return (x2 - x1) / float(sx1 - sx0), (y2 - y1) / float(sy1 - sy0)

    def frame_centre():
        x1, y1, x2, y2 = _frame["rod"]
        return 0.5 * (x1 + x2), 0.5 * (y1 + y2)

    def sample(img, cx, cy, r=2):
        """Mean (r, g, b) on the 0..255 screen scale of a small patch at canonical (cx, cy)."""
        p = to_screen(cx, cy)
        px, py = int(round(p.x())), int(round(p.y()))
        total, n = [0, 0, 0], 0
        for y in range(max(0, py - r), min(img.height(), py + r + 1)):
            for x in range(max(0, px - r), min(img.width(), px + r + 1)):
                c = img.pixelColor(x, y)
                total[0] += c.red()
                total[1] += c.green()
                total[2] += c.blue()
                n += 1
        return tuple(t / float(max(n, 1)) for t in total)

    def luma(c):
        return (c[0] + c[1] + c[2]) / 3.0

    def distance(a, b):
        return sum(abs(a[i] - b[i]) for i in range(3))

    def render():
        yield ("sleep", RENDER_MS)
        return viewer_crop()

    def lit_along_row(img, cy, x1, x2, n=60):
        """How many of n evenly spaced samples on the row at canonical y, between canonical x1 and x2, are lit."""
        count = 0
        for i in range(n):
            if luma(sample(img, x1 + (x2 - x1) * (i + 0.5) / n, cy, 1)) > LIT_LEVEL:
                count += 1
        return count

    # ---- mouse ----------------------------------------------------------------------------------

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), QPointF(widget.mapToGlobal(pos.toPoint())),
                                                   button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def drag(widget, start, end, steps=6):
        start, end = QPointF(start), QPointF(end)
        send_mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * (i / float(steps)), Qt.NoButton,
                       Qt.LeftButton)
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def pair(node, name):
        v = param(node, name).get()
        return v.x, v.y

    # ---- panel ----------------------------------------------------------------------------------

    def panel_fit(node, name):
        """Opens node's panel, screenshots it and checks it fits the screen without scrolling."""
        ok = yield from open_panel(node)
        check(ok, "%s's panel opens" % node.getLabel())
        yield ("sleep", SETTLE_MS)
        panel = node_panel(node)
        if panel is not None:
            from PySide6.QtWidgets import QAbstractScrollArea
            report("NOTE %s panel: height %d, sizeHint height %d, screen height %d" % (
                node.getLabel(), panel.height(), panel.sizeHint().height(), SCREEN_HEIGHT))
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

    def make_shape(plugin, x, y):
        node = create(plugin, x, y)
        return node

    def set_box(node, left, bottom, width, height):
        """Puts the extent on the Size entry and sets the box (the overlay's rectangle)."""
        extent = param(node, "extent")
        i = find_option(extent, "size")
        require(i >= 0, "%s's extent menu has a Size entry (%r)" % (node.getLabel(), [str(o) for o in extent.getOptions()]))
        extent.setValue(i)
        set_pair(node, "bottomLeft", left, bottom)
        set_pair(node, "size", width, height)

    def centred_box(node, half_w, half_h):
        cx, cy = frame_centre()
        set_box(node, cx - half_w, cy - half_h, 2 * half_w, 2 * half_h)
        return cx, cy

    def corner_drag_check(node, label):
        """With node's panel open, drags the box's bottom-left corner handle and checks bottomLeft follows."""
        gl = visible_viewer()
        require(gl is not None, "the viewer is visible")
        before = pair(node, "bottomLeft")
        size_before = pair(node, "size")
        ux, uy = units_per_pixel()
        dx, dy = CORNER_DRAG
        start = to_screen(*before)
        drag(gl, start, start + QPointF(dx, dy))
        yield ("sleep", SETTLE_MS)
        after = pair(node, "bottomLeft")
        want = (before[0] + dx * ux, before[1] - dy * uy)
        report("NOTE %s bottomLeft %r -> %r, wanted %r (size %r -> %r)" % (label, before, after, want, size_before, pair(node, "size")))
        check(abs(after[0] - want[0]) <= TOL_PX * ux and abs(after[1] - want[1]) <= TOL_PX * uy,
              "%s: dragging the box's corner handle moves bottomLeft by the pen's travel (%r -> %r, wanted %r)"
              % (label, before, after, want))

    # ---- sections -------------------------------------------------------------------------------

    def section_plugin_ids():
        gt.section("plugin-ids")
        ids = set(NatronEngine.natron.getPluginIDs())
        for pid in NATIVE_IDS:
            check(pid in ids, "the native %s is registered" % pid)
        for pid in RETIRED_IDS:
            check(pid not in ids, "the retired OpenFX %s is not in the plug-in list" % pid)
        for pid in NATIVE_IDS:
            node = create(pid, 900, 900)
            check(node.getMaxInputCount() == 0,
                  "%s is a pure generator with no inputs (%d)" % (pid, node.getMaxInputCount()))
            node.destroy()

    def section_ramp():
        gt.section("ramp")
        ramp = make_shape("Ramp", 0, 300)
        set_colours(ramp, (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))
        yield from panel_fit(ramp, "m128-ramp-panel.png")
        ok = yield from open_panel(ramp)
        check(ok, "the Ramp's panel opens (the overlay is drawn while it is open)")
        yield from measure_frame()
        x1, y1, x2, y2 = _frame["rod"]
        fw, fh = x2 - x1, y2 - y1
        midy = y1 + 0.5 * fh
        set_pair(ramp, "point0", x1 + 0.2 * fw, midy)
        set_pair(ramp, "point1", x1 + 0.8 * fw, midy)
        set_checked(ramp, "falloff", 0)
        show(ramp)
        img = yield from render()
        gt.shot("m128-ramp.png")
        xs = [x1 + f * fw for f in (0.1, 0.3, 0.5, 0.7, 0.9)]
        ls = [luma(sample(img, x, midy)) for x in xs]
        report("NOTE ramp brightness along x at 10/30/50/70/90%%: %r" % (ls,))
        check(ls[0] < ls[1] - BRIGHT_MARGIN and ls[1] < ls[2] - BRIGHT_MARGIN and ls[2] < ls[3] - BRIGHT_MARGIN
              and ls[3] < ls[4] + 1,
              "brightness increases from Point 0 to Point 1 (%r)" % (ls,))
        check(ls[4] > ls[0] + 100, "Point 1's side is much brighter than Point 0's (%r)" % (ls,))
        top, bottom = luma(sample(img, xs[2], y1 + 0.2 * fh)), luma(sample(img, xs[2], y1 + 0.8 * fh))
        check(abs(top - bottom) < 10, "the ramp is constant across its direction (%.0f vs %.0f)" % (top, bottom))

        kind = param(ramp, "falloff")
        i = find_option(kind, "easein", "ease in")
        require(i >= 0, "the Falloff menu has Ease In (%r)" % ([str(o) for o in kind.getOptions()],))
        quarter = x1 + 0.35 * fw
        linear_value = luma(sample(img, quarter, midy))
        kind.setValue(i)
        img_ease = yield from render()
        eased = luma(sample(img_ease, quarter, midy))
        check(abs(eased - linear_value) > 8, "Ease In changes the curve against Linear (%.0f vs %.0f)" % (eased, linear_value))
        kind.setValue(0)

        before_value = luma(sample(img, x1 + 0.6 * fw, midy))
        set_pair(ramp, "point1", x1 + 0.65 * fw, midy)
        img2 = yield from render()
        after_value = luma(sample(img2, x1 + 0.6 * fw, midy))
        check(after_value > before_value + BRIGHT_MARGIN,
              "moving Point 1 nearer by Python makes the gradient reach white sooner (%.0f -> %.0f)" % (before_value, after_value))

        set_pair(ramp, "point1", x1 + 0.8 * fw, midy)
        yield ("sleep", RENDER_MS)
        gl = visible_viewer()
        require(gl is not None, "the viewer is visible")
        ux, uy = units_per_pixel()
        gt.shot("m128-ramp-overlay-before.png")
        for name, (dx, dy) in RAMP_DRAGS.items():
            other = "point1" if name == "point0" else "point0"
            before, other_before = pair(ramp, name), pair(ramp, other)
            start = to_screen(*before)
            drag(gl, start, start + QPointF(dx, dy))
            yield ("sleep", SETTLE_MS)
            after, other_after = pair(ramp, name), pair(ramp, other)
            want = (before[0] + dx * ux, before[1] - dy * uy)
            report("NOTE %s %r -> %r, wanted %r" % (name, before, after, want))
            check(abs(after[0] - want[0]) <= TOL_PX * ux and abs(after[1] - want[1]) <= TOL_PX * uy,
                  "the Ramp overlay: dragging %s's handle moves it by the pen's travel (%r -> %r, wanted %r)"
                  % (name, before, after, want))
            check(other_after == other_before, "dragging %s leaves %s alone (%r -> %r)" % (name, other, other_before, other_after))
        gt.shot("m128-ramp-overlay-after.png")
        gt.close_all_panels()
        yield ("sleep", 300)

    def section_radial():
        gt.section("radial")
        radial = make_shape("Radial", 300, 300)
        set_colours(radial, (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))
        set_checked(radial, "softness", 0.0)
        yield from panel_fit(radial, "m128-radial-panel.png")
        ok = yield from open_panel(radial)
        check(ok, "the Radial's panel opens")
        yield from measure_frame()
        cx, cy = centred_box(radial, 400, 300)
        show(radial)
        img = yield from render()
        gt.shot("m128-radial.png")
        centre, outside, edge_far = luma(sample(img, cx, cy)), luma(sample(img, cx + 450, cy)), luma(sample(img, cx + 380, cy))
        corner = luma(sample(img, cx + 380, cy + 280))
        report("NOTE radial: centre %.0f, just outside the box %.0f, near the edge %.0f, box corner %.0f" % (centre, outside, edge_far, corner))
        check(centre > outside + 100, "the Radial's centre is bright and outside the ellipse is dark (%.0f vs %.0f)" % (centre, outside))
        check(centre > corner + 100, "the ellipse does not fill the box corner (%.0f vs %.0f)" % (centre, corner))
        hard = lit_along_row(img, cy, cx - 400, cx + 400)

        set_checked(radial, "softness", 300.0)
        soft_img = yield from render()
        gt.shot("m128-radial-soft.png")
        soft = lit_along_row(soft_img, cy, cx - 400, cx + 400)
        near_edge_soft = luma(sample(soft_img, cx + 360, cy))
        report("NOTE radial lit samples along the middle row: softness 0 -> %d, softness 300 -> %d" % (hard, soft))
        check(soft < hard - 6, "Softness widens the falloff: fewer fully lit pixels along the row (%d vs %d)" % (soft, hard))
        check(near_edge_soft < luma(sample(img, cx + 360, cy)) - BRIGHT_MARGIN,
              "Softness darkens the inside near the edge (%.0f vs %.0f)" % (near_edge_soft, luma(sample(img, cx + 360, cy))))
        set_checked(radial, "softness", 0.0)

        set_box(radial, cx - 100, cy - 300 + 0, 200, 600)
        narrow = yield from render()
        moved = luma(sample(narrow, cx + 250, cy))
        check(moved < centre - 100, "narrowing the extent's box narrows the ellipse (%.0f at 250 px from the centre)" % moved)
        centred_box(radial, 400, 300)
        yield ("sleep", RENDER_MS)
        yield from corner_drag_check(radial, "Radial")
        gt.shot("m128-radial-overlay.png")
        gt.close_all_panels()
        yield ("sleep", 300)

    def section_rectangle():
        gt.section("rectangle")
        rect = make_shape("Rectangle", 600, 300)
        set_colours(rect, (0.2, 0.2, 0.2), (1.0, 1.0, 1.0))
        set_checked(rect, "softness", 0.0)
        yield from panel_fit(rect, "m128-rectangle-panel.png")
        ok = yield from open_panel(rect)
        check(ok, "the Rectangle's panel opens")
        yield from measure_frame()
        cx, cy = centred_box(rect, 400, 300)
        param(rect, "cornerRadius").set(0.0, 0.0)
        show(rect)
        img = yield from render()
        gt.shot("m128-rectangle.png")
        inside, outside = luma(sample(img, cx, cy)), luma(sample(img, cx + 450, cy))
        near_edge, corner = luma(sample(img, cx + 380, cy)), luma(sample(img, cx + 385, cy + 285))
        report("NOTE rectangle: inside %.0f, outside %.0f, near the edge %.0f, corner %.0f" % (inside, outside, near_edge, corner))
        check(inside > outside + 100, "inside is Color 1 and outside Color 0 (%.0f vs %.0f)" % (inside, outside))
        check(abs(near_edge - inside) < 10 and abs(corner - inside) < 10,
              "a hard rectangle is Color 1 up to its edge and corners (%.0f, %.0f vs %.0f)" % (near_edge, corner, inside))

        set_checked(rect, "softness", 200.0)
        soft_img = yield from render()
        gt.shot("m128-rectangle-soft.png")
        soft_edge = luma(sample(soft_img, cx + 380, cy))
        check(soft_edge < near_edge - BRIGHT_MARGIN, "Softness darkens the inside near the edge (%.0f vs %.0f)" % (soft_edge, near_edge))
        check(luma(sample(soft_img, cx, cy)) > soft_edge + BRIGHT_MARGIN, "the soft rectangle's centre stays brighter than its edge")
        set_checked(rect, "softness", 0.0)

        param(rect, "cornerRadius").set(150.0, 150.0)
        round_img = yield from render()
        gt.shot("m128-rectangle-round.png")
        round_corner = luma(sample(round_img, cx + 385, cy + 285))
        check(round_corner < corner - 60, "Corner Radius darkens the corner (%.0f vs %.0f)" % (round_corner, corner))
        param(rect, "cornerRadius").set(0.0, 0.0)
        yield ("sleep", RENDER_MS)
        yield from corner_drag_check(rect, "Rectangle")
        gt.shot("m128-rectangle-overlay.png")
        gt.close_all_panels()
        yield ("sleep", 300)

    def section_colorwheel():
        gt.section("colorwheel")
        wheel = make_shape("ColorWheel", 900, 300)
        yield from panel_fit(wheel, "m128-colorwheel-panel.png")
        ok = yield from open_panel(wheel)
        check(ok, "the ColorWheel's panel opens")
        yield from measure_frame()
        gt.close_all_panels()
        yield ("sleep", 300)
        # Same viewer size as measured: the panel is closed again, so re-measure.
        yield from measure_frame()
        radius = 300.0
        cx, cy = centred_box(wheel, radius, radius)
        show(wheel)
        img = yield from render()
        gt.shot("m128-colorwheel.png")
        import math

        def at(img_, angle_deg, frac):
            a = math.radians(angle_deg)
            return sample(img_, cx + frac * radius * math.cos(a), cy + frac * radius * math.sin(a))

        hues = [at(img, a, 0.7) for a in (0, 90, 180, 270)]
        report("NOTE colorwheel samples at 0/90/180/270 degrees: %r" % (hues,))
        pairs_ok = all(distance(hues[i], hues[j]) > HUE_MARGIN for i in range(4) for j in range(i + 1, 4))
        check(pairs_ok, "hue varies around the centre (%r)" % (hues,))
        near, far = at(img, 0, 0.1), at(img, 0, 0.9)
        sat = lambda c: max(c) - min(c)
        report("NOTE saturation (max-min): centre %.0f, edge %.0f" % (sat(near), sat(far)))
        check(sat(far) > sat(near) + SAT_MARGIN, "saturation grows outward (centre %.0f, edge %.0f)" % (sat(near), sat(far)))
        before = at(img, 0, 0.7)
        set_checked(wheel, "rotate", 90.0)
        rotated = yield from render()
        after = at(rotated, 0, 0.7)
        check(distance(before, after) > HUE_MARGIN, "Rotate changes the hue at a fixed angle (%r -> %r)" % (before, after))
        set_checked(wheel, "rotate", 0.0)
        set_checked(wheel, "centerSaturation", 1.0)
        full = yield from render()
        check(sat(at(full, 0, 0.1)) > sat(near) + SAT_MARGIN,
              "Center Saturation 1 saturates the centre (%.0f vs %.0f)" % (sat(at(full, 0, 0.1)), sat(near)))
        set_checked(wheel, "centerSaturation", 0.0)

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        show(create(PLUGIN_CONSTANT, -300, -200))
        yield ("sleep", RENDER_MS)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        section_plugin_ids()
        yield from section_ramp()
        yield from section_radial()
        yield from section_rectangle()
        yield from section_colorwheel()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
