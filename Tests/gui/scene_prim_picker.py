"""The prim picker button on prim path and pattern knobs.

Graph: USDRead(scene-reference.usda) -> USDEditTransform -> Viewer1, with the USDEditTransform's
settings panel open. The picker button beside its Prim Pattern field opens a popup tree over the
node's input scene; rows are picked through the popup tree's model with QModelIndex alone.

Checks:
  - the Prim Pattern field has a picker button, and clicking it opens the popup;
  - the popup tree lists Box, Ball and Cam under /World, and allows several rows;
  - opening on "/World/Box /World/Ball" shows both checked, and on "/World/*" shows the three prims checked;
  - OK with nothing changed leaves a "/World/*" expression's text as it was, and an OK after unchecking
    one prim writes the remaining literal paths as one undo step;
  - a path the input lacks is listed as "not in input", checked and kept unless unchecked;
  - Search Unloaded on a payload scene lists a match inside an unloaded payload, marked unloaded,
    without loading any payload on the viewer's stage;
  - picking Box and Ball and pressing OK sets the pattern to "/World/Box /World/Ball";
  - one Undo restores the previous pattern;
  - picking Cam, Box and Ball, against tree order, sets the pattern in that order;
  - Cancel changes nothing;
  - with Cam selected in the 3D view (through the Scene Graph panel), "Use Viewer Selection"
    sets the pattern to "/World/Cam";
  - with Box then Ball selected in the 3D view, "Use Viewer Selection" on a USDXformPrim's Prim Path
    writes the first prim, "/World/Box";
  - a USDAssignMaterial's panel has a picker button on both its Prim Pattern and Material Path fields.

Shots: scene-prim-picker-search-unloaded.png (after Search Unloaded), scene-prim-picker-popup.png (the popup with Box and Ball picked) and
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

STATE_UNLOADED = 1
UNLOADED_GEO = "/World/Block0/B0/Floor/Geo"
BLOCK0_B0 = "/World/Block0/B0"
CITY_WORLD_CHILDREN = ["/World/Block0", "/World/Plain"]


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


try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt
    from PySide6.QtWidgets import (QAbstractItemView, QApplication, QDialog, QDialogButtonBox, QLabel, QListWidget,
                                   QMainWindow, QPushButton, QTreeView)

    from guitest import (FIXTURES, OUT, any_panel_open, check, class_name, close_all_panels, finish, menu_of,
                         plugin_problem, require, run, shot, viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    edit = app.createNode("USDEditTransform", -1, None, props)
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    pattern = edit.getParam("primPattern")
    require(pattern is not None, "USDEditTransform has a primPattern parameter")
    pattern.setValue(MARKER)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
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
                return hold(w)
        return None

    def popup_tree():
        p = popup()
        try:
            return hold(p.findChild(QTreeView, "scenePrimPickerTree")) if p is not None else None
        except RuntimeError:
            return None

    def popup_status():
        p = popup()
        try:
            label = hold(p.findChild(QLabel, "scenePrimPickerStatus")) if p is not None else None
            return str(label.text()) if label is not None else None
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

    def picker_buttons():
        found = []
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QPushButton) and w.objectName() == "knobPrimPicker" and w.isVisible():
                    found.append(w)
            except RuntimeError:
                continue
        return found

    def popup_button(which):
        box = hold(popup().findChild(QDialogButtonBox, "scenePrimPickerButtons"))
        return hold(box.button(which)) if box is not None else None

    def use_viewer_button():
        return hold(popup().findChild(QPushButton, "scenePrimPickerUseViewer"))

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

    def extras():
        """The popup's list of paths apart from the tree: (path, text, checked) per row."""
        p = popup()
        try:
            lst = hold(p.findChild(QListWidget, "scenePrimPickerExtras")) if p is not None else None
            if lst is None:
                return []
            return [(str(lst.item(i).data(Qt.UserRole)), str(lst.item(i).text()), lst.item(i).checkState() == Qt.Checked)
                    for i in range(lst.count())]
        except RuntimeError:
            return []

    def set_extra_checked(path, checked):
        lst = hold(popup().findChild(QListWidget, "scenePrimPickerExtras"))
        for i in range(lst.count()):
            if str(lst.item(i).data(Qt.UserRole)) == path:
                lst.item(i).setCheckState(Qt.Checked if checked else Qt.Unchecked)
        QApplication.processEvents()

    def tree_selection(tree):
        return [path_of(i) for i in tree.selectionModel().selectedRows(0)]

    def find_path(tree, path):
        """The row of path in tree, walking down from the root without expanding anything."""
        index = QModelIndex()
        walked = ""
        for part in path.strip("/").split("/"):
            walked += "/" + part
            index = row_of(tree, walked, index)
            if not index.isValid():
                return QModelIndex()
        return index

    def search_button():
        return hold(popup().findChild(QPushButton, "scenePrimPickerSearchUnloaded"))

    def open_popup(world_children_expected=(BALL, BOX, CAM)):
        """Opens the picker and waits until it lists world_children_expected under /World."""
        expected = sorted(world_children_expected)
        button = picker_button()
        require(button is not None, "the Prim Pattern field has a picker button")
        check(button.isEnabled(), "the picker button is enabled")
        button.click()
        ok = yield ("until", lambda: popup() is not None and bool(popup().property("natronPickerSynced")), POLL_TIMEOUT_S)
        require(ok, "the picker opens and reads the input scene")
        ok = yield ("until", lambda: bool(popup().property("natronPickerMatchesRead")), POLL_TIMEOUT_S)
        require(ok, "the picker evaluates the knob's text (status %r)" % (popup_status(),))
        ok = yield ("until", lambda: world_children(popup_tree()) == expected, POLL_TIMEOUT_S)
        require(ok, "the picker lists %s under /World (got %r, status %r)"
                % (", ".join(p.rsplit("/", 1)[1] for p in expected), world_children(popup_tree()), popup_status()))

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

        # The knob's matches are checked on opening.
        pattern.setValue(BOX + " " + BALL)
        yield from open_popup()
        check(sorted(tree_selection(popup_tree())) == sorted([BOX, BALL]),
              "opening on '/World/Box /World/Ball' shows both checked (got %r)" % (tree_selection(popup_tree()),))
        check(extras() == [], "no path is listed apart from the tree (got %r)" % (extras(),))
        popup_button(QDialogButtonBox.Cancel).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "Cancel closes the picker")

        # An expression the user leaves alone stays an expression.
        pattern.setValue(WORLD + "/*")
        yield from open_popup()
        check(sorted(tree_selection(popup_tree())) == sorted([BOX, BALL, CAM]),
              "opening on '/World/*' shows its three matches checked (got %r)" % (tree_selection(popup_tree()),))
        popup_button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the picker")
        check(str(pattern.getValue()) == WORLD + "/*",
              "OK with nothing changed leaves the expression's text intact (got %r)" % (pattern.getValue(),))

        # Changing an expression's picks writes literal paths.
        yield from open_popup()
        tree = popup_tree()
        tree.selectionModel().select(find_path(tree, CAM), QItemSelectionModel.Deselect | QItemSelectionModel.Rows)
        QApplication.processEvents()
        popup_button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the picker")
        check(str(pattern.getValue()) == BOX + " " + BALL,
              "OK after unchecking Cam writes the remaining literal paths (got %r)" % (pattern.getValue(),))
        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()
        check(str(pattern.getValue()) == WORLD + "/*", "one Undo restores the expression (got %r)" % (pattern.getValue(),))

        # A path the input lacks is listed, not dropped.
        pattern.setValue(MARKER)
        yield from open_popup()
        listed = extras()
        check(len(listed) == 1 and listed[0][0] == MARKER and listed[0][2] and "not in input" in listed[0][1],
              "a path the input lacks is listed as 'not in input', checked (got %r)" % (listed,))
        check(tree_selection(popup_tree()) == [], "the missing path selects no tree row")
        check(popup_button(QDialogButtonBox.Ok).isEnabled(), "OK is enabled while the missing path is checked")
        set_extra_checked(MARKER, False)
        check(not popup_button(QDialogButtonBox.Ok).isEnabled(), "OK is disabled once nothing is checked")

        # Pick two prims and confirm.
        check(popup_tree().selectionMode() == QAbstractItemView.ExtendedSelection,
              "a pattern knob's picker takes several prims")
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

        # Click order, not tree order; the checked missing path stays.
        yield from open_popup()
        select(popup_tree(), [CAM, BOX, BALL])
        popup_button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the picker")
        picked = str(pattern.getValue())
        check(picked == " ".join([CAM, BOX, BALL, MARKER]),
              "the pattern lists the prims in the order they were picked, not tree order (got %r)" % (picked,))
        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()

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

        # A path knob holds one prim.
        close_all_panels()
        ok = yield ("until", lambda: not any_panel_open(), POLL_TIMEOUT_S)
        require(ok, "the USDEditTransform's panel closes")
        xform = app.createNode("USDXformPrim", -1, None, props)
        require(xform is not None, "create USDXformPrim", plugin_problem("USDXformPrim"))
        xform.setPosition(150, 100)
        require(xform.connectInput(0, edit), "connect the USDXformPrim to the USDEditTransform")
        prim_path = xform.getParam("primPath")
        require(prim_path is not None, "USDXformPrim has a primPath parameter")
        ok = yield ("until", lambda: picker_button() is not None, POLL_TIMEOUT_S)
        require(ok, "the USDXformPrim's panel shows a picker button")
        select(panel_tree(), [BOX, BALL])
        ok = yield ("until", lambda: viewer_selection() == sorted([BOX, BALL]), POLL_TIMEOUT_S)
        require(ok, "Box and Ball are selected in the 3D view (got %r)" % (viewer_selection(),))
        yield from open_popup()
        require(use_viewer_button() is not None, "the path knob's picker has a Use Viewer Selection button")
        use_viewer_button().click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "Use Viewer Selection closes the path knob's picker")
        check(str(prim_path.getValue()) == BOX,
              "Use Viewer Selection writes the first prim to a path knob (got %r)" % (prim_path.getValue(),))

        # A material path knob takes the picker too.
        close_all_panels()
        ok = yield ("until", lambda: not any_panel_open(), POLL_TIMEOUT_S)
        require(ok, "the USDXformPrim's panel closes")
        assign = app.createNode("USDAssignMaterial", -1, None, props)
        require(assign is not None, "create USDAssignMaterial", plugin_problem("USDAssignMaterial"))
        assign.setPosition(300, 100)
        require(assign.connectInput(0, edit), "connect the USDAssignMaterial to the USDEditTransform")
        ok = yield ("until", lambda: len(picker_buttons()) >= 2, POLL_TIMEOUT_S)
        check(ok, "the USDAssignMaterial's panel shows a picker button on both prim fields (got %d)" % (len(picker_buttons()),))
        check(len(picker_buttons()) == 2,
              "exactly Prim Pattern and Material Path have a picker button (got %d)" % (len(picker_buttons()),))

        # Search Unloaded reaches into a payload the viewer has not loaded.
        close_all_panels()
        ok = yield ("until", lambda: not any_panel_open(), POLL_TIMEOUT_S)
        require(ok, "the USDAssignMaterial's panel closes")
        city = write_city(os.path.join(OUT, "scene_prim_picker_city"))
        usd2 = app.createNode("USDRead")
        require(usd2 is not None, "create the second USDRead", plugin_problem("USDRead"))
        usd2.getParam("filename").setValue(city)
        usd2.setPosition(450, 0)
        edit2 = app.createNode("USDEditTransform", -1, None, props)
        require(edit2 is not None, "create the second USDEditTransform", plugin_problem("USDEditTransform"))
        edit2.setPosition(450, 100)
        edit2.getParam("primPattern").setValue(WORLD + "//Geo")
        require(edit2.connectInput(0, usd2), "connect the second USDEditTransform to its USDRead")
        require(connect_viewer(edit2), "connect the Viewer to the payload scene")
        ok = yield ("until", lambda: panel_tree() is not None and find_path(panel_tree(), BLOCK0_B0).isValid(), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists the payload scene")
        b0 = find_path(panel_tree(), BLOCK0_B0)
        check(int(b0.data(Qt.UserRole + 3)) == STATE_UNLOADED, "the viewer lists B0 with its payload unloaded")
        edits_before = view().property("natronSceneLoadEdits")
        ok = yield ("until", lambda: picker_button() is not None, POLL_TIMEOUT_S)
        require(ok, "the second USDEditTransform's panel shows a picker button")
        yield from open_popup(CITY_WORLD_CHILDREN)
        check(extras() == [], "nothing unloaded is listed before Search Unloaded (got %r)" % (extras(),))
        check(search_button().isEnabled(), "Search Unloaded is enabled for a valid expression")
        search_button().click()
        ok = yield ("until", lambda: bool(popup().property("natronPickerSearchDone")), POLL_TIMEOUT_S)
        require(ok, "Search Unloaded ends (status %r)" % (popup_status(),))
        listed = extras()
        check(len(listed) == 1 and listed[0][0] == UNLOADED_GEO and listed[0][2] and "unloaded" in listed[0][1],
              "Search Unloaded lists the match inside the unloaded payload, checked and marked (got %r)" % (listed,))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-picker-search-unloaded.png")
        popup_button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the picker")
        check(str(edit2.getParam("primPattern").getValue()) == WORLD + "//Geo",
              "OK with only the search's matches checked leaves the expression intact")
        b0 = find_path(panel_tree(), BLOCK0_B0)
        check(int(b0.data(Qt.UserRole + 3)) == STATE_UNLOADED, "the viewer's B0 payload is still unloaded")
        check(view().property("natronSceneLoadEdits") == edits_before,
              "the search made no load edit on the viewer's stage (%r -> %r)"
              % (edits_before, view().property("natronSceneLoadEdits")))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
