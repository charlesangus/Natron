"""GUI check that the colour picker's output rect path honours the image's pixel aspect ratio.

A Ramp generator, which has no inputs, is viewed, so the Ctrl+Shift rectangle pick has no input
to read and falls back to the displayed image (getColorAtRect), marked "out". The Ramp steps from
one colour in the left half of the frame to a different one in the right half, on a
pixel-aspect-ratio 2 format and then on a pixel-aspect-ratio 1 format. For each half, a box wholly
inside it is rect-picked (the mean must equal that half's colour) and the same half is
point-picked with Ctrl+Alt as the control (getColorAt, which already divides by the ratio).

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    NATRON_GUI_TEST_OUT=build/m134-shots GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m134_picker_par.py build/fast/App/Natron
Screenshots: m134-par<N>-<half>-<rect|point>.png (the info bar after each pick).

Box positions come from a screen grab of the viewer: the halves are told apart by their colour
(red-dominant or blue-dominant against the grey viewer background), so the script does not need
the viewer's zoom or offset.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 1500
TOL = 1e-3
HALF_TOL = 0.06

RED = (0.8, 0.1, 0.1)
BLUE = (0.1, 0.1, 0.8)

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

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def widget_shot(widget, name):
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to crop, skipping the shot" % name)
            return
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, button, button, modifiers)
        send_mouse(widget, QEvent.MouseButtonRelease, pos, button, Qt.NoButton, modifiers)

    def drag(widget, start, end, button=Qt.LeftButton, modifiers=Qt.NoModifier, steps=4):
        start, end = QPointF(start), QPointF(end)
        send_mouse(widget, QEvent.MouseButtonPress, start, button, button, modifiers)
        for i in range(1, steps + 1):
            move(widget, start + (end - start) * (i / float(steps)), button, modifiers)
        send_mouse(widget, QEvent.MouseButtonRelease, end, button, Qt.NoButton, modifiers)

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_bar():
        found = visible_of("InfoViewerWidget")
        return found[0] if found else None

    def info_labels():
        bar = info_bar()
        if bar is None:
            return []
        out = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                    out.append((w.text(), w.isVisible()))
            except RuntimeError:
                continue
        return out

    def info_rgba():
        for text, visible in info_labels():
            if "d93232" in text and visible:
                nums = re.findall(r"-?\d+\.\d+|-?\d+", strip_tags(text))
                try:
                    return [float(n) for n in nums]
                except ValueError:
                    return None
        return None

    def info_source():
        for text, visible in info_labels():
            t = strip_tags(text)
            if t in ("in", "out"):
                return t if visible else None
        return None

    def rgb_is(values, want):
        return values is not None and len(values) >= 3 and all(abs(values[i] - want[i]) <= TOL for i in range(3))

    def set_rgba(param, rgba):
        for d in range(4):
            param.setValue(rgba[d], d)

    def classify(c):
        r, g, b = c.red(), c.green(), c.blue()
        if r > g + 60 and r > b + 60:
            return "red"
        if b > r + 60 and b > g + 60:
            return "blue"
        return None

    def coloured_spans(gl):
        """{"red": (x0, x1), "blue": (x0, x1), "y": (y0, y1)} in gl's coordinates, from a screen grab."""
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        xs = {"red": [], "blue": []}
        ys = []
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                kind = classify(img.pixelColor(x, y))
                if kind:
                    xs[kind].append(x)
                    ys.append(y)
        if not xs["red"] or not xs["blue"]:
            return None
        return {"red": (min(xs["red"]), max(xs["red"])), "blue": (min(xs["blue"]), max(xs["blue"])),
                "y": (min(ys), max(ys))}

    def set_project_format(spec, name):
        app.addFormat(spec)
        fmt = app.getProjectParam("outputFormat")
        options = list(fmt.getOptions())
        index = next((i for i, o in enumerate(options) if name in str(o)), len(options) - 1)
        fmt.setValue(index)
        QApplication.processEvents()

    def case(par, spec, name):
        gt.section("par%d" % par)
        set_project_format(spec, name)
        yield ("sleep", SETTLE_MS)

        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        ramp = app.createNode("Ramp", -1, None, props)
        require(ramp is not None, "create a Ramp", gt.plugin_problem("Ramp"))
        gt.close_all_panels()
        # Canonical units: the 200x100 format spans 200*par canonical units across. The ramp
        # spans one canonical unit across the middle, so it is a step between the two halves.
        half_width = 100.0 * par
        ramp.getParam("point0").setValue(half_width - 0.5, 0)
        ramp.getParam("point0").setValue(50.0, 1)
        ramp.getParam("point1").setValue(half_width + 0.5, 0)
        ramp.getParam("point1").setValue(50.0, 1)
        set_rgba(ramp.getParam("color0"), RED + (1.0,))
        set_rgba(ramp.getParam("color1"), BLUE + (1.0,))
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, ramp), "the Viewer shows the Ramp")
        yield ("sleep", RENDER_MS)

        gl = visible_of("ViewerGL")[0] if visible_of("ViewerGL") else None
        require(gl is not None, "the viewer is visible")
        spans = coloured_spans(gl)
        require(spans is not None, "both halves are on screen", "no red/blue pixels found in the viewer")
        left_kind = "red" if spans["red"][0] < spans["blue"][0] else "blue"
        right_kind = "blue" if left_kind == "red" else "red"
        want = {"left": RED if left_kind == "red" else BLUE, "right": BLUE if left_kind == "red" else RED}
        l0, l1 = spans[left_kind]
        r0, r1 = spans[right_kind]
        frame_w = float(r1 - l0)
        check(abs((l1 - l0) / frame_w - 0.5) <= HALF_TOL,
              "the left colour fills half of the frame on screen (%.2f)" % ((l1 - l0) / frame_w))
        y0, y1 = spans["y"]
        yc, dy = (y0 + y1) / 2.0, (y1 - y0) / 6.0
        halves = {"left": (l0, l1), "right": (r0, r1)}
        rect_mods = Qt.ControlModifier | Qt.ShiftModifier
        point_mods = Qt.ControlModifier | Qt.AltModifier

        for half in ("left", "right"):
            a, b = halves[half]
            quarter = (b - a) / 4.0
            start = QPointF(a + quarter, yc - dy)
            end = QPointF(b - quarter, yc + dy)
            move(gl, start)
            drag(gl, start, end, Qt.LeftButton, rect_mods)
            yield ("sleep", SETTLE_MS)
            rgba, source = info_rgba(), info_source()
            report("NOTE par %d %s rect: %r %r" % (par, half, rgba, source))
            check(source == "out" and rgb_is(rgba, want[half]),
                  "PAR %d: the Ctrl+Shift rect pick in the %s half reads its colour %r, marked out (got %r %r)"
                  % (par, half, want[half], rgba, source))
            widget_shot(info_bar(), "m134-par%d-%s-rect" % (par, half))

            centre = QPointF((a + b) / 2.0, yc)
            move(gl, centre)
            click(gl, centre, Qt.LeftButton, point_mods)
            yield ("sleep", SETTLE_MS)
            rgba, source = info_rgba(), info_source()
            report("NOTE par %d %s point: %r %r" % (par, half, rgba, source))
            check(rgb_is(rgba, want[half]),
                  "PAR %d: the Ctrl+Alt point pick in the %s half reads its colour %r (got %r)"
                  % (par, half, want[half], rgba))
            widget_shot(info_bar(), "m134-par%d-%s-point" % (par, half))

        ramp.destroy()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

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
        yield from case(2, "m134par2 200x100 2", "m134par2")
        yield from case(1, "m134par1 200x100", "m134par1")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
