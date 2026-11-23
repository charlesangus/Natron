"""The Geometry Spreadsheet panel on the geometry the viewer shows.

Builds, with the Viewer on each in turn: a GeoPrimitive grid of 2 rows by 3 columns; a GeoDelete
deleting points 0-3 of it; a USDRead of a scene written under $NATRON_GUI_TEST_OUT feeding a packed
USDToGeo; and a GeoTransform on the grid whose translate[y] is keyed 0 at frame 1 and 4 at frame 9.
Checks:
  - the grid lists its whole detail with no prim selected: 12 points, 24 vertices, 6 primitives and
    1 detail row, in the class buttons and in the table;
  - after the GeoDelete the 8 points left are numbered 0 to 7, and the vertices name only those;
  - the packed USDToGeo output lists intrinsic:* columns on its primitives, and the tooltip of an
    intrinsic:transform cell holds all 16 values of the matrix;
  - scrubbing the frame on the animated GeoTransform updates P[y], and the column widths, one of
    them set by hand, stay put;
  - with the grid viewed and one of its prims selected in the Scene Graph panel, the Scene Graph's
    Spreadsheet tab lists none of the grid's attributes and points to the Geometry Spreadsheet
    panel, which still lists them;
  - following the node selection (geometrySpreadsheetFollow), with the viewer still on the grid,
    the panel lists the selected GeoDelete; selecting the Viewer, which puts out no geometry, keeps
    it; pinned (geometrySpreadsheetPin), selecting the GeoTransform keeps the GeoDelete; unpinned,
    the panel moves to the selected GeoTransform; back to following the viewer it lists the grid.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
SPREADSHEET = "geometrySpreadsheet"
SCENE_GRAPH_SPREADSHEET = "sceneGraphSpreadsheet"
SCENE_GRAPH_SPREADSHEET_TITLE = "Spreadsheet"
CLASSES = ("Points", "Vertices", "Primitives", "Detail")
GRID_ROWS = 2
GRID_COLUMNS = 3
GRID_COUNTS = {
    "Points": (GRID_ROWS + 1) * (GRID_COLUMNS + 1),
    "Vertices": 4 * GRID_ROWS * GRID_COLUMNS,
    "Primitives": GRID_ROWS * GRID_COLUMNS,
    "Detail": 1,
}
# Deleting the first row of points takes the first row of faces with it.
DELETED_COUNTS = {
    "Points": GRID_ROWS * (GRID_COLUMNS + 1),
    "Vertices": 4 * (GRID_ROWS - 1) * GRID_COLUMNS,
    "Primitives": (GRID_ROWS - 1) * GRID_COLUMNS,
    "Detail": 1,
}
INTRINSICS = ["intrinsic:kind", "intrinsic:transform", "intrinsic:bounds", "intrinsic:sourcePath"]
KEYS = ((1, 0.0), (9, 4.0))
SCRUB = (1, 3, 5, 7, 9)
HAND_WIDTH = 173


def write_scene(directory):
    """Writes two meshes under /World, one moved, and returns the layer's path."""
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "packed.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "World"\n{\n')
        for name, offset in (("Left", 0.0), ("Right", 3.0)):
            f.write('    def Mesh "%s"\n    {\n' % name)
            f.write('        double3 xformOp:translate = (%g, 2, -1)\n' % offset)
            f.write('        uniform token[] xformOpOrder = ["xformOp:translate"]\n')
            f.write('        int[] faceVertexCounts = [3]\n')
            f.write('        int[] faceVertexIndices = [0, 1, 2]\n')
            f.write('        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n')
            f.write('    }\n')
        f.write('}\n')
    return path


try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QComboBox, QLabel, QTabWidget, QTableView, QToolButton, QTreeView, QWidget

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    fixture_dir = os.path.realpath(os.path.join(OUT, "m337_spreadsheet_geo"))
    scene_path = write_scene(fixture_dir)

    def make(plugin):
        node = app.createNode(plugin)
        require(node is not None, "create " + plugin, plugin_problem(plugin))
        return node

    grid = make("GeoPrimitive")
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(GRID_ROWS)
    grid.getParam("gridColumns").setValue(GRID_COLUMNS)

    delete = make("GeoDelete")
    delete.getParam("groupType").setValue(0)
    delete.getParam("group").setValue("0-3")
    require(delete.connectInput(0, grid), "connect the GeoDelete to the grid")

    reader = make("USDRead")
    reader.getParam("filename").setValue(scene_path)
    packed = make("USDToGeo")
    packed.getParam("primPattern").setValue("/World/*")
    packed.getParam("unpack").setValue(False)
    require(packed.connectInput(0, reader), "connect the USDToGeo to the USDRead")

    moved = make("GeoTransform")
    translate = moved.getParam("translate")
    for frame, y in KEYS:
        translate.setValueAtTime(y, frame, 1)
    require(moved.connectInput(0, grid), "connect the GeoTransform to the grid")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    for i, node in enumerate((grid, delete, reader, packed, moved, viewer)):
        node.setPosition(200 * (i % 3), 120 * (i // 3))

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

    def found_panel(key, class_name):
        if key not in _found:
            panels = widgets_of(class_name)
            if panels:
                _found[key] = hold(panels[0])
        return _found.get(key)

    def panel():
        return found_panel("panel", "GeoSpreadsheetPanel")

    def scene_graph():
        return found_panel("sceneGraph", "SceneGraphPanel")

    def child_of(p, cls, name):
        if name not in _found:
            try:
                c = hold(p.findChild(cls, name)) if p is not None else None
            except RuntimeError:
                return None
            if c is None:
                return None
            _found[name] = c
        return _found[name]

    def child(cls, name):
        return child_of(panel(), cls, name)

    def panel_state(name):
        return panel().property("natronGeometrySpreadsheet" + name)

    def sg_child(cls, name):
        return child_of(scene_graph(), cls, name)

    def sg_inspector_state(name):
        return sg_child(QWidget, "sceneGraphInspector").property("natronSceneGraphInspector" + name)

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

    def class_button(label):
        return child(QToolButton, SPREADSHEET + label)

    def message():
        return child(QLabel, SPREADSHEET + "Message")

    def headers():
        m = table().model()
        return [str(m.headerData(c, Qt.Horizontal)) for c in range(m.columnCount())]

    def row_numbers():
        m = table().model()
        return [m.headerData(r, Qt.Vertical) for r in range(m.rowCount())]

    def column_cells(name, role=Qt.DisplayRole):
        m = table().model()
        names = headers()
        if name not in names:
            return None
        c = names.index(name)
        return ["" if m.index(r, c).data(role) is None else str(m.index(r, c).data(role)) for r in range(m.rowCount())]

    def numbers(cells):
        try:
            return [float(x) for x in cells] if cells is not None else None
        except ValueError:
            return None

    def show_class(label):
        class_button(label).click()
        return sheet_state("Class") == label

    def class_counts():
        counts = {}
        for label in CLASSES:
            text = class_button(label).text()
            try:
                counts[label] = int(text.split()[-1])
            except (ValueError, IndexError):
                counts[label] = text
        return counts

    def settled():
        return not panel_state("Busy") and table().isVisible()

    def shows_counts(counts):
        return settled() and class_counts() == counts

    def show_scene_graph_spreadsheet_tab():
        tabs = sg_child(QTabWidget, "sceneGraphInspectorTabs")
        titles = [tabs.tabText(i) for i in range(tabs.count())]
        if SCENE_GRAPH_SPREADSHEET_TITLE not in titles:
            return False
        tabs.setCurrentIndex(titles.index(SCENE_GRAPH_SPREADSHEET_TITLE))
        return True

    def view_node(node):
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "view " + node.getScriptName())

    def class_rows():
        """Shows each class in turn, and returns the rows the table listed for each."""
        listed = {}
        for label in CLASSES:
            if show_class(label):
                listed[label] = int(sheet_state("Rows"))
        return listed

    def grid_section():
        section("grid")
        view_node(grid)
        ok = yield ("until", lambda: shows_counts(GRID_COUNTS), POLL_TIMEOUT_S)
        check(ok, "the grid's class buttons count its detail (got %r, node %r)"
              % (class_counts(), panel_state("Node")))
        listed = class_rows()
        check(listed == GRID_COUNTS, "the table lists every element of each class (got %r)" % (listed,))
        check(show_class("Points"), "back to the Points class")
        names = headers()
        report("NOTE grid point columns: %r" % (names,))
        check(names[:3] == ["P[x]", "P[y]", "P[z]"], "the points list P one column per component (got %r)" % (names[:3],))
        check(numbers(column_cells("P[y]")) == [0.0] * GRID_COUNTS["Points"], "the grid lies flat (P[y] %r)" % (column_cells("P[y]"),))
        shot("m337-spreadsheet-geo-grid.png")

    def delete_section():
        section("delete")
        view_node(delete)
        ok = yield ("until", lambda: shows_counts(DELETED_COUNTS), POLL_TIMEOUT_S)
        check(ok, "after the GeoDelete the class buttons count what is left (got %r)" % (class_counts(),))
        points = DELETED_COUNTS["Points"]
        check(show_class("Points") and row_numbers() == list(range(points)),
              "the points left are numbered 0 to %d (got %r)" % (points - 1, row_numbers()))
        check(show_class("Vertices"), "the Vertices class is shown")
        named = column_cells("Point")
        named = sorted(set(int(x) for x in named)) if named else named
        check(named == list(range(points)), "the vertices name the dense point numbers only (got %r)" % (named,))
        check(row_numbers() == list(range(DELETED_COUNTS["Vertices"])), "the vertices are numbered densely (got %r)" % (row_numbers(),))
        shot("m337-spreadsheet-geo-delete.png")

    def packed_section():
        section("packed")
        view_node(packed)
        ok = yield ("until", lambda: settled() and class_counts().get("Primitives") == 2, POLL_TIMEOUT_S)
        check(ok, "the packed USDToGeo lists one primitive per mesh (got %r, message %r)" % (class_counts(), message().text()))
        check(show_class("Primitives"), "the Primitives class is shown")
        names = headers()
        report("NOTE packed primitive columns: %r" % (names,))
        intrinsics = [n for n in names if n.startswith("intrinsic:")]
        check(intrinsics == INTRINSICS, "the packed primitives list the intrinsic columns (got %r)" % (intrinsics,))
        tips = column_cells("intrinsic:transform", Qt.ToolTipRole) or []
        report("NOTE intrinsic:transform tooltips: %r" % (tips,))
        matrices = [numbers(tip.split()) for tip in tips]
        check(len(matrices) == 2 and all(m is not None and len(m) == 16 for m in matrices),
              "each intrinsic:transform tooltip holds all 16 values (got %r)" % (tips,))
        check(any(m is not None and 3.0 in m for m in matrices),
              "a tooltip carries the moved mesh's translation (got %r)" % (tips,))
        shot("m337-spreadsheet-geo-packed.png")

    def scrub_section(py_viewer):
        section("scrub")
        py_viewer.seek(KEYS[0][0])
        view_node(moved)
        points = GRID_COUNTS["Points"]
        ok = yield ("until", lambda: shows_counts(GRID_COUNTS) and show_class("Points")
                    and numbers(column_cells("P[y]")) == [KEYS[0][1]] * points, POLL_TIMEOUT_S)
        check(ok, "at frame %d P[y] is %g (got %r)" % (KEYS[0][0], KEYS[0][1], column_cells("P[y]")))
        names = headers()
        t = table()
        if "P[y]" in names:
            t.setColumnWidth(names.index("P[y]"), HAND_WIDTH)
        widths = [t.columnWidth(c) for c in range(len(names))]
        report("NOTE widths before scrubbing: %r" % (widths,))
        for frame in SCRUB[1:]:
            py_viewer.seek(frame)
            yield ("sleep", 60)
        last, y = KEYS[-1]
        ok = yield ("until", lambda: settled() and numbers(column_cells("P[y]")) == [y] * points, POLL_TIMEOUT_S)
        check(ok, "scrubbed to frame %d P[y] is %g (got %r)" % (last, y, column_cells("P[y]")))
        yield ("sleep", SETTLE_MS)
        after = [t.columnWidth(c) for c in range(t.model().columnCount())]
        check(headers() == names and after == widths,
              "scrubbing keeps the column widths, the hand-set one included (%r, was %r)" % (after, widths))
        check(sheet_state("Class") == "Points", "scrubbing keeps the class shown (got %r)" % (sheet_state("Class"),))
        shot("m337-spreadsheet-geo-scrub.png")

    def scene_graph_section():
        section("scene graph")
        view_node(grid)
        ok = yield ("until", lambda: shows_counts(GRID_COUNTS), POLL_TIMEOUT_S)
        check(ok, "the Geometry Spreadsheet lists the grid (got %r)" % (class_counts(),))
        home = hold(ancestor_of_class(view(), "TabWidget"))
        require(home is not None, "the scene view is still in a pane")
        lower = app.getTabWidget(home.objectName()).splitVertically()
        require(lower is not None, "split the viewer's pane again")
        require(app.moveTab("sceneGraph", lower), "move the Scene Graph panel under the viewer")
        lower = None
        ok = yield ("until", lambda: scene_graph() is not None and scene_graph().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel shows")
        require(show_scene_graph_spreadsheet_tab(), "the Scene Graph inspector has a Spreadsheet tab")
        tree = sg_child(QTreeView, "sceneGraphTree")
        require(tree is not None, "the Scene Graph panel has its tree")
        ok = yield ("until", lambda: tree.model().rowCount() > 0, POLL_TIMEOUT_S)
        require(ok, "the Scene Graph lists the grid's scene")
        tree.setCurrentIndex(tree.model().index(0, 0))
        path = str(tree.model().index(0, 0).data(Qt.ToolTipRole) or "")
        sg_sheet = sg_child(QWidget, SCENE_GRAPH_SPREADSHEET)
        sg_message = sg_child(QLabel, SCENE_GRAPH_SPREADSHEET + "Message")
        require(sg_sheet is not None and sg_message is not None, "the Scene Graph Spreadsheet tab has its table")
        ok = yield ("until", lambda: bool(sg_inspector_state("Path")) and not sg_inspector_state("Busy")
                    and sg_message.isVisible(), POLL_TIMEOUT_S)
        report("NOTE scene graph prim %r, inspector path %r, message %r" % (path, sg_inspector_state("Path"), sg_message.text()))
        check(ok and "Geometry Spreadsheet" in sg_message.text(),
              "the Scene Graph Spreadsheet tab points to the Geometry Spreadsheet panel (message %r)" % (sg_message.text(),))
        sg_table = sg_child(QTableView, SCENE_GRAPH_SPREADSHEET + "Table")
        sg_headers = [str(sg_table.model().headerData(c, Qt.Horizontal)) for c in range(sg_table.model().columnCount())]
        check(int(sg_sheet.property("natronGeoSpreadsheetRows")) == 0 and "P[x]" not in sg_headers,
              "the Scene Graph Spreadsheet tab lists none of the grid's attributes (rows %r, columns %r)"
              % (sg_sheet.property("natronGeoSpreadsheetRows"), sg_headers))
        check(show_class("Points") and settled() and int(sheet_state("Rows")) == GRID_COUNTS["Points"]
              and "P[x]" in headers(),
              "the Geometry Spreadsheet panel still lists the grid's points (got %r rows)" % (sheet_state("Rows"),))
        shot("m337-spreadsheet-geo-scene-graph.png")

    def shown_node():
        return str(panel_state("Node") or "")

    def lists(node, counts):
        return lambda: shown_node() == node.getScriptName() and shows_counts(counts)

    def select(node):
        app.clearSelection()
        app.selectNode(node, True)

    def follow_section():
        section("follow")
        view_node(grid)
        ok = yield ("until", lists(grid, GRID_COUNTS), POLL_TIMEOUT_S)
        require(ok, "the panel lists the viewed grid (got %r, %r)" % (shown_node(), class_counts()))
        follow = child(QComboBox, SPREADSHEET + "Follow")
        pin = child(QToolButton, SPREADSHEET + "Pin")
        chooser = child(QComboBox, SPREADSHEET + "Node")
        require(follow is not None and pin is not None and chooser is not None, "the panel has its Pin, Follow and node choosers")
        check(panel_state("Follow") == "viewer" and follow.currentIndex() == 0 and not panel_state("Pinned"),
              "the panel follows the viewer by default (got %r)" % (panel_state("Follow"),))

        select(delete)
        yield ("sleep", SETTLE_MS)
        check(lists(grid, GRID_COUNTS)(), "following the viewer, selecting the GeoDelete keeps the grid (got %r)" % (shown_node(),))

        follow.setCurrentIndex(1)
        ok = yield ("until", lambda: panel_state("Follow") == "selection", POLL_TIMEOUT_S)
        require(ok, "the chooser switches the panel to following the selection")
        ok = yield ("until", lists(delete, DELETED_COUNTS), POLL_TIMEOUT_S)
        check(ok, "following the selection, the panel lists the selected GeoDelete while the viewer shows the grid (got %r, %r)"
              % (shown_node(), class_counts()))
        shot("m337-spreadsheet-geo-follow-selection.png")

        select(viewer)
        yield ("sleep", SETTLE_MS)
        check(lists(delete, DELETED_COUNTS)(), "selecting the Viewer, which puts out no geometry, keeps the GeoDelete (got %r)"
              % (shown_node(),))

        pin.click()
        ok = yield ("until", lambda: bool(panel_state("Pinned")), POLL_TIMEOUT_S)
        require(ok, "the pin button pins the panel")
        select(moved)
        yield ("sleep", SETTLE_MS)
        check(lists(delete, DELETED_COUNTS)(), "pinned, selecting the GeoTransform keeps the GeoDelete (got %r)" % (shown_node(),))
        check(chooser.currentText() == delete.getScriptName() or chooser.currentText().endswith("." + delete.getScriptName()),
              "the node chooser names the pinned GeoDelete (got %r)" % (chooser.currentText(),))
        shot("m337-spreadsheet-geo-pinned.png")

        pin.click()
        ok = yield ("until", lambda: not panel_state("Pinned") and lists(moved, GRID_COUNTS)(), POLL_TIMEOUT_S)
        check(ok, "unpinned, the panel lists the selected GeoTransform (got %r, %r)" % (shown_node(), class_counts()))

        follow.setCurrentIndex(0)
        ok = yield ("until", lambda: panel_state("Follow") == "viewer" and lists(grid, GRID_COUNTS)(), POLL_TIMEOUT_S)
        check(ok, "back to following the viewer, the panel lists the viewed grid (got %r, %r)" % (shown_node(), class_counts()))

    def steps():
        view_node(grid)
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
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
        require(sheet() is not None and table() is not None and message() is not None, "the panel has its table")
        require(all(class_button(label) is not None for label in CLASSES), "the panel has its class bar")

        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        py_viewer.seek(1)
        yield from grid_section()
        yield from delete_section()
        yield from packed_section()
        yield from scrub_section(py_viewer)
        yield from scene_graph_section()
        yield from follow_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
