"""What the Scene Graph panel follows: the active viewer, or the node selection.

Graph: USDRead(scene-reference.usda) and a USDCamera, with Viewer1 showing one of them. The panel
is docked beside the 3D viewer; its Follow chooser (sceneGraphFollow) picks the mode, mirrored in
the natronSceneGraphFollow property, and natronSceneGraphNode names the node whose scene is shown.

Checks:
  - following the viewer (the default), selecting the camera in the node graph leaves the tree on
    the USDRead the viewer shows, and switching the viewer to the camera moves the tree to it;
  - following the selection, the tree shows the selected scene node whatever the viewer shows;
    selecting a node with no scene (the Viewer) keeps the last scene node;
  - pinned while following the selection, a new selection does not move the tree; unpinning
    shows the selected node again;
  - back to following the viewer, the tree shows the viewer's node.

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

try:
    from PySide6.QtCore import QModelIndex, Qt
    from PySide6.QtWidgets import QComboBox, QToolButton, QTreeView

    from guitest import FIXTURES, check, finish, plugin_problem, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    camera = app.createNode("USDCamera")
    require(camera is not None, "create USDCamera", plugin_problem("USDCamera"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    camera.setPosition(200, 0)
    viewer.setPosition(0, 150)
    camera_root = "/" + str(camera.getParam("primPath").getValue()).strip("/").split("/")[0]

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

    def model():
        t = child(QTreeView, "sceneGraphTree")
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def top_paths():
        m = model()
        if m is None:
            return []
        try:
            while m.canFetchMore(QModelIndex()):
                m.fetchMore(QModelIndex())
            return [str(m.index(r, 0, QModelIndex()).data(PATH_ROLE)) for r in range(m.rowCount(QModelIndex()))]
        except RuntimeError:
            return []

    def shown_node():
        p = panel()
        return str(p.property("natronSceneGraphNode")) if p is not None else ""

    def follow_mode():
        p = panel()
        return str(p.property("natronSceneGraphFollow")) if p is not None else ""

    def shows(node, root):
        return lambda: shown_node() == node.getScriptName() and top_paths() == [root]

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

    def select(node):
        app.selectNode(node, True)

    def steps():
        require(view_input(usd), "connect the Viewer to the USDRead")
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
        ok = yield ("until", lambda: panel().isVisible() and shows(usd, WORLD)(), POLL_TIMEOUT_S)
        require(ok, "the docked panel shows the USDRead's scene (got %r, %r)" % (shown_node(), top_paths()))

        chooser = child(QComboBox, "sceneGraphFollow")
        require(chooser is not None, "the panel has a Follow chooser")
        check(follow_mode() == "viewer" and chooser.currentIndex() == 0, "the panel follows the viewer by default")

        select(camera)
        yield ("sleep", SETTLE_MS)
        check(shows(usd, WORLD)(), "following the viewer, selecting the camera leaves the tree on the viewed USDRead (got %r, %r)"
              % (shown_node(), top_paths()))
        require(view_input(camera), "switch the viewer to the camera")
        ok = yield ("until", shows(camera, camera_root), POLL_TIMEOUT_S)
        check(ok, "following the viewer, the tree moves to the camera the viewer shows (got %r, %r)" % (shown_node(), top_paths()))

        chooser.setCurrentIndex(1)
        ok = yield ("until", lambda: follow_mode() == "selection", POLL_TIMEOUT_S)
        require(ok, "the chooser switches the panel to following the selection")
        ok = yield ("until", shows(camera, camera_root), POLL_TIMEOUT_S)
        check(ok, "following the selection, the tree shows the selected camera (got %r, %r)" % (shown_node(), top_paths()))

        select(usd)
        ok = yield ("until", shows(usd, WORLD), POLL_TIMEOUT_S)
        check(ok, "selecting the USDRead moves the tree to it while the viewer shows the camera (got %r, %r)"
              % (shown_node(), top_paths()))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-follow-selection.png")

        select(viewer)
        yield ("sleep", SETTLE_MS)
        check(shows(usd, WORLD)(), "selecting the Viewer, which has no scene, keeps the USDRead (got %r, %r)"
              % (shown_node(), top_paths()))
        require(view_input(usd), "switch the viewer to the USDRead")
        yield ("sleep", SETTLE_MS)
        require(view_input(camera), "switch the viewer back to the camera")
        yield ("sleep", SETTLE_MS)
        check(shows(usd, WORLD)(), "following the selection, switching the viewer leaves the tree on the USDRead (got %r, %r)"
              % (shown_node(), top_paths()))

        pin = child(QToolButton, "sceneGraphPin")
        require(pin is not None, "the panel has a pin button")
        pin.click()
        ok = yield ("until", lambda: bool(panel().property("natronSceneGraphPinned")), POLL_TIMEOUT_S)
        require(ok, "the pin button pins the panel")
        select(camera)
        yield ("sleep", SETTLE_MS)
        check(shows(usd, WORLD)(), "pinned, selecting the camera leaves the tree on the USDRead (got %r, %r)"
              % (shown_node(), top_paths()))
        pin.click()
        ok = yield ("until", shows(camera, camera_root), POLL_TIMEOUT_S)
        check(ok, "unpinned, the tree shows the selected camera (got %r, %r)" % (shown_node(), top_paths()))

        select(usd)
        ok = yield ("until", shows(usd, WORLD), POLL_TIMEOUT_S)
        require(ok, "the tree follows the selection to the USDRead")
        chooser.setCurrentIndex(0)
        ok = yield ("until", lambda: follow_mode() == "viewer" and shows(camera, camera_root)(), POLL_TIMEOUT_S)
        check(ok, "back to following the viewer, the tree shows the camera the viewer shows (got %r, %r)"
              % (shown_node(), top_paths()))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
