"""The Scene Graph panel's Draw State column shows and edits the linked viewer's draw states.

Graph: USDRead of a small stage this script writes to $NATRON_GUI_TEST_OUT/scene_draw_column.usda,
into Viewer1, and a second USDRead, Other, of a copy at scene_draw_column_other.usda that no viewer
shows. Rows: /World holding /World/A (holding /World/A/Deep/Leaf, a Cube), /World/B and /World/C,
each an Xform holding a Cube. No row has a payload, so twirling one loads nothing.
The column is found by its header, "Draw State". The script checks:
  - with nothing set in the viewer, /World reads Full set by the USDRead's Initial Draw, and every
    row below it reads Full, inherited: italic and dimmed;
  - each of the five states, picked in the cell's editor on /World/B, reads back in the cell as
    set, reaches the viewer, and a BBox row is boxed there; Load All, picked after Full, draws
    the same and posts nothing;
  - a row that inherits BBox looks unlike a row set to BBox;
  - with /World boxed, Full on /World/A/Deep/Leaf from the row menu switches /World, /World/A and
    /World/A/Deep to Children, and leaves /World/B and /World/C boxed;
  - collapsing and expanding /World/A changes no state and posts nothing to the viewer;
  - Clear Below on /World keeps its own state and clears the rows under it;
  - Reset to Graph in a cell's editor clears the row and the rows below, and the panel's Reset to
    Graph button clears every row;
  - showing Other, by following the node selection, the column still edits Viewer1.

Screenshots for sign-off: scene-column-default, scene-column-bbox, scene-column-children,
scene-column-full, scene-column-loadAll, scene-column-hidden, scene-column-inherited,
scene-column-deep-full, scene-column-clear-below, scene-column-reset, scene-column-other-node.

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
DEEP = "/World/A/Deep"
LEAF = "/World/A/Deep/Leaf"
B = "/World/B"
C = "/World/C"
ROWS = [WORLD, A, DEEP, LEAF, B, C]
# Script name and the label the column shows, in the editor's order.
STATES = [("bbox", "BBox"), ("children", "Children"), ("full", "Full"), ("loadAll", "Load All"), ("hidden", "Hidden")]

SCENE = """#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Xform "A"
    {
        double3 xformOp:translate = (-3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Xform "Deep"
        {
            def Cube "Leaf"
            {
                double size = 1
            }
        }
    }
    def Xform "B"
    {
        def Cube "Cube"
        {
            double size = 1
        }
    }
    def Xform "C"
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
    from PySide6.QtCore import Q_ARG, QItemSelectionModel, QMetaObject, QModelIndex, Qt, QTimer
    from PySide6.QtGui import QAction
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QComboBox, QToolButton, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    # SceneGraphModel's roles after ePrimPathRole: changed here, payload state, unknown bounds,
    # then the draw state, whether it is inherited and where it comes from.
    DRAW_STATE_ROLE = Qt.UserRole + 5
    INHERITED_ROLE = Qt.UserRole + 6
    ORIGIN_ROLE = Qt.UserRole + 7
    # SceneDrawOriginEnum's graph-set value.
    ORIGIN_GRAPH = 1

    scene_path = os.path.join(OUT, "scene_draw_column.usda")
    with open(scene_path, "w") as f:
        f.write(SCENE)
    other_path = os.path.join(OUT, "scene_draw_column_other.usda")
    with open(other_path, "w") as f:
        f.write(SCENE)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(scene_path)
    other = app.createNode("USDRead")
    require(other is not None, "create the second USDRead")
    other.getParam("initialDraw").setValue(3)
    other.setPosition(300, 0)
    other.getParam("filename").setValue(other_path)
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

    def cell(path):
        index = find(path)
        return index.sibling(index.row(), draw_column()) if index.isValid() else index

    def shown(path):
        """(label, inherited, italic) of path's cell."""
        c = cell(path)
        if not c.isValid():
            return (None, None, None)
        font = c.data(Qt.FontRole)
        return (c.data(Qt.DisplayRole), bool(c.data(INHERITED_ROLE)), bool(font is not None and font.italic()))

    def labels():
        return dict((path, shown(path)[0]) for path in ROWS)

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

    def presented():
        v = view()
        return int(v.property("natronSceneDrawPresented") or 0) if v is not None else 0

    def boxed():
        v = view()
        value = v.property("natronSceneBoxes") if v is not None else None
        return sorted(set(str(list(entry)[0]) for entry in (value or [])))

    def shown_node():
        p = panel()
        return str(p.property("natronSceneGraphNode")) if p is not None else ""

    def reveal(path):
        index = find(path)
        t = tree()
        parent = index.parent()
        while parent.isValid():
            t.expand(parent)
            parent = parent.parent()
        t.scrollTo(index)
        QApplication.processEvents()

    def editor_of(c):
        """The editor open on cell c, or None.

        Found through the view rather than by name: a closed editor lingers as a hidden child until
        its deferred delete runs, and findChild would return it."""
        t = tree()
        w = t.indexWidget(c)
        if w is None:
            return None
        if isinstance(w, QComboBox):
            return w
        for combo in t.findChildren(QComboBox, "sceneGraphDrawStateEditor"):
            if combo.isVisible() and combo.geometry() == w.geometry():
                return combo
        return None

    def pick_in_editor(path, label):
        """Opens path's cell editor and picks label in it. False when no editor or no such entry opened."""
        reveal(path)
        t = tree()
        c = cell(path)
        if not c.isValid():
            return False
        t.edit(c)
        QApplication.processEvents()
        QApplication.processEvents()
        combo = editor_of(c)
        if combo is None:
            return False
        check(combo.view().isVisible(), "the editor of %s opens with its list shown" % path)
        entry = combo.findText(label)
        if entry < 0:
            combo = None
            t.closePersistentEditor(c)
            return False
        combo.hidePopup()
        combo.setCurrentIndex(entry)
        combo.activated.emit(entry)
        combo = None
        QApplication.processEvents()
        check(editor_of(cell(path)) is None, "the editor of %s closes after picking %s" % (path, label))
        return True

    def pick_in_menu(path, action_name):
        """Opens the row menu on path and triggers the action named action_name, wherever it is in it."""
        reveal(path)
        t = tree()
        index = find(path)
        t.clearSelection()
        t.setCurrentIndex(index)
        QApplication.processEvents()
        found = []

        def trigger():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            for action in popup.findChildren(QAction):
                if action.objectName() == action_name:
                    found.append(action_name)
                    if action.isEnabled():
                        action.trigger()
                    break
            popup.close()

        QTimer.singleShot(300, trigger)
        t.customContextMenuRequested.emit(t.visualRect(index).center())
        QApplication.processEvents()
        return bool(found)

    def reached(before):
        """Waits until the viewer posts a change after before and presents it."""
        ok = yield ("until", lambda: posted() > before, POLL_TIMEOUT_S)
        if ok:
            target = posted()
            ok = yield ("until", lambda: presented() >= target, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        return ok

    def save(name):
        shot(name + ".png")

    def cell_image(path):
        t = tree()
        c = cell(path)
        return t.viewport().grab(t.visualRect(c)).toImage()

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
        panel().window().activateWindow()
        ok = yield ("until", lambda: panel() is not None and panel().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the panel's window is active before input")
        for path in ROWS:
            reveal(path)

        section("defaults")
        label, inherited, italic = shown(WORLD)
        check(label == "Full" and not inherited and not italic, "/World reads Full, set by the USDRead's Initial Draw (%r)" % (shown(WORLD),))
        check(int(cell(WORLD).data(ORIGIN_ROLE)) == ORIGIN_GRAPH, "/World's state comes from the graph (%r)" % (cell(WORLD).data(ORIGIN_ROLE),))
        for path in ROWS[1:]:
            label, inherited, italic = shown(path)
            check(label == "Full" and inherited and italic, "%s reads Full, inherited and in italics (%r)" % (path, shown(path)))
        save("scene-column-default")

        section("the five states")
        for name, label in STATES:
            before = posted()
            check(pick_in_editor(B, label), "the editor of /World/B offers %s" % label)
            if name == "loadAll":
                # It follows Full, and with no payload to load both draw alike, so the viewer has
                # nothing to post; the column below reads the state back from the viewer itself.
                yield ("sleep", SETTLE_MS)
                check(posted() == before, "Load All after Full on /World/B posts nothing (%d, was %d)" % (posted(), before))
            else:
                ok = yield from reached(before)
                check(ok, "%s on /World/B reaches the viewer" % label)
            got, inherited, italic = shown(B)
            check(got == label and not inherited and not italic, "/World/B reads %s, set (%r)" % (label, shown(B)))
            check(int(cell(B).data(DRAW_STATE_ROLE)) == STATES.index((name, label)), "the state role of /World/B is %s" % name)
            if name == "bbox":
                ok = yield ("until", lambda: B in boxed(), POLL_TIMEOUT_S)
                check(ok, "the viewer boxes /World/B (%r)" % boxed())
            save("scene-column-" + name)

        section("the inherited look")
        before = posted()
        check(pick_in_editor(B, "BBox"), "BBox on /World/B")
        yield from reached(before)
        before = posted()
        check(pick_in_editor(WORLD, "Children"), "Children on /World")
        yield from reached(before)
        tree().clearSelection()
        tree().selectionModel().setCurrentIndex(find(WORLD), QItemSelectionModel.NoUpdate)
        QApplication.processEvents()
        label, inherited, italic = shown(C)
        check(label == "BBox" and inherited and italic, "/World/C inherits BBox from /World's Children (%r)" % (shown(C),))
        check(shown(B)[0] == "BBox" and not shown(B)[1], "/World/B keeps the BBox set on it (%r)" % (shown(B),))
        check(cell_image(B) != cell_image(C), "an inherited BBox looks unlike a set one")
        save("scene-column-inherited")

        section("Full deep down")
        check(pick_in_editor(B, "Reset to Graph"), "Reset to Graph on /World/B")
        ok = yield ("until", lambda: shown(B)[1], POLL_TIMEOUT_S)
        check(ok, "/World/B inherits again (%r)" % (shown(B),))
        before = posted()
        check(pick_in_editor(WORLD, "BBox"), "BBox on /World")
        yield from reached(before)
        before = posted()
        check(pick_in_menu(LEAF, "sceneGraphDrawState_full"), "the row menu of the leaf offers Full")
        ok = yield from reached(before)
        check(ok, "Full on the leaf reaches the viewer")
        for path in (WORLD, A, DEEP):
            check(shown(path)[0] == "Children" and not shown(path)[1], "%s switches to Children (%r)" % (path, shown(path)))
        check(shown(LEAF)[0] == "Full" and not shown(LEAF)[1], "the leaf reads Full, set (%r)" % (shown(LEAF),))
        for path in (B, C):
            check(shown(path)[0] == "BBox" and shown(path)[1], "%s stays boxed, inherited (%r)" % (path, shown(path)))
        ok = yield ("until", lambda: set([B, C]) <= set(boxed()), POLL_TIMEOUT_S)
        check(ok, "the viewer boxes the siblings (%r)" % boxed())
        save("scene-column-deep-full")

        section("twirling")
        states = labels()
        before = posted()
        tree().collapse(find(A))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        reveal(LEAF)
        yield ("sleep", SETTLE_MS)
        check(labels() == states, "twirling /World/A changes no state (%r, was %r)" % (labels(), states))
        check(posted() == before, "twirling posts nothing to the viewer (%d, was %d)" % (posted(), before))

        section("Clear Below and Reset to Graph")
        before = posted()
        check(pick_in_menu(WORLD, "sceneGraphDrawStateClearBelow"), "the row menu of /World offers Clear Below")
        yield from reached(before)
        check(shown(WORLD)[0] == "Children" and not shown(WORLD)[1], "/World keeps its own state (%r)" % (shown(WORLD),))
        for path in (A, B, C):
            check(shown(path)[0] == "BBox" and shown(path)[1], "%s is cleared and inherits BBox (%r)" % (path, shown(path)))
        save("scene-column-clear-below")
        before = posted()
        check(pick_in_editor(A, "Load All"), "Load All on /World/A")
        yield from reached(before)
        before = posted()
        check(pick_in_editor(WORLD, "Reset to Graph"), "Reset to Graph on /World")
        yield from reached(before)
        check(shown(WORLD)[0] == "Full" and int(cell(WORLD).data(ORIGIN_ROLE)) == ORIGIN_GRAPH,
              "/World is back to the USDRead's Full (%r)" % (shown(WORLD),))
        for path in ROWS[1:]:
            check(shown(path)[0] == "Full" and shown(path)[1], "%s is back to the graph (%r)" % (path, shown(path)))
        before = posted()
        check(pick_in_editor(C, "Hidden"), "Hidden on /World/C")
        yield from reached(before)
        button = panel().findChild(QToolButton, "sceneGraphResetDrawStates")
        require(button is not None and button.isEnabled(), "the panel has a Reset to Graph button")
        before = posted()
        QTest.mouseClick(button, Qt.LeftButton)
        button = None
        yield from reached(before)
        check(shown(C)[0] == "Full" and shown(C)[1], "the panel's Reset to Graph clears every row (%r)" % (shown(C),))
        save("scene-column-reset")

        section("another node shown")
        chooser = panel().findChild(QComboBox, "sceneGraphFollow")
        require(chooser is not None, "the panel has a Follow chooser")
        chooser.setCurrentIndex(1)
        chooser = None
        app.selectNode(other, True)
        ok = yield ("until", lambda: shown_node() == other.getScriptName() and len(rows()) == 1, POLL_TIMEOUT_S)
        require(ok, "the panel shows Other (%r)" % shown_node())
        reveal(B)
        before = posted()
        check(pick_in_editor(B, "BBox"), "BBox on /World/B with Other shown")
        ok = yield from reached(before)
        check(ok, "the edit reaches Viewer1")
        ok = yield ("until", lambda: B in boxed(), POLL_TIMEOUT_S)
        check(ok, "Viewer1 boxes /World/B (%r)" % boxed())
        check(shown(B)[0] == "BBox" and not shown(B)[1], "the column shows Viewer1's state (%r)" % (shown(B),))
        save("scene-column-other-node")
        report("NOTE the panel showed %s while Viewer1 drew %r boxed" % (shown_node(), boxed()))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
