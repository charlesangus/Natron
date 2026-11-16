"""Components picked in the 3D view become the selected rows of the prim inspector's Spreadsheet tab.

Builds a GeoPrimitive cube (5 subdivisions each way) with the Viewer on it, and the Scene Graph panel
beside the viewer. Picks are driven through the SceneViewport the way m361_geo_pick.py does. Checks:
  - a click on the cube's corner in points mode selects that point's row in Points and scrolls to it;
  - a marquee in primitives mode selects the primitives' rows in the Primitives tab;
  - an edge pick shows its two endpoints, selected, in the Points tab;
  - under a descending sort a marquee selects the rows that show the picked points, and reordering
    keeps the viewer's selection selected;
  - with a group filter hiding some of the picked points, the selection note reports how many;
  - Objects mode clears the rows and the note;
  - selecting rows in the table does not echo back into the viewer's selection.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
PICK_TIMEOUT_S = 3.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
DEFAULT_NAVIGATION_PRESET = 0
SPREADSHEET = "sceneGraphSpreadsheet"
SPREADSHEET_TITLE = "Spreadsheet"
SUBDIVISIONS = 5
POINTS = ("points", 1, "Points")
EDGES = ("edges", 2, "Points")
PRIMITIVES = ("primitives", 3, "Primitives")
OBJECTS = ("objects", 0, None)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelection, QItemSelectionModel, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QComboBox, QTabWidget, QTableView, QWidget

    from guitest import check, finish, plugin_problem, require, run, section, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    cube = app.createNode("GeoPrimitive")
    require(cube is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    cube.getParam("type").setValue(2)
    cube.getParam("cubeSubdivisions").set(SUBDIVISIONS, SUBDIVISIONS, SUBDIVISIONS)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    cube.setPosition(0, 0)
    viewer.setPosition(0, 120)
    require(viewer.connectInput(0, cube), "view the cube")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def count():
        return int(prop("natronGeoSelectionCount", 0))

    def pattern():
        return str(prop("natronGeoSelection", ""))

    def picked_numbers():
        """The numbers the viewer's selection names: its element numbers, or an edge's two points."""
        numbers = set()
        for token in pattern().split():
            token = token.lstrip("p")
            if "-" in token and not token.startswith("-"):
                parts = re.findall(r"\d+", token)
                if len(parts) == 2 and prop("natronGeoPickMode") != "edges":
                    numbers.update(range(int(parts[0]), int(parts[1]) + 1))
                else:
                    numbers.update(int(x) for x in parts)
            elif token.isdigit():
                numbers.add(int(token))
        return numbers

    _found = {}

    def panel():
        if "panel" not in _found:
            panels = widgets_of("SceneGraphPanel")
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

    def sheet():
        return child(QWidget, SPREADSHEET)

    def sheet_state(name):
        return sheet().property("natronGeoSpreadsheet" + name)

    def table():
        return child(QTableView, SPREADSHEET + "Table")

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

    def selected_elements():
        """The element numbers of the rows selected in the table, from its vertical header."""
        t = table()
        model = t.model()
        rows = sorted({i.row() for i in t.selectionModel().selectedIndexes()})
        return {int(model.headerData(r, Qt.Vertical)) for r in rows}, rows

    def row_visible(row):
        t = table()
        rect = t.visualRect(t.model().index(row, 0))
        return rect.isValid() and t.viewport().rect().intersects(rect)

    def rows_match_pick():
        elements, _ = selected_elements()
        return len(elements) > 0 and elements == picked_numbers()

    def show_spreadsheet_tab():
        tabs = child(QTabWidget, "sceneGraphInspectorTabs")
        titles = [tabs.tabText(i) for i in range(tabs.count())]
        if SPREADSHEET_TITLE not in titles:
            return False
        tabs.setCurrentIndex(titles.index(SPREADSHEET_TITLE))
        return True

    def framebuffer():
        v = view()
        image = v.grab().toImage()
        if image.isNull():
            origin = v.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(), v.height()).toImage()
        return image

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers))

    def click(pos, modifiers=Qt.NoModifier):
        v = view()
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()

    def marquee(start, end, steps=10):
        v = view()
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            mouse(v, QEvent.MouseMove, start + (end - start) * (float(i) / steps), Qt.NoButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def key(k):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k)
        QApplication.processEvents()

    def square():
        """The cube's square in the front view: its centre and its half extent."""
        image = framebuffer()
        background = image.pixel(2, 2)
        cy = image.height() // 2
        cx = image.width() // 2
        right = cx
        for x in range(cx, image.width() - 2):
            if image.pixel(x, cy - 3) != background:
                right = x
        top = cy
        for y in range(cy, 1, -1):
            if image.pixel(cx + 3, y) != background:
                top = y
        return QPoint(cx, cy), right - cx, cy - top

    def around(pos, radius=6, step=3):
        yield pos
        for r in range(step, radius + 1, step):
            for dx, dy in ((-r, r), (-r, 0), (0, r), (r, r), (r, 0), (0, -r), (-r, -r), (r, -r), (-r, r)):
                yield pos + QPoint(dx, dy)

    def pick_until(positions, want):
        for pos in positions:
            click(pos)
            ok = yield ("until", want, PICK_TIMEOUT_S)
            if ok:
                return pos
        return None

    def set_mode(mode):
        key((Qt.Key_F8, Qt.Key_F9, Qt.Key_F10, Qt.Key_F11)[mode[1]])
        return ("until", lambda: prop("natronGeoPickMode") == mode[0], PICK_TIMEOUT_S)

    def whole_view():
        v = view()
        return QPoint(3, 3), QPoint(v.width() - 4, v.height() - 4)

    def sort_by(column):
        header = table().horizontalHeader()
        header.sectionClicked.emit(column)
        header.sectionClicked.emit(column)
        return ("until", lambda: not sheet_state("SortBusy"), POLL_TIMEOUT_S)

    def first_element():
        return int(table().model().headerData(0, Qt.Vertical))

    def points_section(centre, half_w, half_h):
        section("points")
        yield set_mode(POINTS)
        yield ("sleep", SETTLE_MS)
        table().verticalScrollBar().setValue(table().verticalScrollBar().maximum())
        corner = centre + QPoint(half_w, -half_h)
        before = pattern()
        hit = yield from pick_until(around(corner), lambda: count() == 1 and pattern() != before)
        require(hit is not None, "a click on the cube's corner selects one point (%r)" % pattern())
        ok = yield ("until", lambda: sheet_state("Class") == "Points" and rows_match_pick(), POLL_TIMEOUT_S)
        elements, rows = selected_elements()
        check(ok, "the corner point's row is selected in Points (picked %r, rows %r)" % (picked_numbers(), sorted(elements)))
        check(len(rows) == 1 and row_visible(rows[0]), "the table scrolled to the point's row (row %r)" % (rows,))
        check(not sheet().property("natronGeoSpreadsheetSelectionNote") or "hidden" not in sheet().property("natronGeoSpreadsheetSelectionNote"),
              "no point is hidden by a filter")
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-point.png")

    def primitives_section(start, end):
        section("primitives")
        yield set_mode(PRIMITIVES)
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 1 and sheet_state("Class") == "Primitives" and rows_match_pick(), POLL_TIMEOUT_S)
        elements, _ = selected_elements()
        check(ok, "a primitives marquee selects its rows in Primitives (picked %r, rows %r)" % (sorted(picked_numbers()), sorted(elements)))
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-primitives.png")

    def edges_section(centre, half_w):
        section("edges")
        yield set_mode(EDGES)
        yield ("sleep", SETTLE_MS)
        before = pattern()
        side = centre + QPoint(half_w, 0)
        hit = yield from pick_until(around(side, radius=4, step=2), lambda: count() == 1 and pattern() != before)
        require(hit is not None, "a click on the cube's right side selects one edge (%r)" % pattern())
        ok = yield ("until", lambda: sheet_state("Class") == "Points" and rows_match_pick(), POLL_TIMEOUT_S)
        elements, rows = selected_elements()
        check(ok and len(rows) == 2, "the edge's two endpoints are selected in Points (edge %r, rows %r)" % (pattern(), sorted(elements)))
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-edge.png")

    def sort_section(start, end):
        section("sort")
        yield set_mode(POINTS)
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 2 and rows_match_pick(), POLL_TIMEOUT_S)
        require(ok, "a points marquee selects its rows (%r)" % (pattern(),))
        columns = table().model().columnCount()
        reordered = False
        for column in range(columns):
            yield sort_by(column)
            if first_element() != 0:
                reordered = True
                break
        require(reordered, "a descending sort reorders the Points rows")
        ok = yield ("until", rows_match_pick, POLL_TIMEOUT_S)
        _, rows = selected_elements()
        check(ok and rows != sorted(picked_numbers()), "after the sort the rows showing the picked points are selected (rows %r)" % (rows[:8],))
        before = pattern()
        marquee(QPoint(3, 3), QPoint(view().width() // 2, view().height() - 4))
        ok = yield ("until", lambda: pattern() != before and rows_match_pick(), POLL_TIMEOUT_S)
        elements, rows = selected_elements()
        check(ok, "under the sort a new marquee selects the right rows (picked %r, rows %r)" % (sorted(picked_numbers()), sorted(elements)))
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-sorted.png")

    def echo_section():
        section("echo")
        before = pattern()
        t = table()
        model = t.model()
        t.selectionModel().select(QItemSelection(model.index(0, 0), model.index(0, model.columnCount() - 1)),
                                  QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: count() == 1 and pattern() != before, POLL_TIMEOUT_S)
        elements, rows = selected_elements()
        check(ok and rows == [0] and elements == picked_numbers(),
              "selecting a row selects its point in the view and the table keeps just that row (view %r, rows %r)" % (pattern(), rows))

    def filter_section(start, end):
        section("filter")
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 4 and rows_match_pick(), POLL_TIMEOUT_S)
        require(ok, "a marquee selects points again (%r)" % (pattern(),))
        picked = picked_numbers()
        group = child(QComboBox, SPREADSHEET + "Group")
        require(group is not None, "the spreadsheet has a Group box")
        group.setEditText("0-3")
        group.lineEdit().editingFinished.emit()
        ok = yield ("until", lambda: sheet_state("Rows") == 4 and not sheet_state("SortBusy"), POLL_TIMEOUT_S)
        require(ok, "the group filter leaves 4 rows (%r)" % (sheet_state("Rows"),))
        hidden = len([n for n in picked if n > 3])
        shown = len([n for n in picked if n <= 3])
        want = "%d selected, %d hidden by filter" % (len(picked), hidden)
        ok = yield ("until", lambda: sheet_state("SelectionNote") == want, POLL_TIMEOUT_S)
        check(ok, "the note reports the hidden points (want %r, got %r)" % (want, sheet_state("SelectionNote")))
        elements, _ = selected_elements()
        check(elements == {n for n in picked if n <= 3} and len(elements) == shown,
              "only the picked points the filter shows are selected (rows %r)" % (sorted(elements),))
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-filter-note.png")

    def objects_section():
        section("objects")
        yield set_mode(OBJECTS)
        ok = yield ("until", lambda: not table().selectionModel().hasSelection() and sheet_state("SelectionNote") == "", POLL_TIMEOUT_S)
        check(ok, "Objects mode clears the rows and the note")
        yield ("sleep", SETTLE_MS)
        shot("m337-viewer-objects.png")

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the cube (%r)" % (prop("natronSceneError"),))

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel() is not None and panel().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the panel shows")
        require(show_spreadsheet_tab(), "the inspector has a Spreadsheet tab")
        require(sheet() is not None and table() is not None, "the Spreadsheet tab has its table")
        ok = yield ("until", lambda: sheet_state("Rows") is not None and sheet_state("Rows") > 0 and table().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the table lists the cube")

        key(Qt.Key_Z)
        yield ("until", lambda: prop("natronSceneViewMode") == "front", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield ("sleep", SETTLE_MS)
        centre, half_w, half_h = square()
        require(10 < half_w < view().width() // 2 - 4 and 10 < half_h < view().height() // 2 - 4,
                "the cube draws as a square inside the front view")
        start, end = whole_view()

        yield from points_section(centre, half_w, half_h)
        yield from primitives_section(start, end)
        yield from edges_section(centre, half_w)
        yield from sort_section(start, end)
        yield from echo_section()
        yield from filter_section(start, end)
        yield from objects_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
