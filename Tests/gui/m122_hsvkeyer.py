"""The native HSVKeyer panel draws its three ranges (a centre and four offsets each) as trapezoid
strips above their spinboxes, each with Recentre and Reset Zoom buttons, below the Use toggles.
Setting the source colour sets each range's centre to the colour's component and keeps its offsets;
switching the model does the same in the new model. A range's row greys out while its Use toggle is
off, and the strips grow with the panel. Screenshots go to $NATRON_GUI_TEST_OUT as
hsvkeyer_oklch.png, hsvkeyer_hsv.png, hsvkeyer_hue_off.png and hsvkeyer_wide.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m122_hsvkeyer.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "HSVKeyer"
RANGE_KNOBS = ("hueRange", "saturationRange", "lumaRange")
TOGGLE_LABELS = ("Use Hue", "Use Saturation", "Use Luma")
STRIP_PREFIX = "TrapezoidStrip_"
BUTTON_PREFIXES = ("TrapezoidRecentre_", "TrapezoidResetZoom_")
# Orange: hue 30 degrees in HSV and HSL, full saturation, V 1 and L 0.5.
SOURCE = (1.0, 0.5, 0.0)
SOURCE_HUE = 30.0 / 360.0

try:
    from PySide6.QtCore import QMetaObject, QRect, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QScrollArea, QSplitter, QWidget

    import guitest as gt
    from guitest import check, report, require

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

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

    def strip_of(panel, knob):
        if panel is None:
            return None
        strips = [s for s in panel.findChildren(QWidget, STRIP_PREFIX + knob) if s.isVisible()]
        return strips[0] if len(strips) == 1 else None

    def all_strips_shown(panel):
        return all(strip_of(panel, k) is not None for k in RANGE_KNOBS)

    def top_of(widget):
        return widget.mapToGlobal(widget.rect().topLeft()).y()

    def toggle_label(panel, text):
        labels = [w for w in panel.findChildren(QLabel) if w.isVisible() and w.text().strip() == text]
        return labels[0] if labels else None

    def button_of(panel, prefix, knob):
        buttons = [b for b in panel.findChildren(QAbstractButton, prefix + knob) if b.isVisible()]
        return buttons[0] if len(buttons) == 1 else None

    def widen(panel, extra):
        """Widens the panel by extra pixels through its splitter, or its window when it has none."""
        w = panel.parentWidget()
        child = panel
        while w is not None:
            if isinstance(w, QSplitter) and w.orientation() == Qt.Orientation.Horizontal:
                sizes = w.sizes()
                i = w.indexOf(child)
                if i >= 0:
                    others = [j for j in range(len(sizes)) if j != i and sizes[j] > extra]
                    if others:
                        sizes[i] += extra
                        sizes[others[0]] -= extra
                        w.setSizes(sizes)
                        QApplication.processEvents()
                        return "splitter"
            child = w
            w = w.parentWidget()
        window = panel.window()
        window.resize(window.width() + extra, window.height())
        QApplication.processEvents()
        return "window"

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(content.sizeHint().height() + 4)
        QApplication.processEvents()

    def screen_crop(name, panel):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def centre(param):
        return param.getValue(0)

    def offsets(param):
        return [param.getValue(i) for i in range(1, 5)]

    def near(a, b, tol=1e-4):
        return abs(a - b) < tol

    def hue_near(a, b, tol=1e-4):
        d = (a - b) % 1.0
        return min(d, 1.0 - d) < tol

    def steps():
        gt.section("hsvkeyer")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create HSVKeyer", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open HSVKeyer's settings panel")
        yield ("sleep", 800)
        enlarge(panel_of(node))
        yield ("sleep", 300)

        source = node.getParam("sourceColor")
        model = node.getParam("colorModel")
        hue = node.getParam("hueRange")
        sat = node.getParam("saturationRange")
        luma = node.getParam("lumaRange")
        use_hue = node.getParam("useHue")
        require(None not in (source, model, hue, sat, luma, use_hue), "HSVKeyer has its source, model, range and toggle knobs")
        check(model.getValue() == 2, "the model defaults to OkLCh")

        ok = yield ("until", lambda: all_strips_shown(panel_of(node)), POLL_TIMEOUT_S)
        check(ok, "the panel shows a trapezoid strip for each of %s" % ", ".join(RANGE_KNOBS))
        # The panel wrapper is held while its strips are used: child wrappers die with it.
        panel = panel_of(node)
        for k in RANGE_KNOBS:
            strip = strip_of(panel, k)
            if strip is not None:
                check(strip.width() > 100 and strip.height() > 10, "the %s strip has a usable size (%dx%d)" % (k, strip.width(), strip.height()))
            for prefix in BUTTON_PREFIXES:
                check(button_of(panel, prefix, k) is not None, "the %s row has its %s button" % (k, prefix.rstrip("_")))

        strip_tops = [top_of(strip_of(panel, k)) for k in RANGE_KNOBS if strip_of(panel, k) is not None]
        for text in TOGGLE_LABELS:
            label = toggle_label(panel, text)
            check(label is not None, "the panel shows the %s toggle" % text)
            if label is not None and strip_tops:
                check(top_of(label) < min(strip_tops), "%s sits above the range strips (y %d < %d)" % (text, top_of(label), min(strip_tops)))

        screen_crop("hsvkeyer_oklch", panel_of(node))

        model.setValue(0)
        yield ("sleep", 300)
        hue_offsets = offsets(hue)
        sat_offsets = offsets(sat)
        luma_offsets = offsets(luma)
        for i, v in enumerate(SOURCE):
            source.setValue(v, i)
        yield ("sleep", 300)
        check(hue_near(centre(hue), SOURCE_HUE), "HSV: the hue centre is the source hue (got %.4f)" % centre(hue))
        check(near(centre(sat), 1.0), "HSV: the saturation centre is 1 (got %.4f)" % centre(sat))
        check(near(centre(luma), 1.0), "HSV: the value centre is 1 (got %.4f)" % centre(luma))
        check(offsets(hue) == hue_offsets, "recentring keeps the hue offsets")
        check(offsets(sat) == sat_offsets and offsets(luma) == luma_offsets, "recentring keeps the saturation and luma offsets")
        screen_crop("hsvkeyer_hsv", panel_of(node))

        model.setValue(1)
        yield ("sleep", 300)
        check(hue_near(centre(hue), SOURCE_HUE), "HSL: the hue centre stays on the source hue")
        check(near(centre(luma), 0.5), "HSL: the lightness centre is 0.5 (got %.4f)" % centre(luma))
        check(all_strips_shown(panel_of(node)), "HSL: the three strips are still shown")

        model.setValue(2)
        yield ("sleep", 300)
        check(0.0 < centre(luma) < 1.0, "OkLCh: the lightness centre is inside 0..1 (got %.4f)" % centre(luma))
        check(0.0 < centre(sat) < 0.4, "OkLCh: the chroma centre is inside 0..0.4 (got %.4f)" % centre(sat))

        panel = panel_of(node)
        before = [hue.getValue(i) for i in range(5)]
        check(strip_of(panel, "hueRange").isEnabled(), "the hue strip is enabled with the hue term on")
        use_hue.setValue(False)
        yield ("sleep", 300)
        panel = panel_of(node)
        check([hue.getValue(i) for i in range(5)] == before, "turning the hue term off leaves the hue range alone")
        hue_strip = strip_of(panel, "hueRange")
        check(hue_strip is not None, "the hue strip stays on screen with the hue term off")
        if hue_strip is not None:
            check(not hue_strip.isEnabled(), "the hue row greys out with the hue term off")
            check(not button_of(panel, BUTTON_PREFIXES[0], "hueRange").isEnabled(), "the hue row's Recentre button greys out too")
        check(strip_of(panel, "saturationRange").isEnabled(), "the saturation row stays enabled")
        screen_crop("hsvkeyer_hue_off", panel_of(node))
        use_hue.setValue(True)
        yield ("sleep", 300)
        panel = panel_of(node)
        check(strip_of(panel, "hueRange").isEnabled(), "the hue row is enabled again with the hue term back on")

        narrow = strip_of(panel, "hueRange").width()
        how = widen(panel, 300)
        yield ("sleep", 500)
        panel = panel_of(node)
        wide = strip_of(panel, "hueRange").width()
        check(wide > narrow + 100, "the hue strip grows with the panel (%d to %d px, widened by %s)" % (narrow, wide, how))
        screen_crop("hsvkeyer_wide", panel_of(node))

        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
