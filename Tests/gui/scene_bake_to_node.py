"""The Scene Graph panel's Bake to Node writes the viewer's draw states into a USDExpandHierarchy.

Graph: USDRead of a small stage this script writes to $NATRON_GUI_TEST_OUT/scene_bake_to_node.usda,
with Initial Draw at Full, into Viewer1. Rows: /World holding /World/A and /World/B, each an Xform
holding a Cube. The USDRead's Initial Draw sets /World. The viewer sets Full on /World/A/Cube,
then BBox on /World/A, which keeps the cube's Full without drawing it, and Hidden on /World/B. A
row under a BBox or Hidden row reads as inherited whatever is set on it, so the cubes always do.
The script checks:
  - Bake to Node is disabled while the viewer sets nothing, and enabled once it does;
  - baking creates one USDExpandHierarchy reading the USDRead, which the Viewer then views, with
    one rule naming each set path, the cube's before /World/A's;
  - every row reads the same state as before, each row the viewer set now set by that node, and
    Bake to Node is disabled again;
  - one Undo removes the node, reconnects the Viewer to the USDRead and gives the viewer its states
    back; Redo bakes them again;
  - with the USDExpandHierarchy viewed, a new viewer state on /World bakes into it, adding a rule
    and no node, and the node's rule then sets /World in place of the USDRead's;
  - the saved project reopens to the same rows, still set by the node.

Screenshots for sign-off: scene-bake-before, scene-bake-baked, scene-bake-undone,
scene-bake-reopened.

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
# The viewer's states, in the order the script sets them.
VIEWER_STATES = [(A_CUBE, "full"), (A, "bbox"), (B, "hidden")]
# SceneDrawStateEnum, which the panel's draw state role holds, and the USDExpandHierarchy's Draw menu.
STATES = ["bbox", "children", "full", "loadAll", "hidden"]
# SceneDrawOriginEnum, which the panel's origin role holds.
ORIGINS = ["viewer", "graph", "inherited", "default"]
# USDRead's Initial Draw menu, in order.
INITIAL_DRAW = ["bbox", "levels", "toKind", "full", "fullLoad"]
EXPAND = "USDExpandHierarchy"

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
    from PySide6.QtWidgets import QApplication, QMainWindow, QMessageBox, QToolButton, QTreeView

    from guitest import (OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1
    # SceneGraphModel's roles after ePrimPathRole: changed here, payload state, unknown bounds, the
    # draw state, whether it is inherited, where it comes from and the node that set it.
    DRAW_STATE_ROLE = Qt.UserRole + 5
    INHERITED_ROLE = Qt.UserRole + 6
    ORIGIN_ROLE = Qt.UserRole + 7
    ORIGIN_NODE_ROLE = Qt.UserRole + 8

    scene_path = os.path.join(OUT, "scene_bake_to_node.usda")
    with open(scene_path, "w") as f:
        f.write(SCENE)
    project_path = os.path.join(OUT, "scene_bake_to_node.ntpz")

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.setPosition(0, 0)
    usd.getParam("initialDraw").setValue(INITIAL_DRAW.index("full"))
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)
    USD_NAME = usd.getScriptName()

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def forget_widgets():
        """Drops the wrappers of a closed project's widgets, whose C++ objects are gone."""
        del _held[:]

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

    def bake_button():
        p = panel()
        return hold(p.findChild(QToolButton, "sceneGraphBakeDrawStates")) if p is not None else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if class_name(w) == name:
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
        try:
            while m.canFetchMore(parent):
                m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

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
        column = draw_column()
        return index.sibling(index.row(), column) if index.isValid() and column >= 0 else QModelIndex()

    def drawn(path):
        """(state, inherited) path's cell reads, or None."""
        c = cell(path)
        if not c.isValid() or c.data(DRAW_STATE_ROLE) is None:
            return None
        return (STATES[int(c.data(DRAW_STATE_ROLE))], bool(c.data(INHERITED_ROLE)))

    def origin(path):
        """(origin, node) of path's cell, or None."""
        c = cell(path)
        if not c.isValid() or c.data(ORIGIN_ROLE) is None:
            return None
        return (ORIGINS[int(c.data(ORIGIN_ROLE))], str(c.data(ORIGIN_NODE_ROLE) or ""))

    def cells():
        return dict((p, drawn(p)) for p in ROWS)

    def origins():
        return dict((p, origin(p)) for p in ROWS)

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def expand_nodes():
        return [n for n in app.getChildren() if n.getPluginID() == EXPAND]

    def input_name(node, i=0):
        inp = node.getInput(i) if node is not None else None
        return inp.getScriptName() if inp is not None else None

    def rules_of(node):
        count = node.getParam("ruleCount").getValue()
        return [(node.getParam("pattern%d" % i).getValue(), STATES[node.getParam("draw%d" % i).getValue()]) for i in range(count)]

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui":
                return w
        return None

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def trigger_edit_action(prefix):
        action = edit_menu_action(prefix)
        require(action is not None and action.isEnabled(), "an enabled %s action is in the Edit menu" % (prefix,))
        report("NOTE %s action: %r" % (prefix, action.text()))
        action.trigger()
        QApplication.processEvents()

    def set_state(path, state):
        require(QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", path), Q_ARG("QString", state)),
                "%s %s: the scene view has setDrawState" % (state, path))

    def show_panel():
        """Puts the Scene Graph panel beside the viewer, unless a reopened layout already shows it."""
        p = panel()
        if p is None or not p.isVisible():
            home = ancestor_of_class(view(), "TabWidget")
            require(home is not None, "the scene view is in a pane")
            split = app.getTabWidget(home.objectName()).splitHorizontally()
            require(split is not None, "split the viewer's pane")
            require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
            split = None
        ok = yield ("until", lambda: panel() is not None and panel().isVisible() and find(B_CUBE).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists the stage")
        require(draw_column() >= 0, "the tree has a Draw State column")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))

    def wait_for_scene(label):
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "%s: the viewer is up" % label)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "%s: the scene view shows the stage" % label)
        yield from show_panel()

    def expect(label, read, expected):
        ok = yield ("until", lambda: read() == expected, POLL_TIMEOUT_S)
        check(ok, "%s: the panel reads %r (got %r)" % (label, expected, read()))
        return ok

    def frame_drawn():
        """The boxes and markers the viewport drew in its last frame."""
        v = view()
        if v is None:
            return (0, 0)
        return (int(v.property("natronSceneDrawBoxes") or 0), int(v.property("natronSceneDrawMarkers") or 0))

    def bake(label):
        button = bake_button()
        require(button is not None, "%s: the panel has a Bake to Node button" % label)
        check(button.isEnabled(), "%s: Bake to Node is enabled while the viewer sets states" % label)
        button.click()
        button = None
        QApplication.processEvents()

    def reopen(label):
        # Showing the Scene Graph panel splits a pane and moves a tab, which are project edits, so
        # the reset asks whether to save them. The saved project is the one under test: discard.
        asked = []

        def discard_save_question():
            box = QApplication.activeModalWidget()
            if isinstance(box, QMessageBox) and box.button(QMessageBox.StandardButton.Discard) is not None:
                asked.append(box.text())
                box.button(QMessageBox.StandardButton.Discard).click()

        answer = QTimer()
        answer.timeout.connect(discard_save_question)
        answer.start(200)
        try:
            reset = app.resetProject()
        finally:
            answer.stop()
            answer = None
        if asked:
            report("NOTE %s: discarded the layout edits (%r)" % (label, asked[0]))
        require(reset, "%s: reset the project (close it, keep the window)" % label)
        forget_widgets()
        yield ("sleep", SETTLE_MS)
        require(app.loadProject(project_path) is not None, "%s: reopen %s" % (label, project_path))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        yield from wait_for_scene("first open")

        section("nothing set")
        yield from expect("graph", origins, {
            WORLD: ("graph", USD_NAME),
            A: ("inherited", ""),
            A_CUBE: ("inherited", ""),
            B: ("inherited", ""),
            B_CUBE: ("inherited", ""),
        })
        button = bake_button()
        require(button is not None, "the panel has a Bake to Node button")
        check(not button.isEnabled(), "Bake to Node is disabled while the viewer sets nothing")
        button = None

        section("viewer states")
        for path, state in VIEWER_STATES:
            set_state(path, state)
        # The cube's own state stays hidden under /World/A's BBox, so only the rows above it read set.
        set_rows = [A, B]
        ok = yield ("until", lambda: all(origin(p) == ("viewer", "") for p in set_rows), POLL_TIMEOUT_S)
        require(ok, "the viewer sets %r (got %r)" % (set_rows, origins()))
        check(origin(A_CUBE) == ("inherited", ""), "%s under a BBox reads inherited (got %r)" % (A_CUBE, origin(A_CUBE)))
        ok = yield ("until", lambda: bake_button() is not None and bake_button().isEnabled(), POLL_TIMEOUT_S)
        check(ok, "Bake to Node is enabled once the viewer sets states")
        yield ("sleep", SETTLE_MS)
        viewer_cells = cells()
        viewer_origins = origins()
        viewer_drawn = frame_drawn()
        check(viewer_cells[A] == ("bbox", False) and viewer_cells[B] == ("hidden", False),
              "the viewer's states show (%r)" % (viewer_cells,))
        check(viewer_drawn != (0, 0), "the viewer draws the set states (got %r)" % (viewer_drawn,))
        shot("scene-bake-before.png")

        section("bake")
        before_nodes = len(expand_nodes())
        bake("bake")
        ok = yield ("until", lambda: len(expand_nodes()) == before_nodes + 1, POLL_TIMEOUT_S)
        require(ok, "Bake to Node creates a USDExpandHierarchy")
        expand = expand_nodes()[-1]
        name = expand.getScriptName()
        check(input_name(expand) == USD_NAME, "%s reads the USDRead (got %r)" % (name, input_name(expand)))
        check(input_name(viewer) == name, "the Viewer views %s (got %r)" % (name, input_name(viewer)))
        check(rules_of(expand) == [(B, "hidden"), (A_CUBE, "full"), (A, "bbox")],
              "%s holds a rule per set path, below before above (got %r)" % (name, rules_of(expand)))
        baked_origins = dict(viewer_origins)
        for path in set_rows:
            baked_origins[path] = ("graph", name)
        yield from expect("baked origins", origins, baked_origins)
        yield from expect("baked cells", cells, viewer_cells)
        yield from expect("baked drawing", frame_drawn, viewer_drawn)
        ok = yield ("until", lambda: bake_button() is not None and not bake_button().isEnabled(), POLL_TIMEOUT_S)
        check(ok, "Bake to Node is disabled again, the viewer setting nothing")
        yield ("sleep", SETTLE_MS)
        shot("scene-bake-baked.png")

        section("undo")
        trigger_edit_action("undo")
        ok = yield ("until", lambda: name not in [n.getScriptName() for n in app.getChildren()], POLL_TIMEOUT_S)
        check(ok, "one Undo removes %s" % (name,))
        check(input_name(viewer) == USD_NAME, "after Undo the Viewer views the USDRead (got %r)" % (input_name(viewer),))
        yield from expect("undone origins", origins, viewer_origins)
        yield from expect("undone cells", cells, viewer_cells)
        yield from expect("undone drawing", frame_drawn, viewer_drawn)
        yield ("sleep", SETTLE_MS)
        shot("scene-bake-undone.png")

        section("redo")
        trigger_edit_action("redo")
        ok = yield ("until", lambda: name in [n.getScriptName() for n in app.getChildren()], POLL_TIMEOUT_S)
        check(ok, "Redo brings %s back" % (name,))
        expand = app.getNode(name)
        require(expand is not None, "%s is back" % (name,))
        check(input_name(viewer) == name, "after Redo the Viewer views %s (got %r)" % (name, input_name(viewer)))
        yield from expect("redone origins", origins, baked_origins)
        yield from expect("redone cells", cells, viewer_cells)
        yield from expect("redone drawing", frame_drawn, viewer_drawn)

        section("bake into the viewed node")
        set_state(WORLD, "children")
        ok = yield ("until", lambda: origin(WORLD) == ("viewer", ""), POLL_TIMEOUT_S)
        require(ok, "the viewer sets %s (got %r)" % (WORLD, origin(WORLD)))
        ok = yield ("until", lambda: bake_button() is not None and bake_button().isEnabled(), POLL_TIMEOUT_S)
        check(ok, "Bake to Node is enabled again")
        yield ("sleep", SETTLE_MS)
        again_cells = cells()
        again_drawn = frame_drawn()
        bake("bake again")
        ok = yield ("until", lambda: origin(WORLD) == ("graph", name), POLL_TIMEOUT_S)
        check(ok, "%s is set by %s (got %r)" % (WORLD, name, origin(WORLD)))
        check(len(expand_nodes()) == before_nodes + 1, "no node is created (got %r)" % [n.getScriptName() for n in expand_nodes()])
        check(rules_of(expand)[-1] == (WORLD, "children") and len(rules_of(expand)) == 4,
              "%s gains one rule (got %r)" % (name, rules_of(expand)))
        yield from expect("baked again", cells, again_cells)
        yield from expect("baked again drawing", frame_drawn, again_drawn)
        final_origins = dict(baked_origins)
        final_origins[WORLD] = ("graph", name)
        yield from expect("baked again origins", origins, final_origins)
        # What the node's rules decide, rather than what the live viewer showed before saving.
        final_cells = dict(viewer_cells)
        final_cells[WORLD] = ("children", False)

        section("save and reopen")
        require(app.saveProjectAs(project_path), "save the project to " + project_path)
        expand = None
        yield from reopen("reopen")
        yield from wait_for_scene("reopen")
        yield from expect("reopened cells", cells, final_cells)
        yield from expect("reopened origins", origins, final_origins)
        reopened_viewer = app.getNode("Viewer1")
        check(reopened_viewer is not None and input_name(reopened_viewer) == name,
              "the reopened Viewer views %s (got %r)" % (name, input_name(reopened_viewer)))
        yield ("sleep", SETTLE_MS)
        shot("scene-bake-reopened.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
