"""Rows selected in the prim inspector's Spreadsheet tab highlight points and faces in the scene view.

Writes a scene under $NATRON_GUI_TEST_OUT: /World/Grid, a mesh of 5 x 5 quads (36 points, 25 faces)
filling the view, and /World/Other, a small quad off to the side. The Viewer shows a USDRead of it,
with the Scene Graph panel beside the viewer. Checks, on the viewport's natronSceneHighlightCount
property and on grabs of the drawn image:
  - selecting 5 Points rows of the Grid highlights 5 points, and the drawn image changes against the
    one drawn with the Grid selected and nothing highlighted;
  - selecting Vertices rows highlights the points those vertices use;
  - selecting Primitives rows highlights those faces, and the image changes against the baseline;
  - selecting no rows, or the Detail row, highlights nothing;
  - selecting another prim clears the highlight;
  - Selected Only shows only the highlighted rows, and turning it off lists them all again.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 600
WORLD = "/World"
GRID_PRIM = WORLD + "/Grid"
OTHER_PRIM = WORLD + "/Other"
SPREADSHEET = "sceneGraphSpreadsheet"
SPREADSHEET_TITLE = "Spreadsheet"
CELLS = 5
POINTS = (CELLS + 1) ** 2
FACES = CELLS * CELLS
CLASSES = ("Points", "Vertices", "Primitives", "Detail")


def write_scene(directory):
    """Writes the root layer and returns its path."""
    os.makedirs(directory, exist_ok=True)
    points = ", ".join("(%d, %d, 0)" % (x, y) for y in range(CELLS + 1) for x in range(CELLS + 1))
    indices = []
    for y in range(CELLS):
        for x in range(CELLS):
            a = y * (CELLS + 1) + x
            indices.extend([a, a + 1, a + CELLS + 2, a + CELLS + 1])
    path = os.path.join(directory, "selection.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "World"\n{\n')
        f.write('    def Mesh "Grid"\n    {\n'
                '        int[] faceVertexCounts = [%s]\n'
                '        int[] faceVertexIndices = [%s]\n'
                '        point3f[] points = [%s]\n'
                '    }\n' % (", ".join(["4"] * FACES), ", ".join(str(i) for i in indices), points))
        f.write('    def Mesh "Other"\n    {\n'
                '        int[] faceVertexCounts = [4]\n'
                '        int[] faceVertexIndices = [0, 1, 2, 3]\n'
                '        point3f[] points = [(7, 0, 0), (8, 0, 0), (8, 1, 0), (7, 1, 0)]\n'
                '    }\n')
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QModelIndex, QItemSelection, QItemSelectionModel, QPoint, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QTabWidget, QTableView, QToolButton, QTreeView, QWidget

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "m337_selection_usd")))

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

    def select_prim(path):
        index = find(path)
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

    def sheet():
        return child(QWidget, SPREADSHEET)

    def sheet_state(name):
        return sheet().property("natronGeoSpreadsheet" + name)

    def table():
        return child(QTableView, SPREADSHEET + "Table")

    def class_button(label):
        return child(QToolButton, SPREADSHEET + label)

    def selected_only_button():
        return child(QToolButton, SPREADSHEET + "SelectedOnly")

    def show_class(label):
        class_button(label).click()
        return sheet_state("Class") == label

    def select_rows(first, last):
        t = table()
        m = t.model()
        t.selectionModel().select(QItemSelection(m.index(first, 0), m.index(last, m.columnCount() - 1)),
                                  QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)

    def clear_rows():
        table().selectionModel().clearSelection()

    def highlighted():
        v = view()
        value = v.property("natronSceneHighlightCount") if v is not None else None
        return -1 if value is None else int(value)

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

    def differs(a, b, step=2):
        if a.size() != b.size():
            return True
        for y in range(0, a.height(), step):
            for x in range(0, a.width(), step):
                if a.pixel(x, y) != b.pixel(x, y):
                    return True
        return False

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    _baseline = []

    def grid_section():
        section("grid")
        select_prim(GRID_PRIM)
        ok = yield ("until", lambda: settled_on(GRID_PRIM) and int(sheet_state("Rows")) == POINTS, POLL_TIMEOUT_S)
        require(ok, "selecting the Grid lists its %d points (got %r)" % (POINTS, sheet_state("Rows")))
        check(highlighted() == 0, "nothing is highlighted yet (got %r)" % (highlighted(),))
        check(selected_only_button().isEnabled(), "Selected Only is enabled for a prim the viewer shows")
        yield settle()
        _baseline.append(framebuffer())
        shot("m337-selection-usd-baseline.png")

    def points_section():
        section("points")
        require(show_class("Points"), "the Points class is shown")
        select_rows(0, 4)
        ok = yield ("until", lambda: highlighted() == 5, POLL_TIMEOUT_S)
        check(ok, "5 Points rows highlight 5 points (got %r)" % (highlighted(),))
        yield settle()
        check(differs(_baseline[0], framebuffer()), "the drawn image changes with the points highlighted")
        shot("m337-selection-usd-points.png")

        select_rows(10, 11)
        ok = yield ("until", lambda: highlighted() == 2, POLL_TIMEOUT_S)
        check(ok, "other Points rows replace the highlight (got %r)" % (highlighted(),))

        clear_rows()
        ok = yield ("until", lambda: highlighted() == 0, POLL_TIMEOUT_S)
        check(ok, "no rows selected highlights nothing (got %r)" % (highlighted(),))
        yield settle()
        check(not differs(_baseline[0], framebuffer()), "the drawn image is the baseline again")
        shot("m337-selection-usd-points-cleared.png")

    def vertices_section():
        section("vertices")
        require(show_class("Vertices"), "the Vertices class is shown")
        select_rows(0, 3)
        ok = yield ("until", lambda: highlighted() == 4, POLL_TIMEOUT_S)
        check(ok, "4 Vertices rows highlight the 4 points they use (got %r)" % (highlighted(),))
        yield settle()
        shot("m337-selection-usd-vertices.png")
        clear_rows()

    def primitives_section():
        section("primitives")
        require(show_class("Primitives"), "the Primitives class is shown")
        select_rows(6, 12)
        ok = yield ("until", lambda: highlighted() == 7, POLL_TIMEOUT_S)
        check(ok, "7 Primitives rows highlight 7 faces (got %r)" % (highlighted(),))
        yield settle()
        check(differs(_baseline[0], framebuffer()), "the drawn image changes with the faces highlighted")
        shot("m337-selection-usd-primitives.png")

    def detail_section():
        section("detail")
        require(show_class("Detail"), "the Detail class is shown")
        select_rows(0, 0)
        ok = yield ("until", lambda: highlighted() == 0, POLL_TIMEOUT_S)
        check(ok, "the Detail row highlights nothing (got %r)" % (highlighted(),))
        require(show_class("Primitives"), "back to the Primitives class")

    def selected_only_section():
        section("selected only")
        select_rows(6, 12)
        ok = yield ("until", lambda: highlighted() == 7, POLL_TIMEOUT_S)
        require(ok, "7 faces are highlighted")
        selected_only_button().click()
        ok = yield ("until", lambda: bool(sheet_state("SelectedOnly")) and int(sheet_state("Rows")) == 7 and not sheet_state("SortBusy"), POLL_TIMEOUT_S)
        check(ok, "Selected Only lists the 7 highlighted faces (got %r rows)" % (sheet_state("Rows"),))
        yield settle()
        shot("m337-selection-usd-selected-only.png")
        selected_only_button().click()
        ok = yield ("until", lambda: not bool(sheet_state("SelectedOnly")) and int(sheet_state("Rows")) == FACES and not sheet_state("SortBusy"), POLL_TIMEOUT_S)
        check(ok, "turning Selected Only off lists all %d faces (got %r rows)" % (FACES, sheet_state("Rows")))
        check(highlighted() == 7, "the highlight stays while Selected Only is toggled (got %r)" % (highlighted(),))

    def other_prim_section():
        section("other prim")
        select_prim(OTHER_PRIM)
        ok = yield ("until", lambda: settled_on(OTHER_PRIM) and highlighted() == 0, POLL_TIMEOUT_S)
        check(ok, "selecting another prim clears the highlight (got %r)" % (highlighted(),))
        yield settle()
        shot("m337-selection-usd-other-prim.png")

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
        require(sheet() is not None and table() is not None, "the Spreadsheet tab has its table")
        require(all(class_button(label) is not None for label in CLASSES) and selected_only_button() is not None,
                "the Spreadsheet tab has its class bar and Selected Only")
        tree().expand(find(WORLD))
        yield ("sleep", SETTLE_MS)

        yield from grid_section()
        yield from points_section()
        yield from vertices_section()
        yield from primitives_section()
        yield from detail_section()
        yield from selected_only_section()
        yield from other_prim_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
