"""The native ContrastTexture panel: every darktable control as a knob, labelled as its script
name reads, in the Local Contrast and Filter Settings groups, the whole panel on a 1920x1080
screen. Screenshots of the panel at its defaults and with Mask Display on go to
$NATRON_GUI_TEST_OUT as contrasttexture_panel.png and contrasttexture_mask_display.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m213_contrast_texture.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "ContrastTexture"
# Script name, label, default.
KNOBS = [
    ("baseDetailLevel", "Base Detail Level", 5.0),
    ("highlights", "Highlights", 0.0),
    ("shadows", "Shadows", 0.0),
    ("coarseDetails", "Coarse Details", 0.0),
    ("mediumDetails", "Medium Details", 0.0),
    ("fineDetails", "Fine Details", 0.0),
    ("haloControl", "Halo Control", 0.0),
    ("noiseBias", "Noise Bias", 0.001),
]
GROUPS = [("localContrast", "Local Contrast"), ("filterSettings", "Filter Settings")]
MASK_DISPLAY = ("maskDisplay", "Mask Display")
MAX_WIDTH = 1920
MAX_HEIGHT = 1080

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea

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

    def visible_label_texts(panel):
        texts = set()
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible():
                    texts.add(w.text().strip())
            except RuntimeError:
                continue
        return texts

    def content_height(panel):
        heights = [area.widget().sizeHint().height() for area in panel.findChildren(QScrollArea) if area.widget() is not None]
        return max(heights) if heights else panel.sizeHint().height()

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

    def steps():
        gt.section("contrasttexture")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create ContrastTexture", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open ContrastTexture's settings panel")
        yield ("sleep", 800)

        for name, label, default in KNOBS:
            param = node.getParam(name)
            require(param is not None, "ContrastTexture has a %s knob" % name)
            check(param.getLabel() == label, "%s is labelled %r (got %r)" % (name, label, param.getLabel()))
            check(abs(param.getValue() - default) < 1e-9, "%s defaults to %g (got %g)" % (name, default, param.getValue()))
        for name, label in GROUPS + [MASK_DISPLAY]:
            param = node.getParam(name)
            require(param is not None, "ContrastTexture has a %s knob" % name)
            check(param.getLabel() == label, "%s is labelled %r (got %r)" % (name, label, param.getLabel()))
        check(node.getParam(MASK_DISPLAY[0]).getValue() == 0, "Mask Display defaults to Off")

        panel = panel_of(node)
        shown = visible_label_texts(panel)
        for _, label, _ in KNOBS:
            check(label in shown, "the panel shows %s with both groups open" % label)
        height = content_height(panel)
        check(height <= MAX_HEIGHT, "the panel's controls are %d px tall, within %d" % (height, MAX_HEIGHT))
        check(panel.sizeHint().width() <= MAX_WIDTH, "the panel is %d px wide, within %d" % (panel.sizeHint().width(), MAX_WIDTH))
        enlarge(panel)
        yield ("sleep", 300)
        screen_crop("contrasttexture_panel", panel_of(node))

        node.getParam(MASK_DISPLAY[0]).setValue(2)
        yield ("sleep", 300)
        screen_crop("contrasttexture_mask_display", panel_of(node))
        node.getParam(MASK_DISPLAY[0]).setValue(0)

        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
