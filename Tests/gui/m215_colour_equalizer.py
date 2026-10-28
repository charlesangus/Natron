"""Screenshots of the ColorEqualizer panel for sign-off: the read-only Curves preview with its Hue,
Saturation, Brightness and Options tabs, the preview after every node is moved, after each preset,
and with each weight output chosen. The panel must fit a 1920x1080 screen. Shots go to
$NATRON_GUI_TEST_OUT as colorequalizer_<what>.png.

The Curves preview draws the hue ramp, the three interpolated curves and the eight node positions;
it must redraw when a node knob or Node Placement changes.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m215_colour_equalizer.py <Natron binary or AppImage>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "ColorEqualizer"
TABS = (("Hue", "hue"), ("Saturation", "saturation"), ("Brightness", "brightness"), ("Options", "options"))
NODES = ("Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Lavender", "Magenta")
BUSY_HUE = (20.0, -15.0, 10.0, 35.0, -25.0, 12.0, -8.0, 30.0)
BUSY_SATURATION = (1.3, 0.7, 1.1, 0.6, 1.4, 0.9, 1.2, 0.8)
BUSY_BRIGHTNESS = (1.2, 0.85, 1.1, 0.75, 1.15, 0.9, 1.05, 0.8)
PRESETS = ("bleachBypass", "kodachrome64Like", "kodakPortra400Like", "tealAndOrange")
OUTPUTS = (("hue", 1), ("saturation", 2), ("brightness", 3), ("saturationThreshold", 4), ("brightnessThreshold", 5))
SCREEN_W = 1920
SCREEN_H = 1080

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea, QTabWidget

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

    def show_tab(panel, text):
        for tabs in panel.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == text:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def curve_widget(panel):
        for w in gt.widgets_of("CurveWidget"):
            try:
                if w.isVisible() and panel.isAncestorOf(w):
                    return w
            except RuntimeError:
                continue
        return None

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(min(content.sizeHint().height() + 4, SCREEN_H - 120))
        QApplication.processEvents()

    def grab(widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        return screen.copy(rect.intersected(QRect(0, 0, screen.width(), screen.height())))

    def save(name, pixmap):
        path = os.path.join(gt.OUT, name + ".png")
        if pixmap.save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def set_nodes(node, prefix, values):
        for name, value in zip(NODES, values):
            param = node.getParam(prefix + name)
            require(param is not None, "ColorEqualizer has %s%s" % (prefix, name))
            param.setValue(value)

    def steps():
        gt.section("colorequalizer")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create ColorEqualizer", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open ColorEqualizer's settings panel")
        yield ("sleep", 800)
        panel = panel_of(node)

        curves = curve_widget(panel)
        if check(curves is not None, "the panel shows the Curves preview"):
            report("NOTE Curves preview is %dx%d" % (curves.width(), curves.height()))
        save("colorequalizer_default_layout", grab(panel))
        enlarge(panel)
        yield ("sleep", 300)

        screen = QGuiApplication.primaryScreen().geometry()
        report("NOTE screen %dx%d" % (screen.width(), screen.height()))
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        check(rect.width() <= SCREEN_W and rect.height() <= SCREEN_H,
              "the panel fits 1920x1080 (got %dx%d)" % (rect.width(), rect.height()))

        for text, name in TABS:
            check(show_tab(panel_of(node), text), "the panel has a %s tab" % text)
            yield ("sleep", 300)
            save("colorequalizer_" + name, grab(panel_of(node)))

        for prefix in ("hue", "saturation", "brightness"):
            for name in NODES:
                param = node.getParam(prefix + name)
                if check(param is not None, "ColorEqualizer has %s%s" % (prefix, name)):
                    check(param.getLabel() == "%s %s" % (prefix.capitalize(), name),
                          "%s%s is labelled %r (got %r)" % (prefix, name, "%s %s" % (prefix.capitalize(), name), param.getLabel()))

        gt.section("curves")
        show_tab(panel_of(node), "Saturation")
        yield ("sleep", 300)
        curves = curve_widget(panel_of(node))
        require(curves is not None, "found the Curves preview")
        before = grab(curves).toImage()
        set_nodes(node, "hue", BUSY_HUE)
        set_nodes(node, "saturation", BUSY_SATURATION)
        set_nodes(node, "brightness", BUSY_BRIGHTNESS)
        yield ("sleep", 500)
        after = grab(curve_widget(panel_of(node))).toImage()
        check(after != before, "the Curves preview redraws when the node knobs move")
        save("colorequalizer_curves_busy", grab(panel_of(node)))
        placement = node.getParam("nodePlacement")
        require(placement is not None, "ColorEqualizer has nodePlacement")
        placement.setValue(15.0)
        yield ("sleep", 500)
        shifted = grab(curve_widget(panel_of(node))).toImage()
        check(shifted != after, "the Curves preview redraws when Node Placement moves")
        save("colorequalizer_curves_placement", grab(panel_of(node)))

        gt.section("presets")
        preset = node.getParam("preset")
        apply_preset = node.getParam("applyPreset")
        require(preset is not None and apply_preset is not None, "ColorEqualizer has preset and applyPreset")
        for index, name in enumerate(PRESETS):
            preset.setValue(index)
            apply_preset.trigger()
            yield ("sleep", 500)
            save("colorequalizer_preset_" + name, grab(panel_of(node)))

        gt.section("outputs")
        output = node.getParam("output")
        require(output is not None, "ColorEqualizer has an output knob")
        show_tab(panel_of(node), "Options")
        for name, index in OUTPUTS:
            output.setValue(index)
            yield ("sleep", 300)
            save("colorequalizer_output_" + name, grab(panel_of(node)))
        output.setValue(0)
        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
