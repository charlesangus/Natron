"""Interaction checks and screenshots for weighted curve-editor tangents: a Free or Broken key's
handle sits on its Bezier control point, dragging it sets slope and weight together, an unbroken
key's other handle follows, a broken (or Ctrl-dragged) key's other side stays put, the handle does
not jump on release, one undo reverts a whole drag, the drawn curve is the evaluated one, picking
works at a 2x scale, and weights survive a save and reload.

Sections (all run by default; M96_SECTIONS=untouched,broken picks a subset):
  untouched       a key whose handles were never dragged keeps weights of exactly 1/3 and the
                  plain Hermite curve, and selecting it changes nothing
  weighted-drag   dragging the right handle outward along its slope doubles its weight and
                  changes the segment, and the handle lands under the cursor
  unbroken        the other handle of a Free key takes the same slope and scales its weight by the
                  same factor, on screen as well as in the key
  broken          a Ctrl-drag breaks the key and leaves the other side's slope and weight alone
  release         the handle's position while the button is held is where it stays on release
  undo            one undo restores every key exactly after a multi-step drag, and redo re-applies it
  weighted-draw   the drawn curve follows the evaluated values, not the unweighted Hermite, and the
                  handle tip is drawn on the control point
  hidpi-pick      clicking a key shows its handles where they are computed, and dragging the key
                  moves it by the cursor's distance; run with -e QT_SCALE_FACTOR=2 for the 2x case
  save-load       weights read back identically from a saved .ntpz (always last: it resets the
                  project)

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m96-shots/out -e GUI_TEST_TIMEOUT=300 \\
        [-e M96_SECTIONS=weighted-drag,undo] [-e QT_SCALE_FACTOR=2] \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m96_weighted_tangents.py build/fast/App/Natron

The graph is a Grade whose white.r has keys at frames 1, 11 and 21. Every section resets those
keys through Python first, so any subset can run alone. Positions only the C++ side knows are
derived the way Tests/gui/m69_cleanup.py derives them:
  - the value axis follows CurveWidget::frameAll(), which fills the widget's height with the keys'
    range grown by a tenth each way;
  - the time axis is read from where the orange current-time line is drawn at two frames;
  - screen grabs are in device pixels, so every grab is read through its device-pixel ratio and
    all positions here are logical, the space mouse events use.
Undo is the Edit menu's action, triggered directly, because the Ctrl+Z shortcut is matched by the
shortcut map before a sent key event reaches a widget.
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 30.0
WATCHDOG_S = 600
SETTLE_MS = 400
DRAG_STEPS = 4
THIRD = 1.0 / 3.0
# (time, value, slope) of the three keys every section starts from.
KEYS = ((1.0, 0.0, 0.0), (11.0, 0.5, 0.05), (21.0, 1.0, 0.0))
ALL_SECTIONS = ("untouched", "weighted-drag", "unbroken", "broken", "release", "undo",
                "weighted-draw", "hidpi-pick", "save-load")

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QColor, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("M96_SECTIONS")
    selected = [s.strip() for s in (wanted.split(",") if wanted else ALL_SECTIONS) if s.strip()]
    unknown = [s for s in selected if s not in ALL_SECTIONS]
    FREE = NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeFree
    BROKEN = NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeBroken

    # ---- nodes, windows, events -----------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def press(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, button, button, modifiers)

    def release(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, button, Qt.NoButton, modifiers)

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        press(widget, pos, button, modifiers)
        release(widget, pos, button, modifiers)

    def drag(widget, start, end, modifiers=Qt.NoModifier, steps=DRAG_STEPS):
        start = QPointF(start)
        end = QPointF(end)
        press(widget, start, Qt.LeftButton, modifiers)
        for i in range(1, steps + 1):
            move(widget, start + (end - start) * (i / float(steps)), Qt.LeftButton, modifiers)
        release(widget, end, Qt.LeftButton, modifiers)

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    # ---- screen grabs, read in logical coordinates ----------------------------------------

    class Grab(object):
        def __init__(self, widget):
            QApplication.processEvents()
            origin = widget.mapToGlobal(QPoint(0, 0))
            self.img = QGuiApplication.primaryScreen().grabWindow(
                0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
            self.s = self.img.width() / float(max(1, widget.width()))

        def colour(self, x, y):
            xd, yd = int(round(x * self.s)), int(round(y * self.s))
            if 0 <= xd < self.img.width() and 0 <= yd < self.img.height():
                return self.img.pixelColor(xd, yd)
            return None

        def pixels(self, bounds):
            """(x, y, colour) of every device pixel inside the logical bounds (x0, y0, x1, y1),
            with x and y logical."""
            x0, y0, x1, y1 = bounds
            for yd in range(max(0, int(y0 * self.s)), min(self.img.height(), int(y1 * self.s) + 1)):
                for xd in range(max(0, int(x0 * self.s)), min(self.img.width(), int(x1 * self.s) + 1)):
                    yield xd / self.s, yd / self.s, self.img.pixelColor(xd, yd)

        def background(self):
            counts = {}
            for yd in range(0, self.img.height(), 7):
                for xd in range(0, self.img.width(), 7):
                    rgb = self.img.pixel(xd, yd)
                    counts[rgb] = counts.get(rgb, 0) + 1
            best = max(counts, key=counts.get)
            return QColor.fromRgb(best)

    def colour_dist(a, b):
        return abs(a.red() - b.red()) + abs(a.green() - b.green()) + abs(a.blue() - b.blue())

    def is_handle_red(c):
        return c.red() > 200 and 50 < c.green() < 140 and 50 < c.blue() < 140

    def time_cursor_x(cw):
        """The logical column of the curve widget's orange current-time line."""
        g = Grab(cw)
        best, best_x = 0, None
        for xd in range(g.img.width()):
            n = 0
            for yd in range(0, g.img.height(), 4):
                c = g.img.pixelColor(xd, yd)
                if c.red() > 200 and 80 < c.green() < 190 and c.blue() < 90:
                    n += 1
            if n > best:
                best, best_x = n, xd
        return None if best_x is None else best_x / g.s

    def widget_shot(widget, name):
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to crop, skipping the shot" % name)
            return
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    # ---- Edit menu and panes --------------------------------------------------------------

    def edit_actions():
        win = main_window()
        if win is None or win.menuBar() is None:
            return []
        for act in win.menuBar().actions():
            if act.menu() is not None and act.text().replace("&", "").lower().startswith("edit"):
                return act.menu().actions()
        return []

    def trigger(prefix):
        for a in edit_actions():
            if a.text().replace("&", "").lower().startswith(prefix) and a.isEnabled():
                a.trigger()
                QApplication.processEvents()
                return True
        check(False, "found an enabled '%s' action in the Edit menu" % prefix)
        return False

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def make_curve_editor_current(editor):
        """Brings the curve editor's tab to the front of its pane, the way clicking the tab does."""
        pane = ancestor_of_class(editor, "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if "curve" in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def select_node_row(editor, label):
        # The tree wrappers are found again on every call: holding one across a yield crashes.
        found = False
        for tree in editor.findChildren(QTreeWidget):
            for i in range(tree.topLevelItemCount()):
                item = tree.topLevelItem(i)
                if item.text(0) == label:
                    item.setSelected(True)
                    found = True
        QApplication.processEvents()
        return found

    # ---- steps ----------------------------------------------------------------------------

    def steps():
        gt.require(not unknown, "M96_SECTIONS names known sections (unknown: %r)" % (unknown,))
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active", "synthetic presses are ignored by an inactive window")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        gt.require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 2400)
        dpr = QApplication.instance().devicePixelRatio()
        report("NOTE device-pixel ratio %.2f" % dpr)

        grade = create("net.sf.openfx.GradePlugin", 0, 0)
        white = grade.getParam("white")
        gt.require(white is not None, "the Grade has a white parameter")
        grade_label = grade.getLabel()
        grade_name = grade.getScriptName()

        def reset_keys():
            for t, v, s in KEYS:
                white.setValueAtTime(v, t, 0)
                white.setInterpolationAtTime(t, FREE, 0)
                white.setKeyTangents(t, s, s, THIRD, THIRD, 0)
            QApplication.processEvents()

        def tangents(t):
            return tuple(white.getKeyTangents(t, 0))

        def all_tangents():
            return [tangents(t) for t, _v, _s in KEYS]

        sample_times = [1.0 + 0.25 * i for i in range(81)]

        def values():
            return [white.getValueAtTime(t, 0) for t in sample_times]

        def hermite(t, keys):
            """The unweighted cubic between the keys around t; keys holds (time, value, left
            slope, right slope)."""
            for (t0, v0, _l0, r0), (t1, v1, l1, _r1) in zip(keys, keys[1:]):
                if t0 <= t <= t1:
                    dt = t1 - t0
                    x = (t - t0) / dt
                    m0, m1 = r0 * dt, l1 * dt
                    return ((2 * x ** 3 - 3 * x ** 2 + 1) * v0 + (x ** 3 - 2 * x ** 2 + x) * m0
                            + (-2 * x ** 3 + 3 * x ** 2) * v1 + (x ** 3 - x ** 2) * m1)
            return None

        reset_keys()
        yield ("sleep", SETTLE_MS)

        # The editor wrapper is kept for the whole run so the children found through it stay valid.
        editors = gt.widgets_of("CurveEditor")
        gt.require(editors, "found the curve editor")
        editor = editors[0]
        gt.require(make_curve_editor_current(editor), "brought the curve editor's tab to the front")
        yield ("sleep", SETTLE_MS)
        gt.require(select_node_row(editor, grade_label), "selected the Grade's row in the curve editor")
        yield ("sleep", SETTLE_MS)
        cws = [w for w in gt.widgets_of("CurveWidget") if w.isVisible()]
        gt.require(cws, "the curve widget is visible")
        cw = cws[0]
        send_key(cw, Qt.Key_A)
        yield ("sleep", SETTLE_MS)

        W, H = float(cw.width()), float(cw.height())
        y0, y1 = 0.0 - 0.1, 1.0 + 0.1
        sy = H / (y1 - y0)
        pyviewer = app.getViewer(viewer.getScriptName())
        gt.require(pyviewer is not None, "the viewer has a Python handle")
        pyviewer.seek(1)
        yield ("sleep", SETTLE_MS)
        xa = time_cursor_x(cw)
        pyviewer.seek(21)
        yield ("sleep", SETTLE_MS)
        xb = time_cursor_x(cw)
        # Park the time line off the keys so its orange does not mix with the handles' red.
        pyviewer.seek(40)
        yield ("sleep", SETTLE_MS)
        report("NOTE curve widget %dx%d, time line at frame 1: %r, frame 21: %r" % (W, H, xa, xb))
        gt.require(xa is not None and xb is not None and xb > xa, "found the curve widget's time line at frames 1 and 21")
        sx = (xb - xa) / 20.0

        def to_w(t, v):
            return QPointF(xa + (t - 1) * sx, H - (v - y0) * sy)

        key_pt = to_w(11.0, 0.5)
        span_r = to_w(21.0, 0).x() - key_pt.x()
        span_l = key_pt.x() - to_w(1.0, 0).x()

        def handles():
            """Where the middle key's left and right handles are drawn: on its control points."""
            _i, left, right, lw, rw = tangents(11.0)
            k = sy / sx
            return (key_pt + QPointF(-lw * span_l, lw * span_l * left * k),
                    key_pt + QPointF(rw * span_r, -rw * span_r * right * k))

        def dist(a, b):
            return math.hypot(a.x() - b.x(), a.y() - b.y())

        def select_middle():
            click(cw, QPointF(10, 10))
            click(cw, key_pt)

        def handle_reach(g, side):
            """How far from the key the handle-red pixels on one side of it reach, and how many."""
            r = 1.2 * max(span_l, span_r)
            bounds = (key_pt.x() - r, key_pt.y() - r, key_pt.x() + r, key_pt.y() + r)
            d = [math.hypot(x - key_pt.x(), y - key_pt.y()) for x, y, c in g.pixels(bounds)
                 if is_handle_red(c) and ((x > key_pt.x() + 2) if side > 0 else (x < key_pt.x() - 2))]
            return (max(d) if d else 0.0), len(d)

        def check_reach(g, side, tip, label):
            # The tip is drawn as a 7-pixel point, so the red reaches a little past it.
            reach, n = handle_reach(g, side)
            want = dist(tip, key_pt)
            report("NOTE %s: red reaches %.1f px (%d px drawn), control point at %.1f px" % (label, reach, n, want))
            return check(n >= 10 and -1.5 <= reach - want <= 6.0, label)

        def lands_under(cursor, label):
            got = handles()[1]
            report("NOTE %s: handle at (%.1f, %.1f), cursor at (%.1f, %.1f)" % (label, got.x(), got.y(), cursor.x(), cursor.y()))
            return check(dist(got, cursor) <= 1.5, label)

        # =====================================================================================
        if "untouched" in selected:
            gt.section("untouched")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            before = values()
            weights = [(tg[3], tg[4]) for tg in all_tangents()]
            check(all(w == (THIRD, THIRD) for w in weights), "keys whose handles were never dragged have weights of exactly 1/3 (%r)" % (weights,))
            plain = [(t, v, s, s) for t, v, s in KEYS]
            worst = max(abs(v - hermite(t, plain)) for t, v in zip(sample_times, before))
            report("NOTE largest difference from the Hermite cubic: %.3g" % worst)
            check(worst < 1e-9, "a never-dragged curve is the plain Hermite cubic")
            select_middle()
            yield ("sleep", SETTLE_MS)
            widget_shot(cw, "m96-untouched")
            click(cw, QPointF(10, 10))
            yield ("sleep", SETTLE_MS)
            check(values() == before, "selecting and deselecting a key leaves every value bit-identical")
            check(all((tg[3], tg[4]) == (THIRD, THIRD) for tg in all_tangents()),
                  "selecting a key leaves its weights at exactly 1/3")

        # =====================================================================================
        if "weighted-drag" in selected:
            gt.section("weighted-drag")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            mid_before = white.getValueAtTime(16.0, 0)
            select_middle()
            yield ("sleep", SETTLE_MS)
            _left, right = handles()
            target = key_pt + (right - key_pt) * 2.0
            drag(cw, right, target)
            yield ("sleep", SETTLE_MS)
            interp, l, r, lw, rw = tangents(11.0)
            report("NOTE after the drag: interp %r, slopes %.5f %.5f, weights %.4f %.4f" % (interp, l, r, lw, rw))
            check(interp == FREE, "the key is still Free")
            check(abs(rw - 2 * THIRD) <= 1.5 / span_r, "dragging the handle twice as far doubles its weight")
            check(abs(r - KEYS[1][2]) <= 1.5 * sx / sy / (target.x() - key_pt.x()) + 1e-6,
                  "dragging along the handle's line keeps its slope")
            lands_under(target, "the dragged handle lands under the cursor")
            check(abs(white.getValueAtTime(16.0, 0) - mid_before) > 1e-4, "the heavier handle changes the segment")
            widget_shot(cw, "m96-weighted-drag")

        # =====================================================================================
        if "unbroken" in selected:
            gt.section("unbroken")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            select_middle()
            yield ("sleep", SETTLE_MS)
            _i, _l0, _r0, lw0, rw0 = tangents(11.0)
            left, right = handles()
            check_reach(Grab(cw), -1, left, "before the drag the left handle is drawn out to its control point")
            target = key_pt + QPointF(1.5 * (right.x() - key_pt.x()), 1.5 * (right.y() - key_pt.y()) - 20.0)
            drag(cw, right, target)
            yield ("sleep", SETTLE_MS)
            interp, l, r, lw, rw = tangents(11.0)
            report("NOTE after the drag: slopes %.5f %.5f, weights %.4f %.4f (were %.4f %.4f)" % (l, r, lw, rw, lw0, rw0))
            check(interp == FREE, "a plain drag keeps the key Free")
            check(abs(l - r) < 1e-9, "the left handle takes the dragged handle's slope")
            check(abs(lw / lw0 - rw / rw0) < 1e-6 and abs(rw / rw0 - 1.0) > 0.1,
                  "the left weight scales by the same factor as the dragged one")
            lands_under(target, "the dragged handle lands under the cursor")
            left, _right = handles()
            check_reach(Grab(cw), -1, left, "after the drag the left handle is drawn out to its scaled control point")
            widget_shot(cw, "m96-unbroken")

        # =====================================================================================
        if "broken" in selected:
            gt.section("broken")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            select_middle()
            yield ("sleep", SETTLE_MS)
            _i, l0, _r0, lw0, _rw0 = tangents(11.0)
            left, right = handles()
            target = key_pt + QPointF(0.25 * span_r, -40.0)
            drag(cw, right, target, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            interp, l, r, lw, rw = tangents(11.0)
            report("NOTE after the Ctrl-drag: interp %r, slopes %.5f %.5f, weights %.4f %.4f" % (interp, l, r, lw, rw))
            check(interp == BROKEN, "a Ctrl-drag breaks the key")
            check(l == l0 and lw == lw0, "a Ctrl-drag leaves the left slope and weight exactly as they were")
            check(abs(rw - 0.25) <= 1.5 / span_r, "the dragged weight follows the cursor's time distance")
            lands_under(target, "the Ctrl-dragged handle lands under the cursor")
            check_reach(Grab(cw), -1, left, "the left handle is still drawn where it was")
            widget_shot(cw, "m96-broken")

        # =====================================================================================
        if "release" in selected:
            gt.section("release")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            select_middle()
            yield ("sleep", SETTLE_MS)
            _left, right = handles()
            target = key_pt + QPointF(0.55 * span_r, -0.3 * span_r)
            press(cw, right)
            for i in range(1, DRAG_STEPS + 1):
                move(cw, right + (target - right) * (i / float(DRAG_STEPS)), Qt.LeftButton)
            yield ("sleep", SETTLE_MS)
            held = tangents(11.0)
            held_grab = Grab(cw)
            lands_under(target, "while the button is held the handle is under the cursor")
            check_reach(held_grab, 1, target, "while the button is held the handle is drawn out to the cursor")
            widget_shot(cw, "m96-release-held")
            release(cw, target)
            yield ("sleep", SETTLE_MS)
            check(tangents(11.0) == held, "releasing the button leaves the key exactly as it was while held")
            held_reach, _n = handle_reach(held_grab, 1)
            released_reach, _n = handle_reach(Grab(cw), 1)
            check(abs(held_reach - released_reach) <= 1.5, "the drawn handle does not change length on release")
            widget_shot(cw, "m96-release")

        # =====================================================================================
        if "undo" in selected:
            gt.section("undo")
            reset_keys()
            yield ("sleep", SETTLE_MS)
            select_middle()
            yield ("sleep", SETTLE_MS)
            before = all_tangents()
            before_values = values()
            _left, right = handles()
            drag(cw, right, key_pt + QPointF(0.6 * span_r, -50.0), steps=8)
            yield ("sleep", SETTLE_MS)
            dragged = all_tangents()
            check(dragged != before, "the drag changed the key")
            trigger("undo")
            yield ("sleep", SETTLE_MS)
            check(all_tangents() == before, "one undo restores every key's tangents and weights exactly")
            check(values() == before_values, "one undo restores the curve bit-identically")
            trigger("redo")
            yield ("sleep", SETTLE_MS)
            check(all_tangents() == dragged, "redo re-applies the whole drag")
            widget_shot(cw, "m96-undo")

        # =====================================================================================
        if "weighted-draw" in selected:
            gt.section("weighted-draw")
            reset_keys()
            white.setKeyTangents(11.0, 0.05, 0.15, THIRD, 0.85, 0)
            white.setKeyTangents(21.0, 0.0, 0.0, 0.8, 0.8, 0)
            click(cw, QPointF(10, 10))
            yield ("sleep", SETTLE_MS)
            g = Grab(cw)
            bg = g.background()
            keys_now = [(t, white.getValueAtTime(t, 0), tg[1], tg[2]) for (t, _v, _s), tg in zip(KEYS, all_tangents())]

            def ink_near(x, y, radius):
                best = None
                for _x, py, c in g.pixels((x, y - radius, x, y + radius)):
                    if colour_dist(c, bg) > 200 and not is_handle_red(c):
                        d = abs(py - y)
                        best = d if best is None or d < best else best
                return best

            worst, misses, eligible, on_hermite = 0.0, 0, 0, 0
            t = 12.0
            while t <= 20.0:
                p = to_w(t, white.getValueAtTime(t, 0))
                d = ink_near(p.x(), p.y(), 10.0)
                if d is None:
                    misses += 1
                else:
                    worst = max(worst, d)
                hy = to_w(t, hermite(t, keys_now)).y()
                if abs(hy - p.y()) >= 4.0:
                    eligible += 1
                    dh = ink_near(p.x(), hy, 1.0)
                    if dh is not None:
                        on_hermite += 1
                t += 0.5
            report("NOTE drawn curve: worst %.2f px from the evaluated values, %d columns with no ink; "
                   "%d of %d columns also inked on the unweighted cubic" % (worst, misses, on_hermite, eligible))
            check(misses == 0 and worst <= 1.5, "the drawn weighted segment lies on the evaluated values")
            check(eligible > 0 and on_hermite * 4 < eligible, "the drawn segment is not the unweighted cubic")
            widget_shot(cw, "m96-weighted-draw")
            select_middle()
            yield ("sleep", SETTLE_MS)
            _left, right = handles()
            check_reach(Grab(cw), 1, right, "the heavy right handle is drawn out to its control point")
            widget_shot(cw, "m96-weighted-draw-handles")

        # =====================================================================================
        if "hidpi-pick" in selected:
            gt.section("hidpi-pick")
            report("NOTE picking at device-pixel ratio %.2f%s" % (dpr, "" if dpr > 1.0 else " (run with QT_SCALE_FACTOR=2 for the 2x case)"))
            reset_keys()
            yield ("sleep", SETTLE_MS)
            select_middle()
            yield ("sleep", SETTLE_MS)
            left, right = handles()
            g = Grab(cw)
            check_reach(g, 1, right, "a click on the key shows its right handle where it is computed")
            check_reach(g, -1, left, "a click on the key shows its left handle where it is computed")
            widget_shot(cw, "m96-hidpi-pick")
            drag(cw, key_pt, key_pt + QPointF(0, -20.0))
            yield ("sleep", SETTLE_MS)
            moved = white.getValueAtTime(11.0, 0)
            report("NOTE key value after a 20 px drag: %.5f (want %.5f)" % (moved, 0.5 + 20.0 / sy))
            check(abs(moved - (0.5 + 20.0 / sy)) <= 1.5 / sy, "dragging the key moves it by the cursor's distance")
            trigger("undo")
            yield ("sleep", SETTLE_MS)

        # =====================================================================================
        if "save-load" in selected:
            gt.section("save-load")
            reset_keys()
            white.setKeyTangents(1.0, 0.0, 0.02, THIRD, 0.7, 0)
            white.setKeyTangents(11.0, 0.03, 0.08, 0.15, 0.9, 0)
            white.setKeyTangents(21.0, -0.01, -0.01, 0.6, 0.25, 0)
            saved = all_tangents()
            saved_values = values()
            report("NOTE saved tangents %r" % (saved,))
            path = os.path.join(gt.OUT, "m96_weighted_tangents.ntpz")
            gt.require(app.saveProjectAs(path), "save the project to " + path)
            gt.require(app.resetProject(), "reset the project")
            yield ("sleep", SETTLE_MS)
            reloaded = app.loadProject(path)
            gt.require(reloaded is not None, "reopen " + path)
            host = reloaded
            ok = yield ("until", lambda: host.getNode(grade_name) is not None, LOAD_TIMEOUT_S)
            gt.require(ok, "the Grade is back after reopening")
            yield ("sleep", SETTLE_MS)
            white2 = host.getNode(grade_name).getParam("white")
            loaded = [tuple(white2.getKeyTangents(t, 0)) for t, _v, _s in KEYS]
            report("NOTE loaded tangents %r" % (loaded,))
            check(loaded == saved, "every key's interpolation, slopes and weights read back exactly")
            check([white2.getValueAtTime(t, 0) for t in sample_times] == saved_values,
                  "the reloaded curve is bit-identical")
            gt.shot("m96-save-load.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
