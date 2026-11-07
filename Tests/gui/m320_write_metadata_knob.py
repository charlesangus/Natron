"""The Write panel's Metadata choice: four entries, defaulting to All Metadata Except Input.

Screenshot (whole screen): m320-write-metadata-panel.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m320_write_metadata_knob.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
WRITE_ID = "Write"
CONSTANT_ID = "Constant"
EXPECTED_ENTRIES = ["none", "default", "allExceptInput", "all"]
EXPECTED_DEFAULT = "allExceptInput"
EXPECTED_LABELS = ["No Metadata", "Default Metadata", "All Metadata Except Input", "All Metadata"]

try:
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    WRITE_PATH = os.path.join(gt.OUT, "fixtures", "m320", "m320-write.exr")

    # PySide wrappers of the panel and nodes stay referenced here while the script inspects them.
    state = {}

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

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

    def panel_labels(panel):
        found = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip():
                    found.append(w.text().strip().rstrip(":"))
            except RuntimeError:
                continue
        return found

    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer node exists", gt.plugin_problem("Viewer"))
    state["viewer"] = viewer

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        gt.section("Write metadata knob")
        constant = app.createNode(CONSTANT_ID)
        require(constant is not None, "create a Constant", gt.plugin_problem(CONSTANT_ID))
        constant.setPosition(200, 100)
        state["constant"] = constant
        write = app.createWriter(WRITE_PATH)
        require(write is not None, "create a Write")
        require(write.getPluginID() == WRITE_ID, "it is a native Write (%s)" % write.getPluginID())
        write.setPosition(200, 350)
        state["write"] = write
        require(write.connectInput(0, constant), "connect the Constant to the Write")
        check(write.getParam("writeMetadata") is None, "the old writeMetadata bool is gone")

        knob = write.getParam("metadata")
        require(knob is not None, "the Write has a metadata parameter")
        state["knob"] = knob
        entries = [str(knob.getOption(i)) for i in range(knob.getNumOptions())]
        check(entries == EXPECTED_ENTRIES, "the four entries read %r (got %r)" % (EXPECTED_ENTRIES, entries))
        current = entries[knob.get()] if 0 <= knob.get() < len(entries) else None
        check(current == EXPECTED_DEFAULT, "the default is %s (got %r)" % (EXPECTED_DEFAULT, current))

        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(write, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(write) is not None, POLL_TIMEOUT_S)
        require(ok, "open the Write's settings panel")
        yield ("sleep", 800)
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        state["panel"] = panel
        labels = panel_labels(panel)
        report("NOTE panel labels: %r" % labels)
        check("Metadata" in labels, "the panel shows the Metadata label")
        menus = []
        for box in gt.widgets_of("ComboBox"):
            try:
                if box.isVisible() and panel.isAncestorOf(box):
                    texts = [str(i) for i in (box.property("natronItems") or [])]
                    if "No Metadata" in texts:
                        menus.append((box, texts))
            except RuntimeError:
                continue
        require(len(menus) == 1, "the panel has one metadata menu (%d)" % len(menus))
        box, texts = menus[0]
        state["box"] = box
        check(texts == EXPECTED_LABELS, "the menu entries read %r (got %r)" % (EXPECTED_LABELS, texts))
        check(str(box.property("natronCurrentText")) == "All Metadata Except Input", "the menu shows All Metadata Except Input (got %r)" % str(box.property("natronCurrentText")))
        check(box.width() >= box.fontMetrics().horizontalAdvance("All Metadata Except Input"), "the menu is wide enough for its longest entry (%d px)" % box.width())
        gt.shot("m320-write-metadata-panel.png")
        gt.close_all_panels()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
