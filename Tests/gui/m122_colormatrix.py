"""The native ColorMatrix panel shows its matrix as a grid of plain numeric fields: 9 at 3x3 (the
default) and 16 at 4x4, headed R G B (A) across and down. A screenshot of the panel in each size
goes to $NATRON_GUI_TEST_OUT as colormatrix_3x3.png and colormatrix_4x4.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m122_colormatrix.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "ColorMatrix"
SIZE_KNOB = "matrixSize"
MATRIX_KNOB = "matrix"
GRID_NAME = "KnobMatrixGrid"

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

    def grid_of(panel):
        grids = [g for g in panel.findChildren(QWidget, GRID_NAME) if g.isVisible()]
        return grids[0] if len(grids) == 1 else None

    def visible_fields(grid):
        # The fields are numeric line edits; the headers are labels, so they are not counted.
        return [w for w in grid.findChildren(QLineEdit) if w.isVisibleTo(grid)]

    def visible_headers(grid):
        return sorted(w.text() for w in grid.findChildren(QLabel) if w.isVisibleTo(grid) and w.text().strip())

    def field_count(panel):
        grid = grid_of(panel) if panel is not None else None
        return len(visible_fields(grid)) if grid is not None else -1

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
        gt.section("colormatrix")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create ColorMatrix", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open ColorMatrix's settings panel")
        yield ("sleep", 800)
        panel = panel_of(node)
        enlarge(panel)
        yield ("sleep", 300)

        size = node.getParam(SIZE_KNOB)
        matrix = node.getParam(MATRIX_KNOB)
        require(size is not None and matrix is not None, "ColorMatrix has %s and %s knobs" % (SIZE_KNOB, MATRIX_KNOB))
        check(size.getValue() == 0, "the Matrix choice defaults to 3x3")
        require(grid_of(panel) is not None, "the panel shows one matrix grid")

        # An alpha coefficient that only 4x4 shows, to check that switching sizes keeps it.
        matrix.setValue(0.25, 3)
        yield ("sleep", 200)

        ok = yield ("until", lambda: field_count(panel_of(node)) == 9, POLL_TIMEOUT_S)
        check(ok, "3x3 shows 9 fields (got %d)" % field_count(panel_of(node)))
        grid = grid_of(panel_of(node))
        if grid is not None:
            headers = visible_headers(grid)
            check(headers == sorted(["R", "G", "B"] * 2), "3x3 headers are R G B across and down (got %r)" % headers)
        screen_crop("colormatrix_3x3", panel_of(node))

        size.setValue(1)
        ok = yield ("until", lambda: field_count(panel_of(node)) == 16, POLL_TIMEOUT_S)
        check(ok, "4x4 shows 16 fields (got %d)" % field_count(panel_of(node)))
        grid = grid_of(panel_of(node))
        if grid is not None:
            headers = visible_headers(grid)
            check(headers == sorted(["R", "G", "B", "A"] * 2), "4x4 headers are R G B A across and down (got %r)" % headers)
            fields = visible_fields(grid)
            check(any(f.text().strip() in ("0.25", "0.250") for f in fields), "the 4x4 grid shows the stored alpha coefficient")
        yield ("sleep", 300)
        screen_crop("colormatrix_4x4", panel_of(node))

        size.setValue(0)
        ok = yield ("until", lambda: field_count(panel_of(node)) == 9, POLL_TIMEOUT_S)
        check(ok, "back at 3x3 the grid shows 9 fields again")
        check(abs(matrix.getValue(3) - 0.25) < 1e-9, "switching sizes keeps the alpha coefficient")
        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
