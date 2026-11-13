"""The Scene Graph panel's Draw State column tells apart the states the viewer sets, the ones a
node's draw rule sets, and the ones a row inherits.

Graph: USDRead of a small stage this script writes to $NATRON_GUI_TEST_OUT/scene_draw_origin.usda,
with Initial Draw at Full, into a USDExpandHierarchy whose one rule boxes /World/A, into Viewer1.
Rows: /World holding /World/A and /World/B, each an Xform holding a Cube. The script checks:
  - /World is set by the USDRead and /World/A by the USDExpandHierarchy: each cell's origin is the
    graph, its tooltip names that node, and neither is inherited;
  - /World/B and the cubes inherit;
  - BBox picked on /World/A in its cell's editor makes it viewer-set, with a tooltip saying so and
    a look unlike the graph-set BBox it had;
  - Reset to Graph in the row menu gives /World/A back to the USDExpandHierarchy;
  - with /World/A and /World/B set in the viewer, the panel's Reset to Graph button gives both
    back to the graph;
  - changing only the rule, its draw and then its pattern, changes the column with nothing else
    touched.

Screenshots for sign-off: scene-draw-origin-graph, scene-draw-origin-viewer,
scene-draw-origin-reset, scene-draw-origin-rule-moved.

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
WORLD = "/World"
A = "/World/A"
B = "/World/B"
A_CUBE = "/World/A/Cube"
B_CUBE = "/World/B/Cube"
ROWS = [WORLD, A, A_CUBE, B, B_CUBE]
# SceneDrawStateEnum, which the panel's draw state role holds, and the USDExpandHierarchy's Draw menu.
STATES = ["bbox", "children", "full", "loadAll", "hidden"]
# SceneDrawOriginEnum, which the panel's origin role holds.
ORIGINS = ["viewer", "graph", "inherited", "default"]
# USDRead's Initial Draw menu, in order.
INITIAL_DRAW = ["bbox", "levels", "toKind", "full", "fullLoad"]

SCENE = """#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Xform "A"
    {
        double3 xformOp:translate = (-2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
    def Xform "B"
    {
        double3 xformOp:translate = (2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
}
"""

try:
    from PySide6.QtCore import Q_ARG, QMetaObject, QModelIndex, Qt, QTimer
    from PySide6.QtGui import QAction
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QComboBox, QToolButton, QTreeView

    from guitest import OUT, check, finish, plugin_problem, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    # SceneGraphModel's roles after ePrimPathRole: changed here, payload state, unknown bounds, the
    # draw state, whether it is inherited, where it comes from and the node that set it.
    DRAW_STATE_ROLE = Qt.UserRole + 5
    INHERITED_ROLE = Qt.UserRole + 6
    ORIGIN_ROLE = Qt.UserRole + 7
    ORIGIN_NODE_ROLE = Qt.UserRole + 8

    scene_path = os.path.join(OUT, "scene_draw_origin.usda")
    with open(scene_path, "w") as f:
        f.write(SCENE)

    def make(plugin, x, y):
        node = app.createNode(plugin)
        require(node is not None, "create %s" % plugin, plugin_problem(plugin))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("initialDraw").setValue(INITIAL_DRAW.index("full"))
    usd.getParam("filename").setValue(scene_path)
    expand = make("USDExpandHierarchy", 0, 150)
    expand.getParam("pattern0").setValue(A)
    expand.getParam("draw0").setValue(STATES.index("bbox"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)
    USD_NAME = usd.getScriptName()
    EXPAND_NAME = expand.getScriptName()

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

    def cell(path):
        index = find(path)
        return index.sibling(index.row(), draw_column()) if index.isValid() else index

    def origin(path):
        """(state, origin, node, inherited) of path's cell; Nones when it has none."""
        c = cell(path)
        if not c.isValid() or c.data(DRAW_STATE_ROLE) is None or c.data(ORIGIN_ROLE) is None:
            return (None, None, None, None)
        return (STATES[int(c.data(DRAW_STATE_ROLE))], ORIGINS[int(c.data(ORIGIN_ROLE))], str(c.data(ORIGIN_NODE_ROLE) or ""),
                bool(c.data(INHERITED_ROLE)))

    def tooltip(path):
        c = cell(path)
        return str(c.data(Qt.ToolTipRole) or "") if c.isValid() else ""

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def reveal(path):
        index = find(path)
        t = tree()
        parent = index.parent()
        while parent.isValid():
            t.expand(parent)
            parent = parent.parent()
        t.scrollTo(index)
        QApplication.processEvents()

    def editor_of(c):
        """The editor open on cell c, or None; found through the view, as a closed editor lingers."""
        t = tree()
        w = t.indexWidget(c)
        if w is None:
            return None
        if isinstance(w, QComboBox):
            return w
        for combo in t.findChildren(QComboBox, "sceneGraphDrawStateEditor"):
            if combo.isVisible() and combo.geometry() == w.geometry():
                return combo
        return None

    def pick_in_editor(path, label):
        """Opens path's cell editor and picks label in it. False when no editor or no such entry opened."""
        reveal(path)
        t = tree()
        c = cell(path)
        if not c.isValid():
            return False
        t.edit(c)
        QApplication.processEvents()
        QApplication.processEvents()
        combo = editor_of(c)
        if combo is None:
            return False
        entry = combo.findText(label)
        if entry < 0:
            combo = None
            t.closePersistentEditor(c)
            return False
        combo.hidePopup()
        combo.setCurrentIndex(entry)
        combo.activated.emit(entry)
        combo = None
        QApplication.processEvents()
        return True

    def pick_in_menu(path, action_name):
        """Opens the row menu on path and triggers the action named action_name, wherever it is in it."""
        reveal(path)
        t = tree()
        index = find(path)
        t.clearSelection()
        t.setCurrentIndex(index)
        QApplication.processEvents()
        found = []

        def trigger():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            for action in popup.findChildren(QAction):
                if action.objectName() == action_name:
                    found.append(action_name)
                    if action.isEnabled():
                        action.trigger()
                    break
            popup.close()

        QTimer.singleShot(300, trigger)
        t.customContextMenuRequested.emit(t.visualRect(index).center())
        QApplication.processEvents()
        return bool(found)

    def cell_image(path):
        t = tree()
        return t.viewport().grab(t.visualRect(cell(path))).toImage()

    def expect(label, expected):
        """Waits until every path's cell reads its (state, origin, node, inherited)."""
        ok = yield ("until", lambda: all(origin(p) == s for p, s in expected.items()), POLL_TIMEOUT_S)
        check(ok, "%s: the column reads %r (%r)" % (label, expected, dict((p, origin(p)) for p in expected)))
        return ok

    def graph_rows():
        return {
            WORLD: ("full", "graph", USD_NAME, False),
            A: ("bbox", "graph", EXPAND_NAME, False),
            B: ("full", "inherited", "", True),
            B_CUBE: ("full", "inherited", "", True),
        }

    def steps():
        require(expand.connectInput(0, usd), "connect the USDRead into the USDExpandHierarchy")
        require(viewer.connectInput(0, expand), "connect the Viewer to the USDExpandHierarchy")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the stage")
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and find(B_CUBE).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists the stage")
        require(draw_column() >= 0, "the tree has a Draw State column")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        panel().window().activateWindow()
        ok = yield ("until", lambda: panel() is not None and panel().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the panel's window is active before input")
        for path in ROWS:
            reveal(path)

        section("graph-set rows")
        yield from expect("graph", graph_rows())
        check(USD_NAME in tooltip(WORLD), "the tooltip of /World names %s (%r)" % (USD_NAME, tooltip(WORLD)))
        check(EXPAND_NAME in tooltip(A), "the tooltip of /World/A names %s (%r)" % (EXPAND_NAME, tooltip(A)))
        check(EXPAND_NAME not in tooltip(B), "the tooltip of the inherited /World/B names no node (%r)" % (tooltip(B),))
        graph_set = cell_image(A)
        yield ("sleep", SETTLE_MS)
        shot("scene-draw-origin-graph.png")

        section("viewer-set row")
        check(pick_in_editor(A, "BBox"), "BBox on /World/A in its cell's editor")
        yield from expect("viewer", {A: ("bbox", "viewer", "", False)})
        check(EXPAND_NAME not in tooltip(A) and "viewer" in tooltip(A).lower(),
              "the tooltip of /World/A says the viewer sets it (%r)" % (tooltip(A),))
        check(cell_image(A) != graph_set, "a viewer-set BBox looks unlike a graph-set one")
        yield ("sleep", SETTLE_MS)
        shot("scene-draw-origin-viewer.png")

        section("Reset to Graph on the row")
        check(pick_in_menu(A, "sceneGraphDrawStateReset"), "the row menu of /World/A offers Reset to Graph")
        yield from expect("row reset", graph_rows())
        check(EXPAND_NAME in tooltip(A), "the tooltip of /World/A names %s again (%r)" % (EXPAND_NAME, tooltip(A)))

        section("Reset to Graph on the panel")
        check(pick_in_editor(A, "Hidden"), "Hidden on /World/A")
        check(pick_in_editor(B, "BBox"), "BBox on /World/B")
        yield from expect("both set", {A: ("hidden", "viewer", "", False), B: ("bbox", "viewer", "", False)})
        button = panel().findChild(QToolButton, "sceneGraphResetDrawStates")
        require(button is not None and button.isEnabled(), "the panel has a Reset to Graph button")
        QTest.mouseClick(button, Qt.LeftButton)
        button = None
        yield from expect("panel reset", graph_rows())
        yield ("sleep", SETTLE_MS)
        shot("scene-draw-origin-reset.png")

        section("a change of rule alone")
        expand.getParam("draw0").setValue(STATES.index("hidden"))
        yield from expect("rule draw", {A: ("hidden", "graph", EXPAND_NAME, False)})
        expand.getParam("pattern0").setValue(B)
        yield from expect("rule pattern", {
            A: ("full", "inherited", "", True),
            B: ("hidden", "graph", EXPAND_NAME, False),
        })
        check(EXPAND_NAME in tooltip(B), "the tooltip of /World/B names %s (%r)" % (EXPAND_NAME, tooltip(B)))
        yield ("sleep", SETTLE_MS)
        shot("scene-draw-origin-rule-moved.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
