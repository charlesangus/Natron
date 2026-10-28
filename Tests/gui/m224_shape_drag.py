"""Roto shapes render while they are dragged, per the Roto Shape Drag Render preference.

A Roto node draws a square into its alpha, which the viewer displays as white. Each pass grabs the square's top-right corner
point on screen and drags it right without releasing, waits, then samples a viewer pixel that is
inside the dragged shape but outside the original square:
  - On Release: the pixel stays dark until the release, then turns white.
  - Always Live: the pixel turns white before the release.
  - Adaptive (the default): reported only, as it falls back to On Release on a machine where one
    render takes longer than 100 ms.
Saves a screenshot of the live pass before its release.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m224-shots/shape-drag -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m224_shape_drag.py build/fast/App/Natron
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 300
RENDER_MS = 1500
PEN_INTERVAL_MS = 16
DRAG_STEPS = 10
SQUARE_X, SQUARE_Y, SQUARE_SIZE = 760.0, 340.0, 400.0
MODES = {"adaptive": 0, "live": 1, "onRelease": 2}
T = 1.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

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

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def press(widget, pos):
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)

    def move(widget, pos, buttons=Qt.NoButton):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons)

    def release(widget, pos):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)

    def send_key(widget, key):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def select_tool(gl):
        # Roto starts in the Bezier drawing tool, where a press on the corner starts a new shape.
        # Escape leaves it for the last select tool (Select All) and does nothing once a select
        # tool is active. Q is not used: with the select group already active it cycles to the
        # group's next tool (Select Points, then Select Shapes, then Select Feather Points).
        send_key(gl, Qt.Key_Escape)

    class Grab(object):
        """The screen under widget, read in widget coordinates. A widget grab of the GL viewer can
        come out blank under llvmpipe, so this reads the screen."""

        def __init__(self, widget):
            QApplication.processEvents()
            origin = widget.mapToGlobal(QPoint(0, 0))
            self.img = QGuiApplication.primaryScreen().grabWindow(
                0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
            self.s = self.img.width() / float(max(1, widget.width()))
            self.w = widget.width()
            self.h = widget.height()

        def colour(self, p):
            xd, yd = int(round(p.x() * self.s)), int(round(p.y() * self.s))
            if 0 <= xd < self.img.width() and 0 <= yd < self.img.height():
                c = self.img.pixelColor(xd, yd)
                return (c.red(), c.green(), c.blue())
            return None

        def white_bbox(self, step=2):
            """Widget-space bbox of the near-white pixels: the rendered square."""
            xs, ys = [], []
            for y in range(0, self.h, step):
                for x in range(0, self.w, step):
                    c = self.colour(QPointF(x, y))
                    if is_white(c):
                        xs.append(x)
                        ys.append(y)
            if not xs:
                return None
            return (min(xs), min(ys), max(xs), max(ys))

    def is_white(c):
        return c is not None and min(c) >= 200

    def is_dark(c):
        return c is not None and max(c) <= 60

    _keep = {}

    def make_square(roto):
        ctx = roto.getRotoContext()
        layer = ctx.getBaseLayer()
        for item in list(layer.getChildren()):
            layer.removeItem(item)
        shape = ctx.createRectangle(SQUARE_X, SQUARE_Y, SQUARE_SIZE, 1.0)
        require(shape is not None, "created a square on the Roto node")
        _keep["shape"] = shape

    def drag_pass(gl, roto, mode):
        gt.section(mode)
        setting = NatronEngine.natron.getSettings().getParam("rotoShapeDragRender")
        require(setting is not None, "the settings have a rotoShapeDragRender parameter")
        setting.set(MODES[mode])
        check(setting.get() == MODES[mode], "%s: the preference is set" % mode)

        make_square(roto)
        yield ("sleep", RENDER_MS)
        box = Grab(gl).white_bbox()
        if box is None:
            gt.shot("m224_shape_drag_%s_no_square.png" % mode)
        require(box is not None, "%s: the square is on screen" % mode)
        x0, y0, x1, y1 = box
        report("NOTE %s: square at widget (%d, %d)-(%d, %d)" % (mode, x0, y0, x1, y1))
        d = (x1 - x0) * 0.5
        # Inside the dragged shape, whose top-right corner moves right by d, outside the square.
        probe = QPointF(x1 + d * 0.3, y0 + (y1 - y0) * 0.1)
        inside = QPointF((x0 + x1) * 0.5, (y0 + y1) * 0.5)
        check(is_dark(Grab(gl).colour(probe)), "%s: the probe pixel is dark before the drag" % mode)

        select_tool(gl)
        corner_x = _keep["shape"].getControlPointPosition(1, T)[0]
        corner = QPointF(x1, y0)
        press(gl, corner)
        for i in range(1, DRAG_STEPS + 1):
            yield ("sleep", PEN_INTERVAL_MS)
            move(gl, QPointF(x1 + d * i / float(DRAG_STEPS), y0), Qt.LeftButton)
        yield ("sleep", RENDER_MS)
        held = Grab(gl)
        before = held.colour(probe)
        check(is_white(held.colour(inside)), "%s: the square's interior is still white during the drag" % mode)
        if mode == "live":
            gt.shot("m224_shape_drag_live_held.png")
        release(gl, QPointF(x1 + d, y0))
        yield ("sleep", RENDER_MS)
        after = Grab(gl).colour(probe)
        moved = _keep["shape"].getControlPointPosition(1, T)[0] - corner_x
        check(moved > 0, "%s: the drag moved the square's top-right corner (by %.1f)" % (mode, moved))

        if mode == "onRelease":
            check(is_dark(before), "onRelease: the probe pixel stays dark until the release (%s)" % (before,))
        elif mode == "live":
            check(is_white(before), "live: the probe pixel turns white before the release (%s)" % (before,))
        else:
            report("NOTE adaptive: probe pixel before release %s (white = rendered live)" % (before,))
        check(is_white(after), "%s: the probe pixel is white after the release (%s)" % (mode, after))

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

    def display_alpha():
        """Roto writes only the alpha channel by default, which the RGB display shows black."""
        for w in gt.widgets_of("ComboBox") + gt.widgets_of("ChannelsComboBox"):
            try:
                if not (w.isVisible() and "Display Channels:" in (w.toolTip() or "")):
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
            except RuntimeError:
                continue
            if "Alpha" in items:
                QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index("Alpha")))
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
        require(roto.getRotoContext() is not None, "the Roto node has a roto context")
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
        require(display_alpha(), "the viewer displays the alpha channel")
        yield ("sleep", RENDER_MS)

        setting = NatronEngine.natron.getSettings().getParam("rotoShapeDragRender")
        require(setting is not None, "the settings have a rotoShapeDragRender parameter")
        check(setting.get() == MODES["adaptive"], "the preference defaults to Adaptive")
        try:
            for mode in ("onRelease", "live", "adaptive"):
                for cmd in drag_pass(gl, roto, mode):
                    yield cmd
        finally:
            setting.set(MODES["adaptive"])
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
