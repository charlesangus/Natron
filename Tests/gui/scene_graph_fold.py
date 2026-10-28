"""Folding prims to their bounding boxes from the Scene Graph panel.

Graph: USDRead(scene-reference.usda) -> Viewer1, plus a second USDRead of the same file. The panel
is docked beside the 3D viewer.

Checks:
  - selecting /World/Box's row and pressing Fold folds it: natronSceneGraphFolded lists it and the
    viewer redraws (screenshot of the box drawn as its bounding box);
  - Unfold takes the fold away;
  - with the Viewer 3D Prim Budget preference at 1, viewing the second reader for the first time
    folds its root prims;
  - folds are per viewed node: back on the first reader, its own folds (none) come back, and on the
    second reader again, the budget's fold is still there.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 600
WORLD = "/World"
BOX = "/World/Box"

try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt
    from PySide6.QtWidgets import QToolButton, QTreeView

    from guitest import FIXTURES, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    def reader(x):
        node = app.createNode("USDRead")
        require(node is not None, "create USDRead", plugin_problem("USDRead"))
        node.getParam("filename").setValue(scene_path)
        node.setPosition(x, 0)
        return node

    usd = reader(0)
    other = reader(200)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 150)

    _held = []

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

    def child(cls, name):
        p = panel()
        try:
            return hold(p.findChild(cls, name)) if p is not None else None
        except RuntimeError:
            return None

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def model():
        t = tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def rows(parent=QModelIndex()):
        """parent's child rows; none while the tree is being rebuilt."""
        m = model()
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

    def find(path):
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

    def top_paths():
        return [path_of(i) for i in rows()]

    def folded():
        p = panel()
        value = p.property("natronSceneGraphFolded") if p is not None else None
        return sorted(str(x) for x in value) if value else []

    def shown_node():
        p = panel()
        return str(p.property("natronSceneGraphNode")) if p is not None else ""

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def view_input(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the first USDRead")
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
        ok = yield ("until", lambda: panel().isVisible() and WORLD in top_paths(), POLL_TIMEOUT_S)
        require(ok, "the docked panel lists /World (got %r)" % (top_paths(),))
        check(folded() == [], "nothing is folded under the default budget (got %r)" % (folded(),))

        fold = child(QToolButton, "sceneGraphFold")
        unfold = child(QToolButton, "sceneGraphUnfold")
        require(fold is not None and unfold is not None, "the panel has Fold and Unfold buttons")
        require(fold.isEnabled(), "Fold is enabled while following a viewer with a scene")

        tree().expand(find(WORLD))
        box = find(BOX)
        require(box.isValid(), "the tree has /World/Box")
        tree().selectionModel().select(box, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        tree().selectionModel().setCurrentIndex(box, QItemSelectionModel.NoUpdate)
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-fold-before.png")
        fold.click()
        ok = yield ("until", lambda: folded() == [BOX], POLL_TIMEOUT_S)
        check(ok, "Fold folds the selected box (got %r)" % (folded(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-fold-box-bounds.png")

        unfold.click()
        ok = yield ("until", lambda: folded() == [], POLL_TIMEOUT_S)
        check(ok, "Unfold takes the fold away (got %r)" % (folded(),))

        budget = NatronEngine.natron.getSettings().getParam("viewer3DPrimBudget")
        require(budget is not None, "Settings has viewer3DPrimBudget")
        report("NOTE default viewer3DPrimBudget %r" % (budget.getValue(),))
        check(budget.getValue() == 50000, "the budget defaults to 50000")
        budget.setValue(1)

        require(view_input(other), "view the second USDRead")
        ok = yield ("until", lambda: shown_node() == other.getScriptName() and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the panel follows the viewer to the second reader (got %r)" % (shown_node(),))
        ok = yield ("until", lambda: folded() != [] and folded() == sorted(top_paths()), POLL_TIMEOUT_S)
        check(ok, "over the budget, a newly viewed scene has its root prims folded (got %r, roots %r)"
              % (folded(), top_paths()))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-fold-over-budget.png")

        require(view_input(usd), "view the first USDRead again")
        ok = yield ("until", lambda: shown_node() == usd.getScriptName() and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the panel follows the viewer back")
        yield ("sleep", SETTLE_MS)
        check(folded() == [], "the first reader keeps its own folds, none (got %r)" % (folded(),))

        require(view_input(other), "view the second USDRead again")
        ok = yield ("until", lambda: shown_node() == other.getScriptName() and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the panel follows the viewer to the second reader again")
        ok = yield ("until", lambda: folded() == sorted(top_paths()) and folded() != [], POLL_TIMEOUT_S)
        check(ok, "the second reader's fold is remembered (got %r)" % (folded(),))
        budget.setValue(50000)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
