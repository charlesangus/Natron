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

  - the viewer shows the USDPrune's output, whose own matches are deactivated, yet Select Matches searches the node's
    input with the viewer's loads: with "/World//*" it still finds the prims below the deactivated ones, in the
    branch the viewer has loaded;
  - on a payload scene with nothing loaded, Select Matches selects only the loaded matches and reports the
    unloaded branches not searched while no payload loads; its "Load and select" button loads the payloads and
    selects the matches inside them.
  - behind a USDExpandHierarchy whose Full + Load rule on //Trigger loads each Trigger's own payload once a load
    shows the Trigger: Select Matches started right after a load leaves the Trigger's payload to that follow-up
    batch waits for it and selects the Geo inside it; Load and select, whose load the rule follows with another
    batch, selects the matches of both and leaves nothing to offer.

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

STATE_UNLOADED = 1
BLOCK0_B0 = "/World/Block0/B0"
UNLOADED_GEO = "/World/Block0/B0/Floor/Geo"
B0_FLOOR = "/World/Block0/B0/Floor"
PLAIN = "/World/Plain"


def write_city(directory):
    """Writes a stage with one payload prim, B0, under /World/Block0; returns the root layer's path."""
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "b0.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                'def Xform "Building"\n{\n'
                '    def Xform "Floor"\n    {\n'
                '        def Cube "Geo"\n        {\n            double size = 1\n        }\n'
                '    }\n}\n')
    path = os.path.join(directory, "payload_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\ndef Xform "World"\n{\n'
                '    def Cube "Plain"\n    {\n        double size = 0.5\n    }\n'
                '    def Xform "Block0"\n    {\n'
                '        def Xform "B0" (\n            prepend payload = @./b0.usda@\n        )\n        {\n        }\n'
                '    }\n}\n')
    return path


C0 = "/World/Block0/C0"
C1 = "/World/Block1/C1"
STATE_LOADALL = 3


def write_trigger_city(directory):
    """Writes a stage with payload prims C0 and C1, each holding a Trigger with a payload of its own."""
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "trigger.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Inner"\n)\n'
                'def Xform "Inner"\n{\n    def Cube "Geo"\n    {\n        double size = 0.25\n    }\n}\n')
    with open(os.path.join(directory, "building.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                'def Xform "Building"\n{\n'
                '    def Xform "Floor"\n    {\n'
                '        def Cube "Geo"\n        {\n            double size = 1\n        }\n'
                '    }\n'
                '    def Xform "Trigger" (\n        prepend payload = @./trigger.usda@\n    )\n    {\n    }\n'
                '}\n')
    path = os.path.join(directory, "trigger_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\ndef Xform "World"\n{\n'
                '    def Xform "Block0"\n    {\n'
                '        def Xform "C0" (\n            prepend payload = @./building.usda@\n        )\n        {\n        }\n'
                '    }\n'
                '    def Xform "Block1"\n    {\n'
                '        def Xform "C1" (\n            prepend payload = @./building.usda@\n        )\n        {\n        }\n'
                '    }\n}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QItemSelectionModel, QMetaObject, QModelIndex, Qt
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QMenu, QPushButton, QTreeView

    from guitest import (FIXTURES, OUT, any_panel_open, check, class_name, close_all_panels, finish, menu_of,
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
            # The panel's wrapper is held too: the tree's wrapper dies with it.
            return hold(hold(panels[0]).findChild(QTreeView, "sceneGraphTree")) if panels else None
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

    def offer_widget(name, cls):
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, cls) and w.objectName() == name and w.isVisible():
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def find_path(tree, path, parent=QModelIndex()):
        for index in rows(tree, parent):
            p = path_of(index)
            if p == path:
                return index
            if path.startswith(p + "/"):
                return find_path(tree, path, index)
        return QModelIndex()

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        viewer.disconnectInput(0)
        # The viewer shows the USDRead while the pattern is edited: the prims the pattern names are pruned
        # from the prune's output, and the panel would then have no row to select for them.
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
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
        # Select Matches searches through a viewer whose scene reaches the node, so the viewer moves onto
        # the USDPrune; the search reads the node's input, which the prune's own matches do not hide from.
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the USDPrune's scene")
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: viewer_selection() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        check(ok, "Select Matches on /World/* selects Box, Ball and Cam (got %r)" % (viewer_selection(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-knob-tools-matches.png")

        # A payload scene with nothing loaded.
        city = write_city(os.path.join(OUT, "scene_knob_tools_city"))
        usd2 = app.createNode("USDRead")
        require(usd2 is not None, "create the second USDRead", plugin_problem("USDRead"))
        usd2.getParam("filename").setValue(city)
        usd2.setPosition(450, 0)
        pattern.setValue(CAM)
        prune.disconnectInput(0)
        require(prune.connectInput(0, usd2), "connect the USDPrune to the payload scene")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: panel_tree() is not None and find_path(panel_tree(), BLOCK0_B0).isValid(),
                    POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the payload scene")
        check(int(find_path(panel_tree(), BLOCK0_B0).data(Qt.UserRole + 3)) == STATE_UNLOADED,
              "the viewer lists B0 with its payload unloaded")
        pattern.setValue(PLAIN + " //Geo")
        edits_before = view().property("natronSceneLoadEdits")
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: offer_widget("knobPrimSelectMatchesLoad", QPushButton) is not None, POLL_TIMEOUT_S)
        require(ok, "Select Matches offers to load the unloaded branches")
        report = offer_widget("knobPrimSelectMatchesReport", QLabel)
        check(report is not None and "1 unloaded branch not searched" in report.text(),
              "the report counts the unloaded branch not searched (got %r)" % (report.text() if report else None,))
        check(viewer_selection() == [PLAIN],
              "Select Matches selects only the loaded matches (got %r)" % (viewer_selection(),))
        check(int(find_path(panel_tree(), BLOCK0_B0).data(Qt.UserRole + 3)) == STATE_UNLOADED,
              "no payload is loaded by the search")
        check(view().property("natronSceneLoadEdits") == edits_before,
              "the search made no load edit on the viewer's stage (%r -> %r)"
              % (edits_before, view().property("natronSceneLoadEdits")))
        yield ("sleep", SETTLE_MS)
        shot("scene-knob-tools-load-offer.png")
        offer_widget("knobPrimSelectMatchesLoad", QPushButton).click()
        ok = yield ("until", lambda: UNLOADED_GEO in viewer_selection(), POLL_TIMEOUT_S)
        check(ok and PLAIN in viewer_selection(),
              "Load and select selects the match inside the payload besides the loaded one (got %r)" % (viewer_selection(),))
        check(view().property("natronSceneLoadEdits") > edits_before, "Load and select loaded the payload in the viewer")
        check(offer_widget("knobPrimSelectMatchesLoad", QPushButton) is None, "the offer is gone once the payload is loaded")

        # The viewer shows the USDPrune's output, where the pattern's own matches are deactivated and
        # the walk of that stage would not descend into them; the search reads the node's input.
        pattern.setValue("/World//*")
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: B0_FLOOR in viewer_selection() and UNLOADED_GEO in viewer_selection(), POLL_TIMEOUT_S)
        check(ok and BLOCK0_B0 in viewer_selection() and PLAIN in viewer_selection(),
              "Select Matches finds the matches the viewer's pruned stage has deactivated, loaded branch included "
              "(got %r)" % (viewer_selection(),))
        check(offer_widget("knobPrimSelectMatchesLoad", QPushButton) is None,
              "the search of the input uses the viewer's loads, so nothing is left to offer")

        # A Full + Load rule on //Trigger matches again after each load and loads the payload of each Trigger
        # a load shows, in a batch of its own after the one that showed it.
        trigger_city = write_trigger_city(os.path.join(OUT, "scene_knob_tools_trigger_city"))
        usd3 = app.createNode("USDRead")
        require(usd3 is not None, "create the third USDRead", plugin_problem("USDRead"))
        usd3.getParam("filename").setValue(trigger_city)
        usd3.setPosition(900, 0)
        expand = app.createNode("USDExpandHierarchy")
        require(expand is not None, "create USDExpandHierarchy", plugin_problem("USDExpandHierarchy"))
        expand.setPosition(900, 50)
        require(expand.connectInput(0, usd3), "connect the USDExpandHierarchy to the trigger scene")
        expand.getParam("pattern0").setValue("//Trigger")
        expand.getParam("draw0").setValue(STATE_LOADALL)
        pattern.setValue(CAM)
        prune.disconnectInput(0)
        require(prune.connectInput(0, expand), "connect the USDPrune to the USDExpandHierarchy")
        ok = yield ("until", lambda: panel_tree() is not None and find_path(panel_tree(), C1).isValid()
                    and find_path(panel_tree(), C0).isValid(), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the trigger scene")
        check(int(find_path(panel_tree(), C1).data(Qt.UserRole + 3)) == STATE_UNLOADED,
              "the viewer lists C1 with its payload unloaded")
        c1_geos = sorted([C1 + "/Building/Floor/Geo", C1 + "/Building/Trigger/Inner/Geo"])
        pattern.setValue("/World/Block1//Geo")
        # C1's own payload only: its Trigger's payload is left to the rule's batch after this one.
        QMetaObject.invokeMethod(view(), "requestLoad", Q_ARG("QStringList", [C1]), Q_ARG(bool, False))
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: viewer_selection() == c1_geos, POLL_TIMEOUT_S)
        check(ok, "Select Matches started during a load waits for the rule's follow-up batch and selects the Geo "
                  "inside the Trigger's payload (got %r)" % (viewer_selection(),))

        c0_geos = sorted([C0 + "/Building/Floor/Geo", C0 + "/Building/Trigger/Inner/Geo"])
        pattern.setValue("/World/Block0//Geo")
        trigger(menu, "knobPrimSelectMatches")
        ok = yield ("until", lambda: offer_widget("knobPrimSelectMatchesLoad", QPushButton) is not None, POLL_TIMEOUT_S)
        require(ok, "Select Matches offers to load C0")
        offer_widget("knobPrimSelectMatchesLoad", QPushButton).click()
        ok = yield ("until", lambda: viewer_selection() == c0_geos, POLL_TIMEOUT_S)
        check(ok, "Load and select, followed by the rule's batch, selects the matches of both (got %r)"
              % (viewer_selection(),))
        yield ("sleep", SETTLE_MS)
        check(offer_widget("knobPrimSelectMatchesLoad", QPushButton) is None,
              "Load and select reports nothing left to load once the rule's batches are in")
        pattern.setValue(CAM)
        prune.disconnectInput(0)
        require(prune.connectInput(0, usd), "connect the USDPrune back to the first scene")
        ok = yield ("until", lambda: bool(world_children(panel_tree())) and BOX in world_children(panel_tree()), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the first scene again")

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
