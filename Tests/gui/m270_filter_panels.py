"""Screenshots of Transform's and STMap's properties panels showing the resampling Filter list and
its controls, one PNG per node in $NATRON_GUI_TEST_OUT/shots/. For every filter on every node that
has the shared filter controls it checks that Clamp, Lobes, Bicubic B and Bicubic C are visible only
with the filters that read them, also after a project load, and that the Filter row and its visible
controls fit a 1920x1080 screen with the hidden ones' labels gone.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m270_filter_panels.py <Natron.AppImage>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0

# (node, filter shown in its screenshot); between them the shots show Lobes and the Bicubic pair.
NODES = [("Transform", "sinc"), ("STMap", "bicubic")]
OTHER_NODES = ("Reformat", "CornerPin", "IDistort", "LensDistortion", "LensCorrect", "SplineWarp")
CHECKED_NODES = tuple(name for name, _ in NODES) + OTHER_NODES
FILTERS = (
    "impulse", "box", "bilinear", "cubic", "keys", "simon", "rifman", "mitchell", "parzen", "notch",
    "lanczos2", "lanczos3", "lanczos4", "sinc", "blackmanHarris", "gaussian", "bicubic",
)
CLAMPED = ("keys", "simon", "rifman", "mitchell", "lanczos2", "lanczos3", "lanczos4", "sinc", "bicubic")
LABEL_OF = {"clamp": "Clamp", "lobes": "Lobes", "bicubicB": "Bicubic B", "bicubicC": "Bicubic C"}

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

    def expected_visible(filter_id):
        return {
            "clamp": filter_id in CLAMPED,
            "lobes": filter_id == "sinc",
            "bicubicB": filter_id == "bicubic",
            "bicubicC": filter_id == "bicubic",
        }

    def check_state(name, node, filter_id):
        for script_name, want in expected_visible(filter_id).items():
            param = node.getParam(script_name)
            require(param is not None, "%s has %s" % (name, script_name))
            check(param.getIsVisible() == want, "%s %s visible=%s with %s" % (name, script_name, want, filter_id))
            check(param.getIsEnabled(), "%s %s stays enabled with %s" % (name, script_name, filter_id))

    def filter_controls_present(node):
        choice = node.getParam("filter")
        return choice is not None and choice.getIsVisible()

    def check_all_filters(name, node):
        choice = node.getParam("filter")
        options = choice.getOptions()
        check(tuple(options) == FILTERS, "%s offers the seventeen filters in order, got %r" % (name, options))
        for filter_id in FILTERS:
            choice.set(filter_id)
            yield ("sleep", 100)
            check_state(name, node, filter_id)
        report("NOTE %s visibility checked over %d filters" % (name, len(FILTERS)))

    def check_other_nodes():
        for name in OTHER_NODES:
            node = app.createNode(name)
            require(node is not None, "create " + name, gt.plugin_problem(name))
            if not filter_controls_present(node):
                report("NOTE %s shows no filter controls here, skipped" % name)
                node.destroy()
                continue
            yield from check_all_filters(name, node)
            node.destroy()
            yield ("sleep", 200)

    def check_after_reload():
        path = os.path.join(gt.OUT, "m270_filter_panels.ntpz")
        created = {}
        for name in CHECKED_NODES:
            node = app.createNode(name)
            require(node is not None, "create " + name, gt.plugin_problem(name))
            if not filter_controls_present(node):
                node.destroy()
                continue
            shown = dict(NODES).get(name, "bicubic")
            node.getParam("filter").set(shown)
            created[name] = (node.getScriptName(), shown)
        yield ("sleep", 300)
        require(app.saveProjectAs(path), "save the project to " + path)
        require(app.resetProject(), "reset the project")
        reloaded = app.loadProject(path)
        yield ("sleep", 800)
        require(reloaded is not None, "reload " + path)
        for name, (script_name, shown) in created.items():
            node = reloaded.getNode(script_name)
            require(node is not None, "the reloaded project has " + name)
            choice = node.getParam("filter")
            check(choice.getOption(choice.getValue()) == shown, "%s keeps filter %s after reload" % (name, shown))
            check_state(name + " after reload", node, shown)
            other = "sinc" if shown != "sinc" else "lanczos3"
            node.getParam("filter").set(other)
            yield ("sleep", 100)
            check_state(name + " after reload", node, other)
        report("NOTE visibility checked after project load on %d nodes" % len(created))

    def check_labels_fit(name, panel, filter_id):
        height = QGuiApplication.primaryScreen().geometry().height()
        shown = ["Filter", "Black outside"] + [LABEL_OF[k] for k, want in expected_visible(filter_id).items() if want]
        for text in shown:
            found = label_widgets(panel, text)
            check(bool(found), "%s shows the %s label with %s" % (name, text, filter_id))
            for w in found:
                bottom = w.mapToGlobal(w.rect().bottomLeft()).y()
                check(0 <= bottom < height, "%s's %s label is on screen, bottom at %d" % (name, text, bottom))
        for key, text in LABEL_OF.items():
            if text not in shown:
                check(not label_widgets(panel, text), "%s has no %s label on screen with %s" % (name, text, filter_id))

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
            yield from check_all_filters(name, node)
            node.getParam("filter").set(shown)
            yield ("sleep", 800)
            check_labels_fit(name, panel, shown)
            screen_crop(name + "_filter_panel", panel)
            del panel
            node.destroy()
            yield ("sleep", 200)
        gt.close_all_panels()
        yield from check_other_nodes()
        yield from check_after_reload()

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
