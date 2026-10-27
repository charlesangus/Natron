"""Where a drag of the scene view's transform widget writes, per the viewer3DTransformTarget preference.

Graph: USDLight Key (Prim Path /Key, translate 3 1 0) into USDXformPrim Rig (Prim Path /Rig, rotate
Y 30), and USDRead(scene-reference.usda); both into USDMerge Merge, into Viewer1. The light is the
prim /Rig/Key, made by a node with translate knobs under a re-parenting transform; /World/Box comes
from a file and has no owner node. Each drag is the translate tool's X arrow dragged 100 px.

Checks, per policy:
  0 Owner, Else Edit Node: dragging the light writes Key's translate and adds no node; the change,
    in Rig's turned space, moves the light along world X (the local delta is 30 degrees off X); one
    Edit > Undo restores Key's translate exactly. Dragging the box (no owner) inserts a
    USDEditTransform between Merge and the Viewer with pattern /World/Box; one Undo removes it and
    reconnects the Viewer to Merge.
  2 Always Edit Node: dragging the light inserts a USDEditTransform after Merge with pattern
    /Rig/Key and leaves Key alone; a second drag of the same selection reuses it (it is the viewed
    node); Undo twice restores the graph.
  1 Open Edit Node: with the settings panel of a USDEditTransform Open (pattern empty, between
    Merge and the Viewer) shown, dragging the light writes Open's translate and adds /Rig/Key to its
    pattern; one Undo restores both.

Screenshots: scene-policy-owner.png, scene-policy-edit-node.png, scene-policy-always.png,
scene-policy-open.png (full window), each with a _viewer.png crop of the scene view.

Mouse and key events are sent to the SceneViewport. Run through Tests/gui/run-gui-test.sh.
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0
only if every check passed.
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
GRID_COLUMNS = 24
GRID_ROWS = 14
NUKE_PRESET = 0
RIG_ANGLE = 30.0
LIGHT = "/Rig/Key"
BOX = "/World/Box"
POLICY_OWNER = 0
POLICY_OPEN_EDIT = 1
POLICY_ALWAYS_EDIT = 2
TRANSLATE_X = 0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow

    from guitest import (FIXTURES, OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot,
                         widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    settings = NatronEngine.natron.getSettings()
    navigation = settings.getParam("viewer3DNavigation")
    policy = settings.getParam("viewer3DTransformTarget")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    require(policy is not None, "Preferences have a viewer3DTransformTarget parameter")
    original_navigation = navigation.getValue()
    original_policy = policy.getValue()
    navigation.setValue(NUKE_PRESET)
    check(original_policy == POLICY_OWNER, "the policy defaults to Owner, Else Edit Node (got %r)" % (original_policy,))

    def make(plugin_id, name, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setScriptName(name)
        node.setPosition(x, y)
        return node

    key_light = make("USDLight", "Key", 0, 0)
    key_light.getParam("primPath").setValue("/Key")
    key_light.getParam("translate").set(3.0, 1.0, 0.0)
    rig = make("USDXformPrim", "Rig", 0, 100)
    rig.getParam("primPath").setValue("/Rig")
    rig.getParam("rotate").set(0.0, RIG_ANGLE, 0.0)
    usd = make("USDRead", "Read", 300, 100)
    usd.getParam("filename").setValue(scene_path)
    merge = make("USDMerge", "Merge", 150, 200)
    require(rig.connectInput(0, key_light), "connect Key into Rig")
    require(merge.connectInput(0, rig), "connect Rig into Merge")
    require(merge.connectInput(1, usd), "connect Read into Merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(150, 400)
    light_translate = key_light.getParam("translate")

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError") and v.property("natronSceneFreeCamera") is not None)

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def guides():
        v = view()
        value = v.property("natronSceneGuides") if v is not None else None
        return dict((str(e[0]), QPoint(int(round(float(e[2]))), int(round(float(e[3]))))) for e in value or [])

    def handles():
        v = view()
        value = v.property("natronSceneTransformHandles") if v is not None else None
        return dict((int(h[0]), QPointF(float(h[1]), float(h[2]))) for h in value) if value else {}

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), v.mapToGlobal(QPointF(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def click(v, pos):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        return selection()

    def key(v, k, modifiers=Qt.NoModifier):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def values(param):
        return [param.getValue(i) for i in range(3)]

    def edit_nodes():
        return sorted((n for n in app.getChildren() if n.getPluginID() == "USDEditTransform"), key=lambda n: n.getScriptName())

    def viewer_input():
        node = viewer.getInput(0)
        return node.getScriptName() if node is not None else None

    def crop():
        v = view()
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def save_shots(name):
        shot(name + ".png")
        image = crop()
        if image is not None:
            path = os.path.join(OUT, name + "_viewer.png")
            image.save(path)
            report("SHOT " + path)

    def edit_undo():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or class_name(win) != "Gui" or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                    for a in menu.actions():
                        if a.text().replace("&", "").lower().startswith("undo") and a.isEnabled():
                            report("NOTE undoing %r" % (a.text(),))
                            a.trigger()
                            QApplication.processEvents()
                            return True
        return False

    def node_graph_view():
        views = [w for w in widgets_of("NodeGraph") if w.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def wait_scene():
        """Waits for the scene the last edit asked for, then for a paint of it."""
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield settle()
        return ok

    def drag_x(v):
        """Drags the X arrow 100 px to the right; False when the arrow is not drawn."""
        pixels = handles()
        if TRANSLATE_X not in pixels:
            return False
        start = pixels[TRANSLATE_X]
        end = start + QPointF(100.0, 0.0)
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + (end - start) * (i / 10.0), Qt.NoButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        return True

    def select_light(v):
        point = guides().get(LIGHT)
        return point is not None and click(v, point) == [LIGHT]

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to Merge")
        ok = yield ("until", lambda: view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        key(v, Qt.Key_A)
        yield settle()

        # Where the box is drawn, with the select tool.
        sx = max(1, v.width() // GRID_COLUMNS)
        sy = max(1, v.height() // GRID_ROWS)
        on_box = []
        for row in range(GRID_ROWS):
            for column in range(GRID_COLUMNS):
                p = QPoint(sx // 2 + column * sx, sy // 2 + row * sy)
                if click(v, p) == [BOX]:
                    on_box.append(p)
            yield ("sleep", 0)
        require(on_box, "the grid finds %s" % BOX)
        cx = sum(p.x() for p in on_box) / float(len(on_box))
        cy = sum(p.y() for p in on_box) / float(len(on_box))
        box = min(on_box, key=lambda p: abs(p.x() - cx) + abs(p.y() - cy))
        key(v, Qt.Key_T)

        # Owner, else edit node: the light's own knobs.
        policy.setValue(POLICY_OWNER)
        require(select_light(v), "a click on the light guide selects %s (got %r)" % (LIGHT, selection()))
        yield settle()
        before = values(light_translate)
        require(drag_x(v), "the translate tool draws the X arrow on the light")
        after = values(light_translate)
        report("NOTE Key translate %r -> %r" % (before, after))
        moved = [a - b for a, b in zip(after, before)]
        check(math.hypot(moved[0], moved[2]) > 1e-3 and abs(moved[1]) < 1e-9, "the drag writes Key's translate")
        angle = math.degrees(math.atan2(moved[2], moved[0])) % 180.0
        check(abs(angle - RIG_ANGLE) < 0.5 or abs(angle - (180.0 - RIG_ANGLE)) < 0.5,
              "the change is in Rig's turned space, %g degrees off X (got %g)" % (RIG_ANGLE, angle))
        check(edit_nodes() == [] and viewer_input() == "Merge", "an owned prim adds no node")
        ok = yield from wait_scene()
        save_shots("scene-policy-owner")
        check(edit_undo(), "Edit > Undo is enabled after the drag")
        check(values(light_translate) == before, "one Undo restores Key's translate exactly (%r)" % (values(light_translate),))
        ok = yield from wait_scene()

        # Owner, else edit node: a file's prim has no owner.
        require(click(v, box) == [BOX], "a click selects the box (got %r)" % (selection(),))
        yield settle()
        require(drag_x(v), "the translate tool draws the X arrow on the box")
        created = edit_nodes()
        if check(len(created) == 1, "dragging the box creates one USDEditTransform (got %d)" % len(created)):
            node = created[0]
            check(node.getParam("primPattern").getValue() == BOX, "its pattern is the box (%r)" % (node.getParam("primPattern").getValue(),))
            check(abs(node.getParam("translate").getValue(0)) > 1e-3, "its translate moves the box")
            check(viewer_input() == node.getScriptName(), "the Viewer now shows it")
            source = node.getInput(0)
            check(source is not None and source.getScriptName() == "Merge", "it is inserted after Merge")
        ok = yield from wait_scene()
        save_shots("scene-policy-edit-node")
        check(edit_undo(), "Edit > Undo is enabled after the drag")
        ok = yield ("until", lambda: edit_nodes() == [], POLL_TIMEOUT_S)
        check(ok, "one Undo removes the created node")
        check(viewer_input() == "Merge", "and reconnects the Viewer to Merge (got %r)" % (viewer_input(),))
        ok = yield from wait_scene()

        # Always edit node.
        policy.setValue(POLICY_ALWAYS_EDIT)
        require(select_light(v), "a click on the light guide selects %s again" % LIGHT)
        yield settle()
        before = values(light_translate)
        require(drag_x(v), "the X arrow is drawn on the light")
        created = edit_nodes()
        check(len(created) == 1 and created[0].getParam("primPattern").getValue() == LIGHT,
              "the drag creates a USDEditTransform for %s" % LIGHT)
        check(values(light_translate) == before, "and leaves Key's translate alone")
        ok = yield from wait_scene()
        save_shots("scene-policy-always")
        if created:
            first = created[0].getParam("translate").getValue(0)
            require(drag_x(v), "the X arrow is drawn after the first drag")
            again = edit_nodes()
            check(len(again) == 1, "a second drag of the same selection reuses the node (%d nodes)" % len(again))
            check(abs(created[0].getParam("translate").getValue(0) - first) > 1e-3, "and moves it further")
            check(edit_undo(), "Edit > Undo undoes the second drag")
            check(abs(created[0].getParam("translate").getValue(0) - first) < 1e-9, "which restores the first drag's value")
            check(edit_undo(), "Edit > Undo undoes the first drag")
            ok = yield ("until", lambda: edit_nodes() == [], POLL_TIMEOUT_S)
            check(ok and viewer_input() == "Merge", "the graph is as before (%r)" % (viewer_input(),))
        ok = yield from wait_scene()

        # Open edit node.
        policy.setValue(POLICY_OPEN_EDIT)
        open_node = make("USDEditTransform", "Open", 150, 300)
        require(open_node.connectInput(0, merge), "connect Merge into Open")
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, open_node), "connect Open into the Viewer")
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        app.selectNode(open_node, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(open_node) is not None, POLL_TIMEOUT_S)
        require(ok, "open Open's settings panel")
        ok = yield from wait_scene()
        require(select_light(v), "a click on the light guide selects %s once more" % LIGHT)
        yield settle()
        before = values(light_translate)
        pattern = open_node.getParam("primPattern")
        open_translate = open_node.getParam("translate")
        pattern_before = pattern.getValue()
        open_before = values(open_translate)
        require(drag_x(v), "the X arrow is drawn on the light")
        check(abs(open_translate.getValue(0) - open_before[0]) > 1e-3 or abs(open_translate.getValue(2) - open_before[2]) > 1e-3,
              "the drag writes Open's translate")
        check(LIGHT in pattern.getValue().split(), "and adds %s to its pattern (%r)" % (LIGHT, pattern.getValue()))
        check(values(light_translate) == before and len(edit_nodes()) == 1, "and touches nothing else")
        ok = yield from wait_scene()
        save_shots("scene-policy-open")
        check(edit_undo(), "Edit > Undo is enabled after the drag")
        check(pattern.getValue() == pattern_before and values(open_translate) == open_before,
              "one Undo restores Open's pattern and translate")

        policy.setValue(original_policy)
        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
