"""The Scene Graph panel's Changed Here marker and filter.

Graph: a USDRead of a scene the script writes to $NATRON_GUI_TEST_OUT, into a USDEditTransform on
one prim of it, into Viewer1.

Checks:
  - viewing the USDEditTransform marks its target and the target's ancestors, and no other row;
  - the Changed Here button hides every row that is not marked, and shows them again when off;
  - viewing the USDRead, which authors nothing, marks no row, and the filter then hides them all.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off. Run through
Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 120.0
SETTLE_MS = 400

ROOT = "/Chg"
TARGET = ROOT + "/A/B"
MARKED = {ROOT, ROOT + "/A", TARGET}
UNMARKED = [ROOT + "/A/D", ROOT + "/C"]


def write_scene(path):
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Chg"\n)\n\n')
        f.write('def Xform "Chg"\n{\n')
        f.write('    def Xform "A"\n    {\n        def Xform "B"\n        {\n        }\n\n'
                '        def Xform "D"\n        {\n        }\n    }\n\n')
        f.write('    def Xform "C"\n    {\n    }\n}\n')


try:
    import NatronEngine
    from PySide6.QtCore import Qt, QModelIndex
    from PySide6.QtWidgets import QToolButton, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    CHANGED_HERE_ROLE = Qt.UserRole + 2

    os.makedirs(OUT, exist_ok=True)
    scene_path = os.path.join(OUT, "scene-changed-here.usda")
    write_scene(scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    edit = app.createNode("USDEditTransform")
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    edit.setPosition(0, 100)
    viewer.setPosition(0, 200)
    require(edit.connectInput(0, usd), "connect the USDEditTransform to the USDRead")
    edit.getParam("primPattern").setValue(TARGET)
    edit.getParam("translate").set(1.0, 2.0, 3.0)

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

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        try:
            while m.canFetchMore(parent):
                m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def all_rows(parent=QModelIndex()):
        result = []
        for index in rows(parent):
            result.append(index)
            result.extend(all_rows(index))
        return result

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def top_paths():
        return [path_of(i) for i in rows()]

    def marked_paths():
        return {path_of(i) for i in all_rows() if index_marked(i)}

    def index_marked(index):
        return bool(index.data(CHANGED_HERE_ROLE))

    def hidden(path):
        for index in all_rows():
            if path_of(index) == path:
                return tree().isRowHidden(index.row(), index.parent())
        return None

    def prop(name):
        return panel().property("natronSceneGraph" + name)

    def settled_on(node, count):
        return (prop("Node") == node.getScriptName() and not prop("ChangedHereBusy")
                and prop("ChangedHereCount") == count)

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
        require(viewer.connectInput(0, edit), "connect the Viewer to the USDEditTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", lambda: view() is not None, POLL_TIMEOUT_S)
        require(ok, "the scene view is up")
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        button = child(QToolButton, "sceneGraphChangedHere")
        require(button is not None, "the panel has its Changed Here button")

        ok = yield ("until", lambda: panel().isVisible() and ROOT in top_paths() and settled_on(edit, len(MARKED)), LOAD_TIMEOUT_S)
        require(ok, "the panel marks %d rows for the USDEditTransform (got %r, count %r)" % (len(MARKED), top_paths(), prop("ChangedHereCount")))
        marked = marked_paths()
        check(marked == MARKED, "the target and its ancestors are marked, and no other row (got %r)" % (sorted(marked),))
        check(not any(hidden(p) for p in all_paths()), "no row is hidden while the button is off")
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-marked.png")

        button.click()
        ok = yield ("until", lambda: prop("ChangedHereOnly") and all(hidden(p) for p in UNMARKED), POLL_TIMEOUT_S)
        check(ok, "the button hides the rows that are not marked (hidden: %r)" % ({p: hidden(p) for p in UNMARKED},))
        check(all(hidden(p) is False for p in MARKED), "the button keeps the marked rows (hidden: %r)" % ({p: hidden(p) for p in MARKED},))
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-filtered.png")

        button.click()
        ok = yield ("until", lambda: not prop("ChangedHereOnly") and not any(hidden(p) for p in all_paths()), POLL_TIMEOUT_S)
        check(ok, "switching the button off shows every row again")

        button.click()
        ok = yield ("until", lambda: prop("ChangedHereOnly") and all(hidden(p) for p in UNMARKED), POLL_TIMEOUT_S)
        require(ok, "the button hides the unmarked rows again")
        require(view_input(usd), "switch the Viewer to the USDRead")
        ok = yield ("until", lambda: ROOT in top_paths() and settled_on(usd, 0), LOAD_TIMEOUT_S)
        require(ok, "the panel follows the USDRead (node %r, count %r)" % (prop("Node"), prop("ChangedHereCount")))
        check(not marked_paths(), "a node that authors nothing marks no row (got %r)" % (sorted(marked_paths()),))
        check(all(hidden(p) for p in all_paths()), "the filter hides every row of a node that authors nothing")
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-none.png")

        button.click()
        ok = yield ("until", lambda: not prop("ChangedHereOnly") and not any(hidden(p) for p in all_paths()), POLL_TIMEOUT_S)
        check(ok, "switching the button off shows every row of the USDRead's scene")

    def all_paths():
        return sorted(MARKED | set(UNMARKED))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
