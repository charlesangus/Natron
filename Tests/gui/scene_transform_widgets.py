"""The scene view's transform widget: T, R and E pick the translate, rotate and scale tools, Y cycles
them, Q and Esc go back to select, and D toggles local and world space.

Graph: USDRead(scene-reference.usda) into USDXformPrim Rig (Prim Path /Rig, rotate Y 30) into
Viewer1, with the Nuke navigation preset, so /Rig/World/Box is turned 30 degrees about Y. The view's
natronSceneTransformRecordOnly property is set: drags are recorded into natronSceneTransformDelta
(tool, prim paths, the 16 values of the world-space delta, row by row, Gf row vectors) and nothing is
written. The handles drawn are read from natronSceneTransformHandles (handle, x, y in widget
coordinates; handle = kind * 3 + axis, kind 0 translate, 1 rotate, 2 scale) and the frame from
natronSceneTransformFrame (origin, then X, Y and Z axes).

Checks:
  - the select tool draws no handles; T, R and E draw only their own kind;
  - T then a 100 px drag from the X arrow moves along X only;
  - R then a drag on the X ring turns about X only;
  - E then a drag on the X box scales along X only;
  - D switches the frame to the box's own axes, turned 30 degrees about Y, and a drag of the X arrow
    then moves along that axis;
  - Y cycles translate, rotate, scale; Q and Esc return to select;
  - the context menu's Transform submenu lists the tools with their keys.

Screenshots: scene-transform-{translate,rotate,scale,local}.png (full window) and a _viewer.png crop
of the scene view for each, for sign-off.

Mouse and key events are sent to the SceneViewport. Run through Tests/gui/run-gui-test.sh.
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0
only if every check passed.
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
GRID_COLUMNS = 24
GRID_ROWS = 14
NUKE_PRESET = 0
RIG_ANGLE = 30.0
BOX = "/Rig/World/Box"
TOL = 1e-6

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import FIXTURES, OUT, check, finish, menu_of, plugin_problem, report, require, run, shot, widgets_of

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE_PRESET)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("filename").setValue(scene_path)
    rig = make("USDXformPrim", 0, 100)
    rig.getParam("primPath").setValue("/Rig")
    rig.getParam("rotate").set(0.0, RIG_ANGLE, 0.0)
    require(rig.connectInput(0, usd), "connect the USDRead into the USDXformPrim")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 250)

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

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        event = QMouseEvent(kind, QPointF(pos), v.mapToGlobal(QPointF(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def click(v, pos):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        return selection()

    def drag(v, start, end, steps=10):
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            mouse(v, QEvent.MouseMove, start + (end - start) * (float(i) / steps), Qt.NoButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

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

    def handles():
        v = view()
        value = v.property("natronSceneTransformHandles") if v is not None else None
        return dict((int(h[0]), QPointF(float(h[1]), float(h[2]))) for h in value) if value else {}

    def frame():
        v = view()
        value = v.property("natronSceneTransformFrame") if v is not None else None
        return [float(x) for x in value] if value else []

    def drags():
        return int(view().property("natronSceneTransformDrags") or 0)

    def last_delta():
        value = view().property("natronSceneTransformDelta")
        if not value:
            return None, [], []
        return str(value[0]), [str(p) for p in value[1]], [float(x) for x in value[2]]

    def tool():
        return str(view().property("natronSceneTransformTool"))

    def handle_kinds():
        return sorted(set(h // 3 for h in handles()))

    def near(a, b, tol=TOL):
        return abs(a - b) <= tol * max(1.0, abs(b))

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

    def drag_handle(v, handle, offset):
        """Drags handle by offset pixels and returns the recorded delta, or None if nothing was recorded."""
        pixels = handles()
        if handle not in pixels:
            return None
        before = drags()
        start = pixels[handle]
        drag(v, start, start + offset)
        if drags() != before + 1:
            return None
        return last_delta()

    def steps():
        require(viewer.connectInput(0, rig), "connect the Viewer to the USDXformPrim")
        ok = yield ("until", lambda: view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        v.setProperty("natronSceneTransformRecordOnly", True)
        key(v, Qt.Key_G, Qt.ControlModifier | Qt.ShiftModifier)
        key(v, Qt.Key_A)
        yield settle()

        # Where the box is drawn, by clicking a grid of points with the select tool.
        check(tool() == "select", "the select tool is the first (got %r)" % (tool(),))
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
        require(click(v, box) == [BOX], "a click selects the box (got %r)" % (selection(),))
        yield settle()
        check(handles() == {}, "the select tool draws no handles (got %r)" % (handles(),))

        # Translate.
        key(v, Qt.Key_T)
        yield settle()
        check(tool() == "translate", "T picks the translate tool (got %r)" % (tool(),))
        check(handle_kinds() == [0], "the translate tool draws arrows only (kinds %r)" % (handle_kinds(),))
        check(view().property("natronSceneTransformSpace") == "world", "the frame starts in world space")
        world_frame = frame()
        check(len(world_frame) == 12 and all(near(world_frame[3 + i], 1.0 if i in (0, 4, 8) else 0.0) for i in range(9)),
              "the world frame's axes are the world's (%r)" % (world_frame,))
        save_shots("scene-transform-translate")
        name, paths, d = drag_handle(v, 0, QPointF(100.0, 0.0)) or (None, [], [])
        report("NOTE translate delta %r" % (d,))
        check(name == "translate" and paths == [BOX], "the drag is recorded as a translate of the box (%r, %r)" % (name, paths))
        check(len(d) == 16 and abs(d[12]) > 1e-3 and near(d[13], 0.0) and near(d[14], 0.0),
              "a 100 px drag of the X arrow moves along X only")
        check(len(d) == 16 and all(near(d[r * 4 + c], 1.0 if r == c else 0.0) for r in range(3) for c in range(3)),
              "the translate delta neither turns nor scales")

        # Rotate.
        key(v, Qt.Key_R)
        yield settle()
        check(tool() == "rotate", "R picks the rotate tool (got %r)" % (tool(),))
        check(handle_kinds() == [1], "the rotate tool draws rings only (kinds %r)" % (handle_kinds(),))
        save_shots("scene-transform-rotate")
        result = None
        for offset in (QPointF(0.0, 100.0), QPointF(100.0, 0.0)):
            result = drag_handle(v, 3, offset)
            if result and len(result[2]) == 16 and not near(result[2][5], 1.0):
                break
        name, paths, d = result or (None, [], [])
        report("NOTE rotate delta %r" % (d,))
        check(name == "rotate", "the drag is recorded as a rotate (%r)" % (name,))
        check(len(d) == 16 and near(d[0], 1.0) and near(d[1], 0.0) and near(d[2], 0.0) and near(d[4], 0.0) and near(d[8], 0.0)
              and not near(d[5], 1.0) and near(d[5], d[10]) and near(d[6], -d[9]),
              "a drag of the X ring turns about X only")

        # Scale.
        key(v, Qt.Key_E)
        yield settle()
        check(tool() == "scale", "E picks the scale tool (got %r)" % (tool(),))
        check(handle_kinds() == [2], "the scale tool draws boxes only (kinds %r)" % (handle_kinds(),))
        save_shots("scene-transform-scale")
        name, paths, d = drag_handle(v, 6, QPointF(100.0, 0.0)) or (None, [], [])
        if len(d) != 16 or near(d[0], 1.0):
            name, paths, d = drag_handle(v, 6, QPointF(0.0, -100.0)) or (None, [], [])
        report("NOTE scale delta %r" % (d,))
        check(name == "scale", "the drag is recorded as a scale (%r)" % (name,))
        check(len(d) == 16 and not near(d[0], 1.0) and d[0] > 0.0 and near(d[5], 1.0) and near(d[10], 1.0)
              and all(near(d[r * 4 + c], 0.0) for r in range(3) for c in range(3) if r != c),
              "a drag of the X box scales along X only")

        # Local space.
        key(v, Qt.Key_T)
        key(v, Qt.Key_D)
        yield settle()
        check(view().property("natronSceneTransformSpace") == "local", "D switches to local space")
        local_frame = frame()
        report("NOTE local frame %r" % (local_frame,))
        angle = math.radians(RIG_ANGLE)
        check(len(local_frame) == 12 and near(local_frame[3], math.cos(angle), 1e-4) and near(local_frame[4], 0.0, 1e-4)
              and near(abs(local_frame[5]), math.sin(angle), 1e-4),
              "the local frame's X axis is the box's, turned %g degrees about Y" % RIG_ANGLE)
        save_shots("scene-transform-local")
        name, paths, d = drag_handle(v, 0, QPointF(100.0, 0.0)) or (None, [], [])
        report("NOTE local translate delta %r" % (d,))
        moved = d[12:15] if len(d) == 16 else []
        length = math.sqrt(sum(x * x for x in moved)) if moved else 0.0
        check(length > 1e-3 and all(near(moved[i] / length, local_frame[3 + i], 1e-4) or near(-moved[i] / length, local_frame[3 + i], 1e-4)
                                    for i in range(3)),
              "in local space a drag of the X arrow moves along the box's X axis")
        key(v, Qt.Key_D)
        yield settle()
        check(view().property("natronSceneTransformSpace") == "world", "D switches back to world space")

        # Cycling and the select tool.
        cycled = []
        for _ in range(3):
            key(v, Qt.Key_Y)
            cycled.append(tool())
        check(cycled == ["rotate", "scale", "translate"], "Y cycles the tools from translate (got %r)" % (cycled,))
        key(v, Qt.Key_Q)
        check(tool() == "select", "Q picks the select tool (got %r)" % (tool(),))
        key(v, Qt.Key_T)
        key(v, Qt.Key_Escape)
        check(tool() == "select", "Esc picks the select tool (got %r)" % (tool(),))
        yield settle()
        check(handles() == {}, "no handles are drawn after Esc")

        # The context menu.
        menu = {}
        centre = v.rect().center()

        def open_menu():
            mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            menu["closed"] = True

        QTimer.singleShot(0, open_menu)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if check(ok, "a right-click opens the context menu"):
            popup = QApplication.activePopupWidget()
            transform = [a for a in popup.actions() if a.text().replace("&", "") == "Transform"]
            submenu = menu_of(transform[0]) if transform else None
            if check(submenu is not None, "the context menu has a Transform submenu"):
                texts = dict((a.text().replace("&", ""), a.shortcut().toString()) for a in submenu.actions())
                report("NOTE Transform submenu %r" % (texts,))
                check(texts.get("Translate Tool") == "T" and texts.get("Rotate Tool") == "R" and texts.get("Scale Tool") == "E",
                      "the submenu lists the tools with T, R and E")
                check("Local Space" in texts and "World Space" in texts, "the submenu lists the spaces")
            QTest.keyClick(popup, Qt.Key_Escape)
        ok = yield ("until", lambda: menu.get("closed", False), POLL_TIMEOUT_S)
        check(ok, "Esc closes the context menu")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
