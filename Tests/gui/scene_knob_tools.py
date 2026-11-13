"""The selection menu on prim pattern knobs.

Graph: USDRead(scene-reference.usda) -> USDPrune -> Viewer1, with the USDPrune's settings panel
open. The "..." button beside its Prim Pattern field owns a menu (object name knobPrimToolsMenu)
whose actions are triggered directly; prims are selected through the Scene Graph panel's tree.

Checks:
  - the menu offers Replace, Add, Remove, Pick in Viewer and Select Matches on a pattern knob;
  - Add appends the selected prims in selection order and leaves the ones already there;
  - Remove takes the selected prims out of the pattern;
  - Replace swaps the pattern for the selected prims;
  - each of the three is one undo step, restoring the pattern before it;
  - Remove of a prim a wildcard entry covers appends a difference, "/World/* - /World/Cam";
  - Select Matches with the pattern "/World/*" selects Box, Ball and Cam in the 3D view;
  - a USDXformPrim's Prim Path menu offers only Replace, Pick in Viewer and Select Matches, and Replace writes the
    first selected prim.

Shot: scene-knob-tools-matches.png (after Select Matches).

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
WORLD = "/World"
BOX = "/World/Box"
BALL = "/World/Ball"
CAM = "/World/Cam"

try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt
    from PySide6.QtWidgets import QApplication, QMainWindow, QMenu, QTreeView

    from guitest import (FIXTURES, any_panel_open, check, class_name, close_all_panels, finish, menu_of,
                         plugin_problem, require, run, shot, viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    prune = app.createNode("USDPrune", -1, None, props)
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    pattern = prune.getParam("primPattern")
    require(pattern is not None, "USDPrune has a primPattern parameter")
    pattern.setValue(CAM)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    prune.setPosition(0, 100)
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def viewer_selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def panel_tree():
        panels = widgets_of("SceneGraphPanel")
        try:
            return hold(panels[0].findChild(QTreeView, "sceneGraphTree")) if panels else None
        except RuntimeError:
            return None

    def rows(tree, parent=QModelIndex()):
        try:
            m = hold(tree.model()) if tree is not None else None
        except RuntimeError:
            m = None
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

    def row_of(tree, path, parent=QModelIndex()):
        for index in rows(tree, parent):
            if path_of(index) == path:
                return index
        return QModelIndex()

    def world_children(tree):
        world = row_of(tree, WORLD)
        return sorted(path_of(i) for i in rows(tree, world)) if world.isValid() else []

    def select(paths):
        """Selects paths in click order and waits for the 3D view to follow."""
        tree = panel_tree()
        world = row_of(tree, WORLD)
        tree.selectionModel().clearSelection()
        for path in paths:
            index = row_of(tree, path, world)
            require(index.isValid(), "the tree has a row for " + path)
            tree.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            tree.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)
        QApplication.processEvents()
        ok = yield ("until", lambda: viewer_selection() == sorted(paths), POLL_TIMEOUT_S)
        require(ok, "the 3D view selects %r (got %r)" % (paths, viewer_selection()))

    def tools_menus():
        found = []
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QMenu) and w.objectName() == "knobPrimToolsMenu":
                    found.append(hold(w))
            except RuntimeError:
                continue
        return found

    def action_named(menu, name):
        for a in menu.actions():
            if a.objectName() == name:
                return a
        return None

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

    def trigger(menu, name):
        action = action_named(menu, name)
        require(action is not None, "the menu has the %s action" % (name,))
        action.trigger()
        QApplication.processEvents()

    def undo_once():
        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()

    def value():
        return str(pattern.getValue())

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: panel_tree() is not None, POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel's tree exists")
        ok = yield ("until", lambda: bool(world_children(panel_tree())), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the scene")
        ok = yield ("until", lambda: len(tools_menus()) == 1, POLL_TIMEOUT_S)
        require(ok, "the Prim Pattern field has a selection menu")
        menu = tools_menus()[0]
        names = [a.objectName() for a in menu.actions() if a.objectName()]
        check(names == ["knobPrimReplace", "knobPrimAdd", "knobPrimRemove", "knobPrimPick", "knobPrimSelectMatches"],
              "a pattern knob's menu offers Replace, Add, Remove, Pick in Viewer and Select Matches (got %r)" % (names,))

        yield from select([BOX, BALL])
        trigger(menu, "knobPrimAdd")
        check(value() == " ".join([CAM, BOX, BALL]), "Add appends the selection in order (got %r)" % (value(),))
        undo_once()
        check(value() == CAM, "one Undo undoes the Add (got %r)" % (value(),))

        trigger(menu, "knobPrimAdd")
        trigger(menu, "knobPrimAdd")
        check(value() == " ".join([CAM, BOX, BALL]), "Add twice does not repeat entries (got %r)" % (value(),))
        yield from select([BOX])
        trigger(menu, "knobPrimRemove")
        check(value() == " ".join([CAM, BALL]), "Remove takes the selected prim out (got %r)" % (value(),))
        undo_once()
        check(value() == " ".join([CAM, BOX, BALL]), "one Undo undoes the Remove (got %r)" % (value(),))

        yield from select([BALL])
        trigger(menu, "knobPrimReplace")
        check(value() == BALL, "Replace swaps the pattern for the selection (got %r)" % (value(),))
        undo_once()
        check(value() == " ".join([CAM, BOX, BALL]), "one Undo undoes the Replace (got %r)" % (value(),))

        pattern.setValue("/World/*")
        yield from select([CAM])
        trigger(menu, "knobPrimRemove")
        check(value() == "/World/* - " + CAM,
              "Remove of a prim a wildcard covers appends a difference (got %r)" % (value(),))
        undo_once()
        check(value() == "/World/*", "one Undo undoes the difference (got %r)" % (value(),))

        yield from select([BOX])
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: viewer_selection() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        check(ok, "Select Matches on /World/* selects Box, Ball and Cam (got %r)" % (viewer_selection(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-knob-tools-matches.png")

        close_all_panels()
        ok = yield ("until", lambda: not any_panel_open(), POLL_TIMEOUT_S)
        require(ok, "the USDPrune's panel closes")
        xform = app.createNode("USDXformPrim", -1, None, props)
        require(xform is not None, "create USDXformPrim", plugin_problem("USDXformPrim"))
        xform.setPosition(150, 100)
        require(xform.connectInput(0, prune), "connect the USDXformPrim to the USDPrune")
        ok = yield ("until", lambda: len([m for m in tools_menus() if m.parent() is not None and m.parent().isVisible()]) == 1,
                    POLL_TIMEOUT_S)
        require(ok, "the USDXformPrim's panel shows one selection menu")
        path_menu = [m for m in tools_menus() if m.parent().isVisible()][0]
        names = [a.objectName() for a in path_menu.actions() if a.objectName()]
        check(names == ["knobPrimReplace", "knobPrimPick", "knobPrimSelectMatches"],
              "a path knob's menu offers only Replace, Pick in Viewer and Select Matches (got %r)" % (names,))
        yield from select([BALL, BOX])
        trigger(path_menu, "knobPrimReplace")
        check(str(xform.getParam("primPath").getValue()) == BALL,
              "Replace writes the first selected prim to a path knob (got %r)" % (xform.getParam("primPath").getValue(),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
