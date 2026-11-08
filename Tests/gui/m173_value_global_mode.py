"""Evidence for global mode on numeric knobs, in the real application.

Run with:
    docker exec natron-dev bash -c 'cd <tree> && \
        NATRON_GUI_TEST_OUT=build/m173-shots GUI_TEST_TIMEOUT=1200 \
        Tests/gui/run-gui-test.sh Tests/gui/m173_value_global_mode.py <Natron binary or AppImage>'

Sections, each with numbers and captioned screenshots (a NOTE "caption" line precedes each SHOT):
- hover: Ctrl+E (the real shortcut) engages the numeric knob under the pointer inside a node's
  settings panel (Blur size, Transform rotate), the HUD title names it, a hover over a knob that
  is not eligible leaves the last-touched knob as the target, and the colour fallback still
  applies when a Grade is topmost.
- last-touched: with the pointer off every panel, Ctrl+E picks the topmost node panel's
  last-touched knob of any kind.
- rates: Blur size start (10, 10): 50 px right is +5.0 on both dimensions, 20 px up +2.0, a
  (+30, -20) diagonal +5.0; Ctrl gears by 0.1, Shift by 4, Ctrl+Shift by 0.01.
- locks: Q/W/E/R/A on a two-dimension knob (E and R are ignored), a lock changed mid-stroke
  applies from the current value, and the HUD chips change with the lock.
- group: Transform engaged on rotate: 60 px right is +15 degrees; Q/W/E/R/T switch to
  translate/rotate/scale/skew/centre and 1/2/A lock x/y/both; scale is multiplicative
  (1.005^travel); skew edits skewX and skewY together.
- undo: one stroke is one undo step (also for a long drag), skew's two knobs restore together.
- reveal: crosshair mode shows the disc centred on the press point at the documented diameter
  with the puck moving mid-stroke; widget-cursor mode shows the live glyph per kind (group
  translate, rotate, scale, skew, centre, and lone size, translate, generic) and the glyphs
  differ pairwise, except centre which shares translate's.
- cursor: the override cursor before, during and after a stroke in both feedback modes; the
  widget cursor's disc follows the pointer, freezes on press and resumes on release.
- hud: the hint block follows the pointer, its title is "<node>: <knob>", and a screenshot of
  the chips and rate line (their text is drawn, not exposed to Python).
- step: Ctrl+Down/Up across a Transform's knobs in declaration order, and from a Grade's colour
  knobs on to numeric ones.
- colour: Grade gain still engages as before (plain-label title, C/S/T/V, a stroke edits gain,
  one undo, Esc leaves).

Knob widgets carry no object name; KnobGui puts the knob's script name in bold at the start of its
tooltip, so that is how a knob's widgets are found.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 1150
SETTLE_MS = 400
PANEL_MS = 900
STROKE_MS = 150
REVEAL_DIAMETER = 240
GLYPH_DIFF_MIN = 0.003
SHARED_GLYPH_DIFF_MAX = 0.003

BLUR_ID = "Blur"
TRANSFORM_ID = "Transform"
POSITION_ID = "Position"
GRADE_ID = "Grade"
GAIN = "white"
SIZE_INCREMENT = 0.1

FEEDBACK_CROSSHAIR = 0
FEEDBACK_WIDGET = 1

try:
    import faulthandler
    import gc

    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QCursor, QGuiApplication, QImage, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QScrollArea, QWidget

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def dpi_scale():
        return QGuiApplication.primaryScreen().logicalDotsPerInchX() / 96.0

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

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

    def named(name, visible_only=False):
        found = []
        for w in QApplication.allWidgets():
            try:
                if w.objectName() != name:
                    continue
                if visible_only and not w.isVisible():
                    continue
                found.append(w)
            except RuntimeError:
                continue
        return found

    def first_named(name, visible_only=False):
        found = named(name, visible_only)
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

    def tagged(w, name):
        try:
            return ("<b>%s</b>" % name) in (w.toolTip() or "")[:200]
        except RuntimeError:
            return False

    def knob_widget(panel, name):
        """A visible, non-label widget of the knob, scrolled into view."""
        for w in live_children(panel):
            if not tagged(w, name) or isinstance(w, QLabel) or w.width() < 4 or w.height() < 4:
                continue
            a = w.parentWidget()
            while a is not None:
                if isinstance(a, QScrollArea):
                    a.ensureWidgetVisible(w)
                    break
                a = a.parentWidget()
            QApplication.processEvents()
            if w.isVisible() and not w.visibleRegion().isEmpty():
                return w
        return None

    def widget_centre(w):
        return w.mapToGlobal(w.rect().center())

    def hover_knob(panel, name):
        """Moves the real pointer over the knob; true when the widget under it is in the panel."""
        w = knob_widget(panel, name)
        if w is None:
            return False
        g = widget_centre(w)
        QCursor.setPos(g)
        QApplication.processEvents()
        return is_inside(QApplication.widgetAt(g), panel)

    def set_pref(name, value):
        NatronEngine.natron.getSettings().getParam(name).set(value)
        QApplication.processEvents()

    # ---- pixels ------------------------------------------------------------------------------

    def to_image(w):
        QApplication.processEvents()
        return w.grab().toImage().convertToFormat(QImage.Format_RGB32)

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

    def first_diff_x(a, b, tolerance=6):
        """The leftmost column holding a pixel that differs between a and b, or -1."""
        if a.size() != b.size() or a.isNull():
            return -1
        for x in range(a.width()):
            for y in range(a.height()):
                pa, pb = a.pixel(x, y), b.pixel(x, y)
                if (abs(((pa >> 16) & 255) - ((pb >> 16) & 255)) > tolerance
                        or abs(((pa >> 8) & 255) - ((pb >> 8) & 255)) > tolerance
                        or abs((pa & 255) - (pb & 255)) > tolerance):
                    return x
        return -1

    def shoot(w, name, caption, pad=30):
        """A cropped screenshot of w, announced by its caption."""
        if w is None or not w.isVisible():
            report("NOTE %s: nothing visible to shoot (%s)" % (name, caption))
            return
        QApplication.processEvents()
        r = global_rect(w)
        r = QRect(r.x() - pad, r.y() - pad, r.width() + 2 * pad, r.height() + 2 * pad).intersected(screen_rect())
        pix = QGuiApplication.primaryScreen().grabWindow(0, r.x(), r.y(), r.width(), r.height())
        path = os.path.join(gt.OUT, name + ".png")
        report("NOTE caption %s: %s" % (name, caption))
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def drop_stale_wrappers():
        """Collects Python wrappers nothing refers to any more.

        Shiboken maps C++ addresses to wrappers; a wrapper that outlives its widget can be handed
        back for a new widget at the same address, already marked deleted."""
        gc.collect()

    # ---- input -------------------------------------------------------------------------------

    def gmouse(kind, g, button, buttons, mods=Qt.NoModifier):
        w = main_window()
        local = QPointF(w.mapFromGlobal(g))
        QApplication.sendEvent(w, QMouseEvent(kind, local, QPointF(w.mapToGlobal(local)), button, buttons, mods))
        QApplication.processEvents()

    def send_key(key, mods=Qt.NoModifier, target=None):
        target = target or main_window()
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, key, mods))
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyRelease, key, mods))
        QApplication.processEvents()

    def shortcut_action():
        for a in main_window().actions():
            if a.shortcut().toString() == "Ctrl+E" or "Hovered or Last-Touched" in a.text():
                return a
        return None

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

    def engaged():
        return cursor_name() != "none"

    def ctrl_e(via_shortcut=True):
        """Ctrl+E through the window's shortcut map; falls back to the action itself."""
        win = main_window()
        win.activateWindow()
        QApplication.processEvents()
        if via_shortcut:
            QTest.keyClick(win, Qt.Key_E, Qt.ControlModifier)
            QApplication.processEvents()
            return "shortcut"
        a = shortcut_action()
        if a is not None:
            a.trigger()
        QApplication.processEvents()
        return "action"

    def engage_with_ctrl_e():
        """Engages through the real shortcut, else the action; returns which one worked."""
        how = ctrl_e(True)
        if not engaged():
            how = ctrl_e(False)
            report("NOTE the Ctrl+E shortcut did not engage; the action was triggered instead")
        return how if engaged() else None

    def leave():
        if engaged():
            send_key(Qt.Key_Escape)
        QApplication.processEvents()

    # ---- global mode readouts ------------------------------------------------------------------

    def hud_block():
        return first_named("ColorTrackballHudWindow", True) or first_named("ColorTrackballHudBlock", True)

    def hud_knob():
        w = hud_block()
        return w.property("trackballHudKnob") if w is not None else None

    def hud_mode():
        w = hud_block()
        return (w.property("trackballHudMode") or None) if w is not None else None

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

    def disc_widget(r):
        # The disc widgets have no Q_OBJECT, so their className() is QWidget: take the largest visible square child.
        best = None
        for c in r.children():
            if isinstance(c, QWidget) and c.isVisible() and c.width() == c.height():
                if best is None or c.width() > best.width():
                    best = c
        return best

    def disc_centre():
        r = reveal_window()
        d = disc_widget(r) if r is not None else None
        return d.mapToGlobal(d.rect().center()) if d is not None else None

    def disc_image():
        r = reveal_window()
        d = disc_widget(r) if r is not None else None
        return to_image(d) if d is not None else None

    def selection_rgb():
        p = NatronEngine.natron.getSettings().getParam("selection")
        r, g, b = (int(max(0.0, min(1.0, p.getValue(d))) * 255 + 0.5) for d in range(3))
        return 0xff000000 | (r << 16) | (g << 8) | b

    # ---- knob values ---------------------------------------------------------------------------

    def val(node, name):
        p = node.getParam(name)
        return tuple(round(p.getValue(d), 4) for d in range(p.getNumDimensions()))

    def setv(node, name, *values):
        p = node.getParam(name)
        if p.getNumDimensions() == 1:
            p.setValue(values[0])
        else:
            p.set(*values)
        QApplication.processEvents()

    def close_to(actual, expected, tol=0.02):
        return len(actual) == len(expected) and all(abs(a - e) <= tol for a, e in zip(actual, expected))

    def delta(after, before):
        return tuple(round(a - b, 4) for a, b in zip(after, before))

    def fmt(t):
        return "(" + ", ".join("%.4g" % v for v in t) + ")"

    def graph_point():
        """A point on the node graph, off every settings panel."""
        view = node_graph_view()
        return widget_centre(view)

    def park_pointer():
        QCursor.setPos(graph_point())
        QApplication.processEvents()

    def stroke(dx, dy, mods=Qt.NoModifier, steps=10, mid=None):
        """A press at the node graph's centre, steps moves to (dx, dy) away, and a release.

        The real pointer is put on the press point first: the release warps it back to the press
        point only when it moved, and a warp would be a second, spurious move."""
        start = graph_point()
        QCursor.setPos(start)
        QApplication.processEvents()
        gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, mods)
        yield ("sleep", 60)
        for k in range(1, steps + 1):
            g = QPoint(start.x() + dx * k // steps, start.y() + dy * k // steps)
            gmouse(QEvent.MouseMove, g, Qt.NoButton, Qt.LeftButton, mods)
            if mid is not None and k == steps // 2:
                mid()
        end = QPoint(start.x() + dx, start.y() + dy)
        gmouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, mods)
        yield ("sleep", STROKE_MS)

    def engage_on(node, knob_name, expect):
        """Hovers the knob in the node's panel, engages with Ctrl+E and checks the HUD title."""
        raise_panel(node)
        panel = panel_of(node)
        if panel is None:
            return False
        ok = hover_knob(panel, knob_name)
        if not ok:
            report("NOTE could not hover %s.%s" % (node.getLabel(), knob_name))
            return False
        engage_with_ctrl_e()
        title = hud_knob()
        return check(engaged() and title == expect,
                     "Ctrl+E over %s.%s engages it (HUD title %r, expected %r)" % (node.getLabel(), knob_name, title, expect))

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
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)

        blur = create(BLUR_ID, 0, 0)
        transform = create(TRANSFORM_ID, 200, 0)
        position = create(POSITION_ID, 400, 0)
        grade = create(GRADE_ID, 600, 0)
        yield ("until", lambda: all(panel_of(n) is not None for n in (blur, transform, position, grade)), 20.0)
        yield ("sleep", PANEL_MS)
        for n in (blur, transform, position, grade):
            require(panel_of(n) is not None, "%s panel opened" % n.getLabel())
        check(shortcut_action() is not None, "the main window has the Ctrl+E global-mode action")

        # ---- hover ----------------------------------------------------------------------------
        gt.section("hover")
        setv(blur, "size", 10.0, 10.0)
        park_pointer()
        if engage_on(blur, "size", "Blur: size"):
            check(cursor_name() == "cross", "Crosshair feedback: override cursor is the cross (%s)" % cursor_name())
            yield ("sleep", SETTLE_MS)
            shoot(hud_block(), "hover-blur-size", "HUD for Blur size engaged by hover: title Blur: size, lock chips Q W (E R dimmed) and A, rate line")
        leave()
        yield ("sleep", SETTLE_MS)
        check(not engaged(), "Esc leaves global mode and restores the cursor (%s)" % cursor_name())

        if engage_on(transform, "rotate", "Transform: rotate"):
            yield ("sleep", SETTLE_MS)
            shoot(hud_block(), "hover-transform-rotate", "HUD for the Transform group engaged on rotate: member chips Q..T and lock chips 1 2 A")
        leave()
        yield ("sleep", SETTLE_MS)

        # A knob that is not eligible under the pointer leaves the last-touched knob as the target.
        raise_panel(blur)
        bp = panel_of(blur)
        size_w = knob_widget(bp, "size")
        if size_w is not None:
            QTest.mouseClick(size_w, Qt.LeftButton)
            size_w.clearFocus()
            QApplication.processEvents()
        if hover_knob(bp, "filter"):
            engage_with_ctrl_e()
            check(engaged() and hud_knob() == "Blur: size",
                  "Ctrl+E over a knob that is not eligible falls back to the last-touched Blur size (HUD %r)" % hud_knob())
        else:
            report("NOTE could not hover Blur.filter")
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- last-touched ---------------------------------------------------------------------
        gt.section("last-touched")
        raise_panel(transform)
        tp = panel_of(transform)
        scale_w = knob_widget(tp, "scale")
        if check(scale_w is not None, "found the Transform scale widget"):
            QTest.mouseClick(scale_w, Qt.LeftButton)
            scale_w.clearFocus()
            QApplication.processEvents()
            park_pointer()
            at = QApplication.widgetAt(QCursor.pos())
            check(at is not None and not is_inside(at, tp), "the pointer is off the settings panel")
            engage_with_ctrl_e()
            check(engaged() and hud_knob() == "Transform: scale",
                  "pointer off the panel: Ctrl+E picks the topmost panel's last-touched knob (HUD %r)" % hud_knob())
            yield ("sleep", SETTLE_MS)
            shoot(hud_block(), "last-touched-transform-scale", "Ctrl+E with the pointer on the node graph: target is the last-touched Transform scale")
            leave()
            yield ("sleep", SETTLE_MS)
        raise_panel(blur)
        park_pointer()
        engage_with_ctrl_e()
        check(engaged() and hud_knob() == "Blur: size",
              "with Blur on top, the same Ctrl+E picks Blur's last-touched size, not Transform's (HUD %r)" % hud_knob())
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- rates ----------------------------------------------------------------------------
        gt.section("rates")
        setv(blur, "size", 10.0, 10.0)
        if engage_on(blur, "size", "Blur: size"):
            start = val(blur, "size")
            yield from stroke(50, 0)
            now = val(blur, "size")
            check(close_to(delta(now, start), (5.0, 5.0)),
                  "50 px right moves both size dimensions by %.1f (+%s)" % (50 * SIZE_INCREMENT, fmt(delta(now, start))))
            start = now
            yield from stroke(0, -20)
            now = val(blur, "size")
            check(close_to(delta(now, start), (2.0, 2.0)), "20 px up moves both by +2.0 (+%s)" % fmt(delta(now, start)))
            start = now
            yield from stroke(30, -20)
            now = val(blur, "size")
            check(close_to(delta(now, start), (5.0, 5.0)), "a (+30, -20) diagonal moves both by +5.0 (+%s)" % fmt(delta(now, start)))
            start = now
            yield from stroke(-30, 0)
            now = val(blur, "size")
            check(close_to(delta(now, start), (-3.0, -3.0)), "30 px left moves both by -3.0 (%s)" % fmt(delta(now, start)))
            for label, mods, travel, expected in (
                    ("Ctrl", Qt.ControlModifier, 100, 1.0),
                    ("Shift", Qt.ShiftModifier, 20, 8.0),
                    ("Ctrl+Shift", Qt.ControlModifier | Qt.ShiftModifier, 200, 0.2)):
                start = val(blur, "size")
                yield from stroke(travel, 0, mods)
                now = val(blur, "size")
                check(close_to(delta(now, start), (expected, expected), 0.03),
                      "%s gear: %d px moves both by %.2f (+%s)" % (label, travel, expected, fmt(delta(now, start))))
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- locks ----------------------------------------------------------------------------
        gt.section("locks")
        setv(blur, "size", 10.0, 10.0)
        if engage_on(blur, "size", "Blur: size"):
            chip_images = {}
            plan = (
                ("A", Qt.Key_A, (1.0, 1.0)),
                ("Q", Qt.Key_Q, (1.0, 0.0)),
                ("W", Qt.Key_W, (0.0, 1.0)),
                ("E", Qt.Key_E, (0.0, 1.0)),
                ("A", Qt.Key_A, (1.0, 1.0)),
            )
            for label, key, expected in plan:
                send_key(key)
                yield ("sleep", 100)
                block = hud_block()
                if block is not None and label not in chip_images:
                    chip_images[label] = to_image(block)
                start = val(blur, "size")
                yield from stroke(10, 0)
                got = delta(val(blur, "size"), start)
                note = " (a lock at or beyond the dimension count is ignored, so W stays)" if label == "E" else ""
                check(close_to(got, expected), "%s then 10 px: size moves %s, expected %s%s" % (label, fmt(got), fmt(expected), note))
            if len(chip_images) == 4:
                ia, iq, iw, ie = (chip_images[k] for k in ("A", "Q", "W", "E"))
                check(image_diff(ia, iq) > 0.0005 and image_diff(iq, iw) > 0.0005,
                      "the HUD chips differ for A, Q and W (the lit chip moves)")
                xq, xw = first_diff_x(iq, ia), first_diff_x(iw, ia)
                check(0 <= xq < xw, "the Q chip sits left of the W chip (leftmost differing column %d < %d)" % (xq, xw))
                check(image_diff(iw, ie) < 0.0005, "E (ignored) leaves the HUD as W had it")
            send_key(Qt.Key_Q)
            send_key(Qt.Key_A)
            start = val(blur, "size")
            yield from stroke(20, 0, mid=lambda: send_key(Qt.Key_Q))
            got = delta(val(blur, "size"), start)
            check(close_to(got, (2.0, 1.0)), "A then Q at half way: both moved 1.0, then only x 1.0 more, from the current value (%s)" % fmt(got))
            send_key(Qt.Key_A)
            shoot(hud_block(), "locks-blur-all", "Blur size HUD after A: every dimension lit")
            send_key(Qt.Key_W)
            shoot(hud_block(), "locks-blur-w", "Blur size HUD after W: y chip lit")
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- group ----------------------------------------------------------------------------
        gt.section("group")
        setv(transform, "translate", 0.0, 0.0)
        setv(transform, "rotate", 0.0)
        setv(transform, "scale", 1.0, 1.0)
        setv(transform, "skewX", 0.0)
        setv(transform, "skewY", 0.0)
        setv(transform, "center", 0.0, 0.0)
        if engage_on(transform, "rotate", "Transform: rotate"):
            r0 = val(transform, "rotate")
            yield from stroke(60, 0)
            got = delta(val(transform, "rotate"), r0)
            check(close_to(got, (15.0,)), "rotate: 60 px right is +15 degrees (%s)" % fmt(got))
            yield from stroke(-60, 0)

            send_key(Qt.Key_Q)
            check(hud_knob() == "Transform: translate", "Q switches to translate (HUD %r)" % hud_knob())
            for label, key, expected in (("A", Qt.Key_A, (30.0, 20.0)), ("2", Qt.Key_2, (0.0, 20.0)),
                                         ("1", Qt.Key_1, (30.0, 0.0)), ("A", Qt.Key_A, (30.0, 20.0))):
                send_key(key)
                t0 = val(transform, "translate")
                yield from stroke(30, -20)
                got = delta(val(transform, "translate"), t0)
                check(close_to(got, expected), "translate, lock %s, stroke (+30, -20): %s, expected %s" % (label, fmt(got), fmt(expected)))

            send_key(Qt.Key_W)
            check(hud_knob() == "Transform: rotate", "W switches to rotate (HUD %r)" % hud_knob())
            send_key(Qt.Key_2)
            r0 = val(transform, "rotate")
            yield from stroke(60, 0)
            got = delta(val(transform, "rotate"), r0)
            check(close_to(got, (15.0,)), "rotate with a y lock still turns, as it has one axis (%s)" % fmt(got))

            send_key(Qt.Key_E)
            check(hud_knob() == "Transform: scale", "E switches to scale (HUD %r)" % hud_knob())
            factor = round(1.005 ** 40, 4)
            send_key(Qt.Key_A)
            s0 = val(transform, "scale")
            yield from stroke(40, 0)
            s1 = val(transform, "scale")
            check(close_to(s1, (s0[0] * factor, s0[1] * factor), 0.005),
                  "scale: 40 px right multiplies both by 1.005^40 = %.4f (%s -> %s)" % (factor, fmt(s0), fmt(s1)))
            send_key(Qt.Key_2)
            yield from stroke(40, 0)
            s2 = val(transform, "scale")
            check(close_to(s2, (s1[0], s1[1] * factor), 0.005), "scale with lock 2 multiplies y only (%s -> %s)" % (fmt(s1), fmt(s2)))

            send_key(Qt.Key_R)
            check(hud_knob() in ("Transform: skewX", "Transform: skewY"), "R switches to skew (HUD %r)" % hud_knob())
            send_key(Qt.Key_A)
            kx0, ky0 = val(transform, "skewX"), val(transform, "skewY")
            yield from stroke(30, -20)
            kx1, ky1 = val(transform, "skewX"), val(transform, "skewY")
            check(close_to(delta(kx1, kx0), (0.30,)) and close_to(delta(ky1, ky0), (0.20,)),
                  "skew: (+30, -20) is skewX %s and skewY %s" % (fmt(delta(kx1, kx0)), fmt(delta(ky1, ky0))))

            send_key(Qt.Key_T)
            check(hud_knob() == "Transform: center", "T switches to center (HUD %r)" % hud_knob())
            send_key(Qt.Key_A)
            c0 = val(transform, "center")
            yield from stroke(30, -20)
            got = delta(val(transform, "center"), c0)
            check(close_to(got, (30.0, 20.0)), "center moves as translate does (%s)" % fmt(got))

            gt.section("undo")
            send_key(Qt.Key_R)
            send_key(Qt.Key_A)
            before = (val(transform, "skewX"), val(transform, "skewY"))
            yield from stroke(30, -20)
            after = (val(transform, "skewX"), val(transform, "skewY"))
            send_key(Qt.Key_Z, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            undone = (val(transform, "skewX"), val(transform, "skewY"))
            check(after != before, "the skew stroke changed skewX and skewY")
            check(undone == before, "one Ctrl+Z restores skewX and skewY together (%s -> %s)" % (fmt(after[0] + after[1]), fmt(undone[0] + undone[1])))
            send_key(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)
            yield ("sleep", SETTLE_MS)
            check((val(transform, "skewX"), val(transform, "skewY")) == after, "Ctrl+Shift+Z redoes the stroke")

            send_key(Qt.Key_Q)
            send_key(Qt.Key_A)
            t0 = val(transform, "translate")
            yield from stroke(30, 0)
            t1 = val(transform, "translate")
            yield from stroke(30, 0)
            t2 = val(transform, "translate")
            send_key(Qt.Key_Z, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            check(val(transform, "translate") == t1, "two strokes, one Ctrl+Z: back to the first stroke's result (%s)" % fmt(val(transform, "translate")))
            send_key(Qt.Key_Z, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            check(val(transform, "translate") == t0, "second Ctrl+Z: back to the start (%s)" % fmt(val(transform, "translate")))
            check(t0 != t1 != t2, "the strokes themselves changed translate")
        leave()
        yield ("sleep", SETTLE_MS)

        setv(blur, "size", 10.0, 10.0)
        if engage_on(blur, "size", "Blur: size"):
            before = val(blur, "size")
            yield from stroke(80, -40, steps=40)
            check(val(blur, "size") != before, "the long drag changed Blur size")
            leave()
            yield ("sleep", SETTLE_MS)
            panel = panel_of(blur)
            QMetaObject.invokeMethod(panel, "onUndoClicked")
            yield ("sleep", SETTLE_MS)
            check(val(blur, "size") == before, "a 40-move drag is one undo step (%s -> %s)" % (fmt(before), fmt(val(blur, "size"))))
        yield ("sleep", SETTLE_MS)

        # ---- reveal ---------------------------------------------------------------------------
        gt.section("reveal")
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)
        if engage_on(transform, "rotate", "Transform: rotate"):
            start = graph_point()
            QCursor.setPos(start)
            QApplication.processEvents()
            check(not reveal_shown(), "Crosshair feedback: no disc before the press")
            gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
            yield ("sleep", SETTLE_MS)
            if check(reveal_shown(), "the disc comes up on the press"):
                d = disc_widget(reveal_window())
                centre = disc_centre()
                size_ok = d is not None and abs(d.width() - REVEAL_DIAMETER * dpi_scale()) <= 12
                check(size_ok, "the disc is %d px across (%s)" % (REVEAL_DIAMETER, d.width() if d is not None else None))
                check(centre is not None and (centre - start).manhattanLength() <= 4,
                      "the disc is centred on the press point (%s vs %s)" % (centre, start))
                img_a = disc_image()
                shoot(reveal_window(), "reveal-group-rotate-press", "Transform group, rotate: disc at the press point, puck at rest, pills for the two directions")
                for k in range(1, 9):
                    gmouse(QEvent.MouseMove, QPoint(start.x() + 6 * k, start.y()), Qt.NoButton, Qt.LeftButton)
                yield ("sleep", SETTLE_MS)
                img_b = disc_image()
                check(img_a is not None and img_b is not None and image_diff(img_a, img_b) > 0.001,
                      "the disc changes between the press and the middle of the stroke")
                shoot(reveal_window(), "reveal-group-rotate-drag", "Transform group, rotate: the same disc 48 px into the stroke, puck displaced")
                check(cursor_name() == "cross", "the cross cursor holds during the stroke (%s)" % cursor_name())
            gmouse(QEvent.MouseButtonRelease, QPoint(start.x() + 48, start.y()), Qt.LeftButton, Qt.NoButton)
            yield ("sleep", SETTLE_MS)
            check(not reveal_shown(), "the disc goes away on release")
        leave()
        yield ("sleep", SETTLE_MS)

        set_pref("colorTrackballFeedback", FEEDBACK_WIDGET)
        glyphs = {}

        def capture(label):
            img = disc_image()
            if img is not None:
                glyphs[label] = img
            shoot(reveal_window(), "reveal-" + label, "Widget-cursor disc, %s" % label)

        if engage_on(transform, "rotate", "Transform: rotate"):
            yield ("sleep", SETTLE_MS)
            check(reveal_shown(), "Widget cursor feedback: the disc is up from engage")
            for label, key, title in (("group-translate", Qt.Key_Q, "translate"), ("group-rotate", Qt.Key_W, "rotate"),
                                      ("group-scale", Qt.Key_E, "scale"), ("group-skew", Qt.Key_R, None),
                                      ("group-center", Qt.Key_T, "center")):
                send_key(key)
                yield ("sleep", 250)
                if title is not None:
                    check(hud_knob() == "Transform: " + title, "%s: HUD names %s (%r)" % (label, title, hud_knob()))
                capture(label)
        leave()
        yield ("sleep", SETTLE_MS)
        for label, node, knob, title in (("lone-size", blur, "size", "Blur: size"),
                                         ("lone-translate", position, "translate", "Position: translate"),
                                         ("generic", transform, "transformAmount", "Transform: transformAmount")):
            if engage_on(node, knob, title):
                yield ("sleep", SETTLE_MS)
                capture(label)
            leave()
            yield ("sleep", SETTLE_MS)

        labels = sorted(glyphs)
        report("NOTE glyphs captured: %s" % ", ".join(labels))
        # Centre reuses translate's glyph, so that pair is the one that must match.
        pairs = [(a, b) for i, a in enumerate(labels) for b in labels[i + 1:]]
        for a, b in pairs:
            diff = image_diff(glyphs[a], glyphs[b])
            if {a, b} == {"group-translate", "group-center"}:
                check(diff <= SHARED_GLYPH_DIFF_MAX, "%s and %s share one glyph (%.4f differing)" % (a, b, diff))
            elif {a, b} == {"group-translate", "lone-translate"}:
                report("NOTE %s vs %s: %.4f of pixels differ" % (a, b, diff))
            else:
                check(diff >= GLYPH_DIFF_MIN, "%s and %s have different glyphs (%.4f differing)" % (a, b, diff))

        # ---- cursor ---------------------------------------------------------------------------
        gt.section("cursor")
        for feedback, before_name in ((FEEDBACK_CROSSHAIR, "cross"), (FEEDBACK_WIDGET, "blank")):
            set_pref("colorTrackballFeedback", feedback)
            tag = "Crosshair" if feedback == FEEDBACK_CROSSHAIR else "Widget cursor"
            setv(blur, "size", 10.0, 10.0)
            if engage_on(blur, "size", "Blur: size"):
                yield ("sleep", SETTLE_MS)
                check(cursor_name() == before_name, "%s: cursor before the stroke is %s (%s)" % (tag, before_name, cursor_name()))
                if feedback == FEEDBACK_WIDGET:
                    check(reveal_shown(), "Widget cursor: the disc is up before any press")
                    spot = graph_point()
                    follows = []
                    for offset in (QPoint(0, 0), QPoint(120, 40), QPoint(-90, -60)):
                        QCursor.setPos(spot + offset)
                        yield ("sleep", 200)
                        c = disc_centre()
                        follows.append(c is not None and (c - (spot + offset)).manhattanLength() <= 40)
                    check(all(follows), "Widget cursor: the disc follows the pointer to three places (%s)" % follows)
                    shoot(reveal_window(), "cursor-widget-follow", "Widget cursor: the compact disc stands in for the hidden pointer on Blur size")
                    start = spot
                    QCursor.setPos(start)
                    yield ("sleep", 200)
                    frozen_at = disc_centre()
                    gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
                    yield ("sleep", 150)
                    for k in range(1, 7):
                        gmouse(QEvent.MouseMove, QPoint(start.x() + 10 * k, start.y()), Qt.NoButton, Qt.LeftButton)
                    yield ("sleep", 150)
                    c = disc_centre()
                    check(frozen_at is not None and c is not None and (c - frozen_at).manhattanLength() <= 4,
                          "Widget cursor: the disc freezes on press (%s -> %s)" % (frozen_at, c))
                    check(cursor_name() == "blank", "Widget cursor: cursor stays blank during the stroke (%s)" % cursor_name())
                    gmouse(QEvent.MouseButtonRelease, QPoint(start.x() + 60, start.y()), Qt.LeftButton, Qt.NoButton)
                    QCursor.setPos(start + QPoint(150, 80))
                    yield ("sleep", 250)
                    c = disc_centre()
                    check(c is not None and (c - (start + QPoint(150, 80))).manhattanLength() <= 40,
                          "Widget cursor: the disc follows again after the release")
                else:
                    yield from stroke(30, 0, mid=lambda: check(cursor_name() == "cross", "Crosshair: cursor during the stroke is the cross"))
                    check(cursor_name() == "cross", "Crosshair: cursor after the stroke is the cross (%s)" % cursor_name())
            leave()
            yield ("sleep", SETTLE_MS)
            check(cursor_name() == "none", "%s: no override cursor after leaving (%s)" % (tag, cursor_name()))

        # ---- hud ------------------------------------------------------------------------------
        gt.section("hud")
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)
        if engage_on(blur, "size", "Blur: size"):
            yield ("sleep", SETTLE_MS)
            block = hud_block()
            if check(block is not None, "the hint block is up while engaged"):
                spots = []
                for offset in (QPoint(-300, -100), QPoint(300, 150)):
                    QCursor.setPos(graph_point() + offset)
                    yield ("sleep", 250)
                    block = hud_block()
                    spots.append(block.frameGeometry().center() if block is not None else None)
                check(all(s is not None for s in spots) and (spots[0] - spots[1]).manhattanLength() > 300,
                      "the hint block follows the pointer (%s, %s)" % (spots[0], spots[1]))
                lines = block.property("trackballHudLines") if block is not None else None
                report("NOTE the chips and rate line are drawn text; trackballHudLines is %s" % ("readable: %r" % (lines,) if lines else "not exposed"))
                shoot(hud_block(), "hud-blur-size", "HUD for Blur size: chips Q W lit-or-dimmed and A, rate line with the size increment per pixel and the Ctrl/Shift gears")
        leave()
        yield ("sleep", SETTLE_MS)
        if engage_on(transform, "transformAmount", "Transform: transformAmount"):
            yield ("sleep", SETTLE_MS)
            ta = val(transform, "transformAmount")
            yield from stroke(50, 0)
            got = delta(val(transform, "transformAmount"), ta)
            check(close_to(got, (0.5,), 0.03), "the generic knob moves by its increment per pixel, 0.01: 50 px is +0.5 (%s)" % fmt(got))
            shoot(hud_block(), "hud-generic", "HUD for the generic knob: a single dimension, one lock chip lit, rate line for the knob's increment")
        leave()
        yield ("sleep", SETTLE_MS)
        setv(position, "translate", 0.0, 0.0)
        if engage_on(position, "translate", "Position: translate"):
            p0 = val(position, "translate")
            yield from stroke(30, -20)
            got = delta(val(position, "translate"), p0)
            check(close_to(got, (30.0, 20.0), 0.6), "lone translate: 1 unit per pixel, x from right, y from up (%s)" % fmt(got))
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- step -----------------------------------------------------------------------------
        gt.section("step")
        expected_order = ["Transform: translate", "Transform: rotate", "Transform: scale", "Transform: skewX",
                          "Transform: skewY", "Transform: transformAmount", "Transform: center"]
        if engage_on(transform, "translate", "Transform: translate"):
            seen = [hud_knob()]
            for _ in range(14):
                send_key(Qt.Key_Down, Qt.ControlModifier)
                yield ("sleep", 120)
                t = hud_knob()
                if t == seen[-1]:
                    break
                seen.append(t)
            report("NOTE Ctrl+Down order: %s" % " > ".join(str(s) for s in seen))
            positions = [seen.index(t) if t in seen else -1 for t in expected_order]
            check(all(p >= 0 for p in positions) and positions == sorted(positions),
                  "Ctrl+Down visits the numeric knobs in declaration order (missing or misordered: %s)" % positions)
            check(all(s.startswith("Transform: ") for s in seen), "stepping never leaves the node's own knobs")
            shoot(hud_block(), "step-transform-last", "HUD after stepping to the Transform's last eligible knob")
            for _ in range(len(seen) + 2):
                send_key(Qt.Key_Up, Qt.ControlModifier)
                yield ("sleep", 120)
            check(hud_knob() == seen[0], "Ctrl+Up back to the top returns to %r (HUD %r)" % (seen[0], hud_knob()))
        leave()
        yield ("sleep", SETTLE_MS)

        raise_panel(grade)
        gp = panel_of(grade)
        if hover_knob(gp, GAIN):
            engage_with_ctrl_e()
            first = hud_knob()
            seen = [first]
            for _ in range(14):
                send_key(Qt.Key_Down, Qt.ControlModifier)
                yield ("sleep", 120)
                t = hud_knob()
                if t == seen[-1]:
                    break
                seen.append(t)
            report("NOTE Grade Ctrl+Down titles: %s" % " > ".join(str(s) for s in seen))
            check(first is not None and ": " not in first, "the Grade starts on a colour knob, titled by its plain label (%r)" % first)
            numeric = [s for s in seen if s and ": " in s]
            check(len(numeric) > 0, "Ctrl+Down from the Grade's colour knobs reaches a numeric knob (%s)" % (numeric[:1],))
            if numeric:
                shoot(hud_block(), "step-grade-numeric", "HUD after stepping from the Grade's colour knobs onto a numeric knob")
                for _ in range(len(seen) + 2):
                    send_key(Qt.Key_Up, Qt.ControlModifier)
                    yield ("sleep", 120)
                check(hud_knob() == first, "Ctrl+Up returns to the first colour knob (%r)" % hud_knob())
        else:
            report("NOTE could not hover the Grade gain")
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- colour ---------------------------------------------------------------------------
        gt.section("colour")
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)
        for d in range(4):
            grade.getParam(GAIN).setValue(1.0, d)
        QApplication.processEvents()
        raise_panel(grade)
        gp = panel_of(grade)
        if hover_knob(gp, GAIN):
            engage_with_ctrl_e()
            title = hud_knob()
            check(engaged() and title and ": " not in title, "colour knob: the HUD title is the plain label (%r)" % title)
            check(hud_mode() == "C", "a fresh engage starts in C (HUD %r)" % hud_mode())
            for key, mode in ((Qt.Key_S, "S"), (Qt.Key_T, "T"), (Qt.Key_V, "V"), (Qt.Key_C, "C")):
                send_key(key)
                check(hud_mode() == mode or hud_mode() is None, "%s picks mode %s (HUD %r)" % (chr(key), mode, hud_mode()))
            yield ("sleep", SETTLE_MS)
            shoot(hud_block(), "colour-hud", "Colour global mode on Grade gain: the unchanged colour HUD in mode C")
            g0 = val(grade, GAIN)
            yield from stroke(40, -20)
            g1 = val(grade, GAIN)
            check(g1 != g0, "a colour stroke edits gain (%s -> %s)" % (fmt(g0), fmt(g1)))
            shoot(reveal_window() or hud_block(), "colour-after-stroke", "Grade gain after a colour stroke")
            send_key(Qt.Key_Z, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            check(val(grade, GAIN) == g0, "one Ctrl+Z restores gain (%s)" % fmt(val(grade, GAIN)))
            send_key(Qt.Key_Escape)
            yield ("sleep", SETTLE_MS)
            check(not engaged(), "Esc leaves colour global mode (%s)" % cursor_name())
        else:
            report("NOTE could not hover the Grade gain")
        drop_stale_wrappers()
        leave()
        yield ("sleep", SETTLE_MS)
        check(cursor_name() == "none", "the run ends with no override cursor (%s)" % cursor_name())
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
