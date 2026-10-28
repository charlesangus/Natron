"""Set Selection on Current Node, from the 3D view and the Scene Graph panel.

Graph: USDRead(scene-reference.usda) -> USDPrune -> Viewer1, with the USDPrune's settings panel
open. Prims are selected through the Scene Graph panel's tree with QModelIndex, which selects them
in the 3D view too.

Checks:
  - with Box and Ball selected, Ctrl+Shift+E on the 3D view sets the USDPrune's pattern to both
    and they are pruned (/World holds only Cam); one Undo brings them back;
  - the same key on the Scene Graph panel's tree does the same;
  - with a Blur's settings panel opened last, the key changes nothing, and the action is disabled
    in both the 3D view's and the Scene Graph panel's context menus, with a hint as its tooltip.

Shots: scene-set-on-node-pruned.png (after the 3D view's key) and scene-set-on-node-blocked.png
(the 3D view's context menu with the disabled action).

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
MARKER = "/NatronSetOnNodeMarker"
BLUR_ID = "net.sf.cimg.CImgBlur"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QModelIndex, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeView

    from guitest import (FIXTURES, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot,
                         viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1
    SET_MODIFIERS = Qt.ControlModifier | Qt.ShiftModifier

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
    pattern.setValue(MARKER)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
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

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def child(cls, name):
        p = panel()
        try:
            return hold(p.findChild(cls, name)) if p is not None else None
        except RuntimeError:
            return None

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def rows(parent=QModelIndex()):
        t = tree()
        try:
            m = hold(t.model()) if t is not None else None
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

    def row_of(path, parent=QModelIndex()):
        for index in rows(parent):
            if path_of(index) == path:
                return index
        return QModelIndex()

    def world_children():
        world = row_of(WORLD)
        return sorted(path_of(i) for i in rows(world)) if world.isValid() else []

    def select(paths):
        world = row_of(WORLD)
        selection = tree().selectionModel()
        selection.clearSelection()
        for path in paths:
            index = row_of(path, world)
            require(index.isValid(), "the tree has a row for " + path)
            selection.select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            selection.setCurrentIndex(index, QItemSelectionModel.NoUpdate)
        QApplication.processEvents()

    def press_set_key(widget):
        widget.activateWindow()
        widget.setFocus()
        QApplication.processEvents()
        QTest.keyClick(widget, Qt.Key_E, SET_MODIFIERS)
        QApplication.processEvents()

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

    def undo():
        action = edit_menu_action("undo")
        require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
        action.trigger()
        QApplication.processEvents()

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def popup_action(object_name):
        popup = QApplication.activePopupWidget()
        if popup is None:
            return None
        for a in popup.actions():
            if a.objectName() == object_name:
                return a
        return None

    def check_blocked_in_menu(opener, where, shot_name):
        state = {}

        def open_menu():
            opener()
            state["closed"] = True

        QTimer.singleShot(0, open_menu)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if not check(ok, where + ": the context menu opens"):
            return
        popup = QApplication.activePopupWidget()
        action = popup_action("sceneSetSelectionOnNode")
        if check(action is not None, where + ": the menu has Set Selection on Current Node"):
            report("NOTE %s: %r, shortcut %r, tooltip %r" % (where, action.text(), action.shortcut().toString(),
                                                           action.toolTip()))
            check(action.text().replace("&", "") == "Set Selection on Current Node",
                  where + ": the action reads Set Selection on Current Node")
            check(not action.isEnabled(), where + ": the action is disabled for an image node")
            check("Set Selection on Current Node:" in action.toolTip(), where + ": the tooltip says why")
        if shot_name:
            shot(shot_name)
        QTest.keyClick(popup, Qt.Key_Escape)
        ok = yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        check(ok, where + ": Esc closes the context menu")

    def connect_viewer(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        require(connect_viewer(prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(tree() is not None, "the Scene Graph panel's tree exists")
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "the tree lists Box, Ball and Cam under /World (got %r)" % (world_children(),))

        # From the 3D view.
        select([BOX, BALL])
        ok = yield ("until", lambda: viewer_selection() == sorted([BOX, BALL]), POLL_TIMEOUT_S)
        require(ok, "Box and Ball are selected in the 3D view (got %r)" % (viewer_selection(),))
        press_set_key(view())
        got = str(pattern.getValue())
        check(sorted(got.split(" ")) == sorted([BOX, BALL]),
              "Ctrl+Shift+E in the 3D view sets the pattern to both prims (got %r)" % (got,))
        ok = yield ("until", lambda: world_children() == [CAM], POLL_TIMEOUT_S)
        check(ok, "both prims are pruned: /World holds only Cam (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-set-on-node-pruned.png")

        undo()
        check(str(pattern.getValue()) == MARKER, "one Undo restores the previous pattern (got %r)" % (pattern.getValue(),))
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        check(ok, "after Undo the prims come back (got %r)" % (world_children(),))

        # From the Scene Graph panel.
        select([BALL])
        ok = yield ("until", lambda: viewer_selection() == [BALL], POLL_TIMEOUT_S)
        require(ok, "Ball is selected (got %r)" % (viewer_selection(),))
        press_set_key(tree())
        check(str(pattern.getValue()) == BALL,
              "Ctrl+Shift+E in the Scene Graph panel sets the pattern (got %r)" % (pattern.getValue(),))
        ok = yield ("until", lambda: world_children() == sorted([BOX, CAM]), POLL_TIMEOUT_S)
        check(ok, "Ball is pruned (got %r)" % (world_children(),))
        undo()
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "after Undo the scene is whole again (got %r)" % (world_children(),))

        # An image node's panel opened last blocks the action.
        blur = app.createNode(BLUR_ID, -1, None, props)
        require(blur is not None, "create Blur", plugin_problem(BLUR_ID))
        blur.setPosition(200, 0)
        QApplication.processEvents()
        select([BOX])
        ok = yield ("until", lambda: viewer_selection() == [BOX], POLL_TIMEOUT_S)
        require(ok, "Box is selected (got %r)" % (viewer_selection(),))
        press_set_key(view())
        check(str(pattern.getValue()) == MARKER,
              "with a Blur's panel opened last the key changes nothing (got %r)" % (pattern.getValue(),))

        v = view()
        centre = QPoint(v.width() // 2, v.height() // 2)

        def open_view_menu():
            mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton)
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre),
                                                        Qt.NoModifier))

        yield from check_blocked_in_menu(open_view_menu, "3D view", "scene-set-on-node-blocked.png")

        t = tree()

        def open_tree_menu():
            t.customContextMenuRequested.emit(t.visualRect(row_of(BOX, row_of(WORLD))).center())

        yield from check_blocked_in_menu(open_tree_menu, "Scene Graph panel", None)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
