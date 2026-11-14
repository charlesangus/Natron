"""The armed viewer pick on prim knobs.

Graph: USDRead(scene-reference.usda) -> USDPrune -> Viewer1, with the USDPrune's settings panel
open. "Pick in Viewer" in the "..." menu beside its Prim Pattern field arms the 3D view and the
Scene Graph panel; prims are selected through the panel's tree, and Enter and Esc are real key
events sent to the 3D view or the tree.

Checks:
  - arming publishes "<node>.<knob>" as natronSceneArmedPick on the 3D view and the panel, and the
    panel's status line says so;
  - Enter writes the two selected prims to the pattern as one undo step, and ends the mode;
  - Esc, sent to the panel's tree, leaves the pattern unchanged and ends the mode;
  - arming a USDXformPrim's Prim Path while the pattern is armed replaces the armed knob, and Enter
    then writes the first selected prim to the path only;
  - deleting the armed node ends the mode, with the property cleared;
  - closing the armed node's settings panel ends the mode;
  - a knob driven by an expression offers only Select Matches in its menu, and a pick armed before
    the expression was set writes nothing on Enter.

Shot: scene-armed-pick.png (while armed with two prims selected).

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
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QMenu, QTreeView

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

    def armed(widget):
        try:
            return str(widget.property("natronSceneArmedPick") or "")
        except (RuntimeError, AttributeError):
            return ""

    def panel_widget():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def menu_with(name, visible_only=True):
        for m in tools_menus():
            try:
                if action_named(m, name) is not None and (not visible_only or m.parent().isVisible()):
                    return m
            except RuntimeError:
                continue
        return None

    def parts(text):
        return sorted(text.split())

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        viewer.disconnectInput(0)
        # The viewer shows the USDRead, not the USDPrune: the prims the pattern names are pruned from the
        # prune's output, and the panel would then have no row to select for them.
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: panel_tree() is not None and bool(world_children(panel_tree())), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the scene")
        ok = yield ("until", lambda: menu_with("knobPrimPick") is not None, POLL_TIMEOUT_S)
        require(ok, "the Prim Pattern field's menu has Pick in Viewer")
        menu = menu_with("knobPrimPick")
        label = prune.getScriptName() + ".primPattern"
        check(armed(view()) == "" and armed(panel_widget()) == "", "nothing is armed to begin with")

        trigger(menu, "knobPrimPick")
        check(armed(view()) == label, "arming publishes %r on the 3D view (got %r)" % (label, armed(view())))
        check(armed(panel_widget()) == label, "arming publishes %r on the panel (got %r)" % (label, armed(panel_widget())))
        status = panel_widget().findChild(QLabel, "sceneGraphStatus")
        check(status is not None and label in status.text(), "the panel's status line names the armed knob")
        yield from select([BOX, BALL])
        yield ("sleep", SETTLE_MS)
        shot("scene-armed-pick.png")
        QTest.keyClick(view(), Qt.Key_Return)
        QApplication.processEvents()
        check(parts(value()) == sorted([BOX, BALL]), "Enter writes both selected prims (got %r)" % (value(),))
        check(armed(view()) == "" and armed(panel_widget()) == "", "Enter ends the mode")
        undo_once()
        check(value() == CAM, "one Undo restores the pattern (got %r)" % (value(),))

        trigger(menu, "knobPrimPick")
        check(armed(view()) == label, "arming again publishes the knob")
        yield from select([BOX])
        QTest.keyClick(panel_tree(), Qt.Key_Escape)
        QApplication.processEvents()
        check(value() == CAM, "Esc leaves the pattern unchanged (got %r)" % (value(),))
        check(armed(view()) == "" and armed(panel_widget()) == "", "Esc ends the mode")
        QTest.keyClick(view(), Qt.Key_Return)
        QApplication.processEvents()
        check(value() == CAM, "Enter after the mode ended writes nothing (got %r)" % (value(),))

        xform = app.createNode("USDXformPrim", -1, None, props)
        require(xform is not None, "create USDXformPrim", plugin_problem("USDXformPrim"))
        xform.setPosition(150, 100)
        require(xform.connectInput(0, prune), "connect the USDXformPrim to the USDPrune")
        ok = yield ("until", lambda: menu_with("knobPrimPick") is not None and menu_with("knobPrimAdd") is not None
                    and len([m for m in tools_menus() if m.parent().isVisible()]) == 2, POLL_TIMEOUT_S)
        require(ok, "both settings panels show a selection menu")
        pattern_menu = menu_with("knobPrimAdd")
        path_menu = [m for m in tools_menus() if m.parent().isVisible() and m is not pattern_menu][0]
        xform_label = xform.getScriptName() + ".primPath"
        trigger(pattern_menu, "knobPrimPick")
        check(armed(view()) == label, "the pattern knob is armed")
        trigger(path_menu, "knobPrimPick")
        check(armed(view()) == xform_label and armed(panel_widget()) == xform_label,
              "arming another knob replaces the first (got %r)" % (armed(view()),))
        yield from select([BALL, BOX])
        QTest.keyClick(view(), Qt.Key_Enter)
        QApplication.processEvents()
        check(str(xform.getParam("primPath").getValue()) == BALL,
              "Enter writes the first selected prim to the path knob (got %r)" % (xform.getParam("primPath").getValue(),))
        check(value() == CAM, "the first knob was not written (got %r)" % (value(),))

        trigger(path_menu, "knobPrimPick")
        check(armed(view()) == xform_label, "the Prim Path knob is armed again")
        xform.destroy()
        ok = yield ("until", lambda: armed(view()) == "" and armed(panel_widget()) == "", POLL_TIMEOUT_S)
        check(ok, "deleting the armed node ends the mode")
        yield ("sleep", SETTLE_MS)
        check(view() is not None or viewer_up(), "the viewer is still up after the node is deleted")

        ok = yield ("until", lambda: menu_with("knobPrimPick") is not None, POLL_TIMEOUT_S)
        require(ok, "the USDPrune's menu is still shown")
        trigger(menu_with("knobPrimPick"), "knobPrimPick")
        check(armed(view()) == label, "the pattern knob is armed once more")
        close_all_panels()
        ok = yield ("until", lambda: armed(view()) == "" and armed(panel_widget()) == "", POLL_TIMEOUT_S)
        check(ok, "closing the settings panel ends the mode")
        check(value() == CAM, "the pattern is unchanged at the end (got %r)" % (value(),))

        driven = app.createNode("USDXformPrim", -1, None, props)
        require(driven is not None, "create a second USDXformPrim", plugin_problem("USDXformPrim"))
        driven.setPosition(150, 200)
        require(driven.connectInput(0, prune), "connect the second USDXformPrim to the USDPrune")
        ok = yield ("until", lambda: menu_with("knobPrimPick") is not None, POLL_TIMEOUT_S)
        require(ok, "the second USDXformPrim's panel shows a selection menu")
        driven_menu = menu_with("knobPrimPick")
        driven_path = driven.getParam("primPath")
        trigger(driven_menu, "knobPrimPick")
        check(armed(view()) == driven.getScriptName() + ".primPath", "the second Prim Path knob is armed")
        yield from select([BOX])
        require(driven_path.setExpression('"%s"' % (BALL,), False), "set an expression on the second Prim Path")
        QApplication.processEvents()
        QTest.keyClick(view(), Qt.Key_Return)
        QApplication.processEvents()
        check(str(driven_path.getValue()) == BALL,
              "Enter on a pick armed before the knob took an expression writes nothing (got %r)" % (driven_path.getValue(),))
        check(str(driven_path.getExpression(0)[0]) != "", "the expression is still set after Enter")
        QTest.keyClick(view(), Qt.Key_Escape)
        QApplication.processEvents()
        driven_menu.aboutToShow.emit()
        enabled = dict((a.objectName(), a.isEnabled()) for a in driven_menu.actions() if a.objectName())
        check(enabled == {"knobPrimReplace": False, "knobPrimPick": False, "knobPrimSelectMatches": True},
              "an expression-driven knob's menu enables only Select Matches (got %r)" % (enabled,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
