"""The Scene Graph panel docked beside the 3D viewer: its tree, selection both ways, the name
filter, pinning, and the attribute list.

Graph: USDRead(scene-reference.usda) into Viewer1, and a USDCamera off to the side. The panel
(script name "sceneGraph") is moved into a pane split off the viewer's, so it sits beside the
scene view. Its widgets are found by object name; the tree is read through its model with
QModelIndex alone.

Checks:
  - the tree lists /World with Box, Ball and Cam under it, with their types;
  - selecting the box's row selects /World/Box in the viewer, and a click on the ball in the
    viewer selects the ball's row;
  - the filter lists the prims whose names match, and clearing it brings the tree back;
  - the attribute list shows the current prim's attributes, with the node that authored them;
  - pinned to the USDRead, the tree stays on its scene when the viewer is switched to the
    camera; deleting the USDRead unpins the panel, which then shows the camera's scene.

Screenshots of the docked panel go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for
sign-off. Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
GRID_COLUMNS = 24
GRID_ROWS = 14
WORLD = "/World"
BOX = "/World/Box"
BALL = "/World/Ball"
CAM = "/World/Cam"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QModelIndex, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QToolButton, QTreeView

    from guitest import FIXTURES, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    require(usd.getParam("filename") is not None, "USDRead has a filename parameter")
    usd.getParam("filename").setValue(scene_path)
    camera = app.createNode("USDCamera")
    require(camera is not None, "create USDCamera", plugin_problem("USDCamera"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    camera.setPosition(200, 0)
    viewer.setPosition(0, 150)
    camera_root = "/" + str(camera.getParam("primPath").getValue()).strip("/").split("/")[0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return panels[0] if panels else None

    def child(cls, name):
        p = panel()
        return p.findChild(cls, name) if p is not None else None

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def model():
        t = tree()
        return t.model() if t is not None else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def viewer_selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

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

    def top_paths():
        return [path_of(i) for i in rows()]

    def selected_rows():
        t = tree()
        return sorted(path_of(i) for i in t.selectionModel().selectedRows(0)) if t is not None else []

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def click(v, pos):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        return viewer_selection()

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def attribute_rows():
        a = child(QTreeView, "sceneGraphAttributes")
        m = a.model() if a is not None else None
        if m is None:
            return {}
        return dict((str(m.index(r, 0).data()), (str(m.index(r, 1).data()), str(m.index(r, 3).data())))
                    for r in range(m.rowCount()))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(panel() is not None, "the Scene Graph panel exists")

        # Dock the panel in a pane split off the viewer's.
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and WORLD in top_paths(), POLL_TIMEOUT_S)
        require(ok, "the docked panel lists /World (got %r)" % (top_paths(),))
        p = panel()
        report("NOTE panel %dx%d, scene view %dx%d" % (p.width(), p.height(), view().width(), view().height()))
        check(bool(p.property("natronSceneGraphHasScene")), "natronSceneGraphHasScene is set")
        check(str(p.property("natronSceneGraphNode")) == usd.getScriptName(),
              "natronSceneGraphNode names the viewed USDRead (got %r)" % (p.property("natronSceneGraphNode"),))
        check(not p.property("natronSceneGraphPinned"), "the panel follows the viewer at first")

        # The tree.
        world = find(WORLD)
        tree().expand(world)
        children = sorted(path_of(i) for i in rows(world))
        check(children == sorted([BOX, BALL, CAM]), "/World holds Box, Ball and Cam (got %r)" % (children,))
        types = dict((path_of(i), str(i.siblingAtColumn(1).data())) for i in rows(world))
        check(types.get(BOX) == "Cube" and types.get(BALL) == "Sphere" and types.get(CAM) == "Camera",
              "the type column reads Cube, Sphere and Camera (got %r)" % (types,))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-docked.png")

        # Tree to viewer.
        box = find(BOX)
        tree().selectionModel().select(box, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        tree().selectionModel().setCurrentIndex(box, QItemSelectionModel.NoUpdate)
        ok = yield ("until", lambda: viewer_selection() == [BOX], POLL_TIMEOUT_S)
        check(ok, "selecting the box's row selects it in the viewer (got %r)" % (viewer_selection(),))
        ok = yield ("until", lambda: "size" in attribute_rows(), POLL_TIMEOUT_S)
        attributes = attribute_rows()
        check(ok, "the attribute list shows the box's size (got %r)" % (sorted(attributes.keys())[:12],))
        report("NOTE box attributes: %r" % (attributes,))
        author = attributes.get("size", ("", ""))[1]
        check(author.endswith("scene-reference.usda") or author == usd.getScriptName(),
              "the box's size names the file or the USDRead as its author (got %r)" % (author,))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-select-box.png")

        # Viewer to tree: click a grid until the ball is picked.
        v = view()
        sx = max(1, v.width() // GRID_COLUMNS)
        sy = max(1, v.height() // GRID_ROWS)
        picked = False
        for row in range(GRID_ROWS):
            for column in range(GRID_COLUMNS):
                if click(v, QPoint(sx // 2 + column * sx, sy // 2 + row * sy)) == [BALL]:
                    picked = True
                    break
            if picked:
                break
            yield ("sleep", 0)
        require(picked, "a click in the viewer picks the ball")
        ok = yield ("until", lambda: selected_rows() == [BALL], POLL_TIMEOUT_S)
        check(ok, "the ball picked in the viewer selects its row (got %r)" % (selected_rows(),))
        current = tree().selectionModel().currentIndex()
        check(path_of(current) == BALL, "the ball's row is current (got %r)" % (path_of(current),))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-select-ball-in-viewer.png")

        # The filter.
        line = child(QLineEdit, "sceneGraphFilter")
        results = child(QListView, "sceneGraphResults")
        require(line is not None and results is not None, "the panel has its filter and result list")
        line.setText("B*")
        ok = yield ("until", lambda: results.isVisible() and not p.property("natronSceneGraphFilterBusy")
                    and results.model().rowCount() == 2, POLL_TIMEOUT_S)
        matches = sorted(str(results.model().index(r, 0).data()) for r in range(results.model().rowCount()))
        check(ok and matches == sorted([BOX, BALL]), "the filter B* lists the box and the ball (got %r)" % (matches,))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-filter.png")
        line.setText("")
        ok = yield ("until", lambda: tree().isVisible(), POLL_TIMEOUT_S)
        check(ok, "clearing the filter shows the tree again")

        # Pinning.
        pin = child(QToolButton, "sceneGraphPin")
        require(pin is not None, "the panel has a pin button")
        pin.click()
        ok = yield ("until", lambda: bool(p.property("natronSceneGraphPinned")) and WORLD in top_paths(), POLL_TIMEOUT_S)
        check(ok, "the pin button pins the panel to the viewed USDRead (got %r)" % (top_paths(),))
        require(viewer.connectInput(0, camera), "switch the Viewer to the USDCamera")
        ok = yield ("until", lambda: scene_drawn() and view() is not None, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS * 2)
        check(top_paths() == [WORLD] and str(p.property("natronSceneGraphNode")) == usd.getScriptName(),
              "pinned, the tree keeps the USDRead's scene while the viewer shows the camera (got %r)" % (top_paths(),))
        shot("scene-graph-pinned.png")

        usd.destroy()
        ok = yield ("until", lambda: not p.property("natronSceneGraphPinned"), POLL_TIMEOUT_S)
        check(ok, "deleting the pinned node unpins the panel")
        ok = yield ("until", lambda: top_paths() == [camera_root], POLL_TIMEOUT_S)
        check(ok, "unpinned, the panel shows the camera the viewer shows (got %r, want %r)" % (top_paths(), camera_root))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-unpinned.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
