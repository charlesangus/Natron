"""The prim inspector's Spreadsheet tab on a USDRead scene.

Writes a small scene under $NATRON_GUI_TEST_OUT: a mesh with time-sampled points and vertex,
faceVarying, uniform and constant primvars (one of them with an elementSize of 2), and an Xform
whose payload stays unloaded. Checks:
  - selecting the mesh lists its points with points[x], points[y] and points[z] columns, and its
    primvars under the class each belongs to, with each class button showing its row count;
  - the class bar fits the inspector's width;
  - hovering an elementSize 2 cell gives the whole tuple, and double-clicking it opens the full value
    window with it;
  - a frame change updates the time-sampled points and keeps the class shown;
  - selecting the payload prim shows the not-loaded text and loads nothing, so the scene graph still
    reports the payload as unloaded;
  - a class chosen on one prim stays chosen on the next prim that has rows in it.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
WORLD = "/World"
QUAD = WORLD + "/Quad"
LAZY = WORLD + "/Lazy"
SPREADSHEET = "sceneGraphSpreadsheet"
SPREADSHEET_TITLE = "Spreadsheet"
NOT_LOADED_TEXT = "Not loaded — load the payload from the scene graph to inspect it"
CLASSES = ("Points", "Vertices", "Primitives", "Detail")

STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)


def write_scene(directory):
    """Writes the root layer and the payload's file, and returns the root's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)
    with open(os.path.join(payloads, "lazy.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                'def Xform "Building"\n{\n'
                '    def Mesh "Geo"\n    {\n'
                '        int[] faceVertexCounts = [3]\n'
                '        int[] faceVertexIndices = [0, 1, 2]\n'
                '        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n'
                '    }\n}\n')
    path = os.path.join(directory, "spreadsheet.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    timeCodesPerSecond = 24\n'
                '    startTimeCode = 1\n    endTimeCode = 20\n    upAxis = "Y"\n)\n')
        f.write('def Xform "World"\n{\n')
        f.write('    def Mesh "Quad"\n    {\n'
                '        int[] faceVertexCounts = [3, 3]\n'
                '        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]\n'
                '        point3f[] points.timeSamples = {\n'
                '            1: [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],\n'
                '            20: [(0, 0, 0), (3, 0, 0), (3, 1, 0), (0, 1, 0)],\n'
                '        }\n'
                '        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)] (\n'
                '            interpolation = "vertex"\n        )\n'
                '        float2[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 0), (1, 1), (0, 1)] (\n'
                '            interpolation = "faceVarying"\n        )\n'
                '        float[] primvars:faceWeight = [0.5, 0.25] (\n'
                '            interpolation = "uniform"\n        )\n'
                '        float primvars:roughness = 0.75 (\n'
                '            interpolation = "constant"\n        )\n'
                '        float[] primvars:pair = [1, 2, 3, 4, 5, 6, 7, 8] (\n'
                '            elementSize = 2\n            interpolation = "vertex"\n        )\n'
                '        def GeomSubset "top"\n        {\n'
                '            uniform token elementType = "face"\n            uniform token familyName = "part"\n'
                '            int[] indices = [0]\n        }\n'
                '        def GeomSubset "topRim"\n        {\n'
                '            uniform token elementType = "face"\n            uniform token familyName = "rim"\n'
                '            int[] indices = [1]\n        }\n'
                '    }\n')
        f.write('    def Xform "Lazy" (\n        prepend payload = @./payloads/lazy.usda@\n    )\n    {\n    }\n')
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QModelIndex, QItemSelectionModel, Qt
    from PySide6.QtWidgets import QComboBox, QDialog, QLabel, QPlainTextEdit, QTabWidget, QTableView, QToolButton, QTreeView, QWidget
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3

    fixture_dir = os.path.realpath(os.path.join(OUT, "m337_spreadsheet_usd"))
    scene_path = write_scene(fixture_dir)
    payload_dir = os.path.join(fixture_dir, "payloads")

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

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

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def model():
        t = tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def inspector():
        return child(QWidget, "sceneGraphInspector")

    def inspector_state(name):
        return inspector().property("natronSceneGraphInspector" + name)

    def settled_on(path):
        return str(inspector_state("Path")) == path and not inspector_state("Busy")

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        try:
            while m.canFetchMore(parent):
                m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def find(path):
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if path_of(index) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def tree_state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def select(index):
        selection = tree().selectionModel()
        selection.select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        selection.setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def opened_payloads():
        """The payload files of the fixture that are open in the process."""
        return sorted(os.path.basename(layer.realPath or "") for layer in Sdf.Layer.GetLoadedLayers()
                      if (layer.realPath or "").startswith(payload_dir))

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

    def column_cells(name, role=Qt.DisplayRole):
        m = table().model()
        names = headers()
        if name not in names:
            return None
        c = names.index(name)
        return ["" if m.index(r, c).data(role) is None else str(m.index(r, c).data(role)) for r in range(m.rowCount())]

    def show_class(label):
        class_button(label).click()
        return sheet_state("Class") == label

    def show_spreadsheet_tab():
        tabs = child(QTabWidget, "sceneGraphInspectorTabs")
        titles = [tabs.tabText(i) for i in range(tabs.count())]
        if SPREADSHEET_TITLE not in titles:
            return False
        tabs.setCurrentIndex(titles.index(SPREADSHEET_TITLE))
        return True

    def mesh_section(py_viewer):
        section("mesh")
        py_viewer.seek(1)
        select(find(QUAD))
        ok = yield ("until", lambda: settled_on(QUAD) and int(sheet_state("Rows")) == 4, POLL_TIMEOUT_S)
        check(ok, "selecting the mesh lists its 4 points (got %r rows on %r)" % (sheet_state("Rows"), inspector_state("Path")))
        check(sheet_state("Class") == "Points", "the Points class is shown (got %r)" % (sheet_state("Class"),))
        names = headers()
        report("NOTE point columns: %r" % (names,))
        check(names[:3] == ["points[x]", "points[y]", "points[z]"], "the points come first, one column per component (got %r)" % (names[:3],))
        check(column_cells("points[x]") == ["0", "1", "1", "0"], "points[x] reads the frame 1 sample (got %r)" % (column_cells("points[x]"),))
        check(column_cells("displayColor[g]") == ["0", "1", "0", "1"], "a vertex primvar is listed on points (got %r)" % (column_cells("displayColor[g]"),))
        counts = {label: class_button(label).text() for label in CLASSES}
        check(counts == {"Points": "Points 4", "Vertices": "Vertices 6", "Primitives": "Primitives 2", "Detail": "Detail 1"},
              "each class button shows its row count (got %r)" % (counts,))
        check(int(sheet_state("CellReads")) > 0, "the table's cell reads are counted (got %r)" % (sheet_state("CellReads"),))
        shot("m337-spreadsheet-usd-points.png")

        bar = class_button("Points").parentWidget()
        check(bar.minimumSizeHint().width() <= sheet().width(),
              "the class bar fits the inspector (%d px in %d px)" % (bar.minimumSizeHint().width(), sheet().width()))
        shot("m337-spreadsheet-usd-class-bar.png")

        check(show_class("Vertices") and int(sheet_state("Rows")) == 6, "the Vertices class lists 6 rows (got %r)" % (sheet_state("Rows"),))
        check(column_cells("Point") == ["0", "1", "2", "0", "2", "3"], "vertices name their points (got %r)" % (column_cells("Point"),))
        check(column_cells("st[1]") == ["0", "0", "1", "0", "1", "1"], "a faceVarying primvar is listed on vertices (got %r)" % (column_cells("st[1]"),))
        shot("m337-spreadsheet-usd-vertices.png")

        check(show_class("Primitives") and int(sheet_state("Rows")) == 2, "the Primitives class lists 2 rows (got %r)" % (sheet_state("Rows"),))
        check(column_cells("faceWeight") == ["0.5", "0.25"], "a uniform primvar is listed on primitives (got %r)" % (column_cells("faceWeight"),))
        shot("m337-spreadsheet-usd-primitives.png")

        check(show_class("Detail") and int(sheet_state("Rows")) == 1, "the Detail class lists 1 row (got %r)" % (sheet_state("Rows"),))
        check(column_cells("roughness") == ["0.75"], "a constant primvar is listed on the detail (got %r)" % (column_cells("roughness"),))
        shot("m337-spreadsheet-usd-detail.png")

        check(show_class("Points"), "back to the Points class")
        tips = column_cells("pair", Qt.ToolTipRole)
        check(tips is not None and tips[0] == "(2) [1, 2]", "hovering an elementSize 2 cell gives the whole tuple (got %r)" % (tips,))
        names = headers()
        if "pair" in names:
            t = table()
            t.doubleClicked.emit(t.model().index(1, names.index("pair")))
            window = t.findChild(QDialog, "sceneGraphFullValue")
            check(window is not None, "double-clicking a cell opens the full value window")
            if window is not None:
                text = window.findChild(QPlainTextEdit, "sceneGraphFullValueText")
                ok = yield ("until", lambda: not text.property("natronSceneGraphTextLoading") and text.toPlainText() != "", POLL_TIMEOUT_S)
                check(ok and text.toPlainText() == "(2) [3, 4]", "the full value window shows the cell's tuple (got %r)" % (text.toPlainText(),))
                shot("m337-spreadsheet-usd-full-value.png")
                window.close()

    def frame_section(py_viewer):
        section("frame")
        py_viewer.seek(20)
        ok = yield ("until", lambda: settled_on(QUAD) and column_cells("points[x]") == ["0", "3", "3", "0"], POLL_TIMEOUT_S)
        check(ok, "at frame 20 points[x] reads the frame 20 sample (got %r)" % (column_cells("points[x]"),))
        check(sheet_state("Class") == "Points", "a frame change keeps the class shown (got %r)" % (sheet_state("Class"),))
        shot("m337-spreadsheet-usd-frame-20.png")
        py_viewer.seek(1)
        ok = yield ("until", lambda: settled_on(QUAD) and column_cells("points[x]") == ["0", "1", "1", "0"], POLL_TIMEOUT_S)
        check(ok, "back at frame 1 points[x] reads the frame 1 sample (got %r)" % (column_cells("points[x]"),))

    def payload_section():
        section("payload")
        check(tree_state(LAZY) == STATE_UNLOADED, "the payload is listed unloaded (got %r)" % (tree_state(LAZY),))
        loads_before = view().property("natronSceneLoadEdits")
        select(find(LAZY))
        ok = yield ("until", lambda: settled_on(LAZY) and message().isVisible(), POLL_TIMEOUT_S)
        check(ok and message().text() == NOT_LOADED_TEXT, "the payload prim shows the not-loaded text (got %r)" % (message().text(),))
        check(int(sheet_state("Rows")) == 0, "nothing is listed for the payload prim (got %r)" % (sheet_state("Rows"),))
        yield ("sleep", SETTLE_MS)
        check(tree_state(LAZY) == STATE_UNLOADED, "the scene graph still reports the payload as unloaded (got %r)" % (tree_state(LAZY),))
        check(opened_payloads() == [], "the payload's file is not opened (got %r)" % (opened_payloads(),))
        check(view().property("natronSceneLoadEdits") == loads_before,
              "the payload is not loaded (load edits %r, was %r)" % (view().property("natronSceneLoadEdits"), loads_before))
        shot("m337-spreadsheet-usd-not-loaded.png")

    def class_kept_section():
        section("class kept")
        select(find(QUAD))
        ok = yield ("until", lambda: settled_on(QUAD) and table().isVisible() and int(sheet_state("Rows")) > 0, POLL_TIMEOUT_S)
        require(ok, "the mesh is shown again")
        check(show_class("Primitives"), "the Primitives class is chosen")
        select(find(LAZY))
        ok = yield ("until", lambda: settled_on(LAZY) and message().isVisible(), POLL_TIMEOUT_S)
        check(ok, "the payload prim is shown in between")
        select(find(QUAD))
        ok = yield ("until", lambda: settled_on(QUAD) and table().isVisible() and int(sheet_state("Rows")) > 0, POLL_TIMEOUT_S)
        check(ok and sheet_state("Class") == "Primitives" and int(sheet_state("Rows")) == 2,
              "the chosen class stays chosen on a prim with rows in it (got %r, %r rows)" % (sheet_state("Class"), sheet_state("Rows")))
        shot("m337-spreadsheet-usd-class-kept.png")

    def filter_section():
        section("filter")
        select(find(QUAD))
        ok = yield ("until", lambda: settled_on(QUAD) and table().isVisible() and int(sheet_state("Rows")) > 0, POLL_TIMEOUT_S)
        require(ok, "the mesh is shown")
        check(show_class("Primitives"), "the Primitives class is chosen")
        group = child(QComboBox, SPREADSHEET + "Group")
        require(group is not None, "the filter bar has its group box")

        def apply(text):
            group.lineEdit().setText(text)
            group.lineEdit().editingFinished.emit()

        apply("0")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 1, POLL_TIMEOUT_S)
        check(ok and column_cells("faceWeight") == ["0.5"], "a row number keeps that row (got %r)" % (column_cells("faceWeight"),))
        apply("0-1:1")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 2, POLL_TIMEOUT_S)
        check(ok, "a range keeps its rows (got %r rows)" % (sheet_state("Rows"),))
        apply("top*")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 2, POLL_TIMEOUT_S)
        check(ok, "a wildcard subset name keeps both subsets (got %r rows)" % (sheet_state("Rows"),))
        apply("top")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 1 and column_cells("faceWeight") == ["0.5"], POLL_TIMEOUT_S)
        check(ok, "an exact subset name keeps its rows (got %r)" % (column_cells("faceWeight"),))
        apply("* ^top")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 1 and column_cells("faceWeight") == ["0.25"], POLL_TIMEOUT_S)
        check(ok, "an exclusion removes the subset's rows (got %r)" % (column_cells("faceWeight"),))
        apply("@faceWeight>0")
        err = child(QLabel, SPREADSHEET + "Error")
        ok = yield ("until", lambda: err is not None and err.isVisible() and "not supported" in err.text(), POLL_TIMEOUT_S)
        check(ok, "an attribute test is refused on a prim (got %r)" % (err.text() if err is not None else None,))
        shot("m337-spreadsheet-usd-filter.png")
        apply("")
        ok = yield ("until", lambda: int(sheet_state("Rows")) == 2, POLL_TIMEOUT_S)
        check(ok, "clearing the filter lists every row (got %r rows)" % (sheet_state("Rows"),))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(panel() is not None, "the Scene Graph panel exists")

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and [path_of(i) for i in rows()] == [WORLD], POLL_TIMEOUT_S)
        require(ok, "the panel lists /World")
        require(inspector() is not None, "the panel has its inspector")
        require(show_spreadsheet_tab(), "the inspector has a Spreadsheet tab")
        require(sheet() is not None and table() is not None and message() is not None, "the Spreadsheet tab has its table")
        require(all(class_button(label) is not None for label in CLASSES), "the Spreadsheet tab has its class bar")
        tree().expand(find(WORLD))
        yield ("sleep", SETTLE_MS)
        check(opened_payloads() == [], "opening the scene opens no payload (got %r)" % (opened_payloads(),))

        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        yield from mesh_section(py_viewer)
        yield from frame_section(py_viewer)
        yield from payload_section()
        yield from class_kept_section()
        yield from filter_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
