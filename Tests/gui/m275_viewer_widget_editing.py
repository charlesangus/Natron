"""GUI check that dragging a native Viewer widget edits its node's knobs the way the settings panel
does: at a frame where the knob is animated the drag sets a key there, the value stays where it was
dropped, the whole drag is one entry in the node's undo history, and the keys survive a save and
reopen.

A black Constant feeds a Transform that the Viewer shows. Translate, rotate and scale are keyed at
frame 1, then the Transform's widget is dragged at frames 20, 30 and 40: its centre (move, twice),
its right handle (scale) and its rotation bar (rotate, with Interactive Update off so the values are
written on release). A Position node's point is dragged the same way. After each drag the script
checks the new value against the drag distance, that only the dimensions the drag changed gained a
key, that the panel's spin boxes show the key, and that the panel's undo/redo buttons and
Ctrl+Z/Ctrl+Shift+Z each step over the drag as one entry. Every drag is then replayed by typing the
same values into the spin boxes of an identically keyed second node, and the two nodes' keys
(times, values, interpolation and slopes) must match.

A last Position drag closes the node's panel before the button is released, so the widget never
sees its release. An edit typed into another node afterwards must be an undo step of its own: one
Ctrl+Z undoes only that edit, and a second one undoes the drag.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    NATRON_GUI_TEST_OUT=build/m275-shots GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m275_viewer_widget_editing.py build/fast/App/Natron
Screenshots: m275-<widget>-f<frame>.png after each drag and m275-reopened-f<frame>.png after the
reopen, each with the mouse parked away from the widget so its handles draw in the overlay colour.

There is no Python API for the viewer's zoom, so the canonical-to-widget mapping is measured by
moving the mouse to two points and reading the info bar's "x= y=" pixel coordinates.
"""
import math
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 30.0
POLL_TIMEOUT_S = 5.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 1200
EPS = 1e-9

TRANSFORM = "Transform"
POSITION = "Position"
CONSTANT = "Constant"

# Distinct per-dimension values keep 2D knobs unfolded in the panel, so each dimension has a box.
TRANSFORM_KEYS = {"translate": (3.0, 10.0), "rotate": (7.0,), "scale": (1.5, 1.25)}
POSITION_KEYS = {"translate": (200.0, 150.0)}
TRANSFORM_CIRCLE_BASE_PX = 30.0
ROTATION_BAR_GRAB_PX = 18.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QCursor, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- widgets ---------------------------------------------------------------------------

    def visible_of(name):
        return [w for w in gt.widgets_of(name) if w.isVisible()]

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def viewer_gl():
        found = visible_of("ViewerGL")
        return found[0] if found else None

    def node_graph_view():
        views = visible_of("NodeGraph")
        return views[0] if views else None

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def move(widget, pos, buttons=Qt.NoButton):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons)

    def park(gl):
        move(gl, QPointF(8, 8))

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def shortcut(widget, redo):
        """Ctrl+Z (or Ctrl+Shift+Z) through QTest, which sends the shortcut-override step that
        QApplication.sendEvent() skips, so the main window's undo shortcut is what runs."""
        QCursor.setPos(widget.mapToGlobal(widget.rect().center()))
        QApplication.processEvents()
        mods = Qt.ControlModifier | (Qt.ShiftModifier if redo else Qt.NoModifier)
        if QTest is None:
            report("NOTE PySide6.QtTest is unavailable: skipping the Ctrl+Z shortcut")
            return False
        QTest.keyClick(widget, Qt.Key_Z, mods)
        QApplication.processEvents()
        return True

    # ---- panels ----------------------------------------------------------------------------

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def open_only_panel(node):
        """Closes every settings panel and opens node's: the Viewer draws, and sends pen events
        to, the widget of every node whose panel is open."""
        gt.close_all_panels()
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())
        yield ("sleep", SETTLE_MS)

    def knob_boxes(panel, name):
        """The visible spin boxes of knob name on panel, left to right, found by the knob's
        script name in their tooltip."""
        tag = "<b>%s</b>" % name
        boxes = []
        for w in panel.findChildren(QLineEdit):
            try:
                if gt.class_name(w) == "SpinBox" and w.isVisible() and tag in w.toolTip():
                    boxes.append(w)
            except RuntimeError:
                continue
        boxes.sort(key=lambda w: w.mapTo(panel, QPoint(0, 0)).x())
        return boxes

    def box_shows_key(node, name, dim):
        panel = panel_of(node)
        if panel is None:
            return False
        boxes = knob_boxes(panel, name)
        return dim < len(boxes) and boxes[dim].property("animation") == 2

    def type_into(node, name, dim, value):
        panel = panel_of(node)
        if not check(panel is not None, "found the settings panel of " + node.getLabel()):
            return False
        boxes = knob_boxes(panel, name)
        if not check(dim < len(boxes), "%s's panel has a %s box for dimension %d (found %d)"
                     % (node.getLabel(), name, dim, len(boxes))):
            return False
        box = boxes[dim]
        box.setFocus()
        box.setText(repr(value))
        send_key(box, Qt.Key_Return)
        return True

    def click_panel_button(node, what):
        """Clicks the node's panel "<what> the last change" button, holding the panel wrapper
        across the click. Returns whether it was there and enabled."""
        panel = panel_of(node)
        if panel is None:
            return False
        for b in panel.findChildren(QAbstractButton):
            if ("%s the last change" % what) in b.toolTip():
                if not b.isEnabled():
                    return False
                b.click()
                QApplication.processEvents()
                return True
        return False

    # ---- knob state ------------------------------------------------------------------------

    def knob_keys(param, dims):
        out = []
        for d in range(dims):
            keys = []
            for i in range(param.getNumKeys(d)):
                found, t = param.getKeyTime(i, d)
                if found:
                    keys.append((t, param.getValueAtTime(t, d)) + tuple(param.getKeyTangents(t, d)[:3]))
            out.append(tuple(keys))
        return tuple(out)

    def node_keys(node, knobs):
        return {name: knob_keys(node.getParam(name), len(knobs[name])) for name in knobs}

    def values_at(node, knobs, frame):
        return {name: tuple(node.getParam(name).getValueAtTime(frame, d) for d in range(len(knobs[name])))
                for name in knobs}

    def close(a, b, tol=EPS):
        if isinstance(a, (tuple, list, dict)):
            if isinstance(a, dict):
                return isinstance(b, dict) and a.keys() == b.keys() and all(close(a[k], b[k], tol) for k in a)
            return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
        if isinstance(a, float) or isinstance(b, float):
            return abs(a - b) <= tol
        return a == b

    def key_frames(keys_of_dim):
        return [k[0] for k in keys_of_dim]

    def seed_keys(node, knobs):
        for name, values in knobs.items():
            param = node.getParam(name)
            for d, v in enumerate(values):
                param.setValueAtTime(v, 1, d)

    # ---- viewer geometry -------------------------------------------------------------------

    def info_mouse():
        bar = visible_of("InfoViewerWidget")
        if not bar:
            return None
        bar = bar[0]
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) != "Label" or ancestor_of_class(w, "InfoViewerWidget") is not bar:
                    continue
                m = re.search(r"x=(-?\d+)\s+y=(-?\d+)", re.sub(r"<[^>]*>", " ", w.text()))
                if m:
                    return int(m.group(1)), int(m.group(2))
            except RuntimeError:
                continue
        return None

    def calibrate(gl):
        """{sx, sy, ox, oy}: canonical x = ox + sx * widget x, canonical y = oy - sy * widget y.
        The info bar floors to whole pixels, so its readings are taken at pixel centres."""
        a = QPointF(gl.width() * 0.2, gl.height() * 0.2)
        b = QPointF(gl.width() * 0.8, gl.height() * 0.8)
        move(gl, a)
        ca = info_mouse()
        move(gl, b)
        cb = info_mouse()
        park(gl)
        require(ca is not None and cb is not None, "the info bar shows the mouse position over the viewer")
        sx = (cb[0] - ca[0]) / (b.x() - a.x())
        sy = (ca[1] - cb[1]) / (b.y() - a.y())
        require(sx > 0 and sy > 0, "the viewer maps the mouse to image pixels (scale %r, %r)" % (sx, sy))
        check(abs(sx - sy) <= 0.05 * sx, "the viewer zoom is the same on both axes (%g, %g)" % (sx, sy))
        m = {"sx": sx, "sy": sy, "ox": ca[0] + 0.5 - sx * a.x(), "oy": ca[1] + 0.5 + sy * a.y()}
        report("NOTE viewer mapping %r" % (m,))
        return m

    def to_widget(m, x, y):
        return QPointF((x - m["ox"]) / m["sx"], (m["oy"] - y) / m["sy"])

    def on_screen(gl, p, margin):
        return margin <= p.x() <= gl.width() - margin and margin <= p.y() <= gl.height() - margin

    def offset(p, angle, dist):
        """p moved dist screen pixels along a canonical angle (the widget's y axis points down)."""
        return QPointF(p.x() + dist * math.cos(angle), p.y() - dist * math.sin(angle))

    def transform_geometry(node, frame, m):
        p = node.getParam
        cx = p("center").getValueAtTime(frame, 0) + p("translate").getValueAtTime(frame, 0)
        cy = p("center").getValueAtTime(frame, 1) + p("translate").getValueAtTime(frame, 1)
        rx = TRANSFORM_CIRCLE_BASE_PX * p("scale").getValueAtTime(frame, 0)
        ry = TRANSFORM_CIRCLE_BASE_PX * p("scale").getValueAtTime(frame, 1)
        # Outside 15..300 px the widget clamps its radii, which this script does not model.
        require(15.0 <= rx <= 300.0 and 15.0 <= ry <= 300.0, "the Transform circle is unclamped (%g, %g px)" % (rx, ry))
        return to_widget(m, cx, cy), math.radians(p("rotate").getValueAtTime(frame, 0)), rx

    def overlay_pixels_near(gl, p, half=5):
        """How many pixels around widget point p are lit well above the black image and the
        grey viewer background, i.e. drawn by the widget."""
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        n = 0
        for y in range(int(p.y()) - half, int(p.y()) + half + 1):
            for x in range(int(p.x()) - half, int(p.x()) + half + 1):
                if 0 <= x < img.width() and 0 <= y < img.height():
                    c = img.pixelColor(x, y)
                    if max(c.red(), c.green(), c.blue()) > 140:
                        n += 1
        return n

    # ---- the drag --------------------------------------------------------------------------

    def drag_widget(node, knobs, label, frame, start, end, interactive, expect):
        """Drags node's Viewer widget from start to end at frame and checks the edit; expect maps
        a knob name to the (dimension, value, tolerance) triples the drag must produce. Returns
        the edits as (frame, knob, dimension, value) for replaying into a reference node."""
        gt.section(label)
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        require(gl is not None, "the viewer is visible")
        before_keys = node_keys(node, knobs)
        before = values_at(node, knobs, frame)
        require(on_screen(gl, start, 10) and on_screen(gl, end, 10),
                "%s: the drag stays inside the viewer (%r -> %r in %dx%d)" % (label, start, end, gl.width(), gl.height()))

        move(gl, start)
        send_mouse(gl, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        steps = 8
        for i in range(1, steps + 1):
            move(gl, start + (end - start) * (i / float(steps)), Qt.LeftButton)
        during = values_at(node, knobs, frame)
        send_mouse(gl, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        park(gl)
        yield ("sleep", SETTLE_MS)
        after = values_at(node, knobs, frame)
        after_keys = node_keys(node, knobs)
        report("NOTE %s: before %r during %r after %r" % (label, before, during, after))

        for name, triples in expect.items():
            for dim, want, tol in triples:
                got = after[name][dim]
                check(abs(got - want) <= tol, "%s: %s[%d] is %g after the drag (want %g +- %g)" % (label, name, dim, got, want, tol))
        if interactive:
            check(close(after, during), "%s: releasing keeps the values the drag showed (no snap back)" % label)
        edits = []
        for name in knobs:
            for dim in range(len(knobs[name])):
                changed = after[name][dim] != before[name][dim]
                frames = key_frames(after_keys[name][dim])
                if changed:
                    edits.append((frame, name, dim, after[name][dim]))
                    check(float(frame) in frames, "%s: %s[%d] has a key at frame %d (keys at %r)" % (label, name, dim, frame, frames))
                    check(len(frames) - len(before_keys[name][dim]) in (0, 1),
                          "%s: the drag added at most one %s[%d] key" % (label, name, dim))
                else:
                    check(after_keys[name][dim] == before_keys[name][dim],
                          "%s: %s[%d], which the drag left alone, keeps its keys" % (label, name, dim))
        check(len(edits) > 0, "%s: the drag edited the node" % label)

        viewer.seek(frame + 1)
        yield ("sleep", SETTLE_MS)
        viewer.seek(frame)
        yield ("sleep", SETTLE_MS)
        check(close(values_at(node, knobs, frame), after), "%s: leaving frame %d and coming back keeps the dragged values" % (label, frame))
        for _f, name, dim, _v in edits:
            ok = yield ("until", lambda n=name, d=dim: box_shows_key(node, n, d), POLL_TIMEOUT_S)
            check(ok, "%s: the panel's %s[%d] box shows a key at frame %d" % (label, name, dim, frame))

        def keys_are(want_keys, want_values):
            return lambda: close(node_keys(node, knobs), want_keys) and close(values_at(node, knobs, frame), want_values)

        check(click_panel_button(node, "Undo"), "%s: the node's panel offers an undo after the drag" % label)
        ok = yield ("until", keys_are(before_keys, before), POLL_TIMEOUT_S)
        check(ok, "%s: one click on the panel's undo restores the keys and values from before the drag" % label)
        check(click_panel_button(node, "Redo"), "%s: the node's panel offers a redo" % label)
        ok = yield ("until", keys_are(after_keys, after), POLL_TIMEOUT_S)
        check(ok, "%s: the panel's redo puts the drag back" % label)
        graph = node_graph_view()
        if shortcut(graph, False):
            ok = yield ("until", keys_are(before_keys, before), POLL_TIMEOUT_S)
            check(ok, "%s: one Ctrl+Z undoes the whole drag" % label)
            shortcut(graph, True)
            ok = yield ("until", keys_are(after_keys, after), POLL_TIMEOUT_S)
            check(ok, "%s: Ctrl+Shift+Z redoes it" % label)
        yield ("sleep", SETTLE_MS)
        return edits

    def replay(ref, edits):
        """Types each dragged value into ref's panel at the frame it was dragged at."""
        for frame, name, dim, value in edits:
            viewer.seek(frame)
            yield ("sleep", SETTLE_MS // 2)
            type_into(ref, name, dim, value)
            yield ("sleep", SETTLE_MS // 2)

    def compare_with_reference(node, ref, knobs, what):
        got, want = node_keys(node, knobs), node_keys(ref, knobs)
        report("NOTE %s keys: dragged %r typed %r" % (what, got, want))
        for name in knobs:
            for d in range(len(knobs[name])):
                check(close(got[name][d], want[name][d], 1e-6),
                      "%s: the dragged %s[%d] keys match the ones typed into the panel (%r vs %r)"
                      % (what, name, d, got[name][d], want[name][d]))

    def overlay_shot(node, frame, name, transform):
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        m = calibrate(gl)
        if transform:
            centre, _rot, _rx = transform_geometry(node, frame, m)
        else:
            t = node.getParam("translate")
            centre = to_widget(m, t.getValueAtTime(frame, 0), t.getValueAtTime(frame, 1))
        park(gl)
        yield ("sleep", SETTLE_MS)
        lit = overlay_pixels_near(gl, centre)
        check(lit >= 4, "%s: the widget is drawn at the keyed value for frame %d (%d lit pixels at %r)" % (name, frame, lit, centre))
        gt.shot("%s-f%d.png" % (name, frame))

    def lost_release(node, other):
        """Drags node's point and closes its panel before the release, then types a rotate value
        into other's panel: the drag's undo step must have ended without its release."""
        label = "lost release"
        gt.section(label)
        yield from open_only_panel(node)
        frame = 35
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        require(gl is not None, "the viewer is visible")
        m = calibrate(gl)
        t = node.getParam("translate")
        start = to_widget(m, t.getValueAtTime(frame, 0), t.getValueAtTime(frame, 1))
        end = start + QPointF(40.0, 20.0)
        before = values_at(node, POSITION_KEYS, frame)

        move(gl, start)
        send_mouse(gl, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 5):
            move(gl, start + (end - start) * (i / 4.0), Qt.LeftButton)
        dragged = values_at(node, POSITION_KEYS, frame)
        check(not close(dragged, before), "%s: the drag moved the point before its panel closed" % label)
        gt.close_all_panels()
        QApplication.processEvents()
        send_mouse(gl, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        park(gl)
        yield ("sleep", SETTLE_MS)
        check(close(values_at(node, POSITION_KEYS, frame), dragged), "%s: the point stays where the drag left it" % label)

        yield from open_only_panel(other)
        rotate = other.getParam("rotate")
        r0 = rotate.getValueAtTime(frame, 0)
        if not type_into(other, "rotate", 0, r0 + 15.0):
            return
        ok = yield ("until", lambda: abs(rotate.getValueAtTime(frame, 0) - (r0 + 15.0)) <= EPS, POLL_TIMEOUT_S)
        check(ok, "%s: typing into %s's rotate box sets it" % (label, other.getLabel()))

        graph = node_graph_view()
        if not shortcut(graph, False):
            return
        ok = yield ("until", lambda: abs(rotate.getValueAtTime(frame, 0) - r0) <= EPS, POLL_TIMEOUT_S)
        check(ok, "%s: one Ctrl+Z undoes the typed rotate" % label)
        yield ("sleep", SETTLE_MS)
        check(close(values_at(node, POSITION_KEYS, frame), dragged),
              "%s: the same Ctrl+Z leaves the drag alone, its undo step having ended at the panel close" % label)
        shortcut(graph, False)
        ok = yield ("until", lambda: close(values_at(node, POSITION_KEYS, frame), before), POLL_TIMEOUT_S)
        check(ok, "%s: a second Ctrl+Z undoes the drag" % label)
        shortcut(graph, True)
        ok = yield ("until", lambda: close(values_at(node, POSITION_KEYS, frame), dragged), POLL_TIMEOUT_S)
        check(ok, "%s: Ctrl+Shift+Z redoes the drag" % label)
        yield ("sleep", SETTLE_MS)

    # ---- the test --------------------------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def steps():
        global viewer
        ok = yield ("until", lambda: bool(visible_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        viewer_node = app.getNode("Viewer1") or create("Viewer", 400, 400)
        constant = create(CONSTANT, 0, 0)
        for d, v in enumerate((0.0, 0.0, 0.0, 1.0)):
            constant.getParam("color").setValue(v, d)
        transform = create(TRANSFORM, 0, 150)
        transform_ref = create(TRANSFORM, 250, 150)
        position = create(POSITION, 500, 150)
        position_ref = create(POSITION, 750, 150)
        for n in (transform, transform_ref, position, position_ref):
            require(n.connectInput(0, constant), "connect %s to the Constant" % n.getLabel())
        for n in (transform, transform_ref):
            seed_keys(n, TRANSFORM_KEYS)
        for n in (position, position_ref):
            seed_keys(n, POSITION_KEYS)
        if viewer_node.getInput(0) is not None:
            viewer_node.disconnectInput(0)
        require(viewer_node.connectInput(0, transform), "the Viewer shows the Transform")
        viewer = app.getViewer(viewer_node.getScriptName())
        require(viewer is not None, "found the Viewer's Python handle")
        tab = visible_of("ViewerTab")
        if tab:
            QMetaObject.invokeMethod(tab[0], "centerViewer")
        yield ("sleep", RENDER_MS)

        # ---- Transform ----
        yield from open_only_panel(transform)
        transform_edits = []

        for frame, dx, dy in ((20, 60.0, -40.0), (40, -50.0, 30.0)):
            viewer.seek(frame)
            yield ("sleep", RENDER_MS)
            m = calibrate(viewer_gl())
            centre, _rot, _rx = transform_geometry(transform, frame, m)
            t = transform.getParam("translate")
            tol = 3.0 * m["sx"]
            expect = {"translate": ((0, t.getValueAtTime(frame, 0) + dx * m["sx"], tol),
                                    (1, t.getValueAtTime(frame, 1) - dy * m["sy"], tol))}
            transform_edits += yield from drag_widget(
                transform, TRANSFORM_KEYS, "transform move f%d" % frame, frame,
                centre, centre + QPointF(dx, dy), True, expect)
            yield from overlay_shot(transform, frame, "m275-transform-move", True)

        frame = 30
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        m = calibrate(viewer_gl())
        centre, rot, rx = transform_geometry(transform, frame, m)
        sx0 = transform.getParam("scale").getValueAtTime(frame, 0)
        transform_edits += yield from drag_widget(
            transform, TRANSFORM_KEYS, "transform scale f%d" % frame, frame,
            offset(centre, rot, rx), offset(centre, rot, 2.0 * rx), True,
            {"scale": ((0, 2.0 * sx0, 0.1 * sx0),)})
        yield from overlay_shot(transform, frame, "m275-transform-scale", True)

        frame = 40
        render_on_release = NatronEngine.natron.getSettings().getParam("renderOnEditingFinished")
        saved_render_on_release = render_on_release.get()
        render_on_release.set(True)
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        m = calibrate(viewer_gl())
        centre, rot, rx = transform_geometry(transform, frame, m)
        r0 = transform.getParam("rotate").getValueAtTime(frame, 0)
        grab = rx + ROTATION_BAR_GRAB_PX
        transform_edits += yield from drag_widget(
            transform, TRANSFORM_KEYS, "transform rotate f%d" % frame, frame,
            offset(centre, rot, grab), offset(centre, rot + math.radians(30.0), grab), False,
            {"rotate": ((0, r0 + 30.0, 2.0),)})
        render_on_release.set(saved_render_on_release)
        yield from overlay_shot(transform, frame, "m275-transform-rotate", True)

        gt.section("transform reference")
        yield from open_only_panel(transform_ref)
        yield from replay(transform_ref, transform_edits)
        compare_with_reference(transform, transform_ref, TRANSFORM_KEYS, "Transform")

        # ---- Position ----
        viewer_node.disconnectInput(0)
        require(viewer_node.connectInput(0, position), "the Viewer shows the Position")
        yield ("sleep", RENDER_MS)
        yield from open_only_panel(position)
        frame = 25
        viewer.seek(frame)
        yield ("sleep", RENDER_MS)
        m = calibrate(viewer_gl())
        t = position.getParam("translate")
        point = to_widget(m, t.getValueAtTime(frame, 0), t.getValueAtTime(frame, 1))
        dx, dy = 50.0, -30.0
        tol = 3.0 * m["sx"]
        position_edits = yield from drag_widget(
            position, POSITION_KEYS, "position move f%d" % frame, frame, point, point + QPointF(dx, dy), True,
            {"translate": ((0, t.getValueAtTime(frame, 0) + dx * m["sx"], tol),
                           (1, t.getValueAtTime(frame, 1) - dy * m["sy"], tol))})
        yield from overlay_shot(position, frame, "m275-position-move", False)

        gt.section("position reference")
        yield from open_only_panel(position_ref)
        yield from replay(position_ref, position_edits)
        compare_with_reference(position, position_ref, POSITION_KEYS, "Position")

        yield from lost_release(position, transform_ref)

        # ---- save and reopen ----
        gt.section("reopen")
        viewer_node.disconnectInput(0)
        require(viewer_node.connectInput(0, transform), "the Viewer shows the Transform again")
        gt.close_all_panels()
        saved = {"transform": node_keys(transform, TRANSFORM_KEYS), "position": node_keys(position, POSITION_KEYS)}
        names = {"transform": transform.getScriptName(), "position": position.getScriptName()}
        path = os.path.join(gt.OUT, "m275_viewer_widget_editing.ntpz")
        require(app.saveProjectAs(path), "save the project to " + path)
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        require(app.loadProject(path) is not None, "reopen " + path)
        ok = yield ("until", lambda: all(app.getNode(n) is not None for n in names.values()), LOAD_TIMEOUT_S)
        require(ok, "the dragged nodes are back after reopening")
        yield ("sleep", RENDER_MS)
        r_transform, r_position = app.getNode(names["transform"]), app.getNode(names["position"])
        check(close(node_keys(r_transform, TRANSFORM_KEYS), saved["transform"]), "the Transform's keys read back after reopening")
        check(close(node_keys(r_position, POSITION_KEYS), saved["position"]), "the Position's keys read back after reopening")
        viewer_node = app.getNode("Viewer1")
        require(viewer_node is not None, "the Viewer is back after reopening")
        viewer = app.getViewer(viewer_node.getScriptName())
        require(viewer is not None, "found the reopened Viewer's Python handle")
        tab = visible_of("ViewerTab")
        if tab:
            QMetaObject.invokeMethod(tab[0], "centerViewer")
        yield from open_only_panel(r_transform)
        for frame in (20, 30, 40):
            yield from overlay_shot(r_transform, frame, "m275-reopened-transform", True)
        viewer_node.disconnectInput(0)
        require(viewer_node.connectInput(0, r_position), "the reopened Viewer shows the Position")
        yield from open_only_panel(r_position)
        yield from overlay_shot(r_position, 25, "m275-reopened-position", False)

        faulthandler.cancel_dump_traceback_later()

    viewer = None
    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
