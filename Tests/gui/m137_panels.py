"""Screenshots of the native Grade, Saturation, Blur, ErodeDilate, Transform, Dissolve and Switch
properties panels on their main page, one PNG per node in $NATRON_GUI_TEST_OUT/shots/. Each node's
knob labels are listed in results.txt, and the filter nodes are checked for an "(Un)premult by" knob.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m137_panels.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0

NODES = [
    ("Grade", "net.sf.openfx.GradePlugin"),
    ("Saturation", "net.sf.openfx.SaturationPlugin"),
    ("Blur", "net.sf.cimg.CImgBlur"),
    ("ErodeDilate", "net.sf.cimg.CImgErode"),
    ("Transform", "net.sf.openfx.TransformPlugin"),
    ("Dissolve", "net.sf.openfx.DissolvePlugin"),
    ("Switch", "fr.natron.Switch"),
]
FILTERS = ("Blur", "ErodeDilate", "Transform")

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea

    import guitest as gt
    from guitest import check, report, require

    SHOT_DIR = os.path.join(gt.OUT, "shots")
    os.makedirs(SHOT_DIR, exist_ok=True)

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

    def labels_of(panel):
        rows = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip():
                    rows.append((w.mapTo(panel, w.rect().topLeft()).y(), w.text().strip()))
            except RuntimeError:
                continue
        return [t for _, t in sorted(rows)]

    def enlarge(panel):
        """Grows the panel's scroll area to its content so the whole knob list is on screen; the
        panel only gets taller if the dock lets it, so the screenshot is taken from the screen."""
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
        path = os.path.join(SHOT_DIR, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def steps():
        gt.section("panels")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        for name, plugin_id in NODES:
            gt.close_all_panels()
            node = app.createNode(plugin_id)
            require(node is not None, "create " + name, gt.plugin_problem(plugin_id))
            node.setPosition(0, 0)
            app.selectNode(node, True)
            QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
            require(ok, "open %s's settings panel" % name)
            yield ("sleep", 800)
            panel = panel_of(node)
            enlarge(panel)
            yield ("sleep", 500)
            labels = labels_of(panel)
            report("NOTE %s labels top to bottom: %r" % (name, labels))
            if name in FILTERS:
                check(not any("premult" in l.lower() for l in labels), name + " has no (Un)premult by knob")
            screen_crop(name, panel)
            node.destroy()
            yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
