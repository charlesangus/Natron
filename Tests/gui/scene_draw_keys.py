"""The 3D viewer's draw state keys act on the picked box or the selection, and can be rebound.

Graph: USDRead of a small stage this script writes to $NATRON_GUI_TEST_OUT/scene_draw_keys.usda,
into Viewer1, seen from the front (Z, then A to frame). /World holds /World/Group, at x = -3,
holding /World/Group/Inner holding the Cube /World/Group/Inner/Leaf, and /World/B, an Xform at
x = 3 holding a Cube. States are read from the Scene Graph panel's Draw State column, docked
beside the viewer. Keys are real key events sent to the focused SceneViewport. The script checks:
  - with /World/Group boxed, a click on its box picks it and Down sets Children on it;
  - with the leaf selected in the tree, Down opens the box drawn for it, /World/Group/Inner;
  - Shift+Down sets Full on the leaf, now boxed itself;
  - Up boxes the leaf's parent, and Up again boxes the row above the new box;
  - Ctrl+Shift+Down sets Load All on the picked /World/Group;
  - with Draw Children rebound to J in Preferences, Down does nothing and J sets Children.

Screenshots for sign-off: scene-keys-boxed, scene-keys-children, scene-keys-inner-children,
scene-keys-leaf-full, scene-keys-up, scene-keys-up-again, scene-keys-load-all,
scene-keys-shortcut-editor, scene-keys-rebound.

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
GROUP = "/World/Group"
INNER = "/World/Group/Inner"
LEAF = "/World/Group/Inner/Leaf"
B = "/World/B"

SCENE = """#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Xform "Group"
    {
        double3 xformOp:translate = (-3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Xform "Inner"
        {
            def Cube "Leaf"
            {
                double size = 1
            }
        }
    }
    def Xform "B"
    {
        double3 xformOp:translate = (3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
}
"""

try:
    from PySide6.QtCore import Q_ARG, QEvent, QItemSelectionModel, QMetaObject, QModelIndex, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPushButton, QTreeView, QTreeWidget

    from guitest import (OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(OUT, "scene_draw_keys.usda")
    with open(scene_path, "w") as f:
        f.write(SCENE)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if not views:
            return None
        v = hold(views[0])
        if not v.property("natronSceneRecordBoxes"):
            v.setProperty("natronSceneRecordBoxes", True)
            v.update()
        return v

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

    def state(path):
        """What the Draw State column shows for path, and whether it is set on the row."""
        index = find(path)
        if not index.isValid():
            return (None, None)
        c = index.sibling(index.row(), draw_column())
        return (c.data(Qt.DisplayRole), not bool(c.data(Qt.UserRole + 6)))

    def is_set(path, label):
        return lambda: state(path) == (label, True)

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def scene_error():
        v = view()
        return str(v.property("natronSceneError") or "") if v is not None else ""

    def posted():
        v = view()
        stats = v.property("natronSceneDrawStats") if v is not None else None
        return int(dict(stats).get("posted", 0)) if stats else 0

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def drawn_boxes():
        """Path to the centre of its box or marker, in widget pixels."""
        v = view()
        value = v.property("natronSceneBoxes") if v is not None else None
        result = {}
        for entry in (value or []):
            path, kind, lit, x, y, left, top, right, bottom = list(entry)
            result[str(path)] = QPoint(int(x), int(y))
        return result

    def activate():
        view().activateWindow()
        ok = yield ("until", lambda: view() is not None and view().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the scene view's window is active before input")
        view().setFocus()
        QApplication.processEvents()

    def key(k, modifiers=Qt.NoModifier):
        yield from activate()
        check(view().hasFocus(), "the scene view has the focus")
        QTest.keyClick(view(), k, modifiers)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def pick(path):
        """Clicks path's box and waits for it to be the selection."""
        ok = yield ("until", lambda: path in drawn_boxes(), POLL_TIMEOUT_S)
        check(ok, "%s is drawn as a box (%r)" % (path, sorted(drawn_boxes())))
        if not ok:
            return False
        yield from activate()
        v = view()
        centre = drawn_boxes()[path]
        mouse(v, QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        v = None
        QApplication.processEvents()
        ok = yield ("until", lambda: selection() == [path], POLL_TIMEOUT_S)
        check(ok, "a click on the box of %s picks it (%r)" % (path, selection()))
        return ok

    def select_in_tree(path):
        index = find(path)
        check(index.isValid(), "the panel lists %s" % path)
        if not index.isValid():
            return
        t = tree()
        parent = index.parent()
        while parent.isValid():
            t.expand(parent)
            parent = parent.parent()
        t.selectionModel().select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        t.scrollTo(index)
        QApplication.processEvents()

    def expect(label, path, shown):
        ok = yield ("until", is_set(path, shown), POLL_TIMEOUT_S)
        check(ok, "%s sets %s on %s (%r)" % (label, shown, path, state(path)))
        yield ("sleep", SETTLE_MS)

    def set_state(path, name):
        require(QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", path), Q_ARG("QString", name)),
                "the scene view has setDrawState")
        QApplication.processEvents()

    def find_item(tree, text, parent_text=None):
        # Model indexes, not QTreeWidgetItem wrappers: the Preferences trees' item wrappers
        # report their C++ object deleted by the time they are used (see m45_tab_menu.py).
        m = tree.model()
        for idx in m.match(m.index(0, 0), Qt.DisplayRole, text, -1, Qt.MatchExactly | Qt.MatchRecursive):
            parent = idx.parent()
            if parent_text is None or (parent.isValid() and parent.data() == parent_text):
                return idx
        return None

    def open_preferences():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is None or "edit" not in act.text().lower():
                    continue
                for a in menu.actions():
                    if "preferences" in a.text().lower():
                        a.trigger()
                        QApplication.processEvents()
                        panels = [w for w in widgets_of("PreferencesPanel") if w.isVisible()]
                        return panels[0] if len(panels) == 1 else None
        return None

    def rebind_draw_children():
        """Binds Draw Children to J in the Shortcut Editor; False when a step of it is missing."""
        prefs = open_preferences()
        require(prefs is not None, "the Preferences panel opens")
        page_trees = [w for w in prefs.findChildren(QTreeWidget) if w.columnCount() == 1]
        page = find_item(page_trees[0], "Shortcut Editor") if len(page_trees) == 1 else None
        require(page is not None, "found the Shortcut Editor page")
        page_trees[0].setCurrentIndex(page)
        QApplication.processEvents()
        shortcut_trees = [w for w in prefs.findChildren(QTreeWidget)
                          if w.columnCount() == 3 and w.headerItem() is not None and w.headerItem().text(0) == "Command"]
        require(len(shortcut_trees) == 1, "found the shortcuts tree")
        keys = shortcut_trees[0]
        group = find_item(keys, "3DViewer")
        require(group is not None, "the shortcuts tree has a 3DViewer group")
        keys.expand(group)
        for label in ("Draw Children", "Draw Full", "Load All", "Box Parent"):
            check(find_item(keys, label, "3DViewer") is not None, "the 3DViewer group lists %s" % label)
        row = find_item(keys, "Draw Children", "3DViewer")
        require(row is not None, "the 3DViewer group has a Draw Children row")
        keys.setCurrentIndex(row)
        keys.scrollTo(row)
        QApplication.processEvents()
        report("NOTE Draw Children is bound to %r" % (row.siblingAtColumn(1).data(),))
        check("Down" in str(row.siblingAtColumn(1).data()), "Draw Children is on Down by default")
        shot("scene-keys-shortcut-editor.png")
        editors = [w for w in prefs.findChildren(QLineEdit) if w.placeholderText() == "Type to set shortcut" and w.isVisible()]
        validate = [w for w in prefs.findChildren(QPushButton) if w.text() == "Validate" and w.isVisible()]
        require(len(editors) == 1 and len(validate) == 1, "found the Draw Children field and its Validate button")
        editors[0].clear()
        QTest.keyClick(editors[0], Qt.Key_J)
        QApplication.processEvents()
        check("J" in editors[0].text(), "the field records J (%r)" % editors[0].text())
        validate[0].click()
        QApplication.processEvents()
        row = find_item(keys, "Draw Children", "3DViewer")
        check(row is not None and "J" in str(row.siblingAtColumn(1).data()), "the Draw Children row shows J (%r)"
              % (row.siblingAtColumn(1).data() if row is not None else None,))
        prefs.close()
        QApplication.processEvents()

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the stage", scene_error())
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and len(rows()) == 1, POLL_TIMEOUT_S)
        require(ok, "the panel lists /World")
        require(draw_column() >= 0, "the tree has a Draw State column")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        yield from key(Qt.Key_Z)
        yield from key(Qt.Key_A)

        section("down on the picked box")
        set_state(GROUP, "bbox")
        yield from expect("the script", GROUP, "BBox")
        shot("scene-keys-boxed.png")
        yield from pick(GROUP)
        yield from key(Qt.Key_Down)
        yield from expect("Down", GROUP, "Children")
        ok = yield ("until", lambda: INNER in drawn_boxes(), POLL_TIMEOUT_S)
        check(ok, "the viewer boxes /World/Group/Inner (%r)" % sorted(drawn_boxes()))
        shot("scene-keys-children.png")

        section("down on the box drawn for the selection")
        select_in_tree(LEAF)
        ok = yield ("until", lambda: selection() == [LEAF], POLL_TIMEOUT_S)
        check(ok, "selecting the leaf in the tree selects it in the viewer (%r)" % selection())
        yield from key(Qt.Key_Down)
        yield from expect("Down with the leaf selected", INNER, "Children")
        check(state(LEAF) == ("BBox", False), "the leaf inherits BBox (%r)" % (state(LEAF),))
        shot("scene-keys-inner-children.png")
        yield from key(Qt.Key_Down, Qt.ShiftModifier)
        yield from expect("Shift+Down", LEAF, "Full")
        shot("scene-keys-leaf-full.png")

        section("up")
        yield from key(Qt.Key_Up)
        yield from expect("Up with the leaf selected", INNER, "BBox")
        shot("scene-keys-up.png")
        yield from key(Qt.Key_Up)
        yield from expect("Up again", GROUP, "BBox")
        check(selection() == [LEAF], "the keys keep the selection (%r)" % selection())
        shot("scene-keys-up-again.png")

        section("load all")
        yield from pick(GROUP)
        yield from key(Qt.Key_Down, Qt.ControlModifier | Qt.ShiftModifier)
        yield from expect("Ctrl+Shift+Down", GROUP, "Load All")
        shot("scene-keys-load-all.png")

        section("rebound")
        set_state(GROUP, "bbox")
        yield from expect("the script", GROUP, "BBox")
        rebind_draw_children()
        yield ("sleep", SETTLE_MS)
        yield from pick(GROUP)
        before = posted()
        yield from key(Qt.Key_Down)
        check(state(GROUP) == ("BBox", True) and posted() == before, "after the rebind Down leaves /World/Group boxed (%r)"
              % (state(GROUP),))
        yield from key(Qt.Key_J)
        yield from expect("J", GROUP, "Children")
        shot("scene-keys-rebound.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
