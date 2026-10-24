"""Develop RAW in the interface: a camera raw file dropped on the node graph becomes a Read with
RawPrepare, RawTemperature, RawHighlights, RawDemosaic, RawInputColor and RawColorCalibration below
it, connected in that order inside a backdrop titled "Develop RAW". One undo removes the Read, the chain and the
backdrop together, and one redo brings them all back. Python's createReader(), a new filename on an
existing Read, the preference turned off and a project reload add no chain.

With the fixtures checked out, the Bayer CR2, the X-Trans RAF and the linear DNG are each dropped
and then:
  - the Viewer fed by a Read shows the CR2's mosaic as grey with "RAW mosaic" on its info line,
    and the DNG with three channels and "RAW linear", with no error either way; a raw edge in the
    middle of the chain (RawHighlights) also shows as a mosaic;
  - the end of each chain shows a plausible colour image: lit, with some chroma, not magenta;
  - every Bayer and X-Trans demosaic method renders without an error;
  - a lower temperature on RawTemperature turns the image bluer, a higher one yellower;
  - RawHotPixels, RawCACorrect and RawDenoise spliced into the Bayer chain render;
  - RawToImage turns the mosaic into a grey image that the info line no longer calls raw;
  - the preference turned off, a dropped CR2 gets no chain;
  - each raw node's properties panel shows no cut-off label and every visible knob has a tooltip;
  - every highlights mode, Bayer and X-Trans demosaic mode, white balance preset, adaptation and
    illuminant shows only the knobs it uses; Flat Field stays hidden for a file without gain maps;
  - RawTemperature picks its white balance from an area, and RawColorCalibration runs Pick in
    correction and measure mode, Calibrate and Validate on its chart corners, and applies a preset.
Without the fixtures (a checkout without Git LFS has pointer files) the dropped file is an empty
.cr2 and those sections are skipped.

The Viewer scales a raw-stage edge to 0-1 for display (black and white levels from its metadata),
so a Read shows a visible mosaic at the default gain; RawToImage keeps the sensor's values.
The demosaic methods are shown at 200%.

Shots, under $NATRON_GUI_TEST_OUT: m80-develop-raw.png and m80-develop-all.png (the node graph),
m80-viewer-*.png (the window with the Viewer on each state, including the area and chart overlays),
m80-preference-off.png, m80-panel-<node>.png (each panel) and m80-panel-<node>-<mode>.png (a panel
per group of modes, and each RawColorCalibration tab).

Usage: Tests/gui/run-gui-test.sh Tests/gui/m80_raw.py
"""
import os
import re
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
RENDER_TIMEOUT_S = 180.0
START_WAIT_S = 5.0
SETTLE_MS = 500

READ_ID = "fr.inria.built-in.Read"
BACKDROP_ID = "fr.inria.built-in.BackDrop"
CHAIN_IDS = ["fr.natron.RawPrepare", "fr.natron.RawTemperature", "fr.natron.RawHighlights",
             "fr.natron.RawDemosaic", "fr.natron.RawInputColor", "fr.natron.RawColorCalibration"]
EXTRA_IDS = ["fr.natron.RawCACorrect", "fr.natron.RawHotPixels", "fr.natron.RawDenoise"]
TO_IMAGE_ID = "fr.natron.RawToImage"
BAYER_METHODS = ["rcd", "ppg", "amaze", "vng4"]
XTRANS_METHODS = ["markesteijn1", "markesteijn3", "vng"]
BAYER_MODES = ["rcd", "ppg", "amaze", "vng4", "passthroughMonochrome", "photositeColor", "lmmse", "rcdDual", "amazeDual"]
XTRANS_MODES = ["markesteijn1", "markesteijn3", "vng", "passthroughMonochrome", "photositeColor", "markesteijn3Dual", "fdc"]
PASSTHROUGH_MODES = ["passthroughMonochrome", "photositeColor"]
CAPTURE_KNOBS = ["captureSharpenControls", "captureIterations", "captureRadius", "captureContrastSensitivity",
                 "captureContrastSensitivityOverride", "captureCornerBoost"]
HIGHLIGHT_MODES = ["clip", "unclipped", "opposed", "lch", "inpaint", "segments", "laplacian"]
RECOVERY_MODES = ["off", "generic", "flatGeneric", "small", "large", "flatSmall", "flatLarge"]
TUNED_CR2_PRESETS = ["Daylight", "Shade", "Cloudy", "Tungsten", "White Fluorescent", "Flash"]
ADAPTATIONS = ["linearBradford", "cat16", "nonLinearBradford", "xyz", "none"]
ILLUMINANT_KNOBS = ["illuminant", "approxCct", "temperature", "illumFluo", "illumLed", "illumHue", "illumChroma"]
CHECKER_CORNERS = [("checkerTopLeft", 0.3, 0.7), ("checkerTopRight", 0.7, 0.7),
                   ("checkerBottomRight", 0.7, 0.3), ("checkerBottomLeft", 0.3, 0.3)]

try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, QMimeData, QPoint, QPointF, QRect, Qt, QUrl
    from PySide6.QtGui import QDragEnterEvent, QDropEvent, QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QPushButton, QScrollArea, QTabWidget, QWidget

    import guitest as gt
    from guitest import check, report, require

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures")
    RAW_DIR = os.path.join(gt.FIXTURES, "raw")
    CR2 = os.path.join(RAW_DIR, "canon-eos-5d-mark-iii.cr2")
    RAF = os.path.join(RAW_DIR, "fujifilm-x-pro2-14bit-uncompressed.raf")
    DNG = os.path.join(RAW_DIR, "canon-eos-5d-mark-iii-linear-rgb.dng")

    def make_raw_file(name):
        """An empty file with a camera raw extension."""
        os.makedirs(FIXTURE_DIR, exist_ok=True)
        path = os.path.join(FIXTURE_DIR, name)
        open(path, "wb").close()
        return path

    def is_real_fixture(path):
        """A decodable fixture rather than a Git LFS pointer file or an empty stand-in."""
        return os.path.exists(path) and os.path.getsize(path) > (1 << 20)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible() and gt.class_name(w) == "Gui":
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def nodes_of(owner, plugin_id):
        return [n for n in owner.getChildren() if n.getPluginID() == plugin_id]

    def drop_file(path, fx=0.5, fy=0.5):
        """Drags path from a file browser onto the node graph at (fx, fy) of its size. Qt 6
        delivers a drop only to the widget the drag entered, so the drop is preceded by its enter.
        Nothing is selected first: a node made while another is selected goes next to that one
        instead of where it was dropped."""
        app.clearSelection()
        win = main_window()
        graph = root_graph()
        require(win is not None and graph is not None, "the main window and its node graph are up")
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(path)])
        local = QPoint(int(graph.width() * fx), int(graph.height() * fy))
        at = QPointF(win.mapFromGlobal(graph.mapToGlobal(local)))
        QApplication.sendEvent(win, QDragEnterEvent(at.toPoint(), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier))
        QApplication.sendEvent(win, QDropEvent(at, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier))

    def read_of(path):
        for n in nodes_of(app, READ_ID):
            if n.getParam("filename").getValue() == path:
                return n
        return None

    def chain_below(read):
        """The develop chain hanging off read, in order, or None."""
        chain = []
        previous = read
        for pid in CHAIN_IDS:
            found = [n for n in nodes_of(app, pid) if is_input(n, 0, previous)]
            if len(found) != 1:
                return None
            chain.append(found[0])
            previous = found[0]
        return chain

    def is_input(node, index, source):
        inp = node.getInput(index)
        return inp is not None and source is not None and inp.getScriptName() == source.getScriptName()

    def inside(node, backdrop):
        x, y = node.getPosition()
        w, h = node.getSize()
        bx, by = backdrop.getPosition()
        bw, bh = backdrop.getSize()
        return bx <= x and by <= y and x + w <= bx + bw and y + h <= by + bh

    def frame_graph(nodes, margin=60.0):
        graph = root_graph()
        if graph is None or not nodes:
            return
        boxes = [(n.getPosition(), n.getSize()) for n in nodes]
        x0 = min(p[0] for p, s in boxes)
        y0 = min(p[1] for p, s in boxes)
        x1 = max(p[0] + s[0] for p, s in boxes)
        y1 = max(p[1] + s[1] for p, s in boxes)
        vp = graph.viewport()
        zoom = min(1.0, vp.width() / (x1 - x0 + 2 * margin), vp.height() / (y1 - y0 + 2 * margin))
        graph.resetTransform()
        graph.scale(zoom, zoom)
        graph.centerOn((x0 + x1) / 2.0, (y0 + y1) / 2.0)
        QApplication.processEvents()

    def trigger_edit(name):
        """Undo and Redo are QActions on the Edit menu, matched by Qt's shortcut map before a key
        event sent to a widget would reach them, so the action itself is triggered."""
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = act.menu()
                if menu is None or not act.text().lower().startswith("edit"):
                    continue
                for a in menu.actions():
                    if a.text().lower().startswith(name) and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        return check(False, "found an enabled %s action in the Edit menu" % name)

    def info_line_texts():
        texts = []
        for w in gt.widgets_of("InfoViewerWidget"):
            if w.isVisible():
                texts.extend(label.text() for label in w.findChildren(QLabel) if label.isVisible() and label.text())
        return texts

    def info_line_has(text):
        return any(text in t for t in info_line_texts())

    def viewer_widget():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def viewer_samples():
        """The colours on screen over the middle half of the viewer, on a 48x48 grid."""
        w = viewer_widget()
        if w is None:
            return []
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        origin = w.mapToGlobal(QPoint(0, 0))
        samples = []
        for j in range(48):
            for i in range(48):
                x = origin.x() + int(w.width() * (0.25 + 0.5 * i / 47.0))
                y = origin.y() + int(w.height() * (0.25 + 0.5 * j / 47.0))
                c = image.pixelColor(x, y)
                samples.append((c.red(), c.green(), c.blue()))
        return samples

    def stats(samples):
        n = float(max(1, len(samples)))
        mean = tuple(sum(s[c] for s in samples) / n for c in range(3))
        chroma = sum(1 for s in samples if max(s) - min(s) > 12) / n
        spread = max((max(s) - min(s) for s in samples), default=0)
        return {"mean": tuple(round(m, 1) for m in mean), "chroma": round(chroma, 3), "spread": spread}

    def plausible_colour(st):
        r, g, b = st["mean"]
        magenta = r > g + 25 and b > g + 25
        return 12 < (r + g + b) / 3.0 < 245 and st["chroma"] > 0.02 and not magenta

    def greyscale(st):
        return st["spread"] <= 3 and sum(st["mean"]) > 3

    def refresh_icon_key():
        """The cache key of the icon on the viewer's refresh button, or None. Read while the child
        list is alive: PySide drops the wrapper of a widget found through it once the list goes."""
        for tab in gt.widgets_of("ViewerTab"):
            if not tab.isVisible():
                continue
            children = tab.findChildren(QPushButton)
            for b in children:
                try:
                    tip = " ".join(re.sub(r"<[^>]*>|&nbsp;|\xa0", " ", b.toolTip() or "").split())
                    if "new render" in tip:
                        return b.icon().cacheKey()
                except RuntimeError:
                    continue
        return None

    idle_icon = {}

    def viewer_rendering():
        """The viewer swaps its refresh button's icon while any render for it is under way."""
        key = refresh_icon_key()
        if key is None:
            return False
        idle_icon.setdefault("key", key)
        return key != idle_icon["key"]

    def wait_render(before, timeout=RENDER_TIMEOUT_S):
        """Waits for the render that a change started to end and the viewer to show its result:
        the picture differs from before (None when no change is due) or a render was seen, then
        no render runs and the picture holds for a second."""
        deadline = time.time() + timeout
        started_by = time.time() + START_WAIT_S
        while before is not None and time.time() < started_by and not viewer_rendering() and viewer_samples() == before:
            yield ("sleep", 100)
        previous = None
        same = 0
        while time.time() < deadline:
            yield ("sleep", 250)
            current = viewer_samples()
            same = same + 1 if (current and current == previous and not viewer_rendering()) else 0
            previous = current
            if same >= 4:
                return True
        return False

    viewer_holder = {}

    def show_in_viewer(node):
        viewer = viewer_holder.get("v")
        if viewer is None:
            viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
            require(viewer is not None, "a Viewer node exists")
            viewer.setPosition(-400, 0)
            viewer_holder["v"] = viewer
        current = viewer.getInput(0)
        if current is not None and current.getScriptName() == node.getScriptName():
            return
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer accepts " + node.getScriptName())

    def view(node, label, shot_name):
        """Shows node in the viewer, waits for the render and returns its colour statistics."""
        gt.close_all_panels()
        before = viewer_samples()
        show_in_viewer(node)
        ok = yield from wait_render(before)
        check(ok, "%s: the viewer settles on a picture" % label)
        messages = gt.viewer_messages()
        check(not messages, "%s: the viewer shows no error (%r)" % (label, messages))
        st = stats(viewer_samples())
        report("NOTE %s: viewer mean RGB %r, chroma fraction %.3f, max spread %d, info %r"
               % (label, st["mean"], st["chroma"], st["spread"], info_line_texts()))
        gt.shot(shot_name)
        return st

    def set_choice(node, name, option):
        param = node.getParam(name)
        options = [param.getOption(i) for i in range(param.getNumOptions())]
        require(option in options, "%s offers %s (%r)" % (name, option, options))
        param.setValue(options.index(option))

    def set_gain(fstops):
        """Sets the viewer's gain as typing in its f-stop box does."""
        for tab in gt.widgets_of("ViewerTab"):
            if not tab.isVisible():
                continue
            for box in tab.findChildren(QWidget):
                try:
                    if gt.class_name(box) == "SpinBox" and "f-stops" in (box.toolTip() or ""):
                        box.setValue(float(fstops))
                        QMetaObject.invokeMethod(tab, "onGainSpinBoxValueChanged", Q_ARG(float, float(fstops)))
                        return True
                except RuntimeError:
                    continue
        return check(False, "found the viewer's gain box")

    def set_zoom(text):
        """Picks text ("200%", "Fit"...) in the viewer's zoom menu."""
        for combo in gt.widgets_of("ComboBox"):
            try:
                if not combo.isVisible() or "Zoom:" not in (combo.toolTip() or ""):
                    continue
                items = [str(i).strip() for i in (combo.property("natronItems") or [])]
            except RuntimeError:
                continue
            if text in items:
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(text)))
                return True
            return check(False, "the zoom menu offers %s (%r)" % (text, items))
        return check(False, "found the viewer's zoom menu")

    def luma_spread(samples):
        lumas = sorted(sum(s) / 3.0 for s in samples)
        return lumas[int(len(lumas) * 0.95)] - lumas[int(len(lumas) * 0.05)] if lumas else 0

    def settle_counts():
        return {pid: len(nodes_of(app, pid)) for pid in [READ_ID, BACKDROP_ID] + CHAIN_IDS}

    def node_graph_view():
        return root_graph()

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

    def page_of(param):
        parent = param.getParent()
        while parent is not None and parent.getTypeName() != "Page":
            parent = parent.getParent()
        return parent.getLabel() if parent is not None else "?"

    def panel_section(node):
        name = node.getPluginID().split(".")[-1]
        gt.close_all_panels()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        if not check(ok, "%s: its properties panel opens" % name):
            return
        yield ("sleep", 800)
        panel = panel_of(node)
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(min(content.sizeHint().height() + 4, 900))
        yield ("sleep", 500)
        params = [p for p in node.getParams() if p.getTypeName() not in ("Page", "Separator")]
        own = [p for p in params if page_of(p) == "Controls"]
        own_labels = set(p.getLabel() for p in own)
        cut = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip() and not w.wordWrap() and w.width() < w.sizeHint().width():
                    cut.append((w.text().strip(), w.width(), w.sizeHint().width()))
            except RuntimeError:
                continue
        check(not [c for c in cut if c[0] in own_labels], "%s: none of its own labels is cut off (%r)" % (name, cut))
        if [c for c in cut if c[0] not in own_labels]:
            report("NOTE %s: cut-off labels outside its own knobs, on the page the panel opened on: %r" % (name, cut))
        report("NOTE %s: Controls page knobs %r" % (name, [(p.getLabel(), p.getTypeName(), p.getIsVisible()) for p in own]))
        report("NOTE %s: other pages %r" % (name, sorted(set(page_of(p) for p in params) - {"Controls"})))
        missing = [p.getScriptName() for p in own if p.getIsVisible() and not p.getHelp().strip()]
        check(not missing, "%s: every visible knob has a tooltip (missing on %r)" % (name, missing))
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size()).intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, "m80-panel-%s.png" % name)
        report(("SHOT " if screen.copy(rect).save(path) else "NOTE could not save ") + path)
        for area in panel.findChildren(QScrollArea):
            area.setMinimumHeight(0)

    def options_of(node, name):
        param = node.getParam(name)
        return [param.getOption(i) for i in range(param.getNumOptions())] if param is not None else []

    def choice_id(node, name):
        param = node.getParam(name)
        return param.getOption(param.getValue()) if param is not None else None

    def try_choice(node, name, option):
        """set_choice that reports a missing option as a failure instead of ending the run."""
        options = options_of(node, name)
        if not check(option in options, "%s.%s offers %s (%r)" % (node.getScriptName(), name, option, options)):
            return False
        node.getParam(name).setValue(options.index(option))
        return True

    def expect_visible(node, label, expected):
        """One PASS/FAIL line: every knob in expected is shown (True) or hidden (False)."""
        wrong = []
        for name in sorted(expected):
            param = node.getParam(name)
            if param is None or param.getIsVisible() != expected[name]:
                wrong.append(name)
        shown = [n for n in sorted(expected) if expected[n]]
        hidden = [n for n in sorted(expected) if not expected[n]]
        return check(not wrong, "%s: shows %r, hides %r (wrong: %r)" % (label, shown, hidden, wrong))

    def image_box(node):
        """The node's region of definition as (x, y, width, height), in pixels."""
        try:
            rod = node.getRegionOfDefinition(app.timelineGetTime(), 0)
            box = (rod.left(), rod.bottom(), rod.right() - rod.left(), rod.top() - rod.bottom())
            if box[2] > 1 and box[3] > 1:
                return box
        except Exception:
            report("NOTE %s: no region of definition: %s" % (node.getScriptName(), traceback.format_exc().splitlines()[-1]))
        return (0.0, 0.0, 1000.0, 1000.0)

    def open_panel(node):
        gt.close_all_panels()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        check(ok, "%s: its properties panel opens" % node.getScriptName())
        yield ("sleep", 800)
        return ok

    def panel_shot(node, name):
        """Saves the open panel of node, grown to show its knobs, as name."""
        yield ("sleep", 300)
        panel = panel_of(node)
        if not check(panel is not None, "%s: its panel is open for %s" % (node.getScriptName(), name)):
            return
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(min(content.sizeHint().height() + 4, 900))
        yield ("sleep", 500)
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size()).intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if screen.copy(rect).save(path) else "NOTE could not save ") + path)
        for area in panel.findChildren(QScrollArea):
            area.setMinimumHeight(0)

    tab_widgets_alive = []

    def knob_tabs(node):
        """The tab widget of the node's tab groups (not the panel's own page tabs), or None. The findChildren list is kept
        in tab_widgets_alive: PySide invalidates the wrappers when the list is freed, so a widget returned from here would be dead."""
        panel = panel_of(node)
        if panel is None:
            return None
        tab_widgets_alive[:] = [panel] + panel.findChildren(QTabWidget)
        for w in tab_widgets_alive[1:]:
            try:
                if w.parent() is not None and gt.class_name(w.parent()) == "TabGroup":
                    return w
            except RuntimeError:
                continue
        return None

    def select_tab(node, index):
        tabs = knob_tabs(node)
        if tabs is not None:
            tabs.setCurrentIndex(index)

    def highlights_section(highlights):
        saved = {n: highlights.getParam(n).getValue() for n in ("mode", "recovery")}
        yield from open_panel(highlights)
        for mode in HIGHLIGHT_MODES:
            if not try_choice(highlights, "mode", mode):
                continue
            segments = mode == "segments"
            laplacian = mode == "laplacian"
            for recovery in (RECOVERY_MODES if segments else [None]):
                if recovery is not None and not try_choice(highlights, "recovery", recovery):
                    continue
                recovering = segments and recovery != "off"
                label = "RawHighlights %s%s" % (mode, (", recovery " + recovery) if segments else "")
                expect_visible(highlights, label, {
                    "combine": segments, "candidating": segments, "recovery": segments,
                    "strength": recovering, "noiseLevel": recovering or laplacian,
                    "iterations": laplacian, "solidColor": laplacian, "scales": laplacian})
                if mode in ("clip", "laplacian") or recovery in ("off", "generic"):
                    yield from panel_shot(highlights, "m80-panel-RawHighlights-%s%s.png" % (mode, ("-" + recovery) if segments else ""))
        for name, value in saved.items():
            highlights.getParam(name).setValue(value)

    def demosaic_section(demosaic, sensor, param, modes):
        bayer = param == "method"
        names = ["captureSharpen", "captureCornerBoost"]
        saved = {n: demosaic.getParam(n).getValue() for n in [param] + names}
        yield from open_panel(demosaic)
        for mode in modes:
            if not try_choice(demosaic, param, mode):
                continue
            passing = mode in PASSTHROUGH_MODES
            for capture in (False, True):
                demosaic.getParam("captureSharpen").setValue(capture)
                shown = capture and not passing
                expected = {"method": bayer, "xtransMethod": not bayer, "dualThreshold": mode.endswith("Dual"),
                            "lmmseRefine": mode == "lmmse", "captureSharpen": not passing,
                            "captureSharpCenter": shown and demosaic.getParam("captureCornerBoost").getValue() != 0}
                if bayer:
                    expected["edgeThreshold"] = mode == "ppg"
                expected.update((n, shown) for n in CAPTURE_KNOBS)
                expect_visible(demosaic, "%s RawDemosaic %s, capture sharpen %s" % (sensor, mode, "on" if capture else "off"), expected)
            yield from panel_shot(demosaic, "m80-panel-RawDemosaic-%s-%s.png" % (sensor.lower().replace("-", ""), mode))
        try_choice(demosaic, param, modes[0])
        demosaic.getParam("captureSharpen").setValue(True)
        for boost in (0.0, 0.5):
            demosaic.getParam("captureCornerBoost").setValue(boost)
            expect_visible(demosaic, "%s RawDemosaic, Corner Boost %g" % (sensor, boost), {"captureSharpCenter": boost != 0})
        yield from panel_shot(demosaic, "m80-panel-RawDemosaic-%s-capture.png" % sensor.lower().replace("-", ""))
        for name, value in saved.items():
            demosaic.getParam(name).setValue(value)

    def temperature_presets(temperature, label, tuned_expected):
        try_choice(temperature, "mode", "cameraPreset")
        presets = options_of(temperature, "preset")
        if not check(presets, "%s RawTemperature: camera presets are offered (%r)" % (label, presets)):
            return
        tuned = []
        for preset in presets:
            try_choice(temperature, "preset", preset)
            if temperature.getParam("finetune").getIsVisible():
                tuned.append(preset)
        report("NOTE %s RawTemperature: presets with Finetune shown %r" % (label, tuned))
        if tuned_expected:
            expected = [p for p in presets if p.lower() in [t.lower() for t in TUNED_CR2_PRESETS]]
            check(expected and all(p in tuned for p in expected),
                  "%s RawTemperature: Finetune shows for the presets with tunings %r" % (label, expected))
            chosen = expected[0] if expected else presets[0]
        else:
            check(not tuned, "%s RawTemperature: Finetune stays hidden for presets without tunings" % label)
            chosen = presets[0]
        try_choice(temperature, "preset", chosen)
        report("NOTE %s RawTemperature preset %s: multipliers %r" % (label, chosen, [temperature.getParam(c).getValue() for c in ("red", "green", "blue")]))
        yield from open_panel(temperature)
        yield from panel_shot(temperature, "m80-panel-RawTemperature-preset-%s.png" % label.lower())

    def place_spot(node, fx, fy, fw, fh):
        x, y, w, h = image_box(node)
        node.getParam("spotBottomLeft").set(x + fx * w, y + fy * h)
        node.getParam("spotSize").set(fw * w, fh * h)

    def calibration_section(cal):
        cal.getParam("catOverride").setValue(True)
        illuminants = options_of(cal, "illuminant")
        check("camera" in illuminants, "RawColorCalibration offers the camera illuminant for a raw input (%r)" % illuminants)
        yield from open_panel(cal)
        cal.getParam("areaMapping").setOpened(True)
        cal.getParam("colorChecker").setOpened(True)
        yield ("sleep", SETTLE_MS)
        select_tab(cal, 0)
        enabled_wrong = []
        for adaptation in ADAPTATIONS:
            try_choice(cal, "illuminant", "d")
            if not try_choice(cal, "adaptation", adaptation):
                continue
            for illuminant in illuminants:
                cal.getParam("illuminant").setValue(illuminants.index(illuminant))
                actual = choice_id(cal, "adaptation")
                cat = actual != "none"
                expect_visible(cal, "RawColorCalibration adaptation %s (now %s), illuminant %s" % (adaptation, actual, illuminant), {
                    "illuminant": cat, "approxCct": cat,
                    "temperature": cat and illuminant in ("d", "blackBody"),
                    "illumFluo": cat and illuminant == "f", "illumLed": cat and illuminant == "led",
                    "illumHue": cat and illuminant == "custom", "illumChroma": cat and illuminant == "custom"})
                adaptation_enabled = cal.getParam("adaptation").getIsEnabled(0)
                if illuminant == "camera":
                    check(not adaptation_enabled, "RawColorCalibration: the camera illuminant disables the adaptation (with %s)" % adaptation)
                elif not adaptation_enabled:
                    enabled_wrong.append((adaptation, illuminant))
                if (adaptation == "linearBradford" and illuminant in ("d", "f", "led", "custom", "camera")) or (adaptation == "none" and illuminant == "d"):
                    yield from panel_shot(cal, "m80-panel-RawColorCalibration-cat-%s-%s.png" % (adaptation, illuminant))
        check(not enabled_wrong, "RawColorCalibration: the adaptation stays enabled for every other illuminant (disabled for %r)" % enabled_wrong)

        try_choice(cal, "adaptation", "linearBradford")
        for illuminant, knob in (("f", "illumFluo"), ("led", "illumLed")):
            try_choice(cal, "illuminant", illuminant)
            sources = options_of(cal, knob)
            stayed = []
            for source in sources:
                try_choice(cal, knob, source)
                if cal.getParam(knob).getIsVisible() and choice_id(cal, "illuminant") == illuminant:
                    stayed.append(source)
            check(sources and stayed == sources, "RawColorCalibration: every %s source keeps its menu shown (%r of %r)" % (knob, stayed, sources))
        try_choice(cal, "illuminant", "d")

        place_spot(cal, 0.45, 0.45, 0.1, 0.1)
        for mode in ("correction", "measure"):
            if not try_choice(cal, "spotMode", mode):
                continue
            cal.getParam("spotInput").setValue("")
            cal.getParam("pick").trigger()
            ok = yield ("until", lambda: bool(cal.getParam("spotInput").getValue()), RENDER_TIMEOUT_S)
            check(ok, "RawColorCalibration: Pick in %s mode reads the area (%r)" % (mode, cal.getParam("spotInput").getValue()))
            if mode == "correction":
                check(choice_id(cal, "illuminant") == "custom", "RawColorCalibration: Pick in correction mode sets a custom illuminant (%r)" % choice_id(cal, "illuminant"))
            else:
                report("NOTE RawColorCalibration measured L %r, h %r, c %r" % tuple(cal.getParam(n).getValue() for n in ("spotLightness", "spotHue", "spotChroma")))
            yield from panel_shot(cal, "m80-panel-RawColorCalibration-pick-%s.png" % mode)

        x, y, w, h = image_box(cal)
        for name, fx, fy in CHECKER_CORNERS:
            cal.getParam(name).set(x + fx * w, y + fy * h)
        for button in ("calibrate", "validate"):
            cal.getParam("checkerReport").setValue("")
            cal.getParam(button).trigger()
            ok = yield ("until", lambda: bool(cal.getParam("checkerReport").getValue()), RENDER_TIMEOUT_S)
            text = cal.getParam("checkerReport").getValue()
            check(ok, "RawColorCalibration: %s writes a report" % button.capitalize())
            report("NOTE RawColorCalibration %s report %r" % (button, " ".join(text.split())[:300]))
            yield from panel_shot(cal, "m80-panel-RawColorCalibration-checker-%s.png" % button)

        yield from view(cal, "CR2 RawColorCalibration", "m80-viewer-colorcalibration.png")
        yield from open_panel(cal)
        cal.getParam("areaMapping").setOpened(True)
        cal.getParam("colorChecker").setOpened(True)
        yield ("sleep", 1500)
        gt.shot("m80-viewer-colorcalibration-overlays.png")

        if try_choice(cal, "preset", "swapRB"):
            cal.getParam("applyPreset").trigger()
            yield ("sleep", SETTLE_MS)
            rows = {n: cal.getParam(n).getValue() for n in ("redR", "redB", "blueR", "blueB")}
            check(abs(rows["redB"] - 1) < 1e-6 and abs(rows["redR"]) < 1e-6 and abs(rows["blueR"] - 1) < 1e-6,
                  "RawColorCalibration: applying the swap R and B preset swaps the R and B rows (%r)" % rows)

        tabs = knob_tabs(cal)
        if check(tabs is not None and tabs.count() == 7, "RawColorCalibration shows seven tabs (%r)"
                 % ([tabs.tabText(i) for i in range(tabs.count())] if tabs is not None else None)):
            titles = [tabs.tabText(i) for i in range(tabs.count())]
            for i, title in enumerate(titles):
                select_tab(cal, i)
                yield from panel_shot(cal, "m80-panel-RawColorCalibration-tab-%d-%s.png" % (i + 1, re.sub(r"\W+", "", title).lower()))
            select_tab(cal, 0)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        gt.close_all_panels()

        develop = NatronEngine.natron.getSettings().getParam("developRawOnRead")
        require(develop is not None, "the preferences have developRawOnRead")
        check(develop.getValue(), "Develop RAW files on read is on by default")
        fixtures = all(is_real_fixture(p) for p in (CR2, RAF, DNG))
        if fixtures:
            viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
            require(viewer is not None, "a Viewer node exists")
            viewer.setPosition(-400, 0)
            viewer_holder["v"] = viewer
            ok = yield ("until", lambda: refresh_icon_key() is not None, POLL_TIMEOUT_S)
            require(ok, "the viewer's refresh button is on screen")
            check(not viewer_rendering(), "an empty viewer renders nothing")

        gt.section("drop")
        dropped = CR2 if os.path.exists(CR2) else make_raw_file("canon-eos-5d-mark-iii.cr2")
        drop_file(dropped, 0.2, 0.15)
        ok = yield ("until", lambda: len(nodes_of(app, CHAIN_IDS[-1])) == 1, POLL_TIMEOUT_S)
        require(ok, "dropping a .CR2 file builds the Develop RAW chain")
        yield ("sleep", SETTLE_MS)
        reads = nodes_of(app, READ_ID)
        require(len(reads) == 1, "the drop made one Read")
        read = reads[0]
        check(read.getParam("filename").getValue().endswith("canon-eos-5d-mark-iii.cr2"), "the Read reads the dropped file")
        chain = [nodes_of(app, pid)[0] for pid in CHAIN_IDS]
        previous = read
        for node in chain:
            check(is_input(node, 0, previous), "%s is connected below %s" % (node.getPluginID(), previous.getPluginID()))
            previous = node
        backdrops = nodes_of(app, BACKDROP_ID)
        require(len(backdrops) == 1, "the chain has one backdrop")
        backdrop = backdrops[0]
        check(backdrop.getLabel() == "Develop RAW", "the backdrop is titled Develop RAW (got %r)" % backdrop.getLabel())
        for node in chain:
            check(inside(node, backdrop), "%s sits inside the backdrop" % node.getPluginID())
        check(not inside(read, backdrop), "the Read stays above the backdrop")
        rx, ry = read.getPosition()
        check(all(n.getPosition()[1] > ry for n in chain), "the chain is below the Read")
        ys = [n.getPosition()[1] for n in chain]
        check(ys == sorted(ys), "the chain runs top to bottom in order")
        held = sorted((n for n in app.getChildren() if n.getPluginID() not in (BACKDROP_ID, "fr.inria.built-in.Viewer") and inside(n, backdrop)),
                      key=lambda n: n.getPosition()[1])
        check(len(held) == 6 and held[-1].getPluginID() == "fr.natron.RawColorCalibration",
              "Develop RAW holds six nodes with RawColorCalibration last (%r)" % [n.getPluginID() for n in held])
        check(choice_id(chain[1], "mode") == "asShotToReference",
              "RawTemperature starts on As Shot to Reference (%r)" % choice_id(chain[1], "mode"))
        frame_graph([read, backdrop])
        gt.shot("m80-develop-raw.png")

        gt.section("undo")
        if trigger_edit("undo"):
            ok = yield ("until", lambda: not nodes_of(app, READ_ID) and not nodes_of(app, BACKDROP_ID), POLL_TIMEOUT_S)
            check(ok, "one undo removes the Read and the backdrop")
            check(all(not nodes_of(app, pid) for pid in CHAIN_IDS), "the same undo removes the whole chain")
            if trigger_edit("redo"):
                ok = yield ("until", lambda: len(nodes_of(app, CHAIN_IDS[-1])) == 1 and len(nodes_of(app, READ_ID)) == 1, POLL_TIMEOUT_S)
                check(ok, "one redo brings the Read and the chain back")
                check(len(nodes_of(app, BACKDROP_ID)) == 1, "the redo brings the backdrop back")
                check(is_input(nodes_of(app, CHAIN_IDS[0])[0], 0, nodes_of(app, READ_ID)[0]), "the chain is connected below the Read again")

        if fixtures:
            gt.section("x-trans and linear drops")
            frame_graph([read, backdrop], margin=500.0)
            drop_file(RAF, 0.5, 0.15)
            ok = yield ("until", lambda: read_of(RAF) is not None and chain_below(read_of(RAF)) is not None, POLL_TIMEOUT_S)
            check(ok, "dropping the X-Trans RAF builds its own Develop RAW chain")
            drop_file(DNG, 0.8, 0.15)
            ok = yield ("until", lambda: read_of(DNG) is not None and chain_below(read_of(DNG)) is not None, POLL_TIMEOUT_S)
            check(ok, "dropping the linear DNG builds its own Develop RAW chain")
            yield ("sleep", SETTLE_MS)
            backdrops = nodes_of(app, BACKDROP_ID)
            check(len(backdrops) == 3 and all(b.getLabel() == "Develop RAW" for b in backdrops),
                  "three Develop RAW backdrops (%r)" % [b.getLabel() for b in backdrops])
            places = [(os.path.basename(p), read_of(p).getPosition()) for p in (CR2, RAF, DNG) if read_of(p) is not None]
            report("NOTE Read positions after the drops: %r" % places)
            check(len(set(pos for _, pos in places)) == len(places), "Reads dropped at different places land at different places")
            for path in (RAF, DNG):
                r = read_of(path)
                c = chain_below(r) if r is not None else None
                if c is None:
                    continue
                owners = [b for b in backdrops if inside(c[0], b)]
                check(any(all(inside(n, b) for n in c) and not inside(r, b) for b in owners),
                      "%s's chain sits in a backdrop below its Read" % os.path.basename(path))
            frame_graph([n for n in app.getChildren() if n.getPluginID() != "fr.inria.built-in.Viewer"])
            gt.shot("m80-develop-all.png")
        counts = settle_counts()

        cr2_read = read_of(CR2)
        cr2_chain = chain_below(cr2_read) if cr2_read is not None else None
        raf_read = read_of(RAF)
        raf_chain = chain_below(raf_read) if raf_read is not None else None
        dng_read = read_of(DNG)
        dng_chain = chain_below(dng_read) if dng_read is not None else None

        if fixtures and cr2_chain and raf_chain and dng_chain:
            gt.section("viewer on raw")
            for label, read_node, chain, mode, shot in (("CR2 Read", cr2_read, cr2_chain, "RAW mosaic", "m80-viewer-mosaic"),
                                                        ("RAF Read", raf_read, raf_chain, "RAW mosaic", "m80-viewer-xtrans-mosaic"),
                                                        ("DNG Read", dng_read, dng_chain, "RAW linear", "m80-viewer-linear")):
                st = yield from view(read_node, label, shot + ".png")
                check(info_line_has(mode), "%s: the info line reads %s (%r)" % (label, mode, info_line_texts()))
                white = chain[0].getParam("whiteLevel").getValue()
                report("NOTE %s: RawPrepare's white level knob %r" % (label, white))
                check(white > 1 and white != 65535, "%s: RawPrepare's white level knob shows the file's value (%r)" % (label, white))
                samples = viewer_samples()
                report("NOTE %s at gain 0: mean RGB %r, chroma %.3f, luma spread %d" % (label, st["mean"], st["chroma"], luma_spread(samples)))
                check(sum(st["mean"]) / 3.0 < 245, "%s: scaled to 0-1 by the Viewer, not blown out to white at gain 0 (mean %r)" % (label, st["mean"]))
                if mode == "RAW mosaic":
                    check(greyscale(st) and luma_spread(samples) > 10, "%s: the mosaic shows as a visible grey picture at gain 0" % label)
                else:
                    check(st["chroma"] > 0.02, "%s: the linear raw shows three channels at gain 0" % label)
            st = yield from view(cr2_chain[2], "CR2 RawHighlights", "m80-viewer-highlights-mosaic.png")
            check(info_line_has("RAW mosaic"), "CR2 RawHighlights: a raw edge mid-chain reads RAW mosaic (%r)" % info_line_texts())
            check(greyscale(st) and st["spread"] <= 3, "CR2 RawHighlights: the white-balanced mosaic still shows as grey")

            gt.section("developed")
            developed = {}
            for label, c, shot in (("RAF developed", raf_chain, "m80-viewer-xtrans-developed.png"),
                                   ("CR2 developed", cr2_chain, "m80-viewer-bayer-developed.png"),
                                   ("DNG developed", dng_chain, "m80-viewer-linear-developed.png")):
                report("NOTE %s: viewing %s" % (label, " -> ".join(n.getScriptName() for n in c)))
                st = yield from view(c[-1], label, shot)
                check(not info_line_has("RAW mosaic") and not info_line_has("RAW linear"),
                      "%s: the info line no longer calls it raw (%r)" % (label, info_line_texts()))
                check(plausible_colour(st), "%s: a plausible colour image" % label)
                developed[label] = st

            check(developed["RAF developed"]["mean"] != developed["CR2 developed"]["mean"],
                  "the X-Trans and the Bayer chains show different pictures")
            gt.section("demosaic methods")
            set_zoom("200%")
            for method in BAYER_METHODS:
                set_choice(cr2_chain[3], "method", method)
                st = yield from view(cr2_chain[-1], "Bayer " + method, "m80-viewer-bayer-%s.png" % method)
                check(plausible_colour(st), "Bayer %s: a plausible colour image" % method)
            set_choice(cr2_chain[3], "method", "rcd")
            for method in XTRANS_METHODS:
                set_choice(raf_chain[3], "xtransMethod", method)
                st = yield from view(raf_chain[-1], "X-Trans " + method, "m80-viewer-xtrans-%s.png" % method)
                check(plausible_colour(st), "X-Trans %s: a plausible colour image" % method)
            set_choice(raf_chain[3], "xtransMethod", "markesteijn1")
            set_zoom("Fit")

            gt.section("temperature")
            temperature = cr2_chain[1]
            # RawColorCalibration derives its default illuminant from the applied white balance
            # and would adapt the cast away, so the cast is read before it.
            input_color = cr2_chain[CHAIN_IDS.index("fr.natron.RawInputColor")]
            temperature.getParam("temperature").setValue(3000.0)
            check(temperature.getParam("mode").getValue() == 2, "editing the temperature switches the mode to Custom")
            report("NOTE 3000 K multipliers %r" % [temperature.getParam(c).getValue() for c in ("red", "green", "blue")])
            warm = yield from view(input_color, "CR2 at 3000 K", "m80-viewer-temperature-3000k.png")
            temperature.getParam("temperature").setValue(9000.0)
            report("NOTE 9000 K multipliers %r" % [temperature.getParam(c).getValue() for c in ("red", "green", "blue")])
            cool = yield from view(input_color, "CR2 at 9000 K", "m80-viewer-temperature-9000k.png")
            check(warm["mean"][2] - warm["mean"][0] > cool["mean"][2] - cool["mean"][0] + 10,
                  "a 3000 K white point makes the image bluer than 9000 K (B-R %.1f vs %.1f)"
                  % (warm["mean"][2] - warm["mean"][0], cool["mean"][2] - cool["mean"][0]))
            set_choice(temperature, "mode", "asShotToReference")

            gt.section("optional nodes")
            hx, hy = cr2_chain[2].getPosition()
            extras = []
            previous = cr2_chain[2]
            for i, pid in enumerate(EXTRA_IDS):
                node = app.createNode(pid)
                require(node is not None, "create " + pid, gt.plugin_problem(pid))
                node.setPosition(hx + 250, hy + 60 * i)
                require(node.connectInput(0, previous), "%s takes %s" % (pid, previous.getScriptName()))
                extras.append(node)
                previous = node
            cr2_chain[3].disconnectInput(0)
            require(cr2_chain[3].connectInput(0, previous), "RawDemosaic takes the spliced nodes")
            st = yield from view(extras[-1], "CR2 RawDenoise", "m80-viewer-optional-mosaic.png")
            check(info_line_has("RAW mosaic"), "the spliced nodes keep the mosaic (%r)" % info_line_texts())
            st = yield from view(cr2_chain[-1], "CR2 with hot pixels, CA and denoise", "m80-viewer-bayer-optional.png")
            check(plausible_colour(st), "with the optional nodes spliced in: a plausible colour image")

            gt.section("raw to image")
            to_image = app.createNode(TO_IMAGE_ID)
            require(to_image is not None, "create RawToImage", gt.plugin_problem(TO_IMAGE_ID))
            rx, ry = cr2_read.getPosition()
            to_image.setPosition(rx - 200, ry + 80)
            require(to_image.connectInput(0, cr2_read), "RawToImage takes the CR2 Read")
            st = yield from view(to_image, "CR2 RawToImage", "m80-viewer-rawtoimage.png")
            check(not info_line_has("RAW mosaic"), "RawToImage: the info line no longer reads RAW mosaic (%r)" % info_line_texts())
            set_gain(-16.0)
            yield ("sleep", 1500)
            yield from wait_render(None, 30.0)
            samples = viewer_samples()
            st = stats(samples)
            check(greyscale(st) and luma_spread(samples) > 10,
                  "RawToImage: the mosaic becomes a grey image (at -16 f-stops: mean %r, luma spread %d)" % (st["mean"], luma_spread(samples)))
            gt.shot("m80-viewer-rawtoimage-gain.png")
            set_gain(0.0)

            gt.section("panels")
            for node in cr2_chain + extras + [to_image]:
                yield from panel_section(node)
            gt.close_all_panels()

            gt.section("develop defaults")
            for label, c in (("CR2", cr2_chain), ("RAF", raf_chain), ("DNG", dng_chain)):
                expect_visible(c[0], "%s RawPrepare without gain maps" % label, {"flatField": False})
            for label, c in (("RAF", raf_chain), ("DNG", dng_chain)):
                check(choice_id(c[1], "mode") == "asShotToReference",
                      "%s RawTemperature starts on As Shot to Reference (%r)" % (label, choice_id(c[1], "mode")))

            gt.section("highlights modes")
            yield from highlights_section(cr2_chain[2])

            gt.section("demosaic modes")
            yield from demosaic_section(cr2_chain[3], "Bayer", "method", BAYER_MODES)
            yield from demosaic_section(raf_chain[3], "X-Trans", "xtransMethod", XTRANS_MODES)

            gt.section("temperature presets and spot")
            yield from temperature_presets(cr2_chain[1], "CR2", True)
            yield from temperature_presets(raf_chain[1], "RAF", False)
            set_choice(raf_chain[1], "mode", "asShotToReference")
            temperature = cr2_chain[1]
            if try_choice(temperature, "mode", "fromImageArea"):
                place_spot(temperature, 0.45, 0.45, 0.1, 0.1)
                temperature.getParam("pick").trigger()
                yield ("sleep", SETTLE_MS)
                multipliers = [temperature.getParam(c).getValue() for c in ("red", "green", "blue")]
                check(all(m > 0 for m in multipliers), "RawTemperature: Pick from the area gives positive multipliers (%r)" % multipliers)
                yield from view(cr2_chain[-1], "CR2 white balance from an area", "m80-viewer-temperature-area.png")
                yield from open_panel(temperature)
                yield ("sleep", 1500)
                gt.shot("m80-viewer-temperature-area-overlay.png")
            set_choice(temperature, "mode", "asShotToReference")

            gt.section("color calibration")
            yield from calibration_section(cr2_chain[-1])
            gt.close_all_panels()
        else:
            gt.report("NOTE the raw fixtures are not checked out (git lfs pull); the Viewer sections are skipped")

        gt.section("no chain")
        scripted = app.createReader(make_raw_file("scripted.NEF"))
        require(scripted is not None, "Python's createReader() makes a Read")
        yield ("sleep", SETTLE_MS)
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "a Read made from Python gets no chain")

        plain = app.createReader(os.path.join(gt.FIXTURES, "flat-rgba-only.exr"))
        require(plain is not None, "make a Read of an EXR")
        plain.getParam("filename").setValue(make_raw_file("renamed.ARW"))
        yield ("sleep", SETTLE_MS)
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "a new raw filename on an existing Read gets no chain")

        develop.setValue(False)
        drop_file(CR2 if fixtures else make_raw_file("off.DNG"), 0.5, 0.8)
        yield ("until", lambda: len(nodes_of(app, READ_ID)) == counts[READ_ID] + 3, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        check(len(nodes_of(app, READ_ID)) == counts[READ_ID] + 3, "with the preference off the drop still makes a Read")
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "with the preference off the drop gets no chain")
        gt.shot("m80-preference-off.png")
        develop.setValue(True)

        gt.section("reload")
        before = settle_counts()
        path = os.path.join(gt.OUT, "m80-develop.ntp")
        if check(app.saveProjectAs(path), "save the project to " + path):
            if check(app.resetProject(), "reset the project"):
                reloaded = app.loadProject(path)
                yield ("sleep", SETTLE_MS * 2)
                if check(reloaded is not None, "reload " + path):
                    for pid in CHAIN_IDS + [BACKDROP_ID, READ_ID]:
                        n = len(nodes_of(reloaded, pid))
                        check(n == before[pid], "the reloaded project has %d %s (got %d)" % (before[pid], pid, n))

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
