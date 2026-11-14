"""One reload control on each panel with a file knob and a reload of its own: USDRead, USDReference,
USDCamera and Read each show their Reload button on the file's row and no reload icon of the file
knob.

Screenshots (cropped): file-knob-reload-<node>.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/file_knob_reload.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
FILE_RELOAD_TOOLTIP = "Reload the file"
ROW_TOLERANCE_PX = 6
NODES = ("USDRead", "USDReference", "USDCamera", "Read")

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QPushButton

    import guitest as gt
    from guitest import check, report, require

    # PySide wrappers of the panels and nodes stay referenced here while the script inspects them.
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

    def open_panel(node):
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())

    def visible_buttons(panel):
        found = []
        for b in panel.findChildren(QPushButton):
            try:
                if b.isVisible():
                    found.append(b)
            except RuntimeError:
                continue
        return found

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def panel_section(plugin, index):
        gt.section(plugin)
        node = app.createNode(plugin)
        require(node is not None, "create %s" % plugin, gt.plugin_problem(plugin))
        node.setPosition(200 * index, 100)
        state["node" + plugin] = node
        if plugin == "USDCamera":
            read_from_file = node.getParam("readFromFile")
            require(read_from_file is not None, "the USDCamera has readFromFile")
            # Its file knob and Reload are hidden until the camera comes from a file.
            read_from_file.set(True)

        yield from open_panel(node)
        yield ("sleep", 800)
        panel = panel_of(node)
        require(panel is not None, "%s's panel is open" % plugin)
        state["panel" + plugin] = panel
        buttons = visible_buttons(panel)
        state["buttons" + plugin] = buttons
        reloads = [b for b in buttons if b.text() == "Reload"]
        icons = [b for b in buttons if FILE_RELOAD_TOOLTIP in (b.toolTip() or "")]
        fields = [e for e in panel.findChildren(QLineEdit) if e.isVisible() and e.placeholderText() == "File path..."]
        state["fields" + plugin] = fields
        check(len(reloads) == 1, "%s has one Reload button (%d)" % (plugin, len(reloads)))
        check(not icons, "%s's file knobs show no reload icon (%d)" % (plugin, len(icons)))
        check(len(fields) >= 1, "%s has a file field (%d)" % (plugin, len(fields)))
        if len(reloads) == 1 and fields:
            reload_y = reloads[0].mapTo(panel, reloads[0].rect().center()).y()
            same_row = [f for f in fields if abs(f.mapTo(panel, f.rect().center()).y() - reload_y) <= ROW_TOLERANCE_PX]
            check(len(same_row) == 1, "%s's Reload is on the file's row (%d)" % (plugin, len(same_row)))
        screen_crop("file-knob-reload-%s.png" % plugin.lower(), panel)
        gt.close_all_panels()

    def steps():
        # A project started from a script has no Viewer node, so there is no viewer to wait for.
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        for index, plugin in enumerate(NODES):
            yield from panel_section(plugin, index)

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
