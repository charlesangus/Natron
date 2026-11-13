"""The Scene Graph panel on a stage with payloads, shown by a viewer that opens them unloaded.

Generates a small city under $NATRON_GUI_TEST_OUT and checks:
  - opening the scene opens no building's layer, and every payload row is badged as unloaded;
  - twirling one building shows a disabled Loading... row at once, then the building's prims, and
    opens that building's layer alone; other rows keep their expansion, the model is not reset, and
    neither the stage's edit count nor the viewer's sync moves;
  - closing a loaded row keeps it loaded, and opening it again does no work;
  - a payload whose file is missing badges its row and gives the USD error in the tooltip, and the
    rest of the stage is unaffected;
  - the row menu's Load loads the selection, and Load With Descendants the payloads under a group,
    with Load and Unload enabled by what the selection can take;
  - Unload removes the building's prims, keeps its badge, closes its rows and forgets their
    expansion, and it can twirl open again; no undo is offered for any of this.

Screenshots of each state go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
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
BLOCK0 = "/World/Block0"
BLOCK1 = "/World/Block1"
B0 = BLOCK0 + "/B0"
B1 = BLOCK0 + "/B1"
BROKEN = BLOCK0 + "/Broken"
OTHER0 = BLOCK1 + "/B0"
OTHER1 = BLOCK1 + "/B1"
FLOOR = B0 + "/Floor"
LOADING_LABEL = "Loading..."

STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)


def write_city(directory):
    """Writes the root layer and the payload files, Broken's left out, and returns the root's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)

    def building(name, x, z):
        with open(os.path.join(payloads, name + ".usda"), "w") as f:
            f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                    'def Xform "Building"\n{\n'
                    '    def Xform "Floor"\n    {\n'
                    '        def Cube "Geo"\n        {\n            double size = 1\n'
                    '            double3 xformOp:translate = (%g, 0, %g)\n'
                    '            uniform token[] xformOpOrder = ["xformOp:translate"]\n        }\n'
                    '    }\n}\n' % (x, z))
        return '@./payloads/%s.usda@' % name

    def row(prim, arc):
        return '        def Xform "%s" (\n            prepend payload = %s\n        )\n        {\n        }\n' % (prim, arc)

    path = os.path.join(directory, "payload_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\ndef Xform "World"\n{\n')
        f.write('    def Cube "Plain"\n    {\n        double size = 0.5\n    }\n')
        f.write('    def Xform "Block0"\n    {\n')
        f.write(row("B0", building("b0_0", -2, 0)))
        f.write(row("B1", building("b0_1", 2, 0)))
        f.write(row("Broken", '@./payloads/missing.usda@'))
        f.write('    }\n    def Xform "Block1"\n    {\n')
        f.write(row("B0", building("b1_0", -2, 3)))
        f.write(row("B1", building("b1_1", 2, 3)))
        f.write('    }\n}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QModelIndex, QItemSelectionModel, QPersistentModelIndex, Qt, QTimer
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_graph_payloads"))
    scene_path = write_city(fixture_dir)
    payload_dir = os.path.join(fixture_dir, "payloads")

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

    _held = []
    resets = [0]

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

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        while m.canFetchMore(parent):
            m.fetchMore(parent)
        return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def find(path):
        """The index of path's row, reading the tree down to it; an invalid index when absent."""
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

    def state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def names_under(path):
        return [str(i.data()) for i in rows(find(path))]

    def opened_buildings():
        """The payload files of the fixture that are open in the process."""
        names = []
        for layer in Sdf.Layer.GetLoadedLayers():
            real = layer.realPath or ""
            if real.startswith(payload_dir):
                names.append(os.path.basename(real))
        return sorted(names)

    def stage_marks():
        p = panel()
        v = view()
        return {
            "stageEdits": p.property("natronSceneGraphStageEditCount"),
            "syncRequests": v.property("natronSceneSyncRequests"),
            "syncedAt": v.property("natronSceneSyncedAtMs"),
        }

    def load_edits():
        return view().property("natronSceneLoadEdits")

    def loads_quiet():
        return not view().property("natronSceneLoadsPending")

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def select_rows(paths):
        t = tree()
        t.selectionModel().clearSelection()
        for path in paths:
            index = find(path)
            t.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            t.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def row_menu(paths, action_name=None):
        """Opens the row menu on paths; returns {objectName: enabled} and its labels, triggering action_name."""
        seen = {}
        labels = []

        def act():
            menu = QApplication.activePopupWidget()
            if menu is None:
                return
            for action in menu.actions():
                if action.objectName():
                    seen[action.objectName()] = action.isEnabled()
                labels.append(action.text())
            for action in menu.actions():
                if action.objectName() == action_name:
                    action.trigger()
            menu.close()

        select_rows(paths)
        pos = tree().visualRect(find(paths[-1])).center()
        QTimer.singleShot(200, act)
        tree().customContextMenuRequested.emit(pos)
        QApplication.processEvents()
        return seen, labels

    def undo_state():
        """Each main window's Edit > Undo, as its label and whether it is enabled."""
        found = []
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or class_name(win) != "Gui" or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                    for a in menu.actions():
                        if a.text().replace("&", "").lower().startswith("undo"):
                            found.append((a.text(), a.isEnabled()))
        return found

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
        model().modelReset.connect(lambda: resets.__setitem__(0, resets[0] + 1))

        # Listed unloaded.
        check(opened_buildings() == [], "opening the scene opens no building's layer (got %r)" % (opened_buildings(),))
        for path in (WORLD, BLOCK0, BLOCK1):
            tree().expand(find(path))
        yield ("sleep", SETTLE_MS)
        check(opened_buildings() == [], "opening rows without payloads opens no building's layer (got %r)" % (opened_buildings(),))
        check(load_edits() == 0, "opening rows without payloads loads nothing (got %r)" % (load_edits(),))
        check(state(BLOCK0) == STATE_NONE, "a row without a payload has no badge (got %r)" % (state(BLOCK0),))
        for path in (B0, B1, BROKEN, OTHER0, OTHER1):
            check(state(path) == STATE_UNLOADED, "%s is badged as unloaded (got %r)" % (path, state(path)))
            check(model().hasChildren(find(path)), "%s reports that it can have children" % (path,))
        shot("scene-graph-payloads-unloaded.png")

        # Twirling one building.
        other = QPersistentModelIndex(find(OTHER0))
        before = stage_marks()
        undo_before = undo_state()
        tree().expand(find(B0))
        check(state(B0) == STATE_LOADING, "the twirled row shows as loading at once (got %r)" % (state(B0),))
        check(names_under(B0) == [LOADING_LABEL], "the twirled row lists a loading row (got %r)" % (names_under(B0),))
        loading = rows(find(B0))[0]
        check(not (loading.flags() & Qt.ItemIsEnabled), "the loading row is disabled")
        shot("scene-graph-payloads-loading.png")
        ok = yield ("until", lambda: state(B0) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "the twirled row's payload loads (state %r)" % (state(B0),))
        check(names_under(B0) == ["Floor"], "the loaded row lists the building's prims in place of the loading row (got %r)" % (names_under(B0),))
        check(opened_buildings() == ["b0_0.usda"], "the twirl opens that building's layer alone (got %r)" % (opened_buildings(),))
        check(state(B1) == STATE_UNLOADED, "its sibling stays unloaded (got %r)" % (state(B1),))
        check(resets[0] == 0, "the load does not reset the model (%d resets)" % (resets[0],))
        check(other.isValid(), "another block's row survives the load")
        for path in (WORLD, BLOCK0, BLOCK1, B0):
            check(tree().isExpanded(find(path)), "%s stays open through the load" % (path,))
        after = stage_marks()
        check(after == before, "the twirl leaves the stage's edit count and the viewer's sync alone (%r, then %r)" % (before, after))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-payloads-loaded.png")

        # Closing and opening a loaded row.
        edits = load_edits()
        tree().collapse(find(B0))
        yield ("sleep", SETTLE_MS)
        check(state(B0) == STATE_LOADED, "closing a row keeps it loaded (got %r)" % (state(B0),))
        tree().expand(find(B0))
        check(state(B0) == STATE_LOADED, "opening a loaded row shows no loading (got %r)" % (state(B0),))
        yield ("sleep", SETTLE_MS)
        check(load_edits() == edits, "closing and opening a loaded row does no load (%r, then %r)" % (edits, load_edits()))

        # A file that does not open.
        tree().expand(find(BROKEN))
        ok = yield ("until", lambda: state(BROKEN) == STATE_FAILED, POLL_TIMEOUT_S)
        check(ok, "a payload whose file is missing badges its row (state %r)" % (state(BROKEN),))
        check(names_under(BROKEN) == [], "the failed row keeps no loading row (got %r)" % (names_under(BROKEN),))
        tip = str(find(BROKEN).data(Qt.ToolTipRole))
        check("missing.usda" in tip, "the failed row's tooltip gives USD's error (got %r)" % (tip,))
        check(state(B0) == STATE_LOADED and state(B1) == STATE_UNLOADED, "the failure leaves the other rows alone")
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-payloads-failed.png")
        tree().collapse(find(BROKEN))

        # The row menu.
        seen, labels = row_menu([B1])
        check(seen.get("sceneGraphLoad") is True, "Load is enabled on an unloaded row (got %r)" % (seen,))
        check(seen.get("sceneGraphLoadWithDescendants") is True, "Load With Descendants is enabled on an unloaded row (got %r)" % (seen,))
        check(seen.get("sceneGraphUnload") is False, "Unload is disabled on an unloaded row (got %r)" % (seen,))
        check(all("load" not in t.lower() or t in ("Load", "Load With Descendants", "Unload") for t in labels),
              "the load actions read Load, Load With Descendants, Unload (got %r)" % (labels,))
        check(not any("undo" in t.lower() for t in labels), "the row menu offers no undo (got %r)" % (labels,))
        row_menu([B1], "sceneGraphLoad")
        check(state(B1) in (STATE_LOADING, STATE_LOADED), "the row menu's Load shows on the row (got %r)" % (state(B1),))
        ok = yield ("until", lambda: state(B1) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "the row menu's Load loads the row (state %r)" % (state(B1),))
        check(opened_buildings() == ["b0_0.usda", "b0_1.usda"], "Load opens the selected row's layer alone (got %r)" % (opened_buildings(),))
        check(not tree().isExpanded(find(B1)), "Load leaves a closed row closed")

        seen, _ = row_menu([B1])
        check(seen.get("sceneGraphLoad") is False, "Load is disabled on a loaded row (got %r)" % (seen,))
        check(seen.get("sceneGraphUnload") is True, "Unload is enabled on a loaded row (got %r)" % (seen,))

        row_menu([BLOCK1], "sceneGraphLoadWithDescendants")
        ok = yield ("until", lambda: state(OTHER0) == STATE_LOADED and state(OTHER1) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "Load With Descendants loads the payloads under a group (states %r, %r)" % (state(OTHER0), state(OTHER1)))
        check(opened_buildings() == ["b0_0.usda", "b0_1.usda", "b1_0.usda", "b1_1.usda"],
              "Load With Descendants opens their layers (got %r)" % (opened_buildings(),))
        check(tree().isExpanded(find(BLOCK1)), "the group's row stays open")
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-payloads-load-with-descendants.png")

        # Unload.
        tree().expand(find(FLOOR))
        yield ("sleep", SETTLE_MS)
        check(names_under(FLOOR) == ["Geo"], "the building's floor lists its prims (got %r)" % (names_under(FLOOR),))
        edits = load_edits()
        row_menu([B0], "sceneGraphUnload")
        check(not tree().isExpanded(find(B0)), "Unload closes the row")
        ok = yield ("until", lambda: state(B0) == STATE_UNLOADED and load_edits() != edits, POLL_TIMEOUT_S)
        check(ok, "Unload unloads the row, which keeps its badge (state %r)" % (state(B0),))
        check(names_under(B0) == [], "Unload removes the building's prims from the stage (got %r)" % (names_under(B0),))
        check(model().hasChildren(find(B0)), "the unloaded row reports again that it can have children")
        check(state(B1) == STATE_LOADED, "Unload leaves its sibling loaded (got %r)" % (state(B1),))
        check(resets[0] == 0, "Unload does not reset the model (%d resets)" % (resets[0],))
        check(tree().isExpanded(find(BLOCK0)), "the row's parent stays open")
        after = stage_marks()
        check(after == before, "Load and Unload leave the stage's edit count and the viewer's sync alone (%r, then %r)" % (before, after))
        check(undo_state() == undo_before, "no undo is offered for a load or an unload (%r, then %r)" % (undo_before, undo_state()))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-payloads-unloaded-again.png")

        tree().expand(find(B0))
        ok = yield ("until", lambda: state(B0) == STATE_LOADED and names_under(B0) == ["Floor"], POLL_TIMEOUT_S)
        check(ok, "the unloaded row twirls open and loads again (state %r)" % (state(B0),))
        check(not tree().isExpanded(find(FLOOR)), "the reloaded row does not reopen the rows Unload closed")
        ok = yield ("until", loads_quiet, POLL_TIMEOUT_S)
        check(ok, "no load is left waiting")
        report("NOTE load edits %r, last hold %r ms" % (load_edits(), view().property("natronSceneLoadHoldMs")))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
