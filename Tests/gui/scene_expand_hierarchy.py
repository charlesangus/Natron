"""A USDExpandHierarchy Full + Load rule loads only the payloads below the prims it names.

Graph: USDRead of a small city this script writes to $NATRON_GUI_TEST_OUT/scene_expand_hierarchy/:
two top-level prims, /World and /Annex, each a group holding two buildings whose content is a
payload. The USDRead goes into a USDExpandHierarchy, into Viewer1. The script checks:
  - with no rule, the USDRead's Initial Draw (BBox) applies and no payload file is opened;
  - a Full + Load rule on /Wor* has the viewer load the two /World buildings and none of /Annex's,
    and the Scene Graph panel reads Full + Load on /World, set rather than inherited, and BBox
    on /Annex;
  - showing the USDRead instead returns /World to BBox, while its buildings stay loaded;
  - deleting the node changes nothing more;
  - Unload from the row menu unloads the buildings.

Screenshots for sign-off: scene-expand-hierarchy-default, scene-expand-hierarchy-full-load,
scene-expand-hierarchy-removed.

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
PAYLOAD_MARK = "scene_expand_hierarchy_payloads"
# SceneDrawStateEnum, which the panel's draw state role holds, and the Draw menu's order.
STATES = ["bbox", "children", "full", "loadAll", "hidden"]
STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)

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
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QImage
    from PySide6.QtWidgets import QApplication, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3
    DRAW_STATE_ROLE = Qt.UserRole + 5
    INHERITED_ROLE = Qt.UserRole + 6

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_expand_hierarchy"))
    os.makedirs(os.path.join(fixture_dir, PAYLOAD_MARK), exist_ok=True)
    for top in TOP_LEVEL:
        for building in BUILDINGS:
            path = os.path.join(fixture_dir, PAYLOAD_MARK, "%s_%s.usda" % (top.strip("/"), building))
            with open(path, "w") as f:
                f.write(BUILDING)
    city_path = os.path.join(fixture_dir, "scene_expand_hierarchy.usda")
    with open(city_path, "w") as f:
        f.write("#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n)\n")
        for i, top in enumerate(TOP_LEVEL):
            f.write(top_level_prim(top.strip("/"), 10 * i))
    WORLD_BUILDINGS = ["/World/" + b for b in BUILDINGS]

    def make(plugin, x, y):
        node = app.createNode(plugin)
        require(node is not None, "create %s" % plugin, plugin_problem(plugin))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("filename").setValue(city_path)
    expand = make("USDExpandHierarchy", 0, 150)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)

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

    def load_state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def select_rows(paths):
        t = tree()
        t.selectionModel().clearSelection()
        for path in paths:
            index = find(path)
            t.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            t.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def row_menu(paths, action_name):
        """Opens the row menu on paths and triggers action_name; returns {objectName: enabled}."""
        seen = {}

        def act():
            menu = QApplication.activePopupWidget()
            if menu is None:
                return
            for action in menu.actions():
                if action.objectName():
                    seen[action.objectName()] = action.isEnabled()
            for action in menu.actions():
                if action.objectName() == action_name:
                    action.trigger()
            menu.close()

        select_rows(paths)
        pos = tree().visualRect(find(paths[-1])).center()
        QTimer.singleShot(200, act)
        tree().customContextMenuRequested.emit(pos)
        QApplication.processEvents()
        return seen

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def opened_payloads():
        return sorted(os.path.basename(layer.identifier) for layer in Sdf.Layer.GetLoadedLayers()
                      if PAYLOAD_MARK in layer.identifier)

    def world_payloads():
        return ["World_%s.usda" % b for b in BUILDINGS]

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

    def expect_rows(label, expected):
        """Waits until every path's cell reads its (state, inherited) pair."""
        ok = yield ("until", lambda: all(row_state(p) == s for p, s in expected.items()), POLL_TIMEOUT_S)
        check(ok, "%s: the panel reads %r (%r)" % (label, expected, dict((p, row_state(p)) for p in expected)))

    def steps():
        global expand
        require(expand.connectInput(0, usd), "connect the USDRead into the USDExpandHierarchy")
        require(viewer.connectInput(0, expand), "connect the Viewer to the USDExpandHierarchy")
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
        ok = yield ("until", lambda: panel().isVisible() and find(TOP_LEVEL[-1]).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists %s" % TOP_LEVEL[-1])

        section("no rule")
        yield from expect_rows("no rule", dict((p, ("bbox", False)) for p in TOP_LEVEL))
        yield ("sleep", SETTLE_MS)
        save("scene-expand-hierarchy-default")
        check(opened_payloads() == [], "no payload file is open (%r)" % opened_payloads())

        section("full + load on one block")
        expand.getParam("pattern0").setValue("/Wor*")
        expand.getParam("draw0").setValue(STATES.index("loadAll"))
        ok = yield ("until", lambda: opened_payloads() == world_payloads(), LOAD_TIMEOUT_S)
        check(ok, "the /World buildings' payloads load (%r)" % opened_payloads())
        yield from expect_rows("Full + Load", {"/World": ("loadAll", False), "/Annex": ("bbox", False)})
        ok = yield ("until", lambda: all(load_state(p) == STATE_LOADED for p in WORLD_BUILDINGS), POLL_TIMEOUT_S)
        check(ok, "the panel reads the /World buildings loaded (%r)" % [load_state(p) for p in WORLD_BUILDINGS])
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        check(ok, "the scene view shows the loaded block")
        yield ("sleep", SETTLE_MS)
        check(opened_payloads() == world_payloads(), "no /Annex payload loads (%r)" % opened_payloads())
        check(all(load_state("/Annex/" + b) == STATE_UNLOADED for b in BUILDINGS),
              "the /Annex buildings stay unloaded (%r)" % [load_state("/Annex/" + b) for b in BUILDINGS])
        save("scene-expand-hierarchy-full-load")

        section("node taken out")
        # connectInput refuses an input that is already connected.
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        yield from expect_rows("taken out", dict((p, ("bbox", False)) for p in TOP_LEVEL))
        yield ("sleep", SETTLE_MS)
        check(all(load_state(p) == STATE_LOADED for p in WORLD_BUILDINGS),
              "the /World buildings stay loaded (%r)" % [load_state(p) for p in WORLD_BUILDINGS])
        check(opened_payloads() == world_payloads(), "the /World payload files stay open (%r)" % opened_payloads())
        save("scene-expand-hierarchy-removed")

        section("node deleted")
        # Viewed again, so that deleting it changes what the panel reads.
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, expand), "connect the Viewer to the USDExpandHierarchy again")
        yield from expect_rows("viewed again", {"/World": ("loadAll", False), "/Annex": ("bbox", False)})
        expand.destroy()
        expand = None

        def viewed_name():
            viewed = viewer.getInput(0)
            return viewed.getScriptName() if viewed is not None else None

        # The Viewer is reconnected to the deleted node's input after destroy() returns.
        ok = yield ("until", lambda: viewed_name() == usd.getScriptName(), POLL_TIMEOUT_S)
        check(ok, "deleting the node reconnects the Viewer to the USDRead (got %r)" % (viewed_name(),))
        yield from expect_rows("deleted", dict((p, ("bbox", False)) for p in TOP_LEVEL))
        check(all(load_state(p) == STATE_LOADED for p in WORLD_BUILDINGS),
              "the /World buildings stay loaded after deleting the node (%r)" % [load_state(p) for p in WORLD_BUILDINGS])

        section("unload")
        seen = row_menu(WORLD_BUILDINGS, "sceneGraphUnload")
        ok = yield ("until", lambda: bool(seen), POLL_TIMEOUT_S)
        require(ok, "the row menu opens on the /World buildings")
        check(seen.get("sceneGraphUnload") is True, "the row menu offers Unload (%r)" % seen)
        ok = yield ("until", lambda: all(load_state(p) == STATE_UNLOADED for p in WORLD_BUILDINGS), LOAD_TIMEOUT_S)
        check(ok, "Unload unloads the /World buildings (%r)" % [load_state(p) for p in WORLD_BUILDINGS])

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
