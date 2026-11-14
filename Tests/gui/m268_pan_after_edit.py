"""Panning right after an edit never stalls the viewer, on a 3840x2160 synthetic Noise plate.

For Tone Equalizer (settings panel open, so its cursor readout is live), Contrast And Texture and
Profiled Denoise in Wavelets mode, each in turn on the viewer at 100%:

- Edit: change one knob, then at once hover the viewer and middle-drag pan it in PAN_STEPS steps
  PEN_INTERVAL_MS apart, while the edited whole-frame render is still running. With
  NATRON_STALL_LOG set, no GUI-thread stall over STALL_LIMIT_MS may start during the hover and
  pan. Once the renders land, no row or column of the viewer shows a step that its neighbours do
  not (a tile seam).
- Pan: from the settled view, the same drag again; the screen grabbed straight after the release
  shows the settled pixels moved by the pan wherever both views reach (the old image moved, not a
  frozen or stretched frame), and that drag does not stall either.

Screenshots go to $NATRON_GUI_TEST_OUT as m268_pan_<node>_*.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m268-shots/pan -e GUI_TEST_TIMEOUT=900 \\
        -e NATRON_STALL_LOG=build/m268-shots/pan/stalls.jsonl \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m268_pan_after_edit.py build/fast/App/Natron
"""
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

RENDER_TIMEOUT_S = 180.0
STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 900
SETTLE_MS = 500
# A whole-frame compute holds the old picture on screen for seconds, so a shorter hold settles on it.
SETTLED_HOLD_S = 10.0
PLATE_W = 3840
PLATE_H = 2160
PAN = (180, 100)
PAN_STEPS = 20
PEN_INTERVAL_MS = 16
HOVER_STEPS = 5
STALL_LIMIT_MS = 50.0
STALL_FLUSH_MS = 200
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
EDGE = 6  # Viewer border pixels left out of the seam scan and the overlap.
SEAM_RATIO = 3.0
SEAM_FLOOR = 2.0
# The Tone Equalizer's cursor readout is drawn over the image, so only the mean is checked.
OVERLAP_MEAN = 2.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication, QImage, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import report, require

    def check(ok, label, detail=""):
        """gt.check() with require()'s detail, shown only on failure."""
        return gt.check(ok, label + (" (" + detail + ")" if detail and not ok else ""))

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def mono_ms():
        # StallMonitor stamps std::chrono::steady_clock, which is this clock on Linux.
        return time.monotonic() * 1000.0

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
        QApplication.processEvents()

    def hover_and_pan(gl, delta):
        """Hovers then middle-drags by delta, PEN_INTERVAL_MS between events; yields the sleeps and
        returns the start and end of the gesture on the stall log's clock."""
        start = gl.rect().center()
        t_start = mono_ms()
        for i in range(HOVER_STEPS):
            mouse(gl, QEvent.MouseMove, QPoint(start.x() - 4 * (HOVER_STEPS - i), start.y()), Qt.NoButton, Qt.NoButton)
            yield ("sleep", PEN_INTERVAL_MS)
        mouse(gl, QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton)
        for i in range(1, PAN_STEPS + 1):
            yield ("sleep", PEN_INTERVAL_MS)
            p = QPoint(start.x() + delta[0] * i // PAN_STEPS, start.y() + delta[1] * i // PAN_STEPS)
            mouse(gl, QEvent.MouseMove, p, Qt.NoButton, Qt.MiddleButton)
        end = QPoint(start.x() + delta[0], start.y() + delta[1])
        mouse(gl, QEvent.MouseButtonRelease, end, Qt.MiddleButton, Qt.NoButton)
        return t_start, mono_ms()

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
        """True once the viewer has held the same picture, not a flat fill, for SETTLED_HOLD_S."""
        img = grab(gl)
        data = bytes(img.constBits())[:img.sizeInBytes()]
        now = time.time()
        if data != state.get("data"):
            state["data"] = data
            state["since"] = now
        state["img"] = img
        return now - state["since"] >= SETTLED_HOLD_S and len(set(data[0:len(data):997])) > 4

    def wait_settled(gl, what):
        state = {}

        def poll():
            QApplication.processEvents()
            return settled(gl, state)

        ok = yield ("until", poll, RENDER_TIMEOUT_S)
        check(ok, "%s: the viewer settles on an image" % what)
        return state.get("img")

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

    def check_no_stall(what, t_start, t_end):
        if not STALL_LOG:
            report("NOTE NATRON_STALL_LOG is not set; %s: stalls were not checked" % what)
            return
        # The monitor writes a stall once it ends; give it time to flush.
        yield ("sleep", STALL_FLUSH_MS)
        mine = [round(d, 1) for s, d in read_stalls() if t_start <= s <= t_end and d > STALL_LIMIT_MS]
        report("NOTE %s: gesture took %.0f ms" % (what, t_end - t_start))
        check(not mine, "%s: no GUI-thread stall over %d ms" % (what, STALL_LIMIT_MS), repr(mine))

    def save(img, name):
        path = os.path.join(gt.OUT, name + ".png")
        if img is not None and img.save(path):
            report("SHOT " + path)

    def graph_view():
        views = visible_of("NodeGraph")
        return views[0] if views else None

    def steps():
        # A fresh project has no Viewer node, so no viewer shows until one is created.
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
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
        plate = app.createNode("Noise")
        require(plate is not None, "create a Noise", gt.plugin_problem("Noise"))
        set_choice(plate.getParam("extent"), "size")
        plate.getParam("bottomLeft").set(0.0, 0.0)
        plate.getParam("size").set(float(PLATE_W), float(PLATE_H))
        set_choice(plate.getParam("type"), "perlin")
        plate.getParam("noiseSize").set(180.0, 180.0)
        gt.close_all_panels()

        zoom = widget_with_tip("ComboBox", "Zoom:")
        view = widget_with_tip("ComboBox", "View:")
        require(None not in (zoom, view), "found the viewer's zoom and view menus", repr([zoom, view]))
        picked = choose(view, "Un-tone-mapped") or choose(view, "Raw") or choose(view, "un-tone", exact=False)
        check(picked, "the viewer offers an un-tone-mapped view", repr(combo_items(view)))
        gl = visible_of("ViewerGL")[0] if visible_of("ViewerGL") else None
        require(gl is not None, "the viewer is visible")

        def show(node):
            if viewer.getInput(0) is not None:
                viewer.disconnectInput(0)
            require(viewer.connectInput(0, node), "the Viewer shows " + node.getScriptName())

        def create(plugin, panel_open):
            props = {}
            if panel_open:
                # An open settings panel makes the node's viewer interact the active one.
                props["CreateNodeArgsPropSettingsOpened"] = NatronEngine.BoolNodeCreationProperty(True)
            node = app.createNode(plugin, -1, None, props)
            require(node is not None, "create a " + plugin, gt.plugin_problem(plugin))
            require(node.connectInput(0, plate), plugin + " reads the plate")
            return node

        def tone_equalizer():
            node = create("ToneEqualizer", True)
            return node, lambda: node.getParam("midtones").set(0.8)

        def contrast_texture():
            node = create("ContrastTexture", False)
            gt.close_all_panels()
            for name in ("coarseDetails", "mediumDetails"):
                node.getParam(name).set(1.0)
            return node, lambda: node.getParam("fineDetails").set(1.5)

        def denoise_wavelets():
            # Profiled Denoise warns over the top of the viewer about an input that is not a raw
            # camera's and about a camera it has no profile for, which the seam checks would read
            # as the frame. A camera make on the plate and the generic profile leave nothing to warn of.
            camera = create("MetadataModify", False)
            made.append(camera)
            set_choice(camera.getParam("operation1"), "set")
            camera.getParam("key1").set("raw/make")
            set_choice(camera.getParam("type1"), "string")
            camera.getParam("stringValue1").set("Canon")
            node = create("DenoiseProfiled", False)
            node.disconnectInput(0)
            require(node.connectInput(0, camera), "DenoiseProfiled reads the plate with a camera make")
            gt.close_all_panels()
            set_choice(node.getParam("profile"), "generic")
            set_choice(node.getParam("mode"), "wavelets")
            return node, lambda: node.getParam("strength").set(2.0)

        made = []
        for name, make in (("toneequalizer", tone_equalizer), ("contrast", contrast_texture),
                           ("denoise", denoise_wavelets)):
            gt.section(name)
            node, edit = make()
            made.append(node)
            show(node)
            # The pans of the earlier nodes moved the view; fitting first keeps this node's view
            # inside the plate, so its edges are not taken for tile seams.
            QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyPress, Qt.Key_F, Qt.NoModifier))
            QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyRelease, Qt.Key_F, Qt.NoModifier))
            QApplication.processEvents()
            require(choose(zoom, "100%"), "the zoom menu offers 100%", repr(combo_items(zoom)))
            QApplication.processEvents()
            first = yield from wait_settled(gl, name + " before the edit")
            save(first, "m268_pan_%s_before" % name)

            win.activateWindow()
            edit()
            t_start, t_end = yield from hover_and_pan(gl, PAN)
            save(grab(gl), "m268_pan_%s_released" % name)
            yield from check_no_stall(name + ": hover and pan right after an edit", t_start, t_end)
            edited_img = yield from wait_settled(gl, name + " after the edit and pan")
            save(edited_img, "m268_pan_%s_edited" % name)
            require(edited_img is not None, name + ": grabbed the edited view")
            edited = luma_rows(edited_img)
            found = seams(edited)
            check(not found, name + ": no tile seams once the edited renders land", repr(found[:10]))

            win.activateWindow()
            t_start, t_end = yield from hover_and_pan(gl, PAN)
            moved_img = grab(gl)
            save(moved_img, "m268_pan_%s_moved" % name)
            yield from check_no_stall(name + ": a second pan", t_start, t_end)
            best = overlap(edited, luma_rows(moved_img), PAN)
            require(best is not None, name + ": the two views overlap")
            report("NOTE %s: overlap shift %r, mean %.3f, max %d" % (name, best[2], best[0], best[1]))
            check(best[0] <= OVERLAP_MEAN, name + ": the frame right after the pan is the settled one moved by the pan",
                  "mean %.3f max %d at shift %r" % best)
            yield from wait_settled(gl, name + " after the second pan")

        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        for node in made + [plate]:
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
