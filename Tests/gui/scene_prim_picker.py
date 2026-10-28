"""The prim picker button on prim path and pattern knobs.

Graph: USDRead(scene-reference.usda) -> USDEditTransform -> Viewer1, with the USDEditTransform's
settings panel open. The picker button beside its Prim Pattern field opens a popup tree over the
node's input scene; rows are picked through the popup tree's model with QModelIndex alone.

Checks:
  - the Prim Pattern field has a picker button, and clicking it opens the popup;
  - the popup tree lists Box, Ball and Cam under /World, and allows several rows;
  - picking Box and Ball and pressing OK sets the pattern to "/World/Box /World/Ball";
  - one Undo restores the previous pattern;
  - Cancel changes nothing;
  - with Cam selected in the 3D view (through the Scene Graph panel), "Use Viewer Selection"
    sets the pattern to "/World/Cam".

Shots: scene-prim-picker-popup.png (the popup with Box and Ball picked) and
scene-prim-picker-viewer.png (after Use Viewer Selection).

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
MARKER = "/NatronPickerMarker"

try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt
    from PySide6.QtWidgets import (QAbstractItemView, QApplication, QDialog, QDialogButtonBox, QMainWindow,
                                   QPushButton, QTreeView)

    from guitest import (FIXTURES, check, class_name, finish, menu_of, plugin_problem, require, run, shot,
                         viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    edit = app.createNode("USDEditTransform", -1, None, props)
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    pattern = edit.getParam("primPattern")
    require(pattern is not None, "USDEditTransform has a primPattern parameter")
    pattern.setValue(MARKER)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    edit.setPosition(0, 100)
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

    def panel_tree():
        return child(QTreeView, "sceneGraphTree")

    def popup():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QDialog) and w.objectName() == "scenePrimPicker" and w.isVisible():
                return w
        return None

    def popup_tree():
        p = popup()
        try:
            return hold(p.findChild(QTreeView, "scenePrimPickerTree")) if p is not None else None
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

    def select(tree, paths):
        world = row_of(tree, WORLD)
        tree.selectionModel().clearSelection()
        for path in paths:
            index = row_of(tree, path, world)
            require(index.isValid(), "the tree has a row for " + path)
            tree.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            tree.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)
        QApplication.processEvents()

    def picker_button():
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QPushButton) and w.objectName() == "knobPrimPicker" and w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def popup_button(which):
        box = popup().findChild(QDialogButtonBox, "scenePrimPickerButtons")
        return box.button(which) if box is not None else None

    def use_viewer_button():
        return popup().findChild(QPushButton, "scenePrimPickerUseViewer")

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

    def open_popup():
        button = picker_button()
        require(button is not None, "the Prim Pattern field has a picker button")
        check(button.isEnabled(), "the picker button is enabled")
        button.click()
        ok = yield ("until", lambda: popup() is not None and bool(popup().property("natronPickerSynced")), POLL_TIMEOUT_S)
        require(ok, "the picker opens and reads the input scene")
        ok = yield ("until", lambda: world_children(popup_tree()) == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "the picker lists Box, Ball and Cam under /World (got %r)" % (world_children(popup_tree()),))

    def connect_viewer(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def steps():
        require(edit.connectInput(0, usd), "connect the USDEditTransform to the USDRead")
        require(connect_viewer(edit), "connect the Viewer to the USDEditTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")

        # Pick two prims and confirm.
        yield from open_popup()
        check(popup_tree().selectionMode() == QAbstractItemView.ExtendedSelection,
              "a pattern knob's picker takes several prims")
        check(not popup_button(QDialogButtonBox.Ok).isEnabled(), "OK is disabled while nothing is picked")
        select(popup_tree(), [BOX, BALL])
        check(popup_button(QDialogButtonBox.Ok).isEnabled(), "OK is enabled once prims are picked")
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-picker-popup.png")
        popup_button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the picker")
        picked = str(pattern.getValue())
        check(picked == BOX + " " + BALL, "OK sets the pattern to both prims, in pick order (got %r)" % (picked,))

        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()
        check(str(pattern.getValue()) == MARKER, "one Undo restores the previous pattern (got %r)" % (pattern.getValue(),))

        # Cancel writes nothing.
        yield from open_popup()
        select(popup_tree(), [CAM])
        popup_button(QDialogButtonBox.Cancel).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "Cancel closes the picker")
        check(str(pattern.getValue()) == MARKER, "Cancel leaves the pattern alone (got %r)" % (pattern.getValue(),))

        # The 3D view's selection.
        tree = panel_tree()
        require(tree is not None, "the Scene Graph panel's tree exists")
        ok = yield ("until", lambda: world_children(tree) == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the scene")
        select(tree, [CAM])
        ok = yield ("until", lambda: viewer_selection() == [CAM], POLL_TIMEOUT_S)
        require(ok, "Cam is selected in the 3D view (got %r)" % (viewer_selection(),))
        yield from open_popup()
        require(use_viewer_button() is not None, "the picker has a Use Viewer Selection button")
        check(use_viewer_button().isEnabled(), "Use Viewer Selection is enabled with a viewer selection")
        use_viewer_button().click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "Use Viewer Selection closes the picker")
        check(str(pattern.getValue()) == CAM, "Use Viewer Selection sets the pattern to the 3D view's prims (got %r)"
              % (pattern.getValue(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-picker-viewer.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
