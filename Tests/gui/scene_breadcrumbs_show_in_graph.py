"""Show In Scene Graph in the breadcrumb segment menu.

Graph: USDRead(a small hierarchy) -> Viewer1, and a second USDRead nothing views. Checks:
  - with the Scene Graph panel closed, Show In Scene Graph on group_1 opens a panel that follows
    the viewer, with /geo/group_1 selected and its row visible, and the payload under it unloaded,
    in a pane other than the viewer's, so the viewer and its bar stay shown;
  - with the only panel pinned to the second USDRead, Show In Scene Graph on another segment opens
    a second panel that follows the viewer and shows the prim, while the first keeps its pin and node;
  - a second use picks that panel again instead of opening a third.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/).
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
GEO = "/geo"
GROUP = "/geo/group_1"
THING = "/geo/group_1/a_thing"
MESH = "/geo/group_1/a_thing/mesh"
SIBLING = "/geo/group_2"
PAYLOAD = "/pay"


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "show_payload.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Root"\n)\ndef Xform "Root"\n{\n    def Xform "Floor"\n    {\n    }\n}\n')
    path = os.path.join(directory, "show_in_graph.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n)\n'
                'def Xform "geo"\n{\n'
                '    def Xform "group_1"\n    {\n'
                '        def Xform "a_thing"\n        {\n            def Cube "mesh"\n            {\n            }\n        }\n'
                '    }\n'
                '    def Xform "group_2"\n    {\n    }\n}\n'
                'def Xform "pay" (\n    prepend payload = @./show_payload.usda@\n)\n{\n}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QComboBox, QMenu, QToolButton, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3
    STATE_UNLOADED = 1

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_breadcrumbs_show_in_graph")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    other = app.createNode("USDRead")
    require(other is not None, "create the second USDRead", plugin_problem("USDRead"))
    other.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
    usd.setPosition(0, 0)
    other.setPosition(200, 0)
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

    def bar():
        bars = [w for w in widgets_of("SceneBreadcrumbBar") if w.isVisible()]
        return hold(bars[0]) if bars else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def select(paths):
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, Q_ARG("QStringList", list(paths)))

    def segment_button(text):
        b = bar()
        for w in (b.findChildren(QToolButton, "breadcrumbSegment") if b is not None else []):
            if w.isVisibleTo(b) and str(w.text()) == text:
                return hold(w)
        return None

    def show_in_graph(text):
        """Right-clicks the segment and clicks Show In Scene Graph in its menu."""
        QTest.mouseClick(segment_button(text), Qt.RightButton)
        menus = [m for m in bar().findChildren(QMenu, "breadcrumbSegmentMenu") if m.isVisible()]
        require(len(menus) == 1, "a right-click on %s opens its menu" % (text,))
        for a in menus[0].actions():
            if a.objectName() == "breadcrumbShowInSceneGraph":
                require(a.isEnabled(), "Show In Scene Graph is enabled on %s" % (text,))
                QTest.mouseClick(menus[0], Qt.LeftButton, Qt.NoModifier, menus[0].actionGeometry(a).center())
                return
        require(False, "the segment menu has Show In Scene Graph")

    def panels():
        out = []
        for w in widgets_of("SceneGraphPanel"):
            try:
                out.append(hold(w))
            except RuntimeError:
                continue
        return out

    def visible_panels():
        return [p for p in panels() if p.isVisible()]

    def tree_of(p):
        return hold(p.findChild(QTreeView, "sceneGraphTree"))

    def selected_paths(p):
        t = tree_of(p)
        return [str(i.data(PATH_ROLE)) for i in t.selectionModel().selectedRows(0)]

    def row_visible(p, path):
        """Whether the row of path is in the tree's viewport."""
        t = tree_of(p)
        m = t.model()
        parent = t.rootIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = None
            for r in range(m.rowCount(parent)):
                index = m.index(r, 0, parent)
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if found is None:
                return False
            if depth < path.count("/") and not t.isExpanded(found):
                return False
            parent = found
        rect = t.visualRect(parent)
        return rect.isValid() and not rect.isEmpty() and t.viewport().rect().intersects(rect)

    def row_state(p, path):
        t = tree_of(p)
        m = t.model()
        for r in range(m.rowCount(t.rootIndex())):
            index = m.index(r, 0, t.rootIndex())
            if str(index.data(PATH_ROLE)) == path:
                return index.data(STATE_ROLE)
        return None

    def pin_to(p, node):
        chooser = hold(p.findChild(QComboBox, "sceneGraphNode"))
        require(chooser is not None, "the panel has a node chooser")
        chooser.showPopup()
        chooser.hidePopup()
        index = chooser.findData(node.getScriptName())
        require(index >= 0, "the node chooser lists " + node.getScriptName())
        chooser.setCurrentIndex(index)
        chooser.activated.emit(index)

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: bar() is not None, POLL_TIMEOUT_S)
        require(ok, "the breadcrumb bar is visible with the scene view")

        for p in visible_panels():
            if p.metaObject().indexOfMethod("closePanel()") >= 0:
                QMetaObject.invokeMethod(p, "closePanel")
        ok = yield ("until", lambda: not visible_panels(), POLL_TIMEOUT_S)
        require(ok, "every Scene Graph panel is closed")

        select([MESH])
        ok = yield ("until", lambda: segment_button("group_1") is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows group_1")
        show_in_graph("group_1")
        ok = yield ("until", lambda: len(visible_panels()) == 1 and selected_paths(visible_panels()[0]) == [GROUP], POLL_TIMEOUT_S)
        require(ok, "with the panel closed, Show In Scene Graph opens a panel with /geo/group_1 selected")
        first = visible_panels()[0]
        first_name = str(first.objectName())
        check(str(first.property("natronSceneGraphFollow")) == "viewer" and not bool(first.property("natronSceneGraphPinned")),
              "the opened panel follows the viewer without a pin")
        check(row_visible(first, GROUP), "the group_1 row is visible in the panel")
        check(row_state(first, PAYLOAD) == STATE_UNLOADED,
              "showing the prim loads no payload (state %r)" % (row_state(first, PAYLOAD),))
        check(bar() is not None, "the viewer and its bar stay shown beside the opened panel")
        yield ("sleep", SETTLE_MS)
        shot("scene_breadcrumbs_show_in_graph_opened.png")

        pin_to(first, other)
        ok = yield ("until", lambda: bool(first.property("natronSceneGraphPinned"))
                    and str(first.property("natronSceneGraphNode")) == other.getScriptName(), POLL_TIMEOUT_S)
        require(ok, "the panel is pinned to the second USDRead")
        count = len(panels())

        select([THING])
        ok = yield ("until", lambda: segment_button("a_thing") is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows a_thing")
        show_in_graph("a_thing")
        ok = yield ("until", lambda: len(panels()) == count + 1, POLL_TIMEOUT_S)
        require(ok, "with the only panel pinned, a second panel opens (%d, then %d)" % (count, len(panels())))
        second = [p for p in panels() if str(p.objectName()) != first_name][0]
        ok = yield ("until", lambda: second.isVisible() and selected_paths(second) == [THING], POLL_TIMEOUT_S)
        check(ok, "the new panel has /geo/group_1/a_thing selected (got %r)" % (selected_paths(second),))
        check(str(second.property("natronSceneGraphFollow")) == "viewer" and not bool(second.property("natronSceneGraphPinned")),
              "the new panel follows the viewer without a pin")
        check(str(second.property("natronSceneGraphNode")) == usd.getScriptName(),
              "the new panel shows the viewer's node (got %r)" % (second.property("natronSceneGraphNode"),))
        check(row_visible(second, THING), "the a_thing row is visible in the new panel")
        check(bool(first.property("natronSceneGraphPinned")) and str(first.property("natronSceneGraphNode")) == other.getScriptName(),
              "the first panel keeps its pin and node")
        yield ("sleep", SETTLE_MS)
        shot("scene_breadcrumbs_show_in_graph_pinned.png")

        select([SIBLING])
        ok = yield ("until", lambda: segment_button("group_2") is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows group_2")
        show_in_graph("group_2")
        ok = yield ("until", lambda: selected_paths(second) == [SIBLING], POLL_TIMEOUT_S)
        check(ok, "a second use reveals the prim in the same panel (got %r)" % (selected_paths(second),))
        check(len(panels()) == count + 1, "no third panel opens (%d)" % (len(panels()),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
