"""The native HSVKeyer panel draws its three ranges as trapezoid strips above their spinboxes. Setting
the source colour recentres each range on the colour's component and keeps its widths; switching the
model recentres them on the new model's components. Screenshots of the panel in each model and with
the hue term off go to $NATRON_GUI_TEST_OUT as hsvkeyer_hsv.png, hsvkeyer_hsl.png,
hsvkeyer_oklch.png and hsvkeyer_hue_off.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m122_hsvkeyer.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "HSVKeyer"
RANGE_KNOBS = ("hueRange", "saturationRange", "lumaRange")
STRIP_PREFIX = "TrapezoidStrip_"
# Orange: hue 30 degrees in HSV and HSL, full saturation, V 1 and L 0.5.
SOURCE = (1.0, 0.5, 0.0)
SOURCE_HUE = 30.0 / 360.0

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea, QWidget

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

    def points(param):
        return [param.getValue(i) for i in range(4)]

    def hue_centre(param):
        p = points(param)
        full = (p[2] - p[1]) % 1.0
        return (p[1] + full * 0.5) % 1.0

    def linear_centre(param):
        p = points(param)
        return (p[1] + p[2]) * 0.5

    def near(a, b, tol=1e-4):
        return abs(a - b) < tol

    def hue_near(a, b, tol=1e-4):
        d = (a - b) % 1.0
        return min(d, 1.0 - d) < tol

    def widths(param, hue):
        p = points(param)
        if hue:
            return [round((p[i + 1] - p[i]) % 1.0, 6) for i in range(3)]
        return [round(p[i + 1] - p[i], 6) for i in range(3)]

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
        check(model.getValue() == 0, "the model defaults to HSV")

        ok = yield ("until", lambda: all_strips_shown(panel_of(node)), POLL_TIMEOUT_S)
        check(ok, "the panel shows a trapezoid strip for each of %s" % ", ".join(RANGE_KNOBS))
        # The panel wrapper is held while its strips are used: child wrappers die with it.
        panel = panel_of(node)
        for k in RANGE_KNOBS:
            strip = strip_of(panel, k)
            if strip is not None:
                check(strip.width() > 100 and strip.height() > 10, "the %s strip has a usable size (%dx%d)" % (k, strip.width(), strip.height()))

        hue_widths = widths(hue, True)
        sat_widths = widths(sat, False)
        luma_widths = widths(luma, False)
        for i, v in enumerate(SOURCE):
            source.setValue(v, i)
        yield ("sleep", 300)
        check(hue_near(hue_centre(hue), SOURCE_HUE), "HSV: the hue plateau centres on the source hue (got %.4f)" % hue_centre(hue))
        check(near(linear_centre(sat), 1.0), "HSV: the saturation plateau centres on 1 (got %.4f)" % linear_centre(sat))
        check(near(linear_centre(luma), 1.0), "HSV: the value plateau centres on 1 (got %.4f)" % linear_centre(luma))
        check(widths(hue, True) == hue_widths, "recentring keeps the hue widths")
        check(widths(sat, False) == sat_widths and widths(luma, False) == luma_widths, "recentring keeps the saturation and luma widths")
        screen_crop("hsvkeyer_hsv", panel_of(node))

        model.setValue(1)
        yield ("sleep", 300)
        check(hue_near(hue_centre(hue), SOURCE_HUE), "HSL: the hue plateau stays on the source hue")
        check(near(linear_centre(luma), 0.5), "HSL: the lightness plateau centres on 0.5 (got %.4f)" % linear_centre(luma))
        check(all_strips_shown(panel_of(node)), "HSL: the three strips are still shown")
        screen_crop("hsvkeyer_hsl", panel_of(node))

        model.setValue(2)
        yield ("sleep", 300)
        lightness = linear_centre(luma)
        check(0.0 < lightness < 1.0, "OkLCh: the lightness plateau centres inside 0..1 (got %.4f)" % lightness)
        check(0.0 < linear_centre(sat) < 0.4, "OkLCh: the chroma plateau centres inside 0..0.4 (got %.4f)" % linear_centre(sat))
        check(all_strips_shown(panel_of(node)), "OkLCh: the three strips are still shown")
        screen_crop("hsvkeyer_oklch", panel_of(node))

        before = points(hue)
        use_hue.setValue(False)
        yield ("sleep", 300)
        check(points(hue) == before, "turning the hue term off leaves the hue range alone")
        check(strip_of(panel_of(node), "hueRange") is not None, "the hue strip stays on screen with the hue term off")
        screen_crop("hsvkeyer_hue_off", panel_of(node))
        use_hue.setValue(True)

        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
