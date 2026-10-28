"""The stroke being painted is drawn under the pen, and pen-up never blocks the GUI thread.

Preview: pauses the viewer so no render can reach the screen, then drags a 50 px solid red brush
across a RotoPaint node's viewer. After each pen move and one viewer repaint, the screen pixel
under every sample so far must be red: only the overlay preview can have put it there. A pixel
well away from the stroke must stay dark.

Pen-up: unpauses the viewer and draws two strokes 50 ms apart. Each release must return quickly
(the neat render runs asynchronously), the cursor must never be a busy cursor, and once the
renders converge both strokes are on screen. With NATRON_STALL_LOG set, no GUI-thread stall over
50 ms may start during the two strokes. Saves a screenshot of the converged result.

The bit-exact stroke golden is not checked here: it is a half-float crop recorded and compared in
a build session.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m224-shots/preview -e GUI_TEST_TIMEOUT=300 \\
        -e NATRON_STALL_LOG=build/m224-shots/preview/stalls.jsonl \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m224_stroke_preview.py build/fast/App/Natron
"""
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 300
RENDER_MS = 1500
PEN_INTERVAL_MS = 8
STROKE_GAP_MS = 50
STROKE_SAMPLES = 12
BRUSH_PX = 50.0
STALL_LIMIT_MS = 50.0
# A release that waited on the neat render took a whole render; an asynchronous one takes far less.
RELEASE_LIMIT_MS = 50.0
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def mono_ms():
        # StallMonitor stamps std::chrono::steady_clock, which is this clock on Linux.
        return time.monotonic() * 1000.0

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

    def pause_button(gl):
        tab = ancestor_of_class(gl, "ViewerTab")
        if tab is None:
            return None
        # findChildren wrappers die with the parent's wrapper; hold the tab while the button is used.
        _keep["tab"] = tab
        # Natron's Button has no Q_OBJECT, so it reports QPushButton's class name.
        for b in tab.findChildren(QAbstractButton):
            try:
                if b.isCheckable() and "Pause Updates" in b.toolTip():
                    return b
            except RuntimeError:
                continue
        return None

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def press(widget, pos):
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)

    def move(widget, pos, buttons=Qt.NoButton):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons)

    def release(widget, pos):
        t0 = mono_ms()
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        return mono_ms() - t0

    def busy_cursor(gl):
        busy = (Qt.BusyCursor, Qt.WaitCursor)
        override = QGuiApplication.overrideCursor()
        if override is not None and override.shape() in busy:
            return True
        return gl.cursor().shape() in busy

    class Grab(object):
        """The screen under widget, read in widget coordinates. A widget grab of the GL viewer can
        come out blank under llvmpipe, so this reads the screen."""

        def __init__(self, widget):
            QApplication.processEvents()
            origin = widget.mapToGlobal(QPoint(0, 0))
            self.img = QGuiApplication.primaryScreen().grabWindow(
                0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
            self.s = self.img.width() / float(max(1, widget.width()))

        def colour(self, p):
            xd, yd = int(round(p.x() * self.s)), int(round(p.y() * self.s))
            if 0 <= xd < self.img.width() and 0 <= yd < self.img.height():
                c = self.img.pixelColor(xd, yd)
                return (c.red(), c.green(), c.blue())
            return None

    def is_red(c):
        return c is not None and c[0] >= 200 and c[1] <= 60 and c[2] <= 60

    def is_dark(c):
        return c is not None and max(c) <= 60

    def line(gl, y_frac, x0_frac, x1_frac, n):
        w, h = float(gl.width()), float(gl.height())
        return [QPointF(w * (x0_frac + (x1_frac - x0_frac) * i / float(n - 1)), h * y_frac) for i in range(n)]

    def read_stalls():
        if not STALL_LOG or not os.path.isfile(STALL_LOG):
            return []
        out = []
        with open(STALL_LOG) as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    s = json.loads(raw)
                    out.append((float(s["start_ms"]), float(s["duration_ms"])))
                except (KeyError, TypeError, ValueError):
                    continue
        return out

    _keep = {}

    def preview_phase(gl):
        gt.section("preview")
        button = pause_button(gl)
        require(button is not None, "found the viewer's Pause Updates button")
        _keep["pause"] = button
        if not button.isChecked():
            button.click()
        QApplication.processEvents()
        check(button.isChecked(), "the viewer is paused")
        yield ("sleep", SETTLE_MS)

        far = QPointF(gl.width() * 0.5, gl.height() * 0.2)
        check(is_dark(Grab(gl).colour(far)), "the viewer is dark before the stroke")

        pts = line(gl, 0.5, 0.3, 0.7, STROKE_SAMPLES)
        press(gl, pts[0])
        yield ("sleep", PEN_INTERVAL_MS)
        missing = []
        for i in range(1, len(pts)):
            move(gl, pts[i], Qt.LeftButton)
            gl.repaint()
            grab = Grab(gl)
            for j in range(i + 1):
                c = grab.colour(pts[j])
                if not is_red(c):
                    missing.append((i, j, c))
            yield ("sleep", PEN_INTERVAL_MS)
        check(not missing, "the preview covers every sample after each paint (misses: %s)" % missing[:5])
        check(is_dark(Grab(gl).colour(far)), "the preview stays local to the stroke")
        gt.shot("m224_preview_paused.png")
        release(gl, pts[-1])
        yield ("sleep", SETTLE_MS)

        button.click()
        QApplication.processEvents()
        check(not button.isChecked(), "the viewer is unpaused")
        yield ("sleep", RENDER_MS)

    def pen_up_phase(gl, rp):
        gt.section("pen-up")
        strokes = [line(gl, 0.35, 0.3, 0.7, 10), line(gl, 0.65, 0.3, 0.7, 10)]
        busy, releases = [], []
        t_start = mono_ms()
        for k, pts in enumerate(strokes):
            if k > 0:
                yield ("sleep", STROKE_GAP_MS)
            press(gl, pts[0])
            busy.append(busy_cursor(gl))
            for p in pts[1:]:
                yield ("sleep", PEN_INTERVAL_MS)
                move(gl, p, Qt.LeftButton)
                busy.append(busy_cursor(gl))
            releases.append(release(gl, pts[-1]))
            busy.append(busy_cursor(gl))
        t_end = mono_ms()
        report("NOTE release times (ms): %s" % ", ".join("%.1f" % r for r in releases))
        check(all(r < RELEASE_LIMIT_MS for r in releases), "each pen-up returns without waiting on the neat render")
        check(not any(busy), "no busy cursor during or between the strokes")

        yield ("sleep", RENDER_MS)
        layer = rp.getRotoContext().getBaseLayer()
        check(len(layer.getChildren()) >= 1, "the strokes are in the RotoPaint's base layer")
        grab = Grab(gl)
        for k, pts in enumerate(strokes):
            # The viewer labels the RoD corners, which sit on the strokes' ends.
            colours = [grab.colour(p) for p in pts[2:-2]]
            check(all(is_red(c) for c in colours), "stroke %d is on screen once the renders land (%s)" % (k + 1, colours[:3]))
        gt.shot("m224_two_strokes.png")

        if STALL_LOG:
            # The monitor writes a stall once it ends; give it time to flush.
            yield ("sleep", 200)
            mine = [d for s, d in read_stalls() if t_start <= s <= t_end + RENDER_MS and d > STALL_LIMIT_MS]
            check(not mine, "no GUI-thread stall over %d ms during the strokes (%s)" % (STALL_LIMIT_MS, mine))
        else:
            report("NOTE NATRON_STALL_LOG is not set; stalls were not checked")

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
        rp = app.createNode("fr.inria.built-in.RotoPaint", -1, None, props)
        require(rp is not None, "create RotoPaint", gt.plugin_problem("fr.inria.built-in.RotoPaint"))
        require(rp.getRotoContext() is not None, "the RotoPaint node has a roto context")
        _keep["rp"] = rp
        rp.getParam("sizeSpinbox").setValue(BRUSH_PX)
        rp.getParam("hardnessSpinbox").setValue(1.0)
        rp.getParam("opacitySpinbox").setValue(1.0)
        colour = rp.getParam("strokeColorButton")
        for dim, v in enumerate((1.0, 0.0, 0.0)):
            colour.setValue(v, dim)
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, rp), "the Viewer shows the RotoPaint")
        QApplication.processEvents()
        yield ("sleep", RENDER_MS)

        gl = viewer_gl()
        require(gl is not None, "the viewer is visible")
        _keep["gl"] = gl
        activate()
        require(untone_view(), "picked the Un-tone-mapped view")
        yield ("sleep", RENDER_MS)

        for cmd in preview_phase(gl):
            yield cmd
        layer = rp.getRotoContext().getBaseLayer()
        for item in list(layer.getChildren()):
            layer.removeItem(item)
        yield ("sleep", RENDER_MS)
        for cmd in pen_up_phase(gl, rp):
            yield cmd

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
