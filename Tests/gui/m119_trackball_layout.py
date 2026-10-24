"""Evidence for the colour-panel layout and the global-mode feedback designs.

Run with:
    docker exec natron-dev bash -c 'cd <tree> && \
        NATRON_GUI_TEST_OUT=build/m119-shots GUI_TEST_TIMEOUT=1200 \
        Tests/gui/run-gui-test.sh Tests/gui/m119_trackball_layout.py <Natron binary or AppImage>'

What it covers, one section each:
- bounds: on a Grade's gain at a 500 px bin, the panel starts in the label column and runs the
  Hue slider on to the row's last button while the "g" channel line keeps its inset, and a Blur's
  size row does not move when the panel opens.
- sizes: for each colorTrackballWheelWidth preference (1/2, 1/3, 1/4, 1/5), at the default bin
  width and at twice it: a screenshot, the wheel square at that fraction of the panel's width (or
  as tall as the line column when that is larger) flush left, and the Hue line ending at the
  panel's right edge. The hue ring's width is measured on each wheel (pixel probe).
- panel-toggle: the knob's panel button shows the dropdown icon folded and exactly that icon
  flipped top to bottom while open, also after a value change and a slider drag (never a swatch).
- no-flash: no widget of the Grade panel paints at a geometry other than its settled one while
  the panel is folded and unfolded (a paint logger).
- undo: a slider drag is one undo step, including a drag that starts on the marker.
- fresh-engage: global mode starts in C on every fresh engage, and keeps its mode across a knob
  switch while engaged.
- engaged-highlight: while engaged, the target knob's sunken block is drawn highlighted (a grey
  tint, not the selection colour), around the row and its panel when open and around the row
  alone when closed; it moves with a Ctrl+Down knob switch and leaving the mode clears it.
- feedback: for each value of the colorTrackballFeedback preference (Crosshair + reveal, Widget
  cursor), the override cursor before, during and after a stroke, the following HUD on real
  pointer moves (also at the screen's edge), the reveal graphic at the press point, the widget
  cursor following, freezing on press and resuming on release; a modal dialog and repeated
  cycles leave no override cursor.
- hud: the hint block's own window (Crosshair + reveal, no stroke) is a window, and its on-screen pixels match its own
  render.
- right-click: a right-click ends global mode, restores the cursor, and opens no context menu.
- reveal: screenshots of the C, S, T and V graphics, S mid-drag (the disc recentres on the live
  hue, and only an x drag changes it), and a press at the screen's corner keeps the graphic on
  screen.
- ring: the ring's width over its outer radius is the same, within the probe's pixel error, on
  every panel wheel size and on the C reveal's wheel.

Knob widgets carry no object name; KnobGui puts the knob's script name in bold at the start of its
tooltip, so that is how a knob's widgets are found. The Grade's "Gain" knob is named "white".
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 1150
SETTLE_MS = 400
PANEL_MS = 900
PAINT_WINDOW_MS = 300
PAINT_SETTLE_MS = 700

GRADE_ID = "net.sf.openfx.GradePlugin"
CC_ID = "net.sf.openfx.ColorCorrectPlugin"
BLUR_ID = "net.sf.cimg.CImgBlur"
GAIN = "white"
CC_GAIN = "MasterGain"

T3_BIN_WIDTH = 500
EDGE_TOLERANCE = 1
WHEEL_FRACTIONS = {0: 1.0 / 2, 1: 1.0 / 3, 2: 1.0 / 4, 3: 1.0 / 5}
WHEEL_WIDTH_NAMES = {0: "half", 1: "third", 2: "quarter", 3: "fifth"}
WHEEL_WIDTH_DEFAULT = 1
HUD_OFFSET = 16
REVEAL_DIAMETER = 240

FEEDBACK_NAMES = {0: "CrosshairReveal", 1: "WidgetCursor"}

try:
    import faulthandler
    import gc

    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QContextMenuEvent, QCursor, QGuiApplication, QIcon, QImage, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QDialog, QGridLayout, QLabel, QMainWindow, QMenu, QScrollArea, QSplitter, QWidget

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def dpi_scale():
        return QGuiApplication.primaryScreen().logicalDotsPerInchX() / 96.0

    def px(logical):
        return logical * dpi_scale()

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def alive(w):
        try:
            return w is not None and shiboken6.isValid(w)
        except Exception:
            return False

    def is_inside(widget, ancestor):
        if widget is None or ancestor is None:
            return False
        target = ptr(ancestor)
        w = widget
        while w is not None:
            if ptr(w) == target:
                return True
            w = w.parentWidget()
        return False

    def global_rect(w):
        return QRect(w.mapToGlobal(QPoint(0, 0)), w.size())

    def win_rect(w):
        return QRect(w.mapTo(w.window(), QPoint(0, 0)), w.size())

    def fmt_rect(r):
        return "(%d,%d %dx%d)" % (r.x(), r.y(), r.width(), r.height())

    def screen_rect():
        return QGuiApplication.primaryScreen().geometry()

    # ---- application widgets -------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def named(name, visible_only=False, within=None):
        found = []
        for w in QApplication.allWidgets():
            try:
                if w.objectName() != name:
                    continue
                if visible_only and not w.isVisible():
                    continue
                if within is not None and not is_inside(w, within):
                    continue
                found.append(w)
            except RuntimeError:
                continue
        return found

    def first_named(name, visible_only=False, within=None):
        found = named(name, visible_only, within)
        return found[0] if found else None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QWidget):
                    if gt.class_name(w) == "LineEdit" and w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def raise_panel(node):
        """Opens node's panel, or brings it to the top of the properties bin if it is open."""
        view = node_graph_view()
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def bin_wrapper():
        for p in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not p.isVisible():
                    continue
            except RuntimeError:
                continue
            w = p.parentWidget()
            while w is not None and not isinstance(w, QScrollArea):
                w = w.parentWidget()
            if w is not None and w.parentWidget() is not None:
                return w.parentWidget()
        return None

    def bin_scroll_area(wrapper):
        for c in wrapper.children():
            if isinstance(c, QScrollArea):
                return c
        return None

    def set_bin_width(target):
        wrapper = bin_wrapper()
        if wrapper is None:
            return -1
        for _ in range(4):
            child = wrapper
            sp = wrapper.parentWidget()
            while sp is not None and not (isinstance(sp, QSplitter) and sp.orientation() == Qt.Horizontal):
                child = sp
                sp = sp.parentWidget()
            if sp is None:
                return wrapper.width()
            sizes = list(sp.sizes())
            i = sp.indexOf(child)
            delta = target - wrapper.width()
            if delta == 0:
                break
            j = i - 1 if i > 0 else i + 1
            sizes[i] += delta
            sizes[j] -= delta
            sp.setSizes(sizes)
            QApplication.processEvents()
        return wrapper.width()

    # ---- knob rows -----------------------------------------------------------------------------

    def tagged(w, name):
        try:
            return ("<b>%s</b>" % name) in (w.toolTip() or "")[:200]
        except RuntimeError:
            return False

    def knob_row(panel, name):
        """The colour knob's row widget: the one holding its ▾ button and its own fields."""
        for b in live_children(panel):
            if b.objectName() != "ColorSelectorButton":
                continue
            row = b.parentWidget()
            if row is None:
                continue
            if any(tagged(w, name) for w in live_children(row)):
                return row
        return None

    def live_children(root):
        out = []
        try:
            kids = root.findChildren(QWidget)
        except RuntimeError:
            return out
        for w in kids:
            try:
                w.objectName()
                out.append(w)
            except RuntimeError:
                continue
        return out

    def child_named(root, name):
        """A visible child with this name, else any (a widget awaiting deleteLater may share it)."""
        found = [w for w in live_children(root) if w.objectName() == name]
        for w in found:
            if w.isVisible():
                return w
        return found[0] if found else None

    def knob_label_cell(panel, name, column):
        for w in panel.findChildren(QLabel):
            if tagged(w, name) and not is_inside(w, column) and w.isVisible():
                return w.parentWidget()
        return None

    def label_column_left(label_cell):
        """The left edge of the page grid's label column (labels sit right-aligned in it).

        Column 0 starts at the grid's contents left. cellRect() is not used: once heightForWidth has
        run, its x leaves out the grid's left margin."""
        if label_cell is None:
            return None
        page = label_cell.parentWidget()
        grid = page.layout() if page is not None else None
        if not isinstance(grid, QGridLayout):
            return None
        if grid.indexOf(label_cell) < 0:
            return None
        return page.mapToGlobal(QPoint(grid.contentsRect().left(), 0)).x()

    def channel_slider_g(column):
        found = []
        for w in live_children(column):
            if gt.class_name(w) != "ScaleSliderQWidget" or not w.isVisible():
                continue
            a = w.parentWidget()
            inside_panel = False
            while a is not None and ptr(a) != ptr(column):
                if a.objectName() == "ColorAdjustPanel":
                    inside_panel = True
                    break
                a = a.parentWidget()
            if inside_panel or w.parentWidget() is None or ptr(w.parentWidget()) == ptr(column):
                continue
            if any(gt.class_name(c) == "ColorSelectorButton" or c.objectName() == "ColorSelectorButton"
                   for c in w.parentWidget().children() if isinstance(c, QWidget)):
                continue
            found.append(w)
        found.sort(key=lambda s: s.mapToGlobal(QPoint(0, 0)).y())
        return found[0] if found else None

    def set_pref(name, value):
        NatronEngine.natron.getSettings().getParam(name).set(value)
        QApplication.processEvents()

    def set_feedback(value):
        set_pref("colorTrackballFeedback", value)

    def set_wheel_width(value):
        set_pref("colorTrackballWheelWidth", value)

    def grab_screen(rect):
        r = rect.intersected(screen_rect())
        return QGuiApplication.primaryScreen().grabWindow(0, r.x(), r.y(), r.width(), r.height()).toImage() \
            .convertToFormat(QImage.Format_RGB32)

    def image_diff(a, b, tolerance=6):
        """The fraction of pixels whose channels differ by more than tolerance."""
        if a.size() != b.size() or a.isNull():
            return 1.0
        bad = 0
        for y in range(a.height()):
            for x in range(a.width()):
                pa, pb = a.pixel(x, y), b.pixel(x, y)
                if (abs(((pa >> 16) & 255) - ((pb >> 16) & 255)) > tolerance
                        or abs(((pa >> 8) & 255) - ((pb >> 8) & 255)) > tolerance
                        or abs((pa & 255) - (pb & 255)) > tolerance):
                    bad += 1
        return bad / float(a.width() * a.height())

    def pixel_distance(a, b):
        return sum(abs(((a >> sh) & 255) - ((b >> sh) & 255)) for sh in (16, 8, 0))

    # ---- the active adjust panel and its parts -----------------------------------------------------

    def drop_stale_wrappers():
        """Collects Python wrappers nothing refers to any more.

        Shiboken maps C++ addresses to wrappers; a wrapper that outlives its widget can be handed
        back for a new widget at the same address, already marked deleted."""
        gc.collect()

    def active_adjust_panel(grade_panel):
        drop_stale_wrappers()
        for w in named("ColorAdjustPanel", True, grade_panel):
            return w
        return None

    def all_inside(panel):
        out = []
        for w in QApplication.allWidgets():
            try:
                if w.isVisible() and is_inside(w, panel):
                    out.append(w)
            except RuntimeError:
                continue
        return out

    def wheel_of(panel):
        for w in all_inside(panel):
            if gt.class_name(w) == "ColorWheelWidget":
                return w
        return None

    def line_slider(panel, title):
        for w in all_inside(panel):
            if w.objectName() == "ColorAdjustCustomSlider" + title:
                return w
        return None

    def line_column(panel):
        """The global rect from the top of the panel's first line to the bottom of its last."""
        column = QRect()
        for title in ("Hue", "Sat", "Value", "Temp", "Tint"):
            s = line_slider(panel, title)
            if s is not None:
                column = column.united(global_rect(s))
        return column

    def measure(panel):
        if not alive(panel):
            return -1, -1
        try:
            return measure_live(panel)
        except RuntimeError:
            return -1, -1

    def measure_live(panel):
        wheel = wheel_of(panel) if panel is not None else None
        hue = line_slider(panel, "Hue") if panel is not None else None
        d = min(wheel.width(), wheel.height()) if wheel is not None else -1
        if wheel is not None and wheel.width() != wheel.height():
            report("NOTE   wheel widget is %dx%d" % (wheel.width(), wheel.height()))
        track = hue.width() if hue is not None else -1
        return d, track

    # ---- input -----------------------------------------------------------------------------------

    def send_mouse(widget, kind, local, button, buttons, mods=Qt.NoModifier):
        local = QPointF(local)
        ev = QMouseEvent(kind, local, QPointF(widget.mapToGlobal(local)), button, buttons, mods)
        QApplication.sendEvent(widget, ev)

    def drag(widget, start, delta, steps=8, mods=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, mods)
        QApplication.processEvents()
        for k in range(1, steps + 1):
            p = QPoint(start.x() + delta.x() * k // steps, start.y() + delta.y() * k // steps)
            send_mouse(widget, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton, mods)
            QApplication.processEvents()
        end = QPoint(start.x() + delta.x(), start.y() + delta.y())
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, mods)
        QApplication.processEvents()

    def global_press(g):
        w = main_window()
        send_mouse(w, QEvent.MouseButtonPress, w.mapFromGlobal(g), Qt.LeftButton, Qt.LeftButton)
        QApplication.processEvents()

    def global_move(g):
        w = main_window()
        send_mouse(w, QEvent.MouseMove, w.mapFromGlobal(g), Qt.NoButton, Qt.LeftButton)
        QApplication.processEvents()

    def global_release(g):
        w = main_window()
        send_mouse(w, QEvent.MouseButtonRelease, w.mapFromGlobal(g), Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def send_key(key, mods=Qt.NoModifier, target=None):
        target = target or main_window()
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, key, mods))
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyRelease, key, mods))
        QApplication.processEvents()

    def trackball_action():
        win = main_window()
        for a in win.actions():
            if "Colour Trackball" in a.text():
                return a
        return None

    def ctrl_e():
        a = trackball_action()
        if a is not None:
            a.trigger()
        QApplication.processEvents()
        return a is not None

    # ---- global mode state -----------------------------------------------------------------------

    def engaged():
        return any(b.isChecked() for b in named("ColorGlobalModeButton"))

    def hud():
        # The hint block's own window, by the pointer while engaged in Crosshair + reveal with no
        # stroke; with a disc up the block is the reveal's child "ColorTrackballHudBlock".
        return first_named("ColorTrackballHudWindow")

    def hud_block():
        return first_named("ColorTrackballHudWindow", True) or first_named("ColorTrackballHudBlock", True)

    def hud_mode_text():
        w = hud_block()
        return w.property("trackballHudMode") if w is not None else None

    def hud_mode():
        return hud_mode_text() or None

    def hud_knob():
        w = hud_block()
        return w.property("trackballHudKnob") if w is not None else None

    def cursor_name():
        c = QApplication.overrideCursor()
        if c is None:
            return "none"
        shape = c.shape()
        if shape == Qt.CrossCursor:
            return "cross"
        if shape == Qt.BlankCursor:
            return "blank"
        return str(shape)

    def reveal_window():
        for w in QApplication.topLevelWidgets():
            try:
                if gt.class_name(w) != "QWidget":
                    continue
                if any(gt.class_name(c) == "ColorWheelWidget" for c in w.children() if isinstance(c, QWidget)):
                    return w
            except RuntimeError:
                continue
        return None

    def reveal_shown():
        r = reveal_window()
        return r is not None and r.isVisible()

    def reveal_mode_widget_is_wheel():
        r = reveal_window()
        if r is None:
            return None
        for c in r.children():
            if isinstance(c, QWidget) and gt.class_name(c) == "ColorWheelWidget":
                return c.isVisible()
        return None

    def disc_widget(r):
        # ColorModeDiscWidget has no Q_OBJECT, so its className() is QWidget: take the largest visible square child.
        best = None
        for c in r.children():
            if isinstance(c, QWidget) and c.isVisible() and c.width() == c.height():
                if best is None or c.width() > best.width():
                    best = c
        return best

    def disc_centre(r):
        # The reveal window is taller than the disc (pills, hint block), so its own centre is not the disc's.
        d = disc_widget(r)
        return d.mapToGlobal(d.rect().center()) if d is not None else r.frameGeometry().center()

    def crop(rect, name, pad=0):
        QApplication.processEvents()
        r = QRect(rect.x() - pad, rect.y() - pad, rect.width() + 2 * pad, rect.height() + 2 * pad).intersected(screen_rect())
        pix = QGuiApplication.primaryScreen().grabWindow(0, r.x(), r.y(), r.width(), r.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def crop_widget(w, name, pad=0):
        if w is None or not w.isVisible():
            report("NOTE %s: nothing visible to shoot" % name)
            return
        crop(global_rect(w), name, pad)

    def ring_probe(wheel):
        """(outer radius, ring width) in logical px, measured on the wheel's own render along the
        rays left and right of its centre: the run of saturated pixels between the ring's dark
        hairlines, from the outermost one in. None when no ring is found."""
        if wheel is None or not wheel.isVisible():
            return None
        pix = wheel.grab()
        dpr = pix.devicePixelRatio() or 1.0
        img = pix.toImage()
        cx, cy = img.width() // 2, img.height() // 2

        def ringish(x):
            c = img.pixelColor(x, cy)
            return c.alpha() > 200 and c.hsvSaturationF() > 0.3 and c.valueF() > 0.25

        found = []
        for xs in (range(img.width() - 1, cx, -1), range(0, cx)):
            xs = list(xs)
            start = next((i for i, x in enumerate(xs) if ringish(x)), None)
            if start is None:
                continue
            end = start
            while end + 1 < len(xs) and ringish(xs[end + 1]):
                end += 1
            found.append((abs(xs[start] - cx) + 1, end - start + 1))
        if not found:
            return None
        outer = sum(f[0] for f in found) / float(len(found)) / dpr
        width = sum(f[1] for f in found) / float(len(found)) / dpr
        return outer, width

    def highlighted_backdrops(root):
        return [w for w in live_children(root)
                if w.objectName() == "ColorAdjustOpenBackdrop" and w.isVisible() and w.property("colorGlobalModeHighlighted")]

    def backdrop_fill(backdrop):
        img = backdrop.grab().toImage()
        return img.pixelColor(img.width() // 2, img.height() // 2)

    def gain_values(node, name=GAIN):
        p = node.getParam(name)
        return tuple(round(p.getValue(d), 5) for d in range(p.getNumDimensions()))

    def set_gain(node, values, name=GAIN):
        p = node.getParam(name)
        for d, v in enumerate(values):
            p.setValue(v, d)
        QApplication.processEvents()

    # ---- paint logger --------------------------------------------------------------------------

    class PaintLogger(QObject):
        """Records every paint of a widget inside root, with its geometry in its window."""

        def __init__(self, root):
            QObject.__init__(self)
            self.root = root
            self.events = []

        def eventFilter(self, obj, ev):
            try:
                if ev.type() == QEvent.Paint and obj.isWidgetType() and is_inside(obj, self.root):
                    self.events.append((obj, win_rect(obj)))
            except Exception:
                pass
            return False

    def logged_switch(root, label, change):
        """Runs change() with a paint logger on root, then checks no paint was at a stale geometry."""
        logger = PaintLogger(root)
        QApplication.instance().installEventFilter(logger)
        change()
        yield ("sleep", PAINT_WINDOW_MS)
        QApplication.instance().removeEventFilter(logger)
        yield ("sleep", PAINT_SETTLE_MS)
        stale = []
        for w, r in logger.events:
            if not alive(w):
                continue
            final = win_rect(w)
            if r != final or not w.isVisible():
                stale.append("%s%s at %s, settled %s%s" % (gt.class_name(w), ("#" + w.objectName()) if w.objectName() else "",
                                                          fmt_rect(r), fmt_rect(final), "" if w.isVisible() else " hidden"))
        report("NOTE no-flash %s: %d paints logged, %d at a stale geometry" % (label, len(logger.events), len(stale)))
        for s in stale[:4]:
            report("NOTE   stale paint: " + s)
        kinds = []
        for w, r in logger.events:
            if alive(w) and (r != win_rect(w) or not w.isVisible()):
                k = gt.class_name(w) + (("#" + w.objectName()) if w.objectName() else "")
                if k not in kinds:
                    kinds.append(k)
        if kinds:
            report("NOTE   stale widgets: " + ", ".join(kinds[:25]))
        check(not stale, "no paint at a stale geometry on %s (%d of %d)" % (label, len(stale), len(logger.events)))

    # ---- the run -------------------------------------------------------------------------------

    def steps():
        gt.section("setup")
        ok = yield ("until", lambda: node_graph_view() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        win = main_window()
        win.activateWindow()
        win.raise_()
        yield ("until", lambda: win.isActiveWindow(), 10.0)
        report("NOTE binary %s, screen %s, DPI scale %.3f"
               % (QApplication.applicationFilePath(), fmt_rect(screen_rect()), dpi_scale()))
        gt.close_all_panels()

        grade = create(GRADE_ID, 0, 0)
        ok = yield ("until", lambda: panel_of(grade) is not None, 20.0)
        require(ok, "the Grade's panel opened")
        yield ("sleep", PANEL_MS)
        gp = panel_of(grade)
        row = knob_row(gp, GAIN)
        require(row is not None, "found the Grade's gain row")
        column = row.parentWidget()
        selector = child_named(row, "ColorSelectorButton")
        globe = child_named(row, "ColorGlobalModeButton")
        require(selector is not None and globe is not None, "the gain row has its ▾ and global-mode buttons")

        default_width = bin_wrapper().width()
        report("NOTE default properties bin width %d px" % default_width)

        # =========================================================================================
        gt.section("bounds")
        got = set_bin_width(T3_BIN_WIDTH)
        yield ("sleep", SETTLE_MS)
        report("NOTE bin width set to %d (asked %d)" % (got, T3_BIN_WIDTH))
        check(abs(got - T3_BIN_WIDTH) <= 2, "the properties bin is %d px wide (got %d)" % (T3_BIN_WIDTH, got))
        if not selector.isChecked():
            selector.click()
        yield ("sleep", PANEL_MS)
        ap = active_adjust_panel(gp)
        require(ap is not None, "▾ on gain shows the inline adjust panel")
        label_cell = knob_label_cell(gp, GAIN, column)
        g_slider = channel_slider_g(column)
        check(label_cell is not None, "found gain's label cell")
        check(g_slider is not None, "found gain's g channel slider (▾ unfolds r/g/b/a)")
        left = ap.mapToGlobal(QPoint(0, 0)).x()
        hue_right = global_rect(line_slider(ap, "Hue")).right()
        g_right = global_rect(g_slider).right() if g_slider is not None else None
        globe_right = global_rect(globe).right()
        label_left = label_column_left(label_cell)
        report("NOTE panel left %r, label column left %r, Hue right %r, g right %r, last button right %r"
               % (left, label_left, hue_right, g_right, globe_right))
        crop_widget(gp, "bounds-500")
        check(label_left is not None and abs(left - label_left) <= EDGE_TOLERANCE,
              "the panel starts at the label column (panel %r, label %r)" % (left, label_left))
        check(abs(hue_right - globe_right) <= EDGE_TOLERANCE,
              "the Hue slider ends at the last button's right edge (Hue %r, button %r)" % (hue_right, globe_right))
        check(g_right is not None and g_right < globe_right - EDGE_TOLERANCE,
              "the g channel slider keeps its inset short of the buttons (%r, button %r)" % (g_right, globe_right))
        selector.click()
        yield ("sleep", SETTLE_MS)

        blur = create(BLUR_ID, 300, 0)
        ok = yield ("until", lambda: panel_of(blur) is not None, 20.0)
        yield ("sleep", PANEL_MS)
        bp_ = panel_of(blur)

        def size_row():
            return sorted((w.objectName() + gt.class_name(w), fmt_rect(QRect(w.mapToGlobal(QPoint(0, 0)), w.size())))
                          for w in bp_.findChildren(QWidget) if w.isVisible() and tagged(w, "size"))
        if check(bp_ is not None, "the Blur's panel opened"):
            before = size_row()
            check(len(before) > 0, "found the Blur's size row (%d widgets)" % len(before))
            selector.click()
            yield ("sleep", PANEL_MS)
            after = size_row()
            check(before == after, "opening gain's panel leaves the Blur's size row unchanged")
            if before != after:
                report("NOTE size row before %r after %r" % (before, after))
        raise_panel(grade)
        yield ("sleep", PANEL_MS)

        # =========================================================================================
        gt.section("sizes")
        ring_probes = []
        widths = [default_width, min(2 * default_width, screen_rect().width() - 300)]
        report("NOTE bin widths: default %d, wide %d" % tuple(widths))
        if not selector.isChecked():
            selector.click()
            yield ("sleep", PANEL_MS)
        for choice in sorted(WHEEL_FRACTIONS):
            set_wheel_width(choice)
            yield ("sleep", PANEL_MS)
            for wi, width in enumerate(widths):
                got = set_bin_width(width)
                yield ("sleep", SETTLE_MS)
                yield ("sleep", PANEL_MS)
                ap = active_adjust_panel(gp)
                d, track = measure(ap)
                if d < 0:
                    yield ("sleep", PANEL_MS)
                    ap = active_adjust_panel(gp)
                    d, track = measure(ap)
                tag = "%s-%s" % (WHEEL_WIDTH_NAMES[choice], "default" if wi == 0 else "wide")
                if not check(ap is not None and d > 0 and track > 0, "%s at %d px: the panel shows a wheel and a Hue line" % (tag, got)):
                    continue
                wheel = wheel_of(ap)
                hue = line_slider(ap, "Hue")
                row = ap.contentsRect()
                row_left = ap.mapToGlobal(row.topLeft()).x()
                row_right = ap.mapToGlobal(row.topRight()).x()
                lines_area = hue.parentWidget()
                area_layout = wheel.parentWidget().layout()
                spacing = area_layout.spacing() if area_layout is not None else 0
                column = lines_area.sizeHint().height()
                lines_min = lines_area.minimumSizeHint().width()
                fraction = int(row.width() * WHEEL_FRACTIONS[choice] + 0.5)
                expected = max(min(fraction, row.width() - spacing - lines_min), column, 1)
                report("NOTE size %s bin=%d: row %d px, wheel %dx%d, fraction %d px, lines min %d, expected %d, line column %d px, Hue track %d px"
                       % (tag, got, row.width(), wheel.width(), wheel.height(), fraction, lines_min, expected, column, track))
                check(wheel.width() == wheel.height(), "%s: the wheel is square (%dx%d)" % (tag, wheel.width(), wheel.height()))
                check(abs(d - expected) <= 2, "%s: the wheel is %s of the row, capped by the lines and floored at their height (%d, expected %d)"
                      % (tag, WHEEL_WIDTH_NAMES[choice], d, expected))
                check(abs(global_rect(wheel).left() - row_left) <= EDGE_TOLERANCE,
                      "%s: the wheel sits flush left (%d, row %d)" % (tag, global_rect(wheel).left(), row_left))
                check(abs(global_rect(hue).right() - row_right) <= EDGE_TOLERANCE + 1,
                      "%s: the Hue line ends at the row's right edge (%d, row %d)" % (tag, global_rect(hue).right(), row_right))
                check(global_rect(hue).left() > global_rect(wheel).right(), "%s: the lines sit right of the wheel" % tag)
                probe = ring_probe(wheel)
                if check(probe is not None, "%s: found the hue ring on the wheel" % tag):
                    ring_probes.append((tag, wheel.width(), probe[0], probe[1]))
                    report("NOTE ring %s: wheel %d px, outer radius %.1f px, ring width %.1f px, ratio %.3f"
                           % (tag, wheel.width(), probe[0], probe[1], probe[1] / probe[0]))
                crop_widget(bin_wrapper(), "size-" + tag)
        set_wheel_width(WHEEL_WIDTH_DEFAULT)
        set_bin_width(default_width)
        yield ("sleep", PANEL_MS)

        # =========================================================================================
        gt.section("panel-toggle")
        selector = child_named(knob_row(gp, GAIN), "ColorSelectorButton")
        if selector.isChecked():
            selector.click()
            yield ("sleep", SETTLE_MS)
        folded_icon = selector.icon().pixmap(selector.iconSize()).toImage().convertToFormat(QImage.Format_ARGB32)
        flipped = folded_icon.mirrored(False, True)
        folded_tip = selector.toolTip()
        crop_widget(knob_row(gp, GAIN), "toggle-folded", 4)
        selector.click()
        yield ("sleep", PANEL_MS)
        check(selector.isChecked() and active_adjust_panel(gp) is not None, "the button opens the panel")
        check(not folded_icon.isNull() and flipped != folded_icon, "the dropdown icon is not symmetric top to bottom")

        def open_icon_is_flipped(when):
            on = selector.icon().pixmap(selector.iconSize(), QIcon.Normal, QIcon.On).toImage().convertToFormat(QImage.Format_ARGB32)
            off = selector.icon().pixmap(selector.iconSize(), QIcon.Normal, QIcon.Off).toImage().convertToFormat(QImage.Format_ARGB32)
            check(on == flipped and off == flipped, "%s: the open icon is the dropdown icon flipped" % when)

        open_icon_is_flipped("opened")
        crop_widget(knob_row(gp, GAIN), "toggle-open", 4)
        before = gain_values(grade)
        set_gain(grade, [0.8, 0.9, 1.1, 1.0])
        yield ("sleep", SETTLE_MS)
        open_icon_is_flipped("after a value change")
        crop_widget(knob_row(gp, GAIN), "toggle-open-changed", 4)
        hue = line_slider(active_adjust_panel(gp), "Hue")
        if check(hue is not None, "found the Hue line"):
            drag(hue, QPoint(hue.width() // 4, hue.height() // 2), QPoint(40, 0))
            yield ("sleep", SETTLE_MS)
            open_icon_is_flipped("after a slider drag")
            crop_widget(knob_row(gp, GAIN), "toggle-open-dragged", 4)
        set_gain(grade, before)
        check(selector.toolTip() != folded_tip, "the tooltip changes with the state")
        report("NOTE tooltips: folded %r, open %r" % (folded_tip[:60], selector.toolTip()[:60]))
        selector.click()
        yield ("sleep", SETTLE_MS)
        check(not selector.isChecked(), "the button folds the panel again")
        folded_again = selector.icon().pixmap(selector.iconSize()).toImage().convertToFormat(QImage.Format_ARGB32)
        check(folded_again == folded_icon, "folded again, the button shows the dropdown icon")
        selector.click()
        yield ("sleep", PANEL_MS)

        # =========================================================================================
        gt.section("no-flash")
        root = gp
        yield from logged_switch(root, "▾ close", lambda: selector.click())
        yield from logged_switch(root, "▾ open", lambda: selector.click())

        # =========================================================================================
        gt.section("undo")
        set_gain(grade, (1.0, 1.0, 1.0, 1.0))
        yield ("sleep", SETTLE_MS)
        v0 = gain_values(grade)
        ap = active_adjust_panel(gp)
        slider = line_slider(ap, "Value") if alive(ap) else None
        if check(slider is not None, "found the Value slider"):
            start = QPoint(slider.width() // 4, slider.height() // 2)
            drag(slider, start, QPoint(40, 0))
            yield ("sleep", SETTLE_MS)
            v1 = gain_values(grade)
            QMetaObject.invokeMethod(gp, "onUndoClicked")
            yield ("sleep", SETTLE_MS)
            v2 = gain_values(grade)
            report("NOTE undo: before %r, after drag %r, after one undo %r" % (v0, v1, v2))
            check(v1 != v0, "a Value drag changes gain")
            check(v2 == v0, "one undo restores gain (the drag is one undo step)")
        yield ("sleep", SETTLE_MS)

        # =========================================================================================
        gt.section("fresh-engage")
        require(trackball_action() is not None, "found the Engage Colour Trackball action")
        globe = child_named(knob_row(gp, GAIN), "ColorGlobalModeButton")
        globe.click()
        yield ("sleep", SETTLE_MS)
        check(engaged(), "the global-mode button engages the mode on gain")
        check(hud_mode() == "C", "a fresh engage starts in C (HUD %r)" % hud_mode_text())
        send_key(Qt.Key_S)
        yield ("sleep", 200)
        check(hud_mode() == "S", "S picks mode S (HUD %r)" % hud_mode_text())
        target_before = hud_knob()
        send_key(Qt.Key_Down, Qt.ControlModifier)
        yield ("sleep", 300)
        target_after = hud_knob()
        report("NOTE HUD target before Ctrl+Down %r, after %r" % (target_before, target_after))
        check(target_after != target_before, "Ctrl+Down moves global mode to the next knob")
        check(hud_mode() == "S", "switching knob while engaged keeps mode S (HUD %r)" % hud_mode_text())
        send_key(Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Esc leaves global mode")
        cc = create(CC_ID, 600, 0)
        ok = yield ("until", lambda: panel_of(cc) is not None, 20.0)
        yield ("sleep", PANEL_MS)
        ctrl_e()
        yield ("sleep", SETTLE_MS)
        check(engaged(), "Ctrl+E engages on the ColorCorrect")
        target_cc = hud_knob()
        report("NOTE HUD target on the ColorCorrect: %r" % target_cc)
        check(hud_mode() == "C", "Ctrl+E on a second node after leaving in S shows C (HUD %r)" % hud_mode_text())
        gt.shot("fresh-engage-second-node.png")
        ctrl_e()
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Ctrl+E again leaves global mode")
        raise_panel(grade)
        yield ("sleep", PANEL_MS)

        # =========================================================================================
        gt.section("engaged-highlight")
        gain_row = knob_row(gp, GAIN)
        selector = child_named(gain_row, "ColorSelectorButton")
        if not selector.isChecked():
            selector.click()
            yield ("sleep", PANEL_MS)
        plain = [w for w in live_children(gp) if w.objectName() == "ColorAdjustOpenBackdrop" and w.isVisible()]
        plain_fill = backdrop_fill(plain[0]) if plain else None
        check(not highlighted_backdrops(gp), "before engaging, no sunken block is highlighted")
        child_named(gain_row, "ColorGlobalModeButton").click()
        yield ("sleep", SETTLE_MS)
        check(engaged(), "the global-mode button engages the mode on gain")
        check(gain_row.property("_natronColorTrackballTarget") is None, "the row carries no target border property")
        lit = highlighted_backdrops(gp)
        if check(len(lit) == 1, "panel open: exactly one sunken block is highlighted (%d)" % len(lit)):
            block = global_rect(lit[0])
            ap = active_adjust_panel(gp)
            check(block.contains(global_rect(gain_row)), "panel open: the block surrounds gain's row")
            check(ap is not None and block.contains(global_rect(ap)), "panel open: the block surrounds gain's panel")
            fill = backdrop_fill(lit[0])
            report("NOTE engaged block fill %s, saturation %.2f; plain fill %s"
                   % (fill.name(), fill.hsvSaturationF(), plain_fill.name() if plain_fill is not None else None))
            check(fill.hsvSaturationF() < 0.25, "panel open: the highlight is a grey tint, not the selection colour (saturation %.2f)"
                  % fill.hsvSaturationF())
            if plain_fill is not None:
                check(fill.lightnessF() > plain_fill.lightnessF(), "panel open: the highlighted block is lighter than the plain one")
        crop_widget(gp, "engaged-panel-open", 4)

        selector.click()
        yield ("sleep", PANEL_MS)
        check(engaged() and not selector.isChecked(), "closing gain's panel keeps global mode engaged")
        lit = highlighted_backdrops(gp)
        if check(len(lit) == 1, "panel closed: the sunken block is still highlighted (%d)" % len(lit)):
            block = global_rect(lit[0])
            row_rect = global_rect(gain_row)
            label = knob_label_cell(gp, GAIN, gain_row.parentWidget())
            check(block.contains(row_rect), "panel closed: the block surrounds gain's row")
            check(label is None or block.contains(global_rect(label)), "panel closed: the block surrounds gain's label")
            report("NOTE panel closed: block %s, row %s" % (fmt_rect(block), fmt_rect(row_rect)))
            check(block.height() <= row_rect.height() + px(12), "panel closed: the block is the row alone (%d px, row %d px)"
                  % (block.height(), row_rect.height()))
        crop_widget(gp, "engaged-panel-closed", 4)

        target_before = hud_knob()
        send_key(Qt.Key_Down, Qt.ControlModifier)
        yield ("sleep", PANEL_MS)
        target_after = hud_knob()
        report("NOTE highlight switch: HUD target before %r, after %r" % (target_before, target_after))
        check(target_after != target_before, "Ctrl+Down moves global mode to the next knob")
        lit = highlighted_backdrops(gp)
        if check(len(lit) == 1, "after the switch: exactly one sunken block is highlighted (%d)" % len(lit)):
            check(not global_rect(lit[0]).intersects(global_rect(gain_row).adjusted(0, 2, 0, -2)), "after the switch: the highlight left gain's row")
        crop_widget(gp, "engaged-after-switch", 4)

        send_key(Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Esc leaves global mode")
        check(not highlighted_backdrops(gp), "after leaving: no sunken block is highlighted")
        if not selector.isChecked():
            selector.click()
            yield ("sleep", PANEL_MS)

        # =========================================================================================
        gt.section("feedback")
        expected = {
            0: ("cross", "blank", "cross"),
            1: ("blank", "blank", "blank"),
        }
        centre = screen_rect().center()
        for fb in (0, 1):
            name = FEEDBACK_NAMES[fb]
            set_feedback(fb)
            yield ("sleep", SETTLE_MS)
            QCursor.setPos(centre)
            yield ("sleep", 200)
            check(cursor_name() == "none", "%s: no override cursor before engaging (%s)" % (name, cursor_name()))
            globe = child_named(knob_row(gp, GAIN), "ColorGlobalModeButton")
            globe.click()
            yield ("sleep", SETTLE_MS)
            if not check(engaged(), "%s: engaged on gain" % name):
                continue
            before = cursor_name()
            h = hud() if fb == 0 else None
            if fb == 1:
                check(hud() is None or not hud().isVisible(), "%s: no following hint window; the block sits under the disc" % name)
            else:
                check(h is not None and h.isVisible() and h.isWindow(), "%s: the hint block is its own window" % name)
            if h is not None:
                flags = h.windowFlags()
                check(bool(flags & Qt.WindowTransparentForInput) and bool(flags & Qt.FramelessWindowHint),
                      "%s: the following HUD is frameless and click-through" % name)
            p = QPoint(400, 300)
            QCursor.setPos(p)
            yield ("sleep", 300)
            hp = h.pos() if h is not None else None
            off = round(px(HUD_OFFSET))
            check(fb == 1 or hp is not None and abs(hp.x() - (p.x() + off)) <= 2 and abs(hp.y() - (p.y() + off)) <= 2,
                  "%s: a real pointer move to (400, 300) puts the HUD at +%d (HUD at %r)" % (name, off, hp))
            edge = QPoint(screen_rect().right() - 4, screen_rect().bottom() - 4)
            QCursor.setPos(edge)
            yield ("sleep", 300)
            hg = h.frameGeometry() if h is not None else None
            check(fb == 1 or hg is not None and hg.right() < edge.x() and hg.bottom() < edge.y() and screen_rect().contains(hg),
                  "%s: near the bottom-right edge the HUD flips left of and above the pointer (%s, pointer %r)"
                  % (name, fmt_rect(hg) if hg else None, edge))
            gt.shot("feedback-%s-hud-edge.png" % name)
            QCursor.setPos(centre)
            yield ("sleep", 300)
            if fb == 1:
                r = reveal_window()
                check(r is not None and r.isVisible(), "WidgetCursor: the mode widget is up from engage")
                if r is not None:
                    c = disc_centre(r)
                    check(abs(c.x() - centre.x()) <= 2 and abs(c.y() - centre.y()) <= 2,
                          "WidgetCursor: centred on the pointer (%r, pointer %r)" % (c, centre))
                    q = QPoint(centre.x() - 200, centre.y() - 120)
                    QCursor.setPos(q)
                    yield ("sleep", 300)
                    c = disc_centre(r)
                    check(abs(c.x() - q.x()) <= 2 and abs(c.y() - q.y()) <= 2,
                          "WidgetCursor: follows a real pointer move (%r, pointer %r)" % (c, q))
                    QCursor.setPos(centre)
                    yield ("sleep", 300)
                    gt.shot("feedback-WidgetCursor-following.png")
            v0 = gain_values(grade)
            global_press(centre)
            yield ("sleep", 200)
            during = cursor_name()
            r = reveal_window()
            check(reveal_shown(), "%s: the mode graphic is up during the stroke" % name)
            if r is not None:
                c = disc_centre(r)
                check(abs(c.x() - centre.x()) <= 2 and abs(c.y() - centre.y()) <= 2,
                      "%s: the graphic is centred on the press point (%r, press %r)" % (name, c, centre))
            frozen = r.frameGeometry() if (r is not None and r.isVisible()) else None
            for k in range(1, 9):
                global_move(QPoint(centre.x() + 10 * k, centre.y() - 6 * k))
            yield ("sleep", 200)
            if fb == 1 and r is not None:
                check(frozen is not None and r.frameGeometry() == frozen,
                      "WidgetCursor: frozen in place during the stroke (%s, was %s)"
                      % (fmt_rect(r.frameGeometry()), fmt_rect(frozen) if frozen else None))
            gt.shot("feedback-%s-stroke.png" % name)
            global_release(QPoint(centre.x() + 80, centre.y() - 48))
            yield ("sleep", 300)
            after = cursor_name()
            v1 = gain_values(grade)
            check(v1 != v0, "%s: the stroke edited gain (%r -> %r)" % (name, v0, v1))
            report("NOTE %s: override cursor before %s, during %s, after %s a stroke" % (name, before, during, after))
            check((before, during, after) == expected[fb],
                  "%s: cursor before/during/after a stroke is %s (got %s)" % (name, "/".join(expected[fb]), "/".join((before, during, after))))
            if fb == 0:
                check(not reveal_shown(), "%s: release hides the mode graphic" % name)
            if fb == 1 and r is not None:
                check(r.isVisible(), "WidgetCursor: the mode widget stays up after release")
                q = QPoint(centre.x() + 150, centre.y() + 100)
                QCursor.setPos(q)
                yield ("sleep", 300)
                c = disc_centre(r)
                check(abs(c.x() - q.x()) <= 2 and abs(c.y() - q.y()) <= 2,
                      "WidgetCursor: resumes following after release (%r, pointer %r)" % (c, q))
                send_key(Qt.Key_S)
                yield ("sleep", 200)
                check(reveal_mode_widget_is_wheel() is False, "WidgetCursor: S swaps the wheel for the S disc")
                send_key(Qt.Key_C)
                yield ("sleep", 200)
            send_key(Qt.Key_Escape)
            yield ("sleep", SETTLE_MS)
            check(not engaged(), "%s: Esc leaves global mode" % name)
            check(cursor_name() == "none", "%s: no override cursor after exit (%s)" % (name, cursor_name()))
            check(not reveal_shown(), "%s: no mode graphic after exit" % name)
            h = hud()
            check(h is None or not h.isVisible() or not h.isWindow(), "%s: no following HUD after exit" % name)
            QMetaObject.invokeMethod(gp, "onUndoClicked")
            yield ("sleep", 200)

        set_feedback(0)
        yield ("sleep", SETTLE_MS)
        for cycle in (1, 2):
            child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
            yield ("sleep", SETTLE_MS)
            check(cursor_name() == "cross", "Crosshair + reveal cycle %d: engage sets a cross cursor (%s)" % (cycle, cursor_name()))
            ctrl_e()
            yield ("sleep", SETTLE_MS)
        check(not engaged() and cursor_name() == "none" and QApplication.overrideCursor() is None,
              "Crosshair + reveal: two engage/exit cycles leave no override cursor (%s)" % cursor_name())
        child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
        yield ("sleep", SETTLE_MS)
        dialog = QDialog(main_window())
        dialog.setWindowTitle("m119 modal")
        dialog.setModal(True)
        dialog.show()
        yield ("sleep", SETTLE_MS)
        send_key(Qt.Key_Shift, Qt.NoModifier, dialog)
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Crosshair + reveal: a modal dialog takes global mode down")
        check(QApplication.overrideCursor() is None, "Crosshair + reveal: a modal dialog leaves no override cursor (%s)" % cursor_name())
        dialog.close()
        dialog.deleteLater()
        yield ("sleep", SETTLE_MS)
        win.activateWindow()
        yield ("until", lambda: win.isActiveWindow(), 5.0)

        # =========================================================================================
        gt.section("hud")
        set_feedback(0)
        yield ("sleep", SETTLE_MS)
        QCursor.setPos(screen_rect().center())
        yield ("sleep", 200)
        child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
        yield ("sleep", SETTLE_MS)
        h = hud()
        if check(engaged() and h is not None, "engaged for the HUD check"):
            check(h.isWindow(), "the HUD is a window while following")
            p = QPoint(400, 300)
            QCursor.setPos(p)
            yield ("sleep", 400)
            h.update()
            yield ("sleep", 300)
            own = h.grab().toImage().convertToFormat(QImage.Format_RGB32)
            on_screen = grab_screen(QRect(h.mapToGlobal(QPoint(0, 0)), h.size()))
            frac = image_diff(own, on_screen, 6)
            report("NOTE HUD %s at %r, own render %dx%d, screen grab %dx%d, %.4f of pixels differ"
                   % (fmt_rect(h.frameGeometry()), h.pos(), own.width(), own.height(), on_screen.width(), on_screen.height(), frac))
            check(own.size() == on_screen.size() and frac < 0.02,
                  "the following HUD's on-screen pixels match its own render (%.4f differ)" % frac)
            on_screen.save(os.path.join(gt.OUT, "hud-on-screen.png"))
            own.save(os.path.join(gt.OUT, "hud-own-render.png"))
            report("SHOT hud-on-screen.png and hud-own-render.png")
            send_key(Qt.Key_Escape)
            yield ("sleep", SETTLE_MS)
        check(not engaged(), "Esc leaves global mode after the HUD check")

        # =========================================================================================
        gt.section("right-click")
        for fb in (0, 1):
            name = FEEDBACK_NAMES[fb]
            set_feedback(fb)
            yield ("sleep", SETTLE_MS)
            QCursor.setPos(centre)
            yield ("sleep", 200)
            child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
            yield ("sleep", SETTLE_MS)
            if not check(engaged(), "%s: engaged before the right-click" % name):
                continue
            check(cursor_name() != "none", "%s: an override cursor is up while engaged (%s)" % (name, cursor_name()))
            target = main_window().mapFromGlobal(centre)
            send_mouse(main_window(), QEvent.MouseButtonPress, target, Qt.RightButton, Qt.RightButton)
            yield ("sleep", 300)
            check(not engaged(), "%s: a right-click ends global mode" % name)
            check(QApplication.overrideCursor() is None, "%s: the cursor is restored after the right-click (%s)" % (name, cursor_name()))
            check(not reveal_shown(), "%s: no mode graphic after the right-click" % name)
            h = hud()
            check(h is None or not h.isVisible(), "%s: no HUD after the right-click" % name)
            send_mouse(main_window(), QEvent.MouseButtonRelease, target, Qt.RightButton, Qt.NoButton)
            QApplication.sendEvent(main_window(), QContextMenuEvent(QContextMenuEvent.Mouse, target, centre))
            yield ("sleep", 400)
            menus = [m for m in QApplication.topLevelWidgets() if isinstance(m, QMenu) and m.isVisible()]
            check(QApplication.activePopupWidget() is None and not menus,
                  "%s: no context menu opens (%d visible menus)" % (name, len(menus)))
            check(QApplication.overrideCursor() is None, "%s: no override cursor after the release and menu request (%s)" % (name, cursor_name()))
        set_feedback(0)
        yield ("sleep", SETTLE_MS)

        # =========================================================================================
        gt.section("reveal")
        start_gain = (1.4, 1.1, 0.7, 1.0)
        set_feedback(0)
        yield ("sleep", SETTLE_MS)
        for mode_key, mode in ((Qt.Key_C, "C"), (Qt.Key_S, "S"), (Qt.Key_T, "T"), (Qt.Key_V, "V")):
            set_gain(grade, start_gain)
            yield ("sleep", 200)
            child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
            yield ("sleep", SETTLE_MS)
            send_key(mode_key)
            yield ("sleep", 200)
            check(hud_mode() == mode or hud_mode() is None, "mode %s picked" % mode)
            press = QPoint(centre.x() - 200, centre.y())
            global_press(press)
            yield ("sleep", 300)
            r = reveal_window()
            if check(r is not None and r.isVisible(), "%s: the graphic is up on press" % mode):
                check(reveal_mode_widget_is_wheel() == (mode == "C"),
                      "%s: the graphic is the %s" % (mode, "wheel" if mode == "C" else "gradient disc"))
                side = disc_widget(r).width()
                check(abs(side - px(REVEAL_DIAMETER)) <= 1, "%s: the graphic is %d px" % (mode, side))
                if mode == "C":
                    reveal_wheel = next((c for c in r.children() if isinstance(c, QWidget) and gt.class_name(c) == "ColorWheelWidget"), None)
                    probe = ring_probe(reveal_wheel)
                    if check(probe is not None, "C: found the hue ring on the reveal's wheel"):
                        ring_probes.append(("reveal-C", reveal_wheel.width(), probe[0], probe[1]))
                        report("NOTE ring reveal-C: wheel %d px, outer radius %.1f px, ring width %.1f px, ratio %.3f"
                               % (reveal_wheel.width(), probe[0], probe[1], probe[1] / probe[0]))
                crop(r.frameGeometry(), "reveal-%s-press" % mode, 10)
            for k in range(1, 9):
                global_move(QPoint(press.x() + 8 * k, press.y() - 5 * k))
            yield ("sleep", 300)
            if r is not None and r.isVisible():
                crop(r.frameGeometry(), "reveal-%s-stroke" % mode, 10)
            global_release(QPoint(press.x() + 64, press.y() - 40))
            yield ("sleep", 300)
            check(not reveal_shown(), "%s: release hides the graphic" % mode)
            send_key(Qt.Key_Escape)
            yield ("sleep", SETTLE_MS)
        # S mid-drag: an x drag moves the hue, so the disc recentres; a y drag leaves the hue alone.
        for axis, delta in (("x", (70, 0)), ("y", (0, -70))):
            set_gain(grade, start_gain)
            yield ("sleep", 200)
            child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
            yield ("sleep", SETTLE_MS)
            send_key(Qt.Key_S)
            yield ("sleep", 200)
            press = QPoint(centre.x() - 200, centre.y())
            global_press(press)
            yield ("sleep", 300)
            r = reveal_window()
            if not check(r is not None and r.isVisible(), "S %s drag: the disc is up on press" % axis):
                send_key(Qt.Key_Escape)
                continue
            rect = r.frameGeometry()
            w = rect.width()
            probes = [QPoint(rect.center().x() + dx, rect.center().y() + dy)
                      for dx, dy in ((int(-0.18 * w), int(-0.18 * w)), (int(0.18 * w), int(0.18 * w)))]
            before = grab_screen(rect)
            crop(rect, "reveal-S-%s-drag-start" % axis, 10)
            for k in range(1, 9):
                global_move(QPoint(press.x() + delta[0] * k // 8, press.y() + delta[1] * k // 8))
            yield ("sleep", 300)
            after = grab_screen(rect)
            crop(rect, "reveal-S-%s-drag-mid" % axis, 10)
            moved = max(pixel_distance(before.pixel(q.x() - rect.x(), q.y() - rect.y()), after.pixel(q.x() - rect.x(), q.y() - rect.y()))
                        for q in probes)
            report("NOTE S %s drag: largest probe colour change %d" % (axis, moved))
            if axis == "x":
                check(moved > 30, "S x drag: the disc recentres on the live hue (probe change %d)" % moved)
            else:
                check(moved <= 12, "S y drag: the hue, and so the disc, stays put (probe change %d)" % moved)
            global_release(QPoint(press.x() + delta[0], press.y() + delta[1]))
            yield ("sleep", 300)
            send_key(Qt.Key_Escape)
            yield ("sleep", SETTLE_MS)
        set_gain(grade, start_gain)
        child_named(knob_row(gp, GAIN), "ColorGlobalModeButton").click()
        yield ("sleep", SETTLE_MS)
        corner = QPoint(screen_rect().left() + 3, screen_rect().top() + 3)
        global_press(corner)
        yield ("sleep", 300)
        r = reveal_window()
        if check(r is not None and r.isVisible(), "a press at the screen's corner shows the graphic"):
            check(screen_rect().contains(r.frameGeometry()),
                  "a press at the corner keeps the graphic on screen (%s)" % fmt_rect(r.frameGeometry()))
            gt.shot("reveal-corner.png")
        global_release(corner)
        yield ("sleep", 300)
        send_key(Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        set_gain(grade, (1.0, 1.0, 1.0, 1.0))
        set_feedback(0)
        yield ("sleep", SETTLE_MS)

        # =========================================================================================
        gt.section("ring")
        reference = next((p for p in ring_probes if p[0] == "third-default"), None)
        if check(reference is not None, "the default third-width wheel's ring was measured"):
            ratio = reference[3] / reference[2]
            report("NOTE ring reference: %s, ratio %.3f" % (reference[0], ratio))
            for tag, side, outer, width in ring_probes:
                expected = ratio * outer
                check(abs(width - expected) <= 2.0, "%s (%d px): the ring is the same fraction of the radius (%.1f px, expected %.1f)"
                      % (tag, side, width, expected))
            smallest = min(ring_probes, key=lambda p: p[2])
            largest = max(ring_probes, key=lambda p: p[2])
            if largest[2] > 1.5 * smallest[2]:
                check(largest[3] > smallest[3] + 2, "the ring on the largest wheel (%s) is wider than on the smallest (%s): %.1f vs %.1f px"
                      % (largest[0], smallest[0], largest[3], smallest[3]))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
            f.write("FAIL script error: " + traceback.format_exc() + "\n")
        os._exit(1)
