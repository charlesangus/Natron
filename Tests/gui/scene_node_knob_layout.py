"""The USD scene nodes' properties panels lay their numeric knobs out like image nodes': each row's
spinboxes start at the field column and sit packed side by side, even when a knob has no display
range and so no slider. Screenshots of every USD node's main page and of Transform's, for
comparison, go to $NATRON_GUI_TEST_OUT/shots/.

Usage: Tests/gui/run-gui-test.sh Tests/gui/scene_node_knob_layout.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
# How far right of the field column a row's first spinbox may start: room for a dimension label
# such as "x" or "r" in front of it.
FIELD_START_SLACK_PX = 30
# The widest gap allowed between neighbouring spinboxes of one row: a dimension label and the
# layout spacing.
PACKED_GAP_PX = 30

USD_NODES = [
    "USDCamera",
    "USDLight",
    "USDXformPrim",
    "USDScopePrim",
    "USDEditTransform",
    "USDExpandHierarchy",
    "USDMaterial",
    "USDMerge",
    "USDPrune",
    "USDRead",
    "USDRender",
    "USDWrite",
]
# Every node's shared pages, laid out by the host rather than the node.
COMMON_PAGES = ("Node", "Info")
REFERENCE_NODES = [("Transform", "Transform")]

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QComboBox, QFrame, QLabel, QLineEdit, QScrollArea, QTabWidget

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

    def page_of(panel):
        """The shown page's widget and its name, the current tab of the panel's pages."""
        for tabs in panel.findChildren(QTabWidget):
            if tabs.isVisible() and tabs.currentWidget() is not None:
                page = tabs.currentWidget()
                name = tabs.tabText(tabs.currentIndex())
                if isinstance(page, QScrollArea) and page.widget() is not None:
                    return page.widget(), name
                return page, name
        return None, None

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
        path = os.path.join(SHOT_DIR, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def visible_in(page, widgets):
        shown = []
        for w in widgets:
            try:
                if w.isVisible() and w.width() > 0:
                    shown.append(w)
            except RuntimeError:
                continue
        return shown

    def left_of(page, w):
        return w.mapTo(page, w.rect().topLeft()).x()

    def centre_y_of(page, w):
        return w.mapTo(page, w.rect().center()).y()

    def spinbox_rows(page):
        """The page's visible spinboxes grouped by row, each row ordered left to right."""
        spins = [w for w in visible_in(page, page.findChildren(QLineEdit)) if gt.class_name(w) == "SpinBox"]
        rows = []
        for w in sorted(spins, key=lambda s: centre_y_of(page, s)):
            y = centre_y_of(page, w)
            if rows and abs(rows[-1][0] - y) <= w.height() // 2:
                rows[-1][1].append(w)
            else:
                rows.append((y, [w]))
        return [sorted(r, key=lambda s: left_of(page, s)) for _, r in rows]

    def field_column(page):
        """Where the field column starts: the leftmost text field or menu on the page."""
        fields = visible_in(page, page.findChildren(QLineEdit) + page.findChildren(QComboBox))
        return min(left_of(page, w) for w in fields) if fields else None

    def fields_of(page):
        """The page's visible text fields, spinboxes included, and menus, Natron's ComboBox among them."""
        menus = [w for w in page.findChildren(QFrame) if gt.class_name(w) == "ComboBox"]
        return visible_in(page, page.findChildren(QLineEdit) + page.findChildren(QComboBox) + menus)

    def row_start(page, row, fields):
        """Where row's first field starts: a knob laid on the same line as the one before it
        follows that knob's field, so the row starts at the leftmost field level with it."""
        y = centre_y_of(page, row[0])
        level = [w for w in fields if abs(centre_y_of(page, w) - y) <= row[0].height() // 2]
        return min(left_of(page, w) for w in level + row)

    def check_layout(name, page):
        column = field_column(page)
        rows = spinbox_rows(page)
        if column is None or not rows:
            report("NOTE %s: no numeric knobs on its main page" % name)
            return
        report("NOTE %s: field column at x=%d, %d spinbox row(s)" % (name, column, len(rows)))
        late = []
        spread = []
        fields = fields_of(page)
        for row in rows:
            first = row_start(page, row, fields) - column
            if first > FIELD_START_SLACK_PX:
                late.append(first)
            for a, b in zip(row, row[1:]):
                gap = left_of(page, b) - (left_of(page, a) + a.width())
                if gap > PACKED_GAP_PX:
                    spread.append(gap)
        check(not late, "%s: every spinbox row starts at the field column (late by %r px)" % (name, late))
        check(not spread, "%s: every row's spinboxes are packed (gaps %r px)" % (name, spread))

    def steps():
        gt.section("knob layout")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        nodes = [(n, n) for n in USD_NODES] + REFERENCE_NODES
        for name, plugin_id in nodes:
            gt.close_all_panels()
            node = app.createNode(plugin_id)
            require(node is not None, "create " + name, gt.plugin_problem(plugin_id))
            node.setPosition(0, 0)
            app.selectNode(node, True)
            QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
            require(ok, "open %s's settings panel" % name)
            yield ("sleep", 600)
            panel = panel_of(node)
            enlarge(panel)
            yield ("sleep", 400)
            page, page_name = page_of(panel)
            require(page is not None, "%s's panel shows a page" % name)
            if page_name in COMMON_PAGES:
                report("NOTE %s: no page of its own; %s opens first" % (name, page_name))
            else:
                check_layout(name, page)
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
