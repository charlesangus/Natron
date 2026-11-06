"""Screenshots of Transform's and STMap's properties panels showing the resampling Filter list and
its controls, one PNG per node in $NATRON_GUI_TEST_OUT/shots/. For every filter it checks that Clamp,
Lobes, Bicubic B and Bicubic C are enabled only with the filters that read them,
and that the Filter row and its controls fit a 1920x1080 screen.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m270_filter_panels.py <Natron.AppImage>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0

# (node, filter shown in its screenshot); between them the shots show Lobes and the Bicubic pair lit.
NODES = [("Transform", "sinc"), ("STMap", "bicubic")]
FILTERS = (
    "impulse", "box", "bilinear", "cubic", "keys", "simon", "rifman", "mitchell", "parzen", "notch",
    "lanczos2", "lanczos3", "lanczos4", "sinc", "blackmanHarris", "gaussian", "bicubic",
)
CLAMPED = ("keys", "simon", "rifman", "mitchell", "lanczos2", "lanczos3", "lanczos4", "sinc", "bicubic")
LABELS = ("Filter", "Clamp", "Black outside", "Lobes", "Bicubic B", "Bicubic C")

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

    def label_widgets(panel, text):
        found = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip().rstrip(":") == text:
                    found.append(w)
            except RuntimeError:
                continue
        return found

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(content.sizeHint().height() + 4)
        QApplication.processEvents()

    def screen_crop(name, panel):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        check((screen.width(), screen.height()) == (1920, 1080), "the screen is 1920x1080, got %dx%d" % (screen.width(), screen.height()))
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(SHOT_DIR, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            check(False, "save screenshot " + path)

    def check_enabling(name, node, panel):
        choice = node.getParam("filter")
        options = choice.getOptions()
        check(tuple(options) == FILTERS, "%s offers the seventeen filters in order, got %r" % (name, options))
        for filter_id in FILTERS:
            choice.set(filter_id)
            yield ("sleep", 100)
            expected = {
                "clamp": filter_id in CLAMPED,
                "lobes": filter_id == "sinc",
                "bicubicB": filter_id == "bicubic",
                "bicubicC": filter_id == "bicubic",
            }
            for script_name, want in expected.items():
                param = node.getParam(script_name)
                require(param is not None, "%s has %s" % (name, script_name))
                check(param.getIsEnabled() == want, "%s %s enabled=%s with %s" % (name, script_name, want, filter_id))
        report("NOTE %s enabling checked over %d filters" % (name, len(FILTERS)))

    def check_labels_fit(name, panel):
        height = QGuiApplication.primaryScreen().geometry().height()
        for text in LABELS:
            found = label_widgets(panel, text)
            check(bool(found), "%s shows the %s label" % (name, text))
            for w in found:
                bottom = w.mapToGlobal(w.rect().bottomLeft()).y()
                check(0 <= bottom < height, "%s's %s label is on screen, bottom at %d" % (name, text, bottom))

    def steps():
        gt.section("filter panels")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        for name, shown in NODES:
            gt.close_all_panels()
            node = app.createNode(name)
            require(node is not None, "create " + name, gt.plugin_problem(name))
            node.setPosition(0, 0)
            app.selectNode(node, True)
            QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
            require(ok, "open %s's settings panel" % name)
            yield ("sleep", 800)
            panel = panel_of(node)
            enlarge(panel)
            yield from check_enabling(name, node, panel)
            node.getParam("filter").set(shown)
            yield ("sleep", 800)
            check_labels_fit(name, panel)
            screen_crop(name + "_filter_panel", panel)
            del panel
            node.destroy()
            yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
