"""GUI check of the Ramp's host overlay: each of its two points can be dragged in the viewer, and
the drag moves that point's knob by the distance the pen travelled, leaving the other alone.

The Ramp is given the same colour at both ends, so its whole frame is one colour on screen; the
frame's on-screen span gives the mapping from canonical to viewer-widget coordinates, which Python
has no direct access to. The points start on a horizontal line, so their dashed lines are vertical.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    NATRON_GUI_TEST_OUT=build/m128-shots GUI_TEST_TIMEOUT=200 \
        Tests/gui/run-gui-test.sh Tests/gui/m127_ramp_overlay.py build/fast/App/Natron
Screenshots: m127-ramp-overlay-<before|after>.png (the viewer with the overlay drawn).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 180
SETTLE_MS = 400
RENDER_MS = 1500
PLUGIN_RAMP = "Ramp"
RED = (0.8, 0.1, 0.1, 1.0)
# Pen travel per drag, in viewer-widget pixels (y down).
DRAGS = {"point0": (40, -30), "point1": (-35, 25)}
# Rounding to the screen pixel plus a pixel of span measurement error, in screen pixels.
TOL_PX = 2.5

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def visible_of(name):
        return [w for w in gt.widgets_of(name) if w.isVisible()]

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def widget_shot(widget, name):
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

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

    def is_red(c):
        return c.red() > c.green() + 60 and c.red() > c.blue() + 60

    def red_span(gl):
        """(x0, y0, x1, y1) of the red pixels in gl's coordinates, from a screen grab, or None."""
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        xs, ys = [], []
        for y in range(img.height()):
            for x in range(0, img.width(), 2):
                if is_red(img.pixelColor(x, y)):
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return min(xs), min(ys), max(xs) + 1, max(ys) + 1

    def point_of(ramp, name):
        p = ramp.getParam(name)
        return p.getValue(0), p.getValue(1)

    def steps():
        ok = yield ("until", lambda: bool(visible_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        # The overlay is drawn only while the settings panel is open.
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        ramp = app.createNode(PLUGIN_RAMP, -1, None, props)
        require(ramp is not None, "create a Ramp", gt.plugin_problem(PLUGIN_RAMP))
        require(ramp.getParam("falloff") is not None, "the Ramp is the native node (it has a falloff knob)")
        rod = ramp.getRegionOfDefinition(1, 0)
        fw, fh = rod.x2 - rod.x1, rod.y2 - rod.y1
        require(fw > 0 and fh > 0, "the Ramp has a frame (%r x %r)" % (fw, fh))
        for name in ("color0", "color1"):
            ramp.getParam(name).set(*RED)
        ramp.getParam("point0").set(rod.x1 + 0.3 * fw, rod.y1 + 0.5 * fh)
        ramp.getParam("point1").set(rod.x1 + 0.7 * fw, rod.y1 + 0.5 * fh)
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, ramp), "the Viewer shows the Ramp")
        yield ("sleep", RENDER_MS)

        gl = visible_of("ViewerGL")[0] if visible_of("ViewerGL") else None
        require(gl is not None, "the viewer is visible")
        span = red_span(gl)
        require(span is not None, "the Ramp's frame is on screen")
        sx0, sy0, sx1, sy1 = span
        units_x, units_y = fw / float(sx1 - sx0), fh / float(sy1 - sy0)
        report("NOTE frame %r on screen at %r: %.3f x %.3f canonical units per pixel" % ((fw, fh), span, units_x, units_y))
        check(abs(units_x - units_y) <= 0.05 * units_x, "the frame keeps its aspect on screen")

        def to_screen(p):
            return QPointF(sx0 + (p[0] - rod.x1) / units_x, sy1 - (p[1] - rod.y1) / units_y)

        widget_shot(gl, "m127-ramp-overlay-before")
        for name, (dx, dy) in DRAGS.items():
            other = "point1" if name == "point0" else "point0"
            before, other_before = point_of(ramp, name), point_of(ramp, other)
            start = to_screen(before)
            drag(gl, start, start + QPointF(dx, dy))
            yield ("sleep", SETTLE_MS)
            after, other_after = point_of(ramp, name), point_of(ramp, other)
            want = (before[0] + dx * units_x, before[1] - dy * units_y)
            report("NOTE %s %r -> %r, wanted %r" % (name, before, after, want))
            check(abs(after[0] - want[0]) <= TOL_PX * units_x and abs(after[1] - want[1]) <= TOL_PX * units_y,
                  "dragging %s's point moves it by the pen's travel (%r -> %r, wanted %r)" % (name, before, after, want))
            check(other_after == other_before, "dragging %s leaves %s alone (%r -> %r)"
                  % (name, other, other_before, other_after))
        widget_shot(gl, "m127-ramp-overlay-after")

        miss = to_screen((rod.x1 + 0.5 * fw, rod.y1 + 0.2 * fh))
        before = (point_of(ramp, "point0"), point_of(ramp, "point1"))
        drag(gl, miss, miss + QPointF(30, 30))
        yield ("sleep", SETTLE_MS)
        after = (point_of(ramp, "point0"), point_of(ramp, "point1"))
        check(after == before, "a drag away from both points moves neither (%r -> %r)" % (before, after))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
