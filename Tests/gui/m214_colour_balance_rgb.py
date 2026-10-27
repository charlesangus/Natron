"""Screenshots of the ColorBalanceRGB panel for sign-off: its Master, 4 Ways and Masks tabs, the
4 Ways tab with a grade on every wheel, and the Masks tab with each mask output chosen. The panel
must fit a 1920x1080 screen. Shots go to $NATRON_GUI_TEST_OUT as colorbalancergb_<tab>.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m214_colour_balance_rgb.py <Natron binary or AppImage>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "ColorBalanceRGB"
TABS = (("Master", "master"), ("4 Ways", "4ways"), ("Masks", "masks"))
WHEELS = ("offset", "lift", "gain", "power")
WHEEL_GRADE = {
    "offset": (0.01, 200.0, 0.005),
    "lift": (0.2, 30.0, 0.2),
    "gain": (-0.1, 90.0, 0.1),
    "power": (0.1, 300.0, 0.05),
}
OUTPUTS = (("shadowsMask", 1), ("midtonesMask", 2), ("highlightsMask", 3))
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

    def visible_labels(panel):
        return [w.text() for w in panel.findChildren(QLabel) if w.isVisible() and w.text().strip()]

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(min(content.sizeHint().height() + 4, SCREEN_H - 120))
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

    def steps():
        gt.section("colorbalancergb")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create ColorBalanceRGB", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open ColorBalanceRGB's settings panel")
        yield ("sleep", 800)
        panel = panel_of(node)
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
            screen_crop("colorbalancergb_" + name, panel_of(node))

        for wheel in WHEELS:
            luminance, hue, chroma = WHEEL_GRADE[wheel]
            for suffix, value in (("Luminance", luminance), ("Hue", hue), ("Chroma", chroma)):
                param = node.getParam(wheel + suffix)
                require(param is not None, "ColorBalanceRGB has %s%s" % (wheel, suffix))
                param.setValue(value)
        show_tab(panel_of(node), "4 Ways")
        yield ("sleep", 400)
        labels = visible_labels(panel_of(node))
        for wheel in WHEELS:
            for suffix in ("Luminance", "Hue", "Chroma"):
                label = "%s %s" % (wheel.capitalize(), suffix)
                check(any(t.startswith(label) for t in labels), "the 4 Ways tab shows %s" % label)
        screen_crop("colorbalancergb_4ways_graded", panel_of(node))

        output = node.getParam("output")
        require(output is not None, "ColorBalanceRGB has an output knob")
        show_tab(panel_of(node), "Masks")
        for name, index in OUTPUTS:
            output.setValue(index)
            yield ("sleep", 300)
            screen_crop("colorbalancergb_masks_" + name, panel_of(node))
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
