"""The Geometry Spreadsheet panel sorting and filtering a large geometry.

Builds a GeoPrimitive grid of $NATRON_GUI_TEST_GRID (default 3000) rows and columns, about 9 million
points at the default, turned on its side by a GeoTransform so that P[y] varies, with the Viewer on
the GeoTransform. Checks:
  - sorting P[y] descending puts the maximum, 0.5, in the top row;
  - clicking another header while a sort builds cancels it (the cancelled count rises);
  - scrolling the table works while the sort is busy;
  - the group "0-9 14" shows 11 rows, and a malformed pattern shows its error and keeps those rows;
  - changing the GeoTransform while a sort builds (held with natronGeoSpreadsheetHoldSorts) cancels
    it and re-sorts the new data.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
BUILD_TIMEOUT_S = 300.0
STARTUP_TIMEOUT_S = 60.0
SPREADSHEET = "geometrySpreadsheet"
GRID = int(os.environ.get("NATRON_GUI_TEST_GRID", "3000"))
POINTS = (GRID + 1) * (GRID + 1)
HALF = 0.5
LIFT = 10.0
GROUP = "0-9 14"
GROUP_ROWS = 11
BAD_PATTERN = "1-x"
FAR_ROW = 1000000


try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QComboBox, QLabel, QTableView, QWidget

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    def make(plugin):
        node = app.createNode(plugin)
        require(node is not None, "create " + plugin, plugin_problem(plugin))
        return node

    grid = make("GeoPrimitive")
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(GRID)
    grid.getParam("gridColumns").setValue(GRID)

    turned = make("GeoTransform")
    turned.getParam("rotate").set(90.0, 0.0, 0.0)
    require(turned.connectInput(0, grid), "connect the GeoTransform to the grid")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    for i, node in enumerate((grid, turned, viewer)):
        node.setPosition(200 * i, 0)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    _found = {}

    def panel():
        if "panel" not in _found:
            panels = widgets_of("GeoSpreadsheetPanel")
            if panels:
                _found["panel"] = hold(panels[0])
        return _found.get("panel")

    def child(cls, name):
        if name not in _found:
            p = panel()
            try:
                c = hold(p.findChild(cls, name)) if p is not None else None
            except RuntimeError:
                return None
            if c is None:
                return None
            _found[name] = c
        return _found[name]

    def panel_state(name):
        return panel().property("natronGeometrySpreadsheet" + name)

    def view_node(node):
        require(viewer.connectInput(0, node), "view " + node.getScriptName())

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def sheet():
        return child(QWidget, SPREADSHEET)

    def sheet_state(name):
        return sheet().property("natronGeoSpreadsheet" + name)

    def table():
        return child(QTableView, SPREADSHEET + "Table")

    def headers():
        m = table().model()
        return [str(m.headerData(c, Qt.Horizontal)) for c in range(m.columnCount())]

    def column_of(name):
        names = headers()
        return names.index(name) if name in names else -1

    def top_value(name):
        c = column_of(name)
        m = table().model()
        if c < 0 or m.rowCount() == 0:
            return None
        try:
            return float(m.index(0, c).data())
        except (TypeError, ValueError):
            return None

    def header():
        return table().horizontalHeader()

    def click_header(name):
        c = column_of(name)
        require(c >= 0, "the table has a " + name + " column", repr(headers()))
        header().sectionClicked.emit(c)

    def sort_idle():
        return not sheet_state("SortBusy")

    def submit_group(text):
        le = group_box().lineEdit()
        le.setText(text)
        le.editingFinished.emit()

    def group_box():
        return child(QComboBox, SPREADSHEET + "Group")

    def error_label():
        return child(QLabel, SPREADSHEET + "Error")

    def busy_label():
        return child(QLabel, SPREADSHEET + "Busy")

    def loaded():
        return (not panel_state("Busy") and table().isVisible() and sheet_state("Class") == "Points"
                and sheet_state("Rows") == POINTS and sort_idle())

    def descending_sort_section():
        section("sort")
        ok = yield ("until", loaded, BUILD_TIMEOUT_S)
        check(ok, "the grid's %d points are listed (rows %r)" % (POINTS, sheet_state("Rows")))
        click_header("P[y]")
        click_header("P[y]")
        check(header().sortIndicatorSection() == column_of("P[y]")
              and header().sortIndicatorOrder() == Qt.DescendingOrder, "P[y] shows a descending sort indicator")
        ok = yield ("until", lambda: sort_idle() and sheet_state("SortsStarted") >= 1, BUILD_TIMEOUT_S)
        check(ok, "the P[y] sort finishes")
        top = top_value("P[y]")
        check(top is not None and abs(top - HALF) < 1e-3, "the top row's P[y] is the maximum %g (got %r)" % (HALF, top))
        check(sheet_state("Rows") == POINTS, "sorting keeps every row (got %r)" % (sheet_state("Rows"),))
        shot("m337-spreadsheet-sort-descending.png")

    def cancel_section():
        section("cancel")
        started = sheet_state("SortsStarted")
        cancelled = sheet_state("SortsCancelled")
        click_header("P[x]")
        busy = bool(sheet_state("SortBusy"))
        check(busy, "the P[x] sort is still building after the click, so there is something to cancel")
        check(bool(busy_label().isVisibleTo(sheet())), "the toolbar shows Sorting while it builds")
        click_header("P[z]")
        check(sheet_state("SortsCancelled") == cancelled + 1,
              "clicking another header during the build cancels it (cancelled %r, was %r)" % (sheet_state("SortsCancelled"), cancelled))
        check(sheet_state("SortsStarted") == started + 2, "both clicks started a build (started %r, was %r)" % (sheet_state("SortsStarted"), started))
        ok = yield ("until", sort_idle, BUILD_TIMEOUT_S)
        check(ok, "the P[z] sort finishes")
        check(header().sortIndicatorSection() == column_of("P[z]"), "the indicator follows the last click")
        shot("m337-spreadsheet-sort-cancel.png")

    def scroll_section():
        section("scroll")
        click_header("P[z]")
        busy = bool(sheet_state("SortBusy"))
        check(busy, "a build is under way while the table scrolls")
        bar = table().verticalScrollBar()
        bar.setValue(FAR_ROW)
        yield ("sleep", 200)
        check(bar.value() == FAR_ROW, "the table scrolls to row %d while busy (at %r)" % (FAR_ROW, bar.value()))
        m = table().model()
        view_row = table().rowAt(table().viewport().height() // 2)
        cell = m.index(view_row, 0).data() if view_row >= 0 else None
        check(cell not in (None, ""), "the rows in view read (row %r, %r)" % (view_row, cell))
        check(bool(sheet_state("SortBusy")) or not busy, "the scroll did not wait for the build")
        shot("m337-spreadsheet-sort-scroll-busy.png")
        ok = yield ("until", sort_idle, BUILD_TIMEOUT_S)
        check(ok, "the sort finishes")
        table().verticalScrollBar().setValue(0)

    def group_section():
        section("group")
        submit_group(GROUP)
        ok = yield ("until", lambda: sort_idle() and sheet_state("Rows") == GROUP_ROWS, BUILD_TIMEOUT_S)
        check(ok, "the group %r shows %d rows (got %r)" % (GROUP, GROUP_ROWS, sheet_state("Rows")))
        check(not error_label().isVisibleTo(sheet()), "a valid pattern shows no error")
        shot("m337-spreadsheet-group-filter.png")
        submit_group(BAD_PATTERN)
        ok = yield ("until", lambda: sort_idle() and error_label().isVisibleTo(sheet()), BUILD_TIMEOUT_S)
        check(ok, "a malformed pattern shows an error (label %r)" % (error_label().text(),))
        check(len(error_label().text()) > 0 and "1-x" in error_label().text(), "the error names the pattern (got %r)" % (error_label().text(),))
        check(sheet_state("Rows") == GROUP_ROWS, "the previous filter stays applied (rows %r)" % (sheet_state("Rows"),))
        shot("m337-spreadsheet-group-error.png")
        submit_group("")
        ok = yield ("until", lambda: sort_idle() and sheet_state("Rows") == POINTS, BUILD_TIMEOUT_S)
        check(ok, "clearing the group lists every point again (rows %r)" % (sheet_state("Rows"),))
        check(not error_label().isVisibleTo(sheet()), "clearing the group clears the error")

    def upstream_section():
        section("upstream")
        if header().sortIndicatorSection() != column_of("P[y]"):
            click_header("P[y]")
            ok = yield ("until", sort_idle, BUILD_TIMEOUT_S)
            require(ok, "the P[y] sort finishes before the upstream change")
        cancelled = sheet_state("SortsCancelled")
        # The new snapshot only lands once the 9M-point transform has cooked, which a sort can outrun.
        sheet().setProperty("natronGeoSpreadsheetHoldSorts", True)
        click_header("P[y]")
        check(bool(sheet_state("SortBusy")), "a P[y] sort is building when the transform changes")
        order = header().sortIndicatorOrder()
        turned.getParam("translate").set(0.0, LIFT, 0.0)
        ok = yield ("until", lambda: panel_state("Busy") is False and sheet_state("SortsCancelled") > cancelled, BUILD_TIMEOUT_S)
        sheet().setProperty("natronGeoSpreadsheetHoldSorts", False)
        check(ok, "the change cancelled the sort under way (cancelled %r, was %r)" % (sheet_state("SortsCancelled"), cancelled))
        want = HALF + LIFT if order == Qt.DescendingOrder else -HALF + LIFT
        ok = yield ("until", lambda: sort_idle() and not panel_state("Busy")
                    and top_value("P[y]") is not None and abs(top_value("P[y]") - want) < 1e-3, BUILD_TIMEOUT_S)
        check(ok, "the new data is sorted, the top row's P[y] is %g (got %r)" % (want, top_value("P[y]")))
        check(header().sortIndicatorSection() == column_of("P[y]"), "the new snapshot keeps the sort column")
        shot("m337-spreadsheet-sort-upstream.png")

    def steps():
        view_node(turned)
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, BUILD_TIMEOUT_S)
        require(ok, "the scene view shows the grid")
        require(panel() is not None, "the Geometry Spreadsheet panel exists")

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("geometrySpreadsheet", split), "move the Geometry Spreadsheet panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the panel shows")
        require(sheet() is not None and table() is not None, "the panel has its table")
        require(group_box() is not None and error_label() is not None and busy_label() is not None, "the panel has its toolbar")

        yield from descending_sort_section()
        yield from cancel_section()
        yield from scroll_section()
        yield from group_section()
        yield from upstream_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
