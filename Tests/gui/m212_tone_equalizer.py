"""GUI check of ToneEqualizer's viewer readout, with screenshots of its Result and Mask outputs.

A Rectangle fills the left half of the frame with red at 2^-4 on a blue background at 2^-1. With
the HSV Value estimator and no detail preservation the mask is exactly those values, so hovering
the centre of each half must write -4.0 EV (Midtones) and -1.0 EV (Whites), and the corrections
the fitted curve gives there, into the Cursor Readout knob.

A wheel notch up over the red half must raise the correction there by the 0.1 EV step and leave the
viewer's zoom alone; with Shift a notch moves the Midtones gain a fifth as far (0.02 EV), and with
Control the wheel seeks frames and leaves the bands alone. Over the empty viewer outside the image
the wheel still zooms.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    NATRON_GUI_TEST_OUT=build/m212-shots GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m212_tone_equalizer.py build/fast/App/Natron
Screenshots: m212-result.png and m212-mask.png (whole screen).
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
READOUT_TIMEOUT_S = 10.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 2000
EV_TOL = 0.05
WHEEL_NOTCH = 120
WHEEL_STEP_EV = 0.1
FINE_WHEEL_STEP_EV = 0.02
# Once the gains sit on the fitted curve the refit is close to linear in the step, so a fine notch
# moves the gain about a fifth as far.
FINE_RATIO_TOL = 0.05

INSIDE = (0.0625, 0.0, 0.0)
OUTSIDE = (0.0, 0.0, 0.5)
MIDTONES_GAIN = 1.0
WHITES_GAIN = -0.5
# The curve is a least-squares fit of eight gaussians (sigma sqrt(2) EV) to the nine band gains, so it
# does not pass through a lone band's gain: with only these two bands set it reads +0.78 EV at -4 EV
# and -0.28 EV at -1 EV (reproduced with numpy lstsq over the kernel's centres).
MIDTONES_CORRECTION = 0.78
WHITES_CORRECTION = -0.28

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent, QWheelEvent
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

    def move(widget, pos):
        QApplication.sendEvent(widget, QMouseEvent(QEvent.MouseMove, QPointF(pos), Qt.NoButton, Qt.NoButton, Qt.NoModifier))
        QApplication.processEvents()

    def wheel(widget, pos, notches, modifiers=Qt.NoModifier):
        glob = QPointF(widget.mapToGlobal(pos.toPoint()))
        QApplication.sendEvent(widget, QWheelEvent(pos, glob, QPoint(0, 0), QPoint(0, notches * WHEEL_NOTCH),
                                                   Qt.NoButton, modifiers, Qt.NoScrollPhase, False))
        QApplication.processEvents()

    def set_rgba(param, rgb):
        for d in range(3):
            param.setValue(rgb[d], d)
        param.setValue(1.0, 3)

    def grab(gl):
        origin = gl.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()

    def classify(c):
        r, g, b = c.red(), c.green(), c.blue()
        if r > g + 15 and r > b + 15:
            return "red"
        if b > r + 15 and b > g + 15:
            return "blue"
        return None

    def coloured_boxes(gl):
        """{"red": (x0, x1, y0, y1), "blue": ...} in gl's coordinates, from a screen grab."""
        img = grab(gl)
        boxes = {"red": [], "blue": []}
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                kind = classify(img.pixelColor(x, y))
                if kind:
                    boxes[kind].append((x, y))
        out = {}
        for kind, pts in boxes.items():
            if not pts:
                return None
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            out[kind] = (min(xs), max(xs), min(ys), max(ys))
        return out

    def centre(box):
        return QPointF((box[0] + box[1]) / 2.0, (box[2] + box[3]) / 2.0)

    def parse_readout(text):
        """(exposure, band label, correction) from "-4.0 EV  Midtones (-4 EV), correction +1.00 EV"."""
        m = re.match(r"\s*([+-]?\d+\.\d+) EV\s+(.+?) \([+-]?\d+ EV\), correction ([+-]?\d+\.\d+) EV", text)
        if not m:
            return None
        return float(m.group(1)), m.group(2), float(m.group(3))

    def hover(gl, readout, pos, label):
        move(gl, pos)
        ok = yield ("until", lambda: parse_readout(readout.getValue()) is not None, READOUT_TIMEOUT_S)
        text = readout.getValue()
        report("NOTE %s readout: %r" % (label, text))
        check(ok, "hovering the %s half writes a reading into Cursor Readout (got %r)" % (label, text))
        return parse_readout(text)

    def grey_level(gl, pos):
        c = grab(gl).pixelColor(int(pos.x()), int(pos.y()))
        return c.red(), c.green(), c.blue()

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
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        rect = app.createNode("Rectangle")
        require(rect is not None, "create a Rectangle", gt.plugin_problem("Rectangle"))
        gt.close_all_panels()
        # The halves are found on screen, so the format need not be exactly this.
        width = 1920.0
        height = 1080.0
        rect.getParam("bottomLeft").setValue(0.0, 0)
        rect.getParam("bottomLeft").setValue(0.0, 1)
        rect.getParam("size").setValue(width / 2.0, 0)
        rect.getParam("size").setValue(height, 1)
        rect.getParam("softness").setValue(0.0)
        set_rgba(rect.getParam("color1"), INSIDE)
        # The Rectangle's region is its box, so the other half comes from a Constant under it.
        background = app.createNode("Constant")
        require(background is not None, "create a Constant", gt.plugin_problem("Constant"))
        set_rgba(background.getParam("color"), OUTSIDE)
        frame = app.createNode("Merge")
        require(frame is not None, "create a Merge", gt.plugin_problem("Merge"))
        require(frame.connectInput(0, background), "the Constant is the Merge's B")
        require(frame.connectInput(1, rect), "the Rectangle is the Merge's A")
        gt.close_all_panels()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, frame), "the Viewer shows the Rectangle over the Constant")
        yield ("sleep", RENDER_MS)

        gl = visible_of("ViewerGL")[0] if visible_of("ViewerGL") else None
        require(gl is not None, "the viewer is visible")
        boxes = coloured_boxes(gl)
        require(boxes is not None, "both halves are on screen", "no red/blue pixels found in the viewer")
        red_at, blue_at = centre(boxes["red"]), centre(boxes["blue"])
        report("NOTE red half centre %r, blue half centre %r" % (red_at, blue_at))

        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        te = app.createNode("ToneEqualizer", -1, None, props)
        require(te is not None, "create a ToneEqualizer", gt.plugin_problem("ToneEqualizer"))
        require(te.connectInput(0, frame), "ToneEqualizer takes the Rectangle over the Constant")
        te.getParam("luminanceEstimator").setValue(2)
        te.getParam("preserveDetails").setValue(0)
        te.getParam("midtones").setValue(MIDTONES_GAIN)
        te.getParam("whites").setValue(WHITES_GAIN)
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, te), "the Viewer shows the ToneEqualizer")
        readout = te.getParam("cursorReadout")
        require(readout is not None, "the Cursor Readout knob exists")
        yield ("sleep", RENDER_MS)

        gt.section("readout")
        reading = yield from hover(gl, readout, red_at, "red")
        if reading:
            exposure, band, correction = reading
            check(abs(exposure + 4.0) <= EV_TOL and band == "Midtones",
                  "the red half reads -4.0 EV, Midtones (got %.2f, %s)" % (exposure, band))
            check(abs(correction - MIDTONES_CORRECTION) <= EV_TOL,
                  "the red half's correction is the fitted curve's %+.2f EV (got %+.2f)" % (MIDTONES_CORRECTION, correction))
        reading = yield from hover(gl, readout, blue_at, "blue")
        if reading:
            exposure, band, correction = reading
            check(abs(exposure + 1.0) <= EV_TOL and band == "Whites",
                  "the blue half reads -1.0 EV, Whites (got %.2f, %s)" % (exposure, band))
            check(abs(correction - WHITES_CORRECTION) <= EV_TOL,
                  "the blue half's correction is the fitted curve's %+.2f EV (got %+.2f)" % (WHITES_CORRECTION, correction))
        gt.section("scroll")
        midtones = te.getParam("midtones")
        before = yield from hover(gl, readout, red_at, "red before scroll")
        before_gain = midtones.getValue()
        before_boxes = coloured_boxes(gl)
        wheel(gl, red_at, 1)
        after_gain = midtones.getValue()
        after = parse_readout(readout.getValue())
        report("NOTE Midtones knob %+.3f -> %+.3f EV; readout %r -> %r" % (before_gain, after_gain, before, after))
        check(after_gain != before_gain, "a wheel notch over the red half edits the Midtones band")
        if before and after:
            raised = after[2] - before[2]
            check(abs(raised - WHEEL_STEP_EV) <= EV_TOL,
                  "the correction under the cursor rises by the %.2f EV step (got %+.3f)" % (WHEEL_STEP_EV, raised))
        yield ("sleep", RENDER_MS)
        after_boxes = coloured_boxes(gl)
        check(after_boxes is not None and after_boxes == before_boxes,
              "the wheel over the image does not zoom the viewer (boxes %r -> %r)" % (before_boxes, after_boxes))

        gt.section("shift scroll")
        # The first notch also moved the band gains onto the fitted curve, so it is no measure of
        # one step; a second plain notch is.
        start_gain = midtones.getValue()
        wheel(gl, red_at, 1)
        coarse_gain = midtones.getValue()
        coarse = coarse_gain - start_gain
        wheel(gl, red_at, 1, Qt.ShiftModifier)
        fine = midtones.getValue() - coarse_gain
        report("NOTE Midtones moves %+.4f EV a notch, %+.4f EV a Shift notch" % (coarse, fine))
        want = FINE_WHEEL_STEP_EV / WHEEL_STEP_EV
        check(coarse != 0.0 and abs(fine / coarse - want) <= FINE_RATIO_TOL,
              "a Shift notch moves Midtones %.2f of a plain notch (got %+.4f vs %+.4f)" % (want, fine, coarse))
        yield ("sleep", RENDER_MS)

        gt.section("control scroll")
        gain = midtones.getValue()
        frame = app.timelineGetTime()
        wheel(gl, red_at, -1, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        seeked = app.timelineGetTime()
        report("NOTE Control+wheel: frame %r -> %r" % (frame, seeked))
        check(seeked != frame, "Control+wheel over the image seeks frames (frame %r -> %r)" % (frame, seeked))
        check(midtones.getValue() == gain, "Control+wheel leaves Midtones alone")
        if seeked != frame:
            wheel(gl, red_at, 1, Qt.ControlModifier)
            yield ("sleep", RENDER_MS)
        move(gl, red_at)
        yield ("sleep", SETTLE_MS)
        gt.shot("m212-result.png")

        gt.section("mask")
        te.getParam("output").setValue(1)
        yield ("sleep", RENDER_MS)
        dark = grey_level(gl, red_at)
        light = grey_level(gl, blue_at)
        report("NOTE mask on screen: red half %r, blue half %r" % (dark, light))
        check(max(dark) - min(dark) <= 6 and max(light) - min(light) <= 6,
              "the Mask output is grey in both halves (got %r, %r)" % (dark, light))
        check(light[1] > dark[1] + 40, "the brighter half's mask is lighter on screen (got %r, %r)" % (dark, light))
        gt.shot("m212-mask.png")

        gt.section("scroll off the image")
        te.getParam("output").setValue(0)
        yield ("sleep", RENDER_MS)
        image = coloured_boxes(gl)
        require(image is not None, "both halves are on screen again", "no red/blue pixels found in the viewer")
        x0 = min(b[0] for b in image.values())
        y0 = min(b[2] for b in image.values())
        if x0 > 20 or y0 > 20:
            off = QPointF(x0 / 2.0 if x0 > 20 else red_at.x(), y0 / 2.0 if y0 > 20 else red_at.y())
            gain = midtones.getValue()
            wheel(gl, off, 1)
            yield ("sleep", RENDER_MS)
            zoomed = coloured_boxes(gl)
            report("NOTE off-image wheel at %r: boxes %r -> %r" % (off, image, zoomed))
            check(zoomed != image, "the wheel outside the image still zooms the viewer (boxes %r -> %r)" % (image, zoomed))
            check(midtones.getValue() == gain, "the wheel outside the image leaves Midtones alone")
        else:
            report("NOTE the image fills the viewer; the off-image zoom check is skipped")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
