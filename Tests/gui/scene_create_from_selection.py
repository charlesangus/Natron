"""Create Node from Selection: the scene-filtered tab menu, from the 3D view.

Graph: USDRead(scene-reference.usda) -> Viewer1. Box and Ball are selected through the Scene Graph
panel's tree with QModelIndex, which selects them in the 3D view too.

Checks:
  - Tab on the 3D view opens the tab menu, filtered to scene nodes with a prim path or pattern
    knob: it offers USDPrune and USDEditTransform, and no Blur or Grade;
  - choosing USDPrune creates one, between the USDRead and the Viewer, with its pattern set to both
    prims, which are pruned (/World holds only Cam);
  - one Undo removes the USDPrune, reconnects the Viewer to the USDRead and brings the prims back;
  - Tab on the Scene Graph panel's tree opens the same filtered menu.

Shots: scene-create-from-selection-menu.png (the filtered menu) and
scene-create-from-selection-pruned.png (after creating the USDPrune).

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
PRUNE_ID = "USDPrune"

try:
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMainWindow, QTreeView

    from guitest import (FIXTURES, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot,
                         viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
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

    def input_name(node, i=0):
        inp = node.getInput(i) if node is not None else None
        return inp.getScriptName() if inp is not None else None

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

    def open_menu(widget):
        widget.activateWindow()
        widget.setFocus()
        QApplication.processEvents()
        QTest.keyClick(widget, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        if not ok:
            return None, None, None
        popup = next(d for d in widgets_of("TabTabTabWidget") if d.isVisible())
        return popup, popup.findChildren(QLineEdit)[0], popup.findChildren(QListView)[0]

    def close_menu(popup):
        QTest.keyClick(popup, Qt.Key_Escape)
        QApplication.processEvents()

    def listed(line_edit, list_view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = list_view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def row_name(entry):
        return entry.split("[")[0].strip()

    def check_filtered(line_edit, list_view, where):
        scene = [row_name(e) for e in listed(line_edit, list_view, "USD")]
        report("NOTE %s lists for 'USD': %r" % (where, scene))
        check("USDPrune" in scene and "USDEditTransform" in scene,
              where + ": the menu offers USDPrune and USDEditTransform (got %r)" % (scene,))
        check("USDRead" not in scene, where + ": the menu leaves out USDRead, which has no input")
        for text in ("Blur", "Grade"):
            got = [row_name(e) for e in listed(line_edit, list_view, text)]
            check(text not in got, where + ": the menu leaves out %s (got %r)" % (text, got))

    def connect_viewer(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def steps():
        require(connect_viewer(usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(tree() is not None, "the Scene Graph panel's tree exists")
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "the tree lists Box, Ball and Cam under /World (got %r)" % (world_children(),))
        select([BOX, BALL])
        ok = yield ("until", lambda: viewer_selection() == sorted([BOX, BALL]), POLL_TIMEOUT_S)
        require(ok, "Box and Ball are selected in the 3D view (got %r)" % (viewer_selection(),))

        before = set(n.getScriptName() for n in app.getChildren())
        popup, line_edit, list_view = yield from open_menu(view())
        require(popup is not None, "Tab on the 3D view opens the tab menu")
        check_filtered(line_edit, list_view, "3D view")
        entries = listed(line_edit, list_view, "USDPrune")
        yield ("sleep", SETTLE_MS)
        shot("scene-create-from-selection-menu.png")
        row = next((r for r, e in enumerate(entries) if row_name(e) == PRUNE_ID), -1)
        if not check(row >= 0, "the menu offers USDPrune (got %r)" % (entries,)):
            close_menu(popup)
            return
        list_view.setCurrentIndex(list_view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)

        ok = yield ("until", lambda: len(app.getChildren()) > len(before), POLL_TIMEOUT_S)
        require(ok, "a new node was created")
        created = [n for n in app.getChildren() if n.getScriptName() not in before]
        check(len(created) == 1, "exactly one node was created (got %r)" % ([n.getScriptName() for n in created],))
        prune = created[0]
        check(prune.getPluginID() == PRUNE_ID, "the new node is a USDPrune (got %r)" % (prune.getPluginID(),))
        check(input_name(prune) == usd.getScriptName(), "the USDPrune's input is the USDRead (got %r)"
              % (input_name(prune),))
        check(input_name(viewer) == prune.getScriptName(), "the Viewer now shows the USDPrune (got %r)"
              % (input_name(viewer),))
        pattern = str(prune.getParam("primPattern").getValue())
        check(sorted(pattern.split(" ")) == sorted([BOX, BALL]),
              "the USDPrune's pattern holds both prims (got %r)" % (pattern,))
        ok = yield ("until", lambda: world_children() == [CAM], POLL_TIMEOUT_S)
        check(ok, "both prims are pruned: /World holds only Cam (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-create-from-selection-pruned.png")

        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        report("NOTE undo action: %r" % (undo.text(),))
        undo.trigger()
        QApplication.processEvents()
        ok = yield ("until", lambda: set(n.getScriptName() for n in app.getChildren()) == before, POLL_TIMEOUT_S)
        check(ok, "one Undo removes the USDPrune (got %r)" % (sorted(n.getScriptName() for n in app.getChildren()),))
        check(input_name(viewer) == usd.getScriptName(), "after Undo the Viewer shows the USDRead again (got %r)"
              % (input_name(viewer),))
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        check(ok, "after Undo the prims come back (got %r)" % (world_children(),))

        # The Scene Graph panel opens the same menu.
        select([BOX])
        popup, line_edit, list_view = yield from open_menu(tree())
        if check(popup is not None, "Tab on the Scene Graph panel's tree opens the tab menu"):
            check_filtered(line_edit, list_view, "Scene Graph panel")
            close_menu(popup)
        yield ("until", lambda: not any(d.isVisible() for d in widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        check(set(n.getScriptName() for n in app.getChildren()) == before, "closing the menu creates no node")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
