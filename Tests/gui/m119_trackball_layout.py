"""Evidence for the colour-panel layout and the global-mode feedback designs.

Run with:
    docker exec natron-dev bash -c 'cd <tree> && \
        NATRON_GUI_TEST_OUT=build/m119-shots GUI_TEST_TIMEOUT=1200 \
        Tests/gui/run-gui-test.sh Tests/gui/m119_trackball_layout.py <Natron binary or AppImage>'

What it covers, one section each:
- bounds: on a Grade's gain at a 500 px bin, the panel starts in the label column and runs the
  Hue slider on to the row's last button while the "g" channel line keeps its inset, and a Blur's
  size row does not move when the panel opens.
- sizes: at the default bin width and at twice it: a screenshot, the wheel's diameter and the
  Hue track's length.
- no-flash: no widget of the Grade panel paints at a geometry other than its settled one while
  the panel is folded and unfolded (a paint logger, as M44 used for the dropdown).
- undo: a slider drag is one undo step.
- fresh-engage: global mode starts in C on every fresh engage, and keeps its mode across a knob
  switch while engaged.
- feedback: for each value of the colorTrackballFeedback preference (Crosshair + reveal, Widget
  cursor), the override cursor before, during and after a stroke, the following HUD on real
  pointer moves (also at the screen's edge), the reveal graphic at the press point, the widget
  cursor following, freezing on press and resuming on release; a modal dialog and repeated
  cycles leave no override cursor.
- reveal: screenshots of the C, S, T and V graphics, and a press at the screen's corner keeps
  the graphic on screen.

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
WHEEL_MIN = 160
WHEEL_MAX = 320
HUD_OFFSET = 16
REVEAL_DIAMETER = 240

FEEDBACK_NAMES = {0: "CrosshairReveal", 1: "WidgetCursor"}

try:
    import faulthandler
    import gc

    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QCursor, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QDialog, QGridLayout, QLabel, QMainWindow, QScrollArea, QSplitter, QWidget

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

    def set_feedback(value):
        NatronEngine.natron.getSettings().getParam("colorTrackballFeedback").set(value)
        QApplication.processEvents()

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
        return first_named("ColorTrackballHud")

    def hud_mode_text():
        w = first_named("ColorTrackballHudMode")
        return w.text() if w is not None else None

    def hud_mode():
        t = hud_mode_text() or ""
        return t[5:6] if t.startswith("Mode ") else None

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
        widths = [default_width, min(2 * default_width, screen_rect().width() - 300)]
        report("NOTE bin widths: default %d, wide %d" % tuple(widths))
        table = []
        for wi, width in enumerate(widths):
            got = set_bin_width(width)
            yield ("sleep", SETTLE_MS)
            report("NOTE bin width %d (asked %d)" % (got, width))
            ap = wheels = None
            yield ("sleep", PANEL_MS)
            ap = active_adjust_panel(gp)
            d, track = measure(ap)
            if d < 0:
                ap = None
                yield ("sleep", PANEL_MS)
                ap = active_adjust_panel(gp)
                d, track = measure(ap)
            name = "size-%s" % ("default" if wi == 0 else "wide")
            if d < 0:
                wheels = [(w.width(), w.height(), w.isVisible()) for w in live_children(ap if alive(ap) else gp)
                          if gt.class_name(w) == "ColorWheelWidget"]
                report("NOTE   no wheel: adjust panel %r visible=%r, wheels %r"
                       % (ap.objectName() if alive(ap) else None, alive(ap) and ap.isVisible(), wheels))
            table.append((width, d, track))
            report("NOTE size bin=%d: wheel %d px, Hue track %d px" % (got, d, track))
            check(px(WHEEL_MIN) - 1 <= d <= px(WHEEL_MAX) + 1 and track > 0,
                  "at %d px: a %d px wheel in [160, 320] and a %d px Hue track" % (got, d, track))
            crop_widget(bin_wrapper(), name)
        set_bin_width(default_width)
        yield ("sleep", SETTLE_MS)

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
        target_before = (first_named("ColorTrackballHudTarget").text() if first_named("ColorTrackballHudTarget") else None)
        send_key(Qt.Key_Down, Qt.ControlModifier)
        yield ("sleep", 300)
        target_after = (first_named("ColorTrackballHudTarget").text() if first_named("ColorTrackballHudTarget") else None)
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
        target_cc = first_named("ColorTrackballHudTarget").text() if first_named("ColorTrackballHudTarget") else None
        report("NOTE HUD target on the ColorCorrect: %r" % target_cc)
        check(hud_mode() == "C", "Ctrl+E on a second node after leaving in S shows C (HUD %r)" % hud_mode_text())
        gt.shot("fresh-engage-second-node.png")
        ctrl_e()
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Ctrl+E again leaves global mode")
        raise_panel(grade)
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
            h = hud()
            check(h is not None and h.isVisible() and h.isWindow(), "%s: the HUD is its own window" % name)
            if h is not None:
                flags = h.windowFlags()
                check(bool(flags & Qt.WindowTransparentForInput) and bool(flags & Qt.FramelessWindowHint),
                      "%s: the following HUD is frameless and click-through" % name)
            p = QPoint(400, 300)
            QCursor.setPos(p)
            yield ("sleep", 300)
            hp = h.pos() if h is not None else None
            off = round(px(HUD_OFFSET))
            check(hp is not None and abs(hp.x() - (p.x() + off)) <= 2 and abs(hp.y() - (p.y() + off)) <= 2,
                  "%s: a real pointer move to (400, 300) puts the HUD at +%d (HUD at %r)" % (name, off, hp))
            edge = QPoint(screen_rect().right() - 4, screen_rect().bottom() - 4)
            QCursor.setPos(edge)
            yield ("sleep", 300)
            hg = h.frameGeometry() if h is not None else None
            check(hg is not None and hg.right() < edge.x() and hg.bottom() < edge.y() and screen_rect().contains(hg),
                  "%s: near the bottom-right edge the HUD flips left of and above the pointer (%s, pointer %r)"
                  % (name, fmt_rect(hg) if hg else None, edge))
            gt.shot("feedback-%s-hud-edge.png" % name)
            QCursor.setPos(centre)
            yield ("sleep", 300)
            if fb == 1:
                r = reveal_window()
                check(r is not None and r.isVisible(), "WidgetCursor: the mode widget is up from engage")
                if r is not None:
                    c = r.frameGeometry().center()
                    check(abs(c.x() - centre.x()) <= 2 and abs(c.y() - centre.y()) <= 2,
                          "WidgetCursor: centred on the pointer (%r, pointer %r)" % (c, centre))
                    q = QPoint(centre.x() - 200, centre.y() - 120)
                    QCursor.setPos(q)
                    yield ("sleep", 300)
                    c = r.frameGeometry().center()
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
                c = r.frameGeometry().center()
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
                c = r.frameGeometry().center()
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
                side = r.width()
                check(abs(side - px(REVEAL_DIAMETER)) <= 1, "%s: the graphic is %d px" % (mode, side))
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


        report("NOTE size table (bin width, layout, wheel px, Hue track px):")
        for width, text, d, track in table:
            report("NOTE   %4d  %-28s wheel %3d  Hue %3d" % (width, text, d, track))
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
