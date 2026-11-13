"""The Scene Graph panel on nodes the viewer does not show.

Graph: USDRead(city) -> USDEditTransform "look" -> Viewer1, with a USDPrune removing /World/Block1
and four USDEditTransforms off the USDRead. Generates a small city with payload buildings and a long
list of props under $NATRON_GUI_TEST_OUT, docks the panel beside the viewer, and checks:
  - pinned to a node the viewer does not show, the tree is that node's, and no building's layer
    opens until a row is twirled; the twirl loads on the panel's stage alone, not the viewer's;
  - pinned to a second node and back, the first node's stage is reused and composes nothing; an
    upstream edit makes a kept stage compose again when it is shown;
  - an unloaded payload row the panel had open stays closed on a stage where it is not loaded, and
    reopens on the stage where it is;
  - following the selection, clicking through four nodes quickly composes only the last one, the
    tree is greyed while it is read, and no timer runs once the panel is idle;
  - open rows and the row at the top of the tree survive a viewer switch to a downstream USDPrune
    and back, the row the prune removes reopens on return, and restoring loads nothing;
  - a pinned node that puts out no scene keeps the pin, and deleting it unpins;
  - a second panel opened from the pane menu follows the viewer; pinned to another node than the
    first, each tree is its own node's, a payload twirled in one loads in neither the other nor the
    viewer, and closing the second panel destroys it.

Screenshots of each state go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
IDLE_MS = 1000
CLICK_GAP_MS = 30
PROP_COUNT = 120
WORLD = "/World"
PLAIN = "/World/Plain"
BLOCK0 = "/World/Block0"
BLOCK1 = "/World/Block1"
PROPS = "/World/Props"
B0 = BLOCK0 + "/B0"
FLOOR = B0 + "/Floor"

STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)


def write_city(directory):
    """Writes the root layer and the payload files and returns the root's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)

    def building(name, x, z):
        with open(os.path.join(payloads, name + ".usda"), "w") as f:
            f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                    'def Xform "Building"\n{\n'
                    '    def Xform "Floor"\n    {\n'
                    '        def Cube "Geo"\n        {\n            double size = 1\n'
                    '            double3 xformOp:translate = (%g, 0, %g)\n'
                    '            uniform token[] xformOpOrder = ["xformOp:translate"]\n        }\n'
                    '    }\n}\n' % (x, z))
        return '@./payloads/%s.usda@' % name

    def row(prim, arc):
        return '        def Xform "%s" (\n            prepend payload = %s\n        )\n        {\n        }\n' % (prim, arc)

    path = os.path.join(directory, "points_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\ndef Xform "World"\n{\n')
        f.write('    def Cube "Plain"\n    {\n        double size = 0.5\n    }\n')
        f.write('    def Xform "Block0"\n    {\n')
        f.write(row("B0", building("b0_0", -2, 0)))
        f.write(row("B1", building("b0_1", 2, 0)))
        f.write('    }\n    def Xform "Block1"\n    {\n')
        f.write(row("B0", building("b1_0", -2, 3)))
        f.write(row("B1", building("b1_1", 2, 3)))
        f.write('    }\n    def Xform "Props"\n    {\n')
        for i in range(PROP_COUNT):
            f.write('        def Cube "P%03d"\n        {\n            double size = 0.1\n'
                    '            double3 xformOp:translate = (%g, 0, -2)\n'
                    '            uniform token[] xformOpOrder = ["xformOp:translate"]\n        }\n' % (i, i * 0.2))
        f.write('    }\n}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QModelIndex, QObject, QPoint, Qt
    from PySide6.QtWidgets import QAbstractItemView, QComboBox, QLabel, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_graph_points"))
    scene_path = write_city(fixture_dir)
    payload_dir = os.path.join(fixture_dir, "payloads")

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    usd = create("USDRead", 0, 0)
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    frame_offset = usd.getParam("frameOffset")
    require(frame_offset is not None, "USDRead has a frameOffset parameter")

    look = create("USDEditTransform", 0, 120)
    require(look.connectInput(0, usd), "connect the viewed USDEditTransform to the USDRead")
    look.getParam("primPattern").setValue(PLAIN)
    look.getParam("translate").set(0.0, 1.0, 0.0)

    prune = create("USDPrune", 200, 120)
    require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
    prune.getParam("primPattern").setValue(BLOCK1)

    clicks = []
    for i in range(4):
        node = create("USDEditTransform", 400 + 150 * i, 120)
        require(node.connectInput(0, usd), "connect USDEditTransform %d to the USDRead" % (i,))
        node.getParam("primPattern").setValue(PLAIN)
        clicks.append(node)

    viewer = app.getNode("Viewer1") or create("Viewer", 0, 260)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    FIRST = "sceneGraph"
    SECOND = "sceneGraph2"
    on = [FIRST]

    def panel_named(name):
        for w in widgets_of("SceneGraphPanel"):
            try:
                if w.objectName() == name:
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def panel():
        """The panel the helpers below act on, the one use() last named."""
        return panel_named(on[0])

    def use(name):
        on[0] = name

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
        return hold(t.model()) if t is not None else None

    def prop(name, default=None):
        p = panel()
        value = p.property(name) if p is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

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

    def state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def expanded(path):
        index = find(path)
        return index.isValid() and tree().isExpanded(index)

    def world_children():
        return [str(i.data()) for i in rows(find(WORLD))]

    def opened_buildings():
        """The payload files of the fixture that are open in the process."""
        names = []
        for layer in Sdf.Layer.GetLoadedLayers():
            real = layer.realPath or ""
            if real.startswith(payload_dir):
                names.append(os.path.basename(real))
        return sorted(names)

    def compose_count():
        return int(prop("natronSceneGraphComposeCount", 0))

    def viewer_load_edits():
        return int(view().property("natronSceneLoadEdits") or 0)

    def shown_node():
        return str(prop("natronSceneGraphNode", ""))

    def shows(node, children=None):
        """Whether the panel shows node's rows, read, and with /World's children children when given."""
        def ready():
            if shown_node() != node.getScriptName() or bool(prop("natronSceneGraphSwitching", False)):
                return False
            if [path_of(i) for i in rows()] != [WORLD]:
                return False
            return children is None or world_children() == children
        return ready

    def top_path():
        index = tree().indexAt(QPoint(0, 0))
        return path_of(index) if index.isValid() else ""

    def status_text():
        label = child(QLabel, "sceneGraphStatus")
        return str(label.text()) if label is not None else ""

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

    def choose(index):
        chooser = child(QComboBox, "sceneGraphNode")
        require(chooser is not None, "the panel has a node chooser")
        chooser.setCurrentIndex(index)
        chooser.activated.emit(index)

    def pin_to(node):
        """Pins the panel through its node chooser, whose list is refreshed when it opens."""
        chooser = child(QComboBox, "sceneGraphNode")
        require(chooser is not None, "the panel has a node chooser")
        chooser.showPopup()
        chooser.hidePopup()
        index = chooser.findData(node.getScriptName())
        require(index >= 0, "the node chooser lists " + node.getScriptName())
        choose(index)
        ok = yield ("until", lambda: bool(prop("natronSceneGraphPinned", False)) and shown_node() == node.getScriptName(),
                    POLL_TIMEOUT_S)
        require(ok, "the panel is pinned to " + node.getScriptName())

    def unpin():
        choose(0)
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphPinned", True)), POLL_TIMEOUT_S)
        require(ok, "choosing the first entry unpins the panel")

    class SwitchWatch(QObject):
        """Records the tree's enabled state and the status each time the switching property flips."""

        def __init__(self):
            QObject.__init__(self)
            self.seen = []
            self.last = False

        def eventFilter(self, watched, event):
            if event.type() == QEvent.DynamicPropertyChange:
                switching = bool(watched.property("natronSceneGraphSwitching"))
                if switching != self.last:
                    self.last = switching
                    t = tree()
                    self.seen.append((switching, bool(t.isEnabled()) if t is not None else None, status_text()))
            return False

    switch_watch = SwitchWatch()

    def steps():
        require(view_input(look), "connect the Viewer to the viewed USDEditTransform")
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
        ok = yield ("until", lambda: panel().isVisible() and shows(look)(), POLL_TIMEOUT_S)
        require(ok, "the docked panel shows the viewed node's scene (got %r)" % (shown_node(),))
        panel().installEventFilter(switch_watch)
        all_blocks = ["Plain", "Block0", "Block1", "Props"]
        pruned = ["Plain", "Block0", "Props"]

        # Pinned to nodes the viewer does not show.
        edits_before = viewer_load_edits()
        composed = compose_count()
        yield from pin_to(usd)
        ok = yield ("until", shows(usd, all_blocks), POLL_TIMEOUT_S)
        check(ok, "pinned to the USDRead the viewer does not show, the tree is the USDRead's (got %r)" % (world_children(),))
        check(compose_count() > composed, "the pinned node is read into a stage of the panel's own (%d, then %d)"
              % (composed, compose_count()))
        check(opened_buildings() == [], "reading the pinned node opens no building's layer (got %r)" % (opened_buildings(),))
        for path in (B0, BLOCK1 + "/B0"):
            check(state(path) == STATE_UNLOADED, "%s is unloaded on the panel's stage (got %r)" % (path, state(path)))
        shot("scene-graph-points-pinned-read.png")

        yield from pin_to(prune)
        ok = yield ("until", shows(prune, pruned), POLL_TIMEOUT_S)
        check(ok, "pinned to the USDPrune, the tree is the USDPrune's (got %r)" % (world_children(),))
        check(opened_buildings() == [], "reading a second node opens no building's layer (got %r)" % (opened_buildings(),))
        check(int(prop("natronSceneGraphCachedStages", 0)) >= 1, "the USDRead's stage is kept (%r kept)"
              % (prop("natronSceneGraphCachedStages"),))
        shot("scene-graph-points-pinned-prune.png")
        yield ("sleep", SETTLE_MS)

        composed = compose_count()
        yield from pin_to(usd)
        check(shows(usd, all_blocks)(), "returning to a kept node shows its tree at once (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        check(compose_count() == composed, "returning to a kept node composes nothing (%d, then %d)" % (composed, compose_count()))
        check(shows(usd, all_blocks)(), "the kept node's tree stays up (got %r)" % (world_children(),))

        # Twirling on the panel's stage.
        tree().expand(find(WORLD))
        tree().expand(find(BLOCK0))
        tree().expand(find(B0))
        ok = yield ("until", lambda: state(B0) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "twirling a payload row loads it on the panel's stage (state %r)" % (state(B0),))
        check(opened_buildings() == ["b0_0.usda"], "the twirl opens that building's layer alone (got %r)" % (opened_buildings(),))
        check(viewer_load_edits() == edits_before, "the twirl loads nothing on the viewer's stage (%d, then %d)"
              % (edits_before, viewer_load_edits()))
        check([str(i.data()) for i in rows(find(B0))] == ["Floor"], "the loaded row lists the building's prims")
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-points-twirl-own-stage.png")

        # A remembered payload row on a stage where it is not loaded.
        composed = compose_count()
        yield from pin_to(prune)
        ok = yield ("until", shows(prune, pruned), POLL_TIMEOUT_S)
        check(ok, "the kept USDPrune's tree comes back (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        check(compose_count() == composed, "returning to the kept USDPrune composes nothing (%d, then %d)" % (composed, compose_count()))
        check(expanded(WORLD) and expanded(BLOCK0), "the open rows carry over to the USDPrune")
        check(state(B0) == STATE_UNLOADED and not expanded(B0),
              "the payload row unloaded here stays closed and unloaded (state %r, open %r)" % (state(B0), expanded(B0)))
        check(opened_buildings() == ["b0_0.usda"], "restoring the open rows opens no building's layer (got %r)" % (opened_buildings(),))
        shot("scene-graph-points-unloaded-stays-closed.png")

        yield from pin_to(usd)
        ok = yield ("until", lambda: shows(usd, all_blocks)() and expanded(B0), POLL_TIMEOUT_S)
        check(ok, "back on the USDRead, its loaded payload row reopens (state %r, open %r)" % (state(B0), expanded(B0)))

        # A kept stage after an upstream edit.
        yield ("sleep", SETTLE_MS)
        frame_offset.setValue(frame_offset.getValue() + 1.0)
        yield ("sleep", SETTLE_MS)
        composed = compose_count()
        yield from pin_to(prune)
        ok = yield ("until", lambda: compose_count() > composed and shows(prune, pruned)(), POLL_TIMEOUT_S)
        check(ok, "a kept stage whose upstream changed composes again when shown (%d, then %d)" % (composed, compose_count()))

        # Following the selection through four nodes.
        yield from unpin()
        follow = child(QComboBox, "sceneGraphFollow")
        require(follow is not None, "the panel has a Follow chooser")
        app.selectNode(prune, True)
        follow.setCurrentIndex(1)
        ok = yield ("until", lambda: str(prop("natronSceneGraphFollow", "")) == "selection", POLL_TIMEOUT_S)
        require(ok, "the panel follows the selection")
        ok = yield ("until", shows(prune, pruned), POLL_TIMEOUT_S)
        require(ok, "following the selection, the panel shows the selected USDPrune")
        yield ("sleep", SETTLE_MS)
        del switch_watch.seen[:]
        composed = compose_count()
        for node in clicks:
            app.selectNode(node, True)
            yield ("sleep", CLICK_GAP_MS)
        ok = yield ("until", shows(clicks[-1]), POLL_TIMEOUT_S)
        check(ok, "after four quick clicks the panel shows the last node (got %r)" % (shown_node(),))
        yield ("sleep", SETTLE_MS)
        check(compose_count() == composed + 1, "four quick clicks compose only the last node (%d, then %d)"
              % (composed, compose_count()))
        greyed = [s for s in switch_watch.seen if s[0]]
        check(bool(greyed) and all(s[1] is False for s in greyed),
              "the tree is greyed while the next node is read (got %r)" % (switch_watch.seen,))
        check(any(clicks[-1].getScriptName() in s[2] and "Reading" in s[2] for s in greyed),
              "the status names the node being read (got %r)" % ([s[2] for s in greyed],))
        check(not bool(prop("natronSceneGraphSwitching", True)) and tree().isEnabled(), "the tree is enabled once read")
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphTimersActive", True)), POLL_TIMEOUT_S)
        check(ok, "no timer runs once the panel is idle")
        yield ("sleep", IDLE_MS)
        check(not bool(prop("natronSceneGraphTimersActive", True)), "no timer starts while the panel stays idle")
        shot("scene-graph-points-follow-selection.png")

        # Open rows and scroll across a viewer switch.
        follow.setCurrentIndex(0)
        ok = yield ("until", lambda: str(prop("natronSceneGraphFollow", "")) == "viewer", POLL_TIMEOUT_S)
        require(ok, "the panel follows the viewer again")
        require(view_input(usd), "switch the viewer to the USDRead")
        ok = yield ("until", lambda: shows(usd, all_blocks)() and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the panel shows the viewer's USDRead (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        check(state(B0) == STATE_UNLOADED and not expanded(B0),
              "on the viewer's stage, the remembered payload row stays closed and unloaded (state %r)" % (state(B0),))
        for path in (BLOCK1, PROPS):
            tree().expand(find(path))
        target = PROPS + "/P%03d" % (PROP_COUNT // 2,)
        tree().scrollTo(find(target), QAbstractItemView.PositionAtTop)
        ok = yield ("until", lambda: top_path().startswith(PROPS + "/") and str(prop("natronSceneGraphTopPath", "")) == top_path(),
                    POLL_TIMEOUT_S)
        require(ok, "the tree scrolls to the props (top %r, published %r)" % (top_path(), prop("natronSceneGraphTopPath")))
        top = top_path()
        edits = viewer_load_edits()
        layers = opened_buildings()
        shot("scene-graph-points-scrolled.png")

        require(view_input(prune), "switch the viewer to the downstream USDPrune")
        ok = yield ("until", lambda: shows(prune, pruned)() and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "the panel shows the USDPrune the viewer shows (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        check(expanded(WORLD) and expanded(BLOCK0) and expanded(PROPS), "the open rows survive the switch to the USDPrune")
        check(top_path() == top, "the top row survives the switch to the USDPrune (%r, then %r)" % (top, top_path()))
        shot("scene-graph-points-viewer-prune.png")

        require(view_input(usd), "switch the viewer back to the USDRead")
        ok = yield ("until", lambda: shows(usd, all_blocks)() and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "the panel shows the USDRead again (got %r)" % (world_children(),))
        ok = yield ("until", lambda: expanded(BLOCK1), POLL_TIMEOUT_S)
        check(ok, "the row the USDPrune removed reopens on return")
        yield ("sleep", SETTLE_MS)
        check(expanded(WORLD) and expanded(BLOCK0) and expanded(PROPS), "the open rows survive the switch back")
        check(top_path() == top, "the top row survives the switch back (%r, then %r)" % (top, top_path()))
        check(viewer_load_edits() == edits, "restoring the rows loads nothing (%d, then %d)" % (edits, viewer_load_edits()))
        check(opened_buildings() == layers, "restoring the rows opens no building's layer (%r, then %r)" % (layers, opened_buildings()))
        check(state(B0) == STATE_UNLOADED and not expanded(B0), "the unloaded payload row stays closed (state %r)" % (state(B0),))
        shot("scene-graph-points-viewer-back.png")

        # A pinned node that loses its scene, then is deleted.
        gone = clicks[0]
        yield from pin_to(gone)
        ok = yield ("until", shows(gone), POLL_TIMEOUT_S)
        require(ok, "the panel is pinned to %s and shows its scene" % (gone.getScriptName(),))
        gone.disconnectInput(0)
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphHasScene", True)), POLL_TIMEOUT_S)
        check(ok, "a pinned node that puts out no scene shows no scene (status %r)" % (status_text(),))
        check(bool(prop("natronSceneGraphPinned", False)) and shown_node() == gone.getScriptName(),
              "a pinned node that puts out no scene keeps the pin (node %r)" % (shown_node(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-points-no-scene.png")
        name = gone.getScriptName()
        gone.destroy()
        gone = None
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphPinned", True)), POLL_TIMEOUT_S)
        check(ok, "deleting the pinned node unpins the panel (node %r)" % (shown_node(),))
        check(shown_node() != name, "the panel no longer names the deleted node (got %r)" % (shown_node(),))
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphTimersActive", True)), POLL_TIMEOUT_S)
        check(ok, "no timer runs once the panel is idle again")
        report("NOTE composed %d times, %r stages kept" % (compose_count(), prop("natronSceneGraphCachedStages")))

        # A second panel on another node.
        panel().removeEventFilter(switch_watch)
        home = ancestor_of_class(panel(), "TabWidget")
        require(home is not None, "the first panel is in a pane")
        lower = app.getTabWidget(home.objectName()).splitVertically()
        home = None
        require(lower is not None, "split the first panel's pane")
        lower_name = str(lower.getScriptName())
        lower = None
        panes = [w for w in widgets_of("TabWidget") if w.objectName() == lower_name]
        require(len(panes) == 1, "the new pane is found (%d named %s)" % (len(panes), lower_name))
        require(QMetaObject.invokeMethod(panes[0], "newSceneGraphHere"), "choose New Scene Graph in the new pane")
        panes = None
        ok = yield ("until", lambda: panel_named(SECOND) is not None and panel_named(SECOND).isVisible(), POLL_TIMEOUT_S)
        require(ok, "New Scene Graph opens a panel named %s" % (SECOND,))
        check(len(widgets_of("SceneGraphPanel")) == 2, "there are two Scene Graph panels (got %d)" % (len(widgets_of("SceneGraphPanel")),))
        use(SECOND)
        ok = yield ("until", lambda: str(prop("natronSceneGraphFollow", "")) == "viewer" and shows(usd, all_blocks)(), POLL_TIMEOUT_S)
        check(ok, "the new panel follows the viewer and shows its USDRead (got %r)" % (shown_node(),))
        check(not bool(prop("natronSceneGraphPinned", True)), "the new panel starts unpinned")

        use(FIRST)
        yield from pin_to(prune)
        ok = yield ("until", shows(prune, pruned), POLL_TIMEOUT_S)
        check(ok, "the first panel pinned to the USDPrune shows its tree (got %r)" % (world_children(),))
        use(SECOND)
        yield from pin_to(look)
        ok = yield ("until", shows(look, all_blocks), POLL_TIMEOUT_S)
        check(ok, "the second panel pinned to the viewed USDEditTransform shows its tree (got %r)" % (world_children(),))
        check(compose_count() >= 1, "the second panel reads its node into a stage of its own (%d)" % (compose_count(),))
        use(FIRST)
        check(shows(prune, pruned)(), "pinning the second panel leaves the first on the USDPrune (got %r, %r)"
              % (shown_node(), world_children()))
        check(state(BLOCK0 + "/B1") == STATE_UNLOADED, "Block0/B1 is unloaded on the first panel's stage")

        edits = viewer_load_edits()
        composed_first = compose_count()
        use(SECOND)
        tree().expand(find(WORLD))
        tree().expand(find(BLOCK0))
        tree().expand(find(BLOCK0 + "/B1"))
        ok = yield ("until", lambda: state(BLOCK0 + "/B1") == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "twirling Block0/B1 loads it on the second panel's stage (state %r)" % (state(BLOCK0 + "/B1"),))
        yield ("sleep", SETTLE_MS)
        use(FIRST)
        check(state(BLOCK0 + "/B1") == STATE_UNLOADED and not expanded(BLOCK0 + "/B1"),
              "the twirl in the second panel loads nothing on the first's stage (state %r)" % (state(BLOCK0 + "/B1"),))
        check(compose_count() == composed_first, "the twirl in the second panel composes nothing in the first (%d, then %d)"
              % (composed_first, compose_count()))
        check(viewer_load_edits() == edits, "the twirl in the second panel loads nothing on the viewer's stage (%d, then %d)"
              % (edits, viewer_load_edits()))
        check(shows(prune, pruned)(), "the first panel still shows the USDPrune")
        shot("scene-graph-points-two-panels.png")

        lower = app.getTabWidget(lower_name)
        require(lower is not None, "the second panel's pane is found")
        index = -1
        for i in range(lower.count()):
            if str(lower.getTabLabel(i)) == "Scene Graph 2":
                index = i
        require(index >= 0, "the second panel's pane has its tab")
        del _held[:]
        lower.closeTab(index)
        lower = None
        ok = yield ("until", lambda: panel_named(SECOND) is None and len(widgets_of("SceneGraphPanel")) == 1, POLL_TIMEOUT_S)
        check(ok, "closing the second panel destroys it (%d panels)" % (len(widgets_of("SceneGraphPanel")),))
        check(shows(prune, pruned)() and bool(prop("natronSceneGraphPinned", False)),
              "the first panel keeps its pin once the second is gone (got %r)" % (shown_node(),))
        yield ("sleep", SETTLE_MS)
        check(scene_drawn(), "the viewer still shows the scene once the second panel is gone")
        ok = yield ("until", lambda: not bool(prop("natronSceneGraphTimersActive", True)), POLL_TIMEOUT_S)
        check(ok, "no timer runs in the first panel once the second is gone")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
