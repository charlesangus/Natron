"""Roto gestures and modes from the Nuke parity pass, driven through the viewer of a Roto node.

Checks, each on its own shape:
  - Shift+drag of a tangent handle turns and stretches both handles together: two handles at 90
    degrees stay at 90 degrees and grow by the same ratio. One undo restores them.
  - Ctrl+drag of a tangent handle moves only that handle.
  - Ctrl+click on a point breaks its handles: a later plain drag of one handle leaves the other.
  - E grows the feather of the selected point outwards by 10 screen pixels; one undo restores it.
  - Bezier tool: a click-drag pulls out mirrored handles; Ctrl pressed during that drag moves only
    the exit handle from then on; Shift+click on the next point makes the previous point's exit
    sharp (its exit handle collapses onto it, the entry handle stays); clicking the first point
    closes the shape and dragging on from that click pulls out mirrored handles.
  - Esc in the Bezier tool returns to the selection tool: a click on empty canvas then adds
    nothing.
  - Ctrl+drag in the Bezier tool sketches a shape: several points, all on the sketched arc, undone
    in one step.
  - Ctrl+drag of the transform box's cross hair moves the pivot: a scale from a corner then keeps
    the new pivot fixed.
  - S cycles the drawing tools (Bezier to Ellipse); Shift+drag draws a circle (equal width and
    height); Ctrl+Shift+drag draws it from the centre (the shape is centred on the press).
  - Ripple edit draws a red border just inside the viewer edges, and no border once it is off.
Saves screenshots of the sketched shape, the moved pivot and the ripple border.

Modifiers are read by the roto interact from key events, so every modified gesture sends the
Shift/Control QKeyEvent press before the mouse events and the release after them. Undo is the Edit
menu action (the Ctrl+Z shortcut is matched before a sent key event reaches a widget).

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m224-shots/gestures -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m224_gestures.py build/fast/App/Natron
"""
import math
import os
import re
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 300
RENDER_MS = 1500
DRAG_STEPS = 10
T = 1.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = {}

    # ---- windows and events ----------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def activate():
        win = main_window()
        if win is None:
            return
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def press(widget, pos, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)

    def release(widget, pos, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, modifiers=Qt.NoModifier):
        press(widget, pos, modifiers)
        release(widget, pos, modifiers)

    def drag_path(widget, points, modifiers=Qt.NoModifier):
        press(widget, points[0], modifiers)
        for p in points[1:]:
            move(widget, p, Qt.LeftButton, modifiers)
        release(widget, points[-1], modifiers)

    def segment(start, end, steps=DRAG_STEPS):
        start = QPointF(start)
        end = QPointF(end)
        return [start + (end - start) * (i / float(steps)) for i in range(steps + 1)]

    def drag(widget, start, end, modifiers=Qt.NoModifier):
        drag_path(widget, segment(start, end), modifiers)

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    MOD_KEYS = ((Qt.ShiftModifier, Qt.Key_Shift), (Qt.ControlModifier, Qt.Key_Control))

    def hold(widget, modifiers):
        held = Qt.NoModifier
        for mod, key in MOD_KEYS:
            if modifiers & mod:
                held = held | mod
                QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, held))
        QApplication.processEvents()

    def unhold(widget, modifiers):
        held = modifiers
        for mod, key in reversed(MOD_KEYS):
            if modifiers & mod:
                held = held & ~mod
                QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, held))
        QApplication.processEvents()

    def with_modifiers(widget, modifiers, gesture):
        hold(widget, modifiers)
        try:
            gesture(modifiers)
        finally:
            unhold(widget, modifiers)

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

    # ---- viewer calibration ----------------------------------------------------------------

    def info_bar():
        bars = [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]
        return bars[0] if bars else None

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_mouse():
        bar = info_bar()
        if bar is None:
            return None
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                    m = re.match(r"x=(-?\d+) y=(-?\d+)", strip_tags(w.text()))
                    if m:
                        return (int(m.group(1)), int(m.group(2)))
            except RuntimeError:
                continue
        return None

    class ViewerMap(object):
        """Widget pixels to canonical coordinates, calibrated from the info bar's whole-pixel mouse
        readout at two positions per axis where it steps, found by bisection."""

        def __init__(self, gl):
            self.gl = gl
            w, h = float(gl.width()), float(gl.height())
            self.ok = True
            (x1, cx1), (x2, cx2) = self._edge(0, w * 0.1, w * 0.5, h / 2.0), self._edge(0, w * 0.6, w * 0.9, h / 2.0)
            (y1, cy1), (y2, cy2) = self._edge(1, h * 0.1, h * 0.5, w / 2.0), self._edge(1, h * 0.6, h * 0.9, w / 2.0)
            if None in (x1, x2, y1, y2) or x1 == x2 or y1 == y2:
                self.ok = False
                return
            self.sx = (cx2 - cx1) / (x2 - x1)
            self.sy = (cy2 - cy1) / (y2 - y1)
            self.p0 = QPointF(x1, y1)
            self.c0 = (cx1, cy1)

        def _coord(self, axis, pos, other):
            p = QPointF(pos, other) if axis == 0 else QPointF(other, pos)
            move(self.gl, p)
            c = info_mouse()
            return None if c is None else c[axis]

        def _edge(self, axis, lo, hi, other):
            c_lo = self._coord(axis, lo, other)
            c_hi = self._coord(axis, hi, other)
            if c_lo is None or c_hi is None or c_lo == c_hi:
                return (None, None)
            for _ in range(30):
                if hi - lo < 0.05:
                    break
                mid = (lo + hi) / 2.0
                if self._coord(axis, mid, other) == c_lo:
                    lo = mid
                else:
                    hi = mid
            stepped = self._coord(axis, hi, other)
            return (hi, max(c_lo, stepped))

        def to_canonical(self, p):
            return (self.c0[0] + (p.x() - self.p0.x()) * self.sx, self.c0[1] + (p.y() - self.p0.y()) * self.sy)

        def to_widget(self, x, y):
            return QPointF(self.p0.x() + (x - self.c0[0]) / self.sx, self.p0.y() + (y - self.c0[1]) / self.sy)

        def pixel_scale(self):
            return abs(self.sx)

    class Grab(object):
        """The screen under widget, in widget coordinates: a widget grab of the GL viewer can come
        out blank under llvmpipe."""

        def __init__(self, widget):
            QApplication.processEvents()
            origin = widget.mapToGlobal(QPoint(0, 0))
            self.img = QGuiApplication.primaryScreen().grabWindow(
                0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
            self.s = self.img.width() / float(max(1, widget.width()))

        def colour(self, x, y):
            xd, yd = int(round(x * self.s)), int(round(y * self.s))
            if 0 <= xd < self.img.width() and 0 <= yd < self.img.height():
                c = self.img.pixelColor(xd, yd)
                return (c.red(), c.green(), c.blue())
            return None

    def is_red(c):
        return c is not None and c[0] >= 150 and c[1] <= 90 and c[2] <= 90

    # ---- shape helpers ---------------------------------------------------------------------

    def shapes(ctx):
        return [it for it in ctx.getBaseLayer().getChildren() if isinstance(it, NatronEngine.BezierCurve)]

    def clear_shapes(ctx):
        layer = ctx.getBaseLayer()
        for item in list(layer.getChildren()):
            layer.removeItem(item)
        QApplication.processEvents()

    def point(bez, i):
        return bez.getControlPointPosition(i, T)

    def feather(bez, i):
        return bez.getFeatherPointPosition(i, T)

    def left_off(p):
        return (p[2] - p[0], p[3] - p[1])

    def right_off(p):
        return (p[4] - p[0], p[5] - p[1])

    def length(v):
        return math.hypot(v[0], v[1])

    def angle_between(a, b):
        la, lb = length(a), length(b)
        if la == 0 or lb == 0:
            return None
        c = (a[0] * b[0] + a[1] * b[1]) / (la * lb)
        return math.degrees(math.acos(max(-1.0, min(1.0, c))))

    def close(a, b, tol):
        return all(abs(a[i] - b[i]) <= tol for i in range(len(a)))

    def star(ctx, vmap, gl, handle_fn=None):
        """A closed five-point star-ordered pentagon around the viewer centre, with tangents along
        the outline, or as handle_fn(i, x, y, tx, ty, tl) returns them."""
        clear_shapes(ctx)
        cx, cy = vmap.to_canonical(QPointF(gl.width() / 2.0, gl.height() / 2.0))
        r = 110.0 * vmap.pixel_scale()
        angles = [math.radians(90 - 72 * i) for i in range(5)]
        pts = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a in angles]
        bez = ctx.createBezier(pts[0][0], pts[0][1], T)
        require(bez is not None, "created a Bezier")
        for p in pts[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        tl = 0.3 * r
        for i, a in enumerate(angles):
            tx, ty = math.sin(a), -math.cos(a)
            x, y = pts[i]
            if handle_fn is not None:
                lx, ly, rx, ry = handle_fn(i, x, y, tx, ty, tl)
            else:
                lx, ly, rx, ry = x - tx * tl, y - ty * tl, x + tx * tl, y + ty * tl
            bez.setPointAtIndex(i, T, x, y, lx, ly, rx, ry)
            bez.setFeatherPointAtIndex(i, T, x, y, lx, ly, rx, ry)
        _keep["bez"] = bez
        return bez, pts, angles, (cx, cy)

    def select_tool(gl):
        send_key(gl, Qt.Key_Escape)
        send_key(gl, Qt.Key_Q)

    # ---- sections ----------------------------------------------------------------------------

    def tangent_sections(gl, ctx, vmap):
        ps = vmap.pixel_scale()

        gt.section("shift-tangent-together")
        select_tool(gl)

        def right_angle(i, x, y, tx, ty, tl):
            # Entry handle along the outline, exit handle turned 90 degrees outwards.
            return (x - tx * tl, y - ty * tl, x + ty * tl, y - tx * tl)

        bez, pts, _angles, _c = star(ctx, vmap, gl, right_angle)
        yield ("sleep", RENDER_MS)
        click(gl, vmap.to_widget(pts[0][0], pts[0][1]))
        yield ("sleep", SETTLE_MS)
        before = point(bez, 0)
        a0 = angle_between(left_off(before), right_off(before))
        handle = vmap.to_widget(before[4], before[5])
        with_modifiers(gl, Qt.ShiftModifier, lambda m: drag(gl, handle, handle + QPointF(25, -35), m))
        yield ("sleep", SETTLE_MS)
        after = point(bez, 0)
        a1 = angle_between(left_off(after), right_off(after))
        report("NOTE shift drag: handle angle %.2f -> %s" % (a0, a1))
        check(a1 is not None and abs(a1 - a0) < 1.0, "Shift+drag keeps the angle between the handles")
        check(length(left_off(after)) > 0 and length(right_off(before)) > 0
              and abs(length(left_off(after)) / length(left_off(before))
                      - length(right_off(after)) / length(right_off(before))) < 0.02,
              "Shift+drag stretches both handles by the same ratio")
        check(not close(left_off(after), left_off(before), 1e-4), "Shift+drag moved the other handle too")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(close(point(bez, 0), before, 1e-6), "one undo restores both handles")

        gt.section("ctrl-tangent-break")
        click(gl, vmap.to_widget(pts[1][0], pts[1][1]))
        yield ("sleep", SETTLE_MS)
        before = point(bez, 1)
        handle = vmap.to_widget(before[4], before[5])
        with_modifiers(gl, Qt.ControlModifier, lambda m: drag(gl, handle, handle + QPointF(0, 30), m))
        yield ("sleep", SETTLE_MS)
        after = point(bez, 1)
        check(close(left_off(after), left_off(before), 1e-6) and not close(right_off(after), right_off(before), 1e-4),
              "Ctrl+drag moves only the dragged handle")

        gt.section("ctrl-click-break")
        bez, pts, _angles, _c = star(ctx, vmap, gl)
        yield ("sleep", RENDER_MS)
        # Unbroken first: a plain drag of one handle turns the other with it.
        click(gl, vmap.to_widget(pts[3][0], pts[3][1]))
        yield ("sleep", SETTLE_MS)
        before = point(bez, 3)
        handle = vmap.to_widget(before[4], before[5])
        drag(gl, handle, handle + QPointF(0, 30))
        yield ("sleep", SETTLE_MS)
        check(not close(left_off(point(bez, 3)), left_off(before), 1e-4), "a plain handle drag on a smooth point turns both handles")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        target = vmap.to_widget(pts[2][0], pts[2][1])
        with_modifiers(gl, Qt.ControlModifier, lambda m: click(gl, target, m))
        yield ("sleep", SETTLE_MS)
        before = point(bez, 2)
        check(close(before[:2], pts[2], 1e-6), "Ctrl+click does not move the point")
        handle = vmap.to_widget(before[4], before[5])
        drag(gl, handle, handle + QPointF(0, 30))
        yield ("sleep", SETTLE_MS)
        after = point(bez, 2)
        check(close(left_off(after), left_off(before), 1e-6) and not close(right_off(after), right_off(before), 1e-4),
              "after Ctrl+click a plain handle drag leaves the other handle")

        gt.section("increase-feather")
        bez, pts, angles, _c = star(ctx, vmap, gl)
        yield ("sleep", RENDER_MS)
        click(gl, vmap.to_widget(pts[0][0], pts[0][1]))
        yield ("sleep", SETTLE_MS)
        send_key(gl, Qt.Key_E)
        yield ("sleep", SETTLE_MS)
        c, f = point(bez, 0), feather(bez, 0)
        out = (f[0] - c[0]) * math.cos(angles[0]) + (f[1] - c[1]) * math.sin(angles[0])
        report("NOTE E moved the feather %.3f outwards (10 px = %.3f)" % (out, 10 * ps))
        check(abs(out - 10 * ps) < 0.15 * 10 * ps, "E grows the selected feather outwards by 10 screen pixels")
        f1 = feather(bez, 1)
        check(close(f1[:2], pts[1], 1e-6), "E leaves unselected feathers alone")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(close(feather(bez, 0)[:2], pts[0], 1e-6), "one undo restores the feather")

    def drawing_sections(gl, ctx, vmap):
        ps = vmap.pixel_scale()
        w, h = float(gl.width()), float(gl.height())

        gt.section("draw-bezier-modifiers")
        clear_shapes(ctx)
        select_tool(gl)
        send_key(gl, Qt.Key_V)
        yield ("sleep", SETTLE_MS)
        p0 = QPointF(w * 0.35, h * 0.35)
        p1 = QPointF(w * 0.55, h * 0.30)
        p2 = QPointF(w * 0.65, h * 0.55)
        p3 = QPointF(w * 0.40, h * 0.65)
        click(gl, p0)
        drag(gl, p1, p1 + QPointF(30, 0))
        yield ("sleep", SETTLE_MS)
        made = shapes(ctx)
        require(len(made) == 1, "V then two clicks start one Bezier (%d shapes)" % len(made))
        bez = made[0]
        _keep["drawn"] = bez
        b1 = point(bez, 1)
        check(close(right_off(b1), (-left_off(b1)[0], -left_off(b1)[1]), 1e-6) and length(right_off(b1)) > 20 * ps,
              "a click-drag pulls out mirrored handles")
        with_modifiers(gl, Qt.ShiftModifier, lambda m: click(gl, p2, m))
        yield ("sleep", SETTLE_MS)
        s1 = point(bez, 1)
        check(close(right_off(s1), (0.0, 0.0), 1e-6), "Shift+click collapses the previous point's exit handle")
        check(close(left_off(s1), left_off(b1), 1e-6), "Shift+click keeps the previous point's entry handle")

        # Ctrl pressed partway through the drag: the press itself must not be a Ctrl press.
        press(gl, p3)
        for p in segment(p3, p3 + QPointF(25, 0))[1:]:
            move(gl, p, Qt.LeftButton)
        mid = point(bez, 3)
        hold(gl, Qt.ControlModifier)
        for p in segment(p3 + QPointF(25, 0), p3 + QPointF(25, -35))[1:]:
            move(gl, p, Qt.LeftButton, Qt.ControlModifier)
        release(gl, p3 + QPointF(25, -35), Qt.ControlModifier)
        unhold(gl, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        b3 = point(bez, 3)
        check(close(left_off(b3), left_off(mid), 1e-6) and not close(right_off(b3), right_off(mid), 1e-4),
              "Ctrl during the draw drag moves only the exit handle")

        # Close on the first point and keep dragging.
        drag(gl, p0, p0 + QPointF(0, -40))
        yield ("sleep", SETTLE_MS)
        check(bez.isCurveFinished(), "clicking the first point closes the shape")
        c0 = point(bez, 0)
        check(length(right_off(c0)) > 30 * ps and close(left_off(c0), (-right_off(c0)[0], -right_off(c0)[1]), 1e-6),
              "dragging on from the closing click pulls out mirrored handles on the first point")
        check(len(shapes(ctx)) == 1, "closing adds no shape")

        gt.section("escape-returns-to-select")
        send_key(gl, Qt.Key_V)
        yield ("sleep", SETTLE_MS)
        send_key(gl, Qt.Key_Escape)
        yield ("sleep", SETTLE_MS)
        click(gl, QPointF(w * 0.85, h * 0.85))
        yield ("sleep", SETTLE_MS)
        check(len(shapes(ctx)) == 1, "after Esc a click on empty canvas adds no point or shape")

        gt.section("ctrl-sketch")
        clear_shapes(ctx)
        select_tool(gl)
        send_key(gl, Qt.Key_V)
        yield ("sleep", SETTLE_MS)
        centre = QPointF(w * 0.5, h * 0.5)
        rad = min(w, h) * 0.25
        arc = [centre + QPointF(rad * math.cos(math.radians(a)), rad * math.sin(math.radians(a))) for a in range(200, 341, 4)]
        with_modifiers(gl, Qt.ControlModifier, lambda m: drag_path(gl, arc, m))
        yield ("sleep", SETTLE_MS)
        made = shapes(ctx)
        require(len(made) == 1, "Ctrl+drag in the Bezier tool sketches one shape (%d shapes)" % len(made))
        sk = made[0]
        _keep["sketch"] = sk
        n = sk.getNumControlPoints()
        report("NOTE sketch made %d points from %d samples" % (n, len(arc)))
        check(3 <= n < len(arc), "the sketch has several points, fewer than the samples")
        ccx, ccy = vmap.to_canonical(centre)
        crad = rad * ps
        off = [abs(math.hypot(point(sk, i)[0] - ccx, point(sk, i)[1] - ccy) - crad) for i in range(n)]
        check(max(off) < 4 * ps, "every sketched point lies on the arc (worst %.2f px)" % (max(off) / ps))
        gt.shot("m224_gestures_sketch.png")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(len(shapes(ctx)) == 0, "one undo removes the whole sketch")
        send_key(gl, Qt.Key_Escape)

        gt.section("ellipse-modifiers")
        clear_shapes(ctx)
        select_tool(gl)
        send_key(gl, Qt.Key_V)
        send_key(gl, Qt.Key_S)
        yield ("sleep", SETTLE_MS)
        a = QPointF(w * 0.3, h * 0.3)
        with_modifiers(gl, Qt.ShiftModifier, lambda m: drag(gl, a, a + QPointF(120, 50), m))
        yield ("sleep", SETTLE_MS)
        made = shapes(ctx)
        require(len(made) == 1 and made[0].getNumControlPoints() == 4,
                "V then S selects the ellipse tool, and a drag draws a four-point ellipse")
        xs = [point(made[0], i)[0] for i in range(4)]
        ys = [point(made[0], i)[1] for i in range(4)]
        wd, ht = max(xs) - min(xs), max(ys) - min(ys)
        check(abs(wd - ht) < 1.5 * ps, "Shift+drag draws a circle (%.2f x %.2f)" % (wd, ht))
        clear_shapes(ctx)
        send_key(gl, Qt.Key_V)
        yield ("sleep", SETTLE_MS)
        b = QPointF(w * 0.5, h * 0.5)
        with_modifiers(gl, Qt.ShiftModifier | Qt.ControlModifier, lambda m: drag(gl, b, b + QPointF(60, 60), m))
        yield ("sleep", SETTLE_MS)
        made = shapes(ctx)
        require(len(made) == 1 and made[0].getNumControlPoints() == 4, "V returns to the ellipse tool")
        xs = [point(made[0], i)[0] for i in range(4)]
        ys = [point(made[0], i)[1] for i in range(4)]
        bx, by = vmap.to_canonical(b)
        check(abs((min(xs) + max(xs)) / 2 - bx) < 1.5 * ps and abs((min(ys) + max(ys)) / 2 - by) < 1.5 * ps,
              "Ctrl+Shift+drag draws the ellipse from its centre")
        check(abs((max(xs) - min(xs)) - (max(ys) - min(ys))) < 1.5 * ps, "Ctrl+Shift+drag keeps the circle")

    def pivot_section(gl, ctx, vmap):
        ps = vmap.pixel_scale()
        gt.section("ctrl-drag-pivot")
        clear_shapes(ctx)
        select_tool(gl)
        cx, cy = vmap.to_canonical(QPointF(gl.width() / 2.0, gl.height() / 2.0))
        r = 80.0 * ps
        corners = [(cx - r, cy + r), (cx + r, cy + r), (cx + r, cy - r), (cx - r, cy - r)]
        bez = ctx.createBezier(corners[0][0], corners[0][1], T)
        require(bez is not None, "created a square Bezier")
        for p in corners[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        # Zero-length handles keep the transform box on the corners.
        for i, (x, y) in enumerate(corners):
            bez.setPointAtIndex(i, T, x, y, x, y, x, y)
            bez.setFeatherPointAtIndex(i, T, x, y, x, y, x, y)
        _keep["square"] = bez
        yield ("sleep", RENDER_MS)
        click(gl, vmap.to_widget(*corners[0]))
        hold(gl, Qt.ShiftModifier)
        for p in corners[1:]:
            click(gl, vmap.to_widget(*p), Qt.ShiftModifier)
        unhold(gl, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)

        start = vmap.to_widget(cx, cy)
        end = start + QPointF(60, 0)
        with_modifiers(gl, Qt.ControlModifier, lambda m: drag(gl, start, end, m))
        yield ("sleep", SETTLE_MS)
        check(all(close(point(bez, i)[:2], corners[i], 1e-6) for i in range(4)), "Ctrl+drag of the cross hair moves no point")
        gt.shot("m224_gestures_pivot.png")
        px, py = vmap.to_canonical(end)

        # The top-left scale arrow sits up-left of the corner, outside the point's pick radius.
        arrow = vmap.to_widget(corners[0][0], corners[0][1]) + QPointF(-11, -11)
        drag(gl, arrow, arrow + QPointF(-30, -30))
        yield ("sleep", SETTLE_MS)
        after = [point(bez, i)[:2] for i in range(4)]
        k = (after[1][0] - after[0][0]) / (corners[1][0] - corners[0][0])
        report("NOTE pivot scale k=%.4f" % k)
        check(abs(k - 1.0) > 0.02, "the corner drag scaled the square")
        if abs(k - 1.0) > 0.02:
            fx = (after[0][0] - k * corners[0][0]) / (1 - k)
            fy = (after[0][1] - k * corners[0][1]) / (1 - k)
            report("NOTE fixed point (%.2f, %.2f), pivot (%.2f, %.2f), box centre (%.2f, %.2f)" % (fx, fy, px, py, cx, cy))
            check(abs(fx - px) < 2 * ps and abs(fy - py) < 2 * ps, "the scale keeps the moved pivot fixed")

    def ripple_section(gl, roto):
        gt.section("ripple-border")

        def ripple_button():
            for wdg in QApplication.allWidgets():
                try:
                    if isinstance(wdg, QAbstractButton) and wdg.isVisible() and wdg.isCheckable() \
                            and "ripple" in wdg.toolTip().lower():
                        return wdg
                except RuntimeError:
                    continue
            return None

        def red_edges():
            g = Grab(gl)
            w, h = gl.width(), gl.height()
            samples = [(w * f, 1) for f in (0.25, 0.5, 0.75)] + [(w * f, h - 2) for f in (0.25, 0.5, 0.75)]
            samples += [(1, h * f) for f in (0.25, 0.5, 0.75)] + [(w - 2, h * f) for f in (0.25, 0.5, 0.75)]
            return sum(1 for (x, y) in samples if is_red(g.colour(x, y))), len(samples)

        button = ripple_button()
        require(button is not None, "the viewer toolbar has the ripple edit button")
        _keep["ripple"] = button
        off_hits, n = red_edges()
        check(off_hits == 0, "no red border while ripple edit is off (%d/%d)" % (off_hits, n))
        button.click()
        yield ("sleep", SETTLE_MS)
        check(button.isChecked(), "the ripple edit button is on")
        on_hits, n = red_edges()
        check(on_hits == n, "ripple edit draws a red border on every viewer edge (%d/%d)" % (on_hits, n))
        gt.shot("m224_gestures_ripple_border.png")
        button.click()
        yield ("sleep", SETTLE_MS)
        off_hits, n = red_edges()
        check(off_hits == 0, "the red border goes when ripple edit is turned off (%d/%d)" % (off_hits, n))

    def untone_view():
        """The viewer defaults to a tone-mapped view; absolute pixel checks need the plain one."""
        for w in gt.widgets_of("ComboBox"):
            try:
                if not (w.isVisible() and "View:" in (w.toolTip() or "")):
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
            except RuntimeError:
                continue
            for name in ("Un-tone-mapped", "Raw"):
                if name in items:
                    QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(name)))
                    QApplication.processEvents()
                    return True
        return False

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        activate()

        # An open settings panel makes the node's viewer interact the active one.
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        roto = app.createNode("fr.inria.built-in.Roto", -1, None, props)
        require(roto is not None, "create Roto", gt.plugin_problem("fr.inria.built-in.Roto"))
        ctx = roto.getRotoContext()
        require(ctx is not None, "the Roto node has a roto context")
        _keep["roto"] = roto
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, roto), "the Viewer shows the Roto")
        QApplication.processEvents()
        yield ("sleep", RENDER_MS)

        gl = viewer_gl()
        require(gl is not None, "the viewer is visible")
        _keep["gl"] = gl
        activate()
        require(untone_view(), "picked the Un-tone-mapped view")
        yield ("sleep", RENDER_MS)
        vmap = ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")
        report("NOTE pixel scale %.4f" % vmap.pixel_scale())

        for cmd in tangent_sections(gl, ctx, vmap):
            yield cmd
        for cmd in drawing_sections(gl, ctx, vmap):
            yield cmd
        for cmd in pivot_section(gl, ctx, vmap):
            yield cmd
        for cmd in ripple_section(gl, roto):
            yield cmd
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
