"""USDRead's Initial Draw sets how the viewer draws the file's top-level prims, and nothing else.

Graph: USDRead of a small city this script writes to $NATRON_GUI_TEST_OUT/scene_initial_draw/:
two top-level prims, /World and /Annex, each a group with an extents hint holding two buildings
whose content is a payload. A USDCube at /Cube and the USDRead go into USDMerge, into Viewer1.
The script checks:
  - a new USDRead draws one box per top-level prim, two boxes and no marker, and the Scene Graph
    panel reads BBox on /World and /Annex, set rather than inherited;
  - the merged /Cube stays Full, inherited from the default;
  - no payload file is opened;
  - Full draws the two prims in full, still with no payload file opened;
  - Full + Load has the viewer load every building's payload.

Screenshots for sign-off: scene-initial-draw-bbox, scene-initial-draw-full,
scene-initial-draw-full-load.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 1000
TOP_LEVEL = ["/World", "/Annex"]
BUILDINGS = ["B0", "B1"]
CUBE = "/Cube"
PAYLOAD_MARK = "scene_initial_draw_payloads"
# The Initial Draw menu, in order.
INITIAL_DRAW = ["bbox", "levels", "toKind", "full", "fullLoad"]
# SceneDrawStateEnum, which the panel's draw state role holds.
STATES = ["bbox", "children", "full", "loadAll", "hidden"]

BUILDING = """#usda 1.0

def Xform "Building"
{
    def Cube "Body"
    {
        double size = 2
        double3 xformOp:translate = (1, 1, 1)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
"""


def top_level_prim(name, x):
    buildings = ""
    for i, building in enumerate(BUILDINGS):
        buildings += """
    def Xform "%s" (
        kind = "assembly"
        payload = @./%s/%s_%s.usda@</Building>
    )
    {
        double3 xformOp:translate = (%d, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
""" % (building, PAYLOAD_MARK, name, building, 4 * i)
    return """
def Xform "%s" (
    kind = "group"
)
{
    uniform float3[] extentsHint = [(0, 0, 0), (6, 2, 2)]
    double3 xformOp:translate = (%d, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
%s}
""" % (name, x, buildings)


try:
    from PySide6.QtCore import QModelIndex, Qt
    from PySide6.QtGui import QGuiApplication, QImage
    from PySide6.QtWidgets import QApplication, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    DRAW_STATE_ROLE = Qt.UserRole + 5
    INHERITED_ROLE = Qt.UserRole + 6

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_initial_draw"))
    os.makedirs(os.path.join(fixture_dir, PAYLOAD_MARK), exist_ok=True)
    payload_files = []
    for top in TOP_LEVEL:
        for building in BUILDINGS:
            path = os.path.join(fixture_dir, PAYLOAD_MARK, "%s_%s.usda" % (top.strip("/"), building))
            with open(path, "w") as f:
                f.write(BUILDING)
            payload_files.append(path)
    city_path = os.path.join(fixture_dir, "scene_initial_draw.usda")
    with open(city_path, "w") as f:
        f.write("#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n)\n")
        for i, top in enumerate(TOP_LEVEL):
            f.write(top_level_prim(top.strip("/"), 10 * i))

    def make(plugin, x, y):
        node = app.createNode(plugin)
        require(node is not None, "create %s" % plugin, plugin_problem(plugin))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("filename").setValue(city_path)
    cube = make("USDCube", 250, 0)
    cube.getParam("primPath").setValue(CUBE)
    merge = make("USDMerge", 100, 150)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(100, 300)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def tree():
        p = panel()
        return hold(p.findChild(QTreeView, "sceneGraphTree")) if p is not None else None

    def model():
        t = tree()
        return hold(t.model()) if t is not None else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def draw_column():
        m = model()
        if m is None:
            return -1
        for column in range(m.columnCount()):
            if str(m.headerData(column, Qt.Horizontal)) == "Draw State":
                return column
        return -1

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        while m.canFetchMore(parent):
            m.fetchMore(parent)
        return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]

    def find(path):
        """The index of path's row, reading the tree down to it; an invalid index when absent."""
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def row_state(path):
        """(state name, inherited) of path's Draw State cell; (None, None) when it has none."""
        index = find(path)
        if not index.isValid() or draw_column() < 0:
            return (None, None)
        c = index.sibling(index.row(), draw_column())
        state = c.data(DRAW_STATE_ROLE)
        if state is None:
            return (None, None)
        return (STATES[int(state)], bool(c.data(INHERITED_ROLE)))

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def drawn():
        v = view()
        if v is None:
            return (0, 0)
        return (int(v.property("natronSceneDrawBoxes") or 0), int(v.property("natronSceneDrawMarkers") or 0))

    def opened_payloads():
        return sorted(layer.identifier for layer in Sdf.Layer.GetLoadedLayers() if PAYLOAD_MARK in layer.identifier)

    def grab():
        v = view()
        if v is None:
            return QImage()
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height()).convertToFormat(QImage.Format_RGBX8888)

    def save(name):
        shot(name + ".png")
        image = grab()
        path = os.path.join(OUT, name + "_view.png")
        image.save(path)
        report("SHOT " + path)

    def set_initial_draw(value):
        usd.getParam("initialDraw").setValue(INITIAL_DRAW.index(value))

    def expect_rows(label, expected):
        """Waits until every path's cell reads its (state, inherited) pair."""
        ok = yield ("until", lambda: all(row_state(p) == s for p, s in expected.items()), POLL_TIMEOUT_S)
        check(ok, "%s: the panel reads %r (%r)" % (label, expected, dict((p, row_state(p)) for p in expected)))

    def steps():
        require(merge.connectInput(0, usd), "connect the USDRead into the merge")
        require(merge.connectInput(1, cube), "connect the USDCube into the merge")
        require(viewer.connectInput(0, merge), "connect the Viewer to the USDMerge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the city")
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and find(CUBE).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists %s" % CUBE)

        section("bbox by default")
        check(usd.getParam("initialDraw").getValue() == INITIAL_DRAW.index("bbox"), "a new USDRead's Initial Draw is BBox")
        ok = yield ("until", lambda: drawn() == (len(TOP_LEVEL), 0), POLL_TIMEOUT_S)
        check(ok, "one box per top-level prim and no marker (%r)" % (drawn(),))
        expected = dict((p, ("bbox", False)) for p in TOP_LEVEL)
        expected[CUBE] = ("full", True)
        yield from expect_rows("BBox", expected)
        yield ("sleep", SETTLE_MS)
        save("scene-initial-draw-bbox")
        check(opened_payloads() == [], "no payload file is open (%r)" % opened_payloads()[:3])

        section("full")
        set_initial_draw("full")
        ok = yield ("until", lambda: drawn() == (0, 0), POLL_TIMEOUT_S)
        check(ok, "Full boxes nothing (%r)" % (drawn(),))
        expected = dict((p, ("full", False)) for p in TOP_LEVEL)
        expected[CUBE] = ("full", True)
        yield from expect_rows("Full", expected)
        yield ("sleep", SETTLE_MS)
        save("scene-initial-draw-full")
        check(opened_payloads() == [], "Full opens no payload file (%r)" % opened_payloads()[:3])

        section("full + load")
        set_initial_draw("fullLoad")
        ok = yield ("until", lambda: len(opened_payloads()) == len(payload_files), LOAD_TIMEOUT_S)
        check(ok, "Full + Load loads every building's payload (%r)" % opened_payloads())
        expected = dict((p, ("loadAll", False)) for p in TOP_LEVEL)
        expected[CUBE] = ("full", True)
        yield from expect_rows("Full + Load", expected)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        check(ok, "the scene view shows the loaded city")
        yield ("sleep", SETTLE_MS)
        save("scene-initial-draw-full-load")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
