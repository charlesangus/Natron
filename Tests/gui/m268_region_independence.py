"""Region independence in the viewer, on a 3840x2160 synthetic Noise plate.

DenoiseProfiled in Compute Variance mode: the readout names the whole frame (3840x2160 at 100%)
and does not change when the viewer is panned at 100%, or zoomed to 200% and back to 100%.

ContrastTexture with every detail band boosted: at 100%, before and after a middle-button pan,
no row or column of the viewer shows a step that its neighbours do not (a tile seam), and the
area seen in both views shows the same pixels. Screenshots go to $NATRON_GUI_TEST_OUT as
m268_variance_*.png and m268_contrast_*.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m268_region_independence.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
RENDER_TIMEOUT_S = 180.0
STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 900
SETTLE_MS = 500
PLATE_W = 3840
PLATE_H = 2160
PAN = (173, 97)  # Not a multiple of any tile size.
EDGE = 6  # Viewer border pixels left out of the seam scan.
SEAM_RATIO = 3.0
SEAM_FLOOR = 2.0
OVERLAP_MEAN = 1.5
OVERLAP_MAX = 8

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
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
            if isinstance(w, QMainWindow):
                return w
        return None

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def combo_items(combo):
        return [str(i) for i in (combo.property("natronItems") or [])]

    def choose(combo, text, exact=True):
        for index, item in enumerate(combo_items(combo)):
            if item.strip() == text or (not exact and text.lower() in item.lower()):
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
                return True
        return False

    def set_choice(param, option_id):
        options = [str(o) for o in param.getOptions()]
        require(option_id in options, "the %s menu offers %s" % (param.getScriptName(), option_id), repr(options))
        param.setValue(options.index(option_id))

    def mouse(widget, kind, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(kind, QPointF(pos), QPointF(widget.mapToGlobal(pos)), button,
                                                   buttons, Qt.NoModifier))

    def pan(gl, delta):
        start = gl.rect().center()
        mouse(gl, QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton)
        steps = 8
        for i in range(1, steps + 1):
            p = QPoint(start.x() + delta[0] * i // steps, start.y() + delta[1] * i // steps)
            mouse(gl, QEvent.MouseMove, p, Qt.NoButton, Qt.MiddleButton)
            QApplication.processEvents()
        end = QPoint(start.x() + delta[0], start.y() + delta[1])
        mouse(gl, QEvent.MouseButtonRelease, end, Qt.MiddleButton, Qt.NoButton)
        QApplication.processEvents()

    def grab(gl):
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        return img.convertToFormat(QImage.Format_RGBX8888)

    def luma_rows(img):
        """Rows of 0..255 lumas (sum of RGB / 3)."""
        data = bytes(img.constBits())[:img.sizeInBytes()]
        stride = img.bytesPerLine()
        rows = []
        for y in range(img.height()):
            row = data[y * stride:y * stride + img.width() * 4]
            rows.append([(row[i] + row[i + 1] + row[i + 2]) / 3.0 for i in range(0, len(row), 4)])
        return rows

    def settled(gl, state):
        """True once two grabs a beat apart are identical and not a flat fill."""
        img = grab(gl)
        data = bytes(img.constBits())[:img.sizeInBytes()]
        previous = state.get("data")
        state["data"] = data
        state["img"] = img
        return previous == data and len(set(data[0:len(data):997])) > 4

    def wait_settled(gl, what):
        state = {}

        def poll():
            QApplication.processEvents()
            return settled(gl, state)

        ok = yield ("until", poll, RENDER_TIMEOUT_S)
        check(ok, "%s: the viewer settles on an image" % what)
        return state.get("img")

    def wait_readout(param, what):
        ok = yield ("until", lambda: "whole frame" in (param.getValue() or ""), RENDER_TIMEOUT_S)
        check(ok, "%s: the variance readout is filled" % what, repr(param.getValue()))
        yield ("sleep", SETTLE_MS)
        return param.getValue() or ""

    def seams(rows):
        """Column and row boundaries whose mean step stands out from the steps around them."""
        h = len(rows)
        w = len(rows[0]) if rows else 0
        found = []
        col = [0.0] * (w - 1)
        for y in range(EDGE, h - EDGE, 2):
            r = rows[y]
            for x in range(w - 1):
                col[x] += abs(r[x + 1] - r[x])
        n = max(1, len(range(EDGE, h - EDGE, 2)))
        col = [c / n for c in col]
        row = []
        for y in range(h - 1):
            a, b = rows[y], rows[y + 1]
            row.append(sum(abs(b[x] - a[x]) for x in range(EDGE, w - EDGE, 2)) / max(1, len(range(EDGE, w - EDGE, 2))))
        for axis, steps in (("column", col), ("row", row)):
            for i in range(EDGE, len(steps) - EDGE):
                around = sorted(steps[i - 8:i] + steps[i + 1:i + 9])
                local = around[len(around) // 2]
                if steps[i] > SEAM_RATIO * local + SEAM_FLOOR:
                    found.append((axis, i, round(steps[i], 2), round(local, 2)))
        return found

    def overlap(before, after, delta):
        """Mean and max luma difference over the area both views show, at the best shift near delta."""
        h = len(before)
        w = len(before[0])
        best = None
        for sx in (delta[0], -delta[0]):
            for sy in (delta[1], -delta[1]):
                for ex in (-1, 0, 1):
                    for ey in (-1, 0, 1):
                        dx, dy = sx + ex, sy + ey
                        diffs = []
                        for y in range(max(EDGE, -dy + EDGE), min(h - EDGE, h - dy - EDGE), 7):
                            for x in range(max(EDGE, -dx + EDGE), min(w - EDGE, w - dx - EDGE), 7):
                                diffs.append(abs(after[y + dy][x + dx] - before[y][x]))
                        if diffs:
                            mean = sum(diffs) / len(diffs)
                            if best is None or mean < best[0]:
                                best = (mean, max(diffs), (dx, dy))
        return best

    def save(img, name):
        path = os.path.join(gt.OUT, name + ".png")
        if img is not None and img.save(path):
            report("SHOT " + path)

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "a viewer is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        plate = app.createNode("Noise")
        require(plate is not None, "create a Noise", gt.plugin_problem("Noise"))
        set_choice(plate.getParam("extent"), "size")
        plate.getParam("bottomLeft").set(0.0, 0.0)
        plate.getParam("size").set(float(PLATE_W), float(PLATE_H))
        set_choice(plate.getParam("type"), "perlin")
        plate.getParam("noiseSize").set(180.0, 180.0)
        gt.close_all_panels()

        tab = next(iter(visible_of("ViewerTab")), None)
        zoom = widget_with_tip("ComboBox", "Zoom:")
        view = widget_with_tip("ComboBox", "View:")
        require(None not in (tab, zoom, view), "found the viewer tab, zoom and view menus", repr([tab, zoom, view]))
        picked = choose(view, "Un-tone-mapped") or choose(view, "Raw") or choose(view, "un-tone", exact=False)
        check(picked, "the viewer offers an un-tone-mapped view", repr(combo_items(view)))
        gl = visible_of("ViewerGL")[0] if visible_of("ViewerGL") else None
        require(gl is not None, "the viewer is visible")

        def show(node):
            if viewer.getInput(0) is not None:
                viewer.disconnectInput(0)
            require(viewer.connectInput(0, node), "the Viewer shows " + node.getScriptName())

        def zoom_to(text):
            require(choose(zoom, text), "the zoom menu offers " + text, repr(combo_items(zoom)))
            QApplication.processEvents()

        gt.section("compute variance")
        denoise = app.createNode("DenoiseProfiled")
        require(denoise is not None, "create a DenoiseProfiled", gt.plugin_problem("DenoiseProfiled"))
        gt.close_all_panels()
        require(denoise.connectInput(0, plate), "DenoiseProfiled reads the plate")
        set_choice(denoise.getParam("mode"), "computeVariance")
        readout = denoise.getParam("variance")
        require(readout is not None, "DenoiseProfiled has a variance readout")
        show(denoise)
        zoom_to("100%")
        img = yield from wait_settled(gl, "variance at 100%")
        first = yield from wait_readout(readout, "variance at 100%")
        save(img, "m268_variance_100")
        report("NOTE variance readout: " + first)
        check("%d×%d" % (PLATE_W, PLATE_H) in first, "the readout names the whole %dx%d frame" % (PLATE_W, PLATE_H), first)
        check("at 100%" in first, "the readout names the 100% scale", first)

        win.activateWindow()
        pan(gl, PAN)
        img = yield from wait_settled(gl, "variance after a pan")
        save(img, "m268_variance_panned")
        panned = yield from wait_readout(readout, "variance after a pan")
        check(panned == first, "panning at 100% leaves the readout unchanged", "%r -> %r" % (first, panned))

        zoom_to("200%")
        yield from wait_settled(gl, "variance at 200%")
        report("NOTE variance readout at 200%: " + (readout.getValue() or ""))
        zoom_to("100%")
        yield from wait_settled(gl, "variance back at 100%")
        back = yield from wait_readout(readout, "variance back at 100%")
        check(back == first, "zooming to 200% and back to 100% leaves the readout unchanged", "%r -> %r" % (first, back))

        gt.section("contrast texture")
        contrast = app.createNode("ContrastTexture")
        require(contrast is not None, "create a ContrastTexture", gt.plugin_problem("ContrastTexture"))
        gt.close_all_panels()
        require(contrast.connectInput(0, plate), "ContrastTexture reads the plate")
        for name in ("coarseDetails", "mediumDetails", "fineDetails"):
            contrast.getParam(name).set(1.5)
        show(contrast)
        zoom_to("100%")
        before_img = yield from wait_settled(gl, "contrast at 100%")
        save(before_img, "m268_contrast_100")
        require(before_img is not None, "grabbed the ContrastTexture view")
        before = luma_rows(before_img)
        found = seams(before)
        check(not found, "no tile seams at 100%", repr(found[:10]))

        win.activateWindow()
        pan(gl, PAN)
        after_img = yield from wait_settled(gl, "contrast after a pan")
        save(after_img, "m268_contrast_panned")
        require(after_img is not None, "grabbed the panned ContrastTexture view")
        after = luma_rows(after_img)
        found = seams(after)
        check(not found, "no tile seams after a pan at 100%", repr(found[:10]))
        best = overlap(before, after, PAN)
        require(best is not None, "the two views overlap")
        report("NOTE overlap shift %r, mean %.3f, max %d" % (best[2], best[0], best[1]))
        check(best[0] <= OVERLAP_MEAN and best[1] <= OVERLAP_MAX,
              "the area seen before and after the pan shows the same pixels",
              "mean %.3f max %d at shift %r" % best)

        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        for node in (contrast, denoise, plate):
            node.destroy()
        yield ("sleep", 200)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
            f.write("FAIL script error: " + traceback.format_exc() + "\n")
        os._exit(1)
