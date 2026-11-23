"""The 3D viewer's polygon budget demotes the farthest units to budget boxes once the camera rests.

Graph: USDRead of a generated scene into Viewer1, drawn Full under Storm. The scene is a 5 x 5 grid of
400-face meshes and one PointInstancer of 50 instances of a 100-face mesh, 15,000 faces in all. The
view's natronSceneBudget property publishes what the applied budget did (counts, the camera it was
solved for, the demoted, culled and fractioned units while natronSceneRecordBudget is set), and
natronSceneBoxes lists the budget boxes drawn while natronSceneRecordBoxes is set. The script checks:
  - with the default limit nothing is demoted, and the budget applies to Storm's filter;
  - a lower Polygon Limit demotes the farthest units first, and leaves at most the limit drawn;
  - Count Whole Scene counts the units outside the view too;
  - Cull Distance culls the units beyond it, and draws no budget box for them;
  - the instances Fraction Distance turns into budget boxes are the same from two cameras;
  - each Budget Boxes value draws its kind of budget box, or nothing for Off, with one screenshot each;
  - the HUD line reads the budget's numbers, and names a unit that alone exceeds the limit;
  - a click on a budget box selects its prim;
  - a row set to Full is demoted like any other;
  - moving the camera holds the applied budget, which changes once the camera rests;
  - the Polygon Budget submenu edits the Viewer's knobs, and Edit > Undo takes an edit back;
  - turning the budget off restores every unit and hides the HUD line.

Screenshots for sign-off: scene-budget-none, scene-budget-boxes-box-wire,
scene-budget-boxes-box-shaded, scene-budget-boxes-marker, scene-budget-boxes-off,
scene-budget-menu, scene-budget-off.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import math
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
GRID = 5
GRID_SPACING = 3.0
GRID_CELLS = 20
INSTANCES = 50
INSTANCE_SPACING = 2.5
INSTANCE_Z = -20.0
INSTANCE_CELLS = 10
MESH_FACES = GRID_CELLS * GRID_CELLS
INSTANCE_FACES = INSTANCE_CELLS * INSTANCE_CELLS
TOTAL_FACES = GRID * GRID * MESH_FACES + INSTANCES * INSTANCE_FACES
DEFAULT_LIMIT = 20000000
# Every unit is a 2 x 2 plane; its bounds reach this far from its centre.
UNIT_RADIUS = math.sqrt(2.0)
INSTANCER = "/World/Inst"
PROTOTYPE = "/World/Inst/Protos/Tile"
BUDGET_BOXES = [("box-wire", 0), ("box-shaded", 1), ("marker", 2), ("off", 3)]

try:
    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QMainWindow, QSpinBox, QWidgetAction
    from pxr import Gf, Sdf, Usd, UsdGeom, Vt

    from guitest import (OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    def plane(stage, path, cells):
        mesh = UsdGeom.Mesh.Define(stage, path)
        points = []
        for j in range(cells + 1):
            for i in range(cells + 1):
                points.append(Gf.Vec3f(-1.0 + 2.0 * i / cells, 0.0, -1.0 + 2.0 * j / cells))
        counts = []
        indices = []
        for j in range(cells):
            for i in range(cells):
                a = j * (cells + 1) + i
                counts.append(4)
                indices.extend([a, a + 1, a + cells + 2, a + cells + 1])
        mesh.CreatePointsAttr(Vt.Vec3fArray(points))
        mesh.CreateFaceVertexCountsAttr(Vt.IntArray(counts))
        mesh.CreateFaceVertexIndicesAttr(Vt.IntArray(indices))
        mesh.CreateExtentAttr(Vt.Vec3fArray([Gf.Vec3f(-1, 0, -1), Gf.Vec3f(1, 0, 1)]))
        return mesh

    def mesh_path(i, j):
        return "/World/Grid/M%d_%d" % (i, j)

    # The centre of every unit, an instance named "instancer[index]".
    CENTRES = {}
    for i in range(GRID):
        for j in range(GRID):
            CENTRES[mesh_path(i, j)] = (i * GRID_SPACING, 0.0, -j * GRID_SPACING)
    for k in range(INSTANCES):
        CENTRES["%s[%d]" % (INSTANCER, k)] = (k * INSTANCE_SPACING - 30.0, 0.0, INSTANCE_Z)

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_viewer_budget"))
    os.makedirs(fixture_dir, exist_ok=True)
    scene_path = os.path.join(fixture_dir, "budget_scene.usda")
    stage = Usd.Stage.CreateNew(scene_path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.Xform.Define(stage, "/World")
    UsdGeom.Xform.Define(stage, "/World/Grid")
    for i in range(GRID):
        for j in range(GRID):
            m = plane(stage, mesh_path(i, j), GRID_CELLS)
            m.AddTranslateOp().Set(Gf.Vec3d(*CENTRES[mesh_path(i, j)]))
    instancer = UsdGeom.PointInstancer.Define(stage, INSTANCER)
    plane(stage, PROTOTYPE, INSTANCE_CELLS)
    instancer.CreatePrototypesRel().SetTargets([Sdf.Path(PROTOTYPE)])
    instancer.CreateProtoIndicesAttr(Vt.IntArray([0] * INSTANCES))
    instancer.CreatePositionsAttr(Vt.Vec3fArray([Gf.Vec3f(*CENTRES["%s[%d]" % (INSTANCER, k)]) for k in range(INSTANCES)]))
    stage.GetRootLayer().Save()
    stage = None

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
    for name in ("polygonBudget", "polygonLimit", "countWholeScene", "cullDistance", "fractionDistance",
                 "fractionPercent", "budgetBoxes"):
        require(viewer.getParam(name) is not None, "the Viewer has a %s knob" % name)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if not views:
            return None
        v = views[0]
        if not v.property("natronSceneRecordBudget") or not v.property("natronSceneRecordBoxes"):
            v.setProperty("natronSceneRecordBudget", True)
            v.setProperty("natronSceneRecordBoxes", True)
            v.update()
        return v

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def budget():
        v = view()
        value = v.property("natronSceneBudget") if v is not None else None
        return dict(value) if value else {}

    def hud():
        v = view()
        return str(v.property("natronSceneBudgetHud") or "") if v is not None else ""

    def budget_boxes():
        """The budget boxes the last frame drew: [path, kind, x, y] each."""
        v = view()
        boxes = v.property("natronSceneBoxes") if v is not None else None
        return [(str(b[0]), str(b[1]), float(b[3]), float(b[4])) for b in (boxes or []) if str(b[1]).startswith("budgetBox")]

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def distance(path, eye):
        c = CENTRES[path]
        return math.sqrt(sum((c[a] - eye[a]) ** 2 for a in range(3)))

    def eye():
        e = budget().get("eye") or [0.0, 0.0, 0.0]
        return [float(x) for x in e]

    def paths(key):
        return sorted(str(p) for p in (budget().get(key) or []))

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def solved():
        b = budget()
        # A recount of the units bumps the engine's serial without a new request, so an applied budget
        # answers the latest request when its serial is at least the requested one.
        return (int(b.get("serial", 0)) > 0 and int(b.get("serial", 0)) >= int(b.get("requested", 0))
                and not b.get("idlePending", False))

    def resolve(label, edit):
        """Runs edit, then waits for the budget it brings to reach the screen."""
        before = int(budget().get("requested", 0))
        edit()
        ok = yield ("until", lambda: int(budget().get("requested", 0)) > before and solved(), POLL_TIMEOUT_S)
        check(ok, "%s: a new budget is solved and applied (%r)" % (label, dict((k, budget().get(k)) for k in
                                                                              ("serial", "requested", "idlePending"))))
        yield settle()

    def set_knob(name, value):
        return lambda: viewer.getParam(name).setValue(value)

    def wheel(v, notches=1):
        centre = QPointF(v.rect().center())
        event = QWheelEvent(centre, QPointF(v.mapToGlobal(v.rect().center())), QPoint(0, 0), QPoint(0, 120 * notches),
                            Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
        QApplication.sendEvent(v, event)
        QApplication.processEvents()

    def mouse(v, kind, pos, button, buttons):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons,
                                              Qt.NoModifier))

    def click(v, pos):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def frame_all():
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, Qt.Key_A)
        QApplication.processEvents()

    def open_menu(on_menu):
        """menu.exec() runs a nested event loop, so on_menu is scheduled before it and runs inside it,
        fetching the menu fresh; it must close the menu itself."""
        state = {}

        def fire():
            try:
                on_menu()
            except Exception:
                check(False, "menu handler error: " + traceback.format_exc())
                close_menus()

        def go():
            v = view()
            centre = v.rect().center()
            QTimer.singleShot(500, fire)
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Keyboard, centre, v.mapToGlobal(centre),
                                                        Qt.NoModifier))
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

    def close_menus():
        for _ in range(4):
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()

    def open_budget_submenu():
        """Opens the Polygon Budget submenu from the open context menu with a Right key press and
        returns it as the active popup. A wrapper from findChild() or menu() is marked deleted once
        the wrapper it came from is collected, so only the static activePopupWidget() is used."""
        top = QApplication.activePopupWidget()
        if top is None:
            return None
        entry = next((a for a in top.actions() if a.text().replace("&", "") == "Polygon Budget"), None)
        if entry is None:
            return None
        top.setActiveAction(entry)
        QApplication.processEvents()
        QTest.keyClick(top, Qt.Key_Right)
        QApplication.processEvents()
        sub = QApplication.activePopupWidget()
        return sub if sub is not None and sub.objectName() == "polygonBudgetMenu" else None

    def edit_menu_action(prefix):
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui" and w.menuBar() is not None:
                for act in w.menuBar().actions():
                    menu = menu_of(act)
                    if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                        for a in menu.actions():
                            if a.text().replace("&", "").lower().startswith(prefix):
                                return a
        return None

    def save(name):
        shot(name + ".png")

    def check_farthest_first(label):
        e = eye()
        demoted = paths("demotedPaths")
        kept = [p for p in CENTRES if p not in demoted and p not in paths("culledPaths")]
        if not check(len(demoted) > 0 and len(kept) > 0, "%s: some units are demoted and some kept (%d, %d)" %
                     (label, len(demoted), len(kept))):
            return
        nearest_demoted = min(distance(p, e) for p in demoted)
        farthest_kept = max(distance(p, e) for p in kept)
        check(nearest_demoted + 2 * UNIT_RADIUS >= farthest_kept,
              "%s: no kept unit is farther than a demoted one (nearest demoted %.2f, farthest kept %.2f)" %
              (label, nearest_demoted, farthest_kept))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        frame_all()
        ok = yield ("until", solved, POLL_TIMEOUT_S)
        if not ok:
            b = budget()
            # solved: the worker's latest budget, applied or not; countsDiscarded: unit counts dropped as stale.
            report("NOTE budget worker: requested %r, solved %r, applied %r, counts discarded %r, camera serial %r" %
                   (b.get("requested"), b.get("solved"), b.get("serial"), b.get("countsDiscarded"), b.get("cameraSerial")))
        require(ok, "the default budget is solved once the camera rests (%r)" % (budget(),))

        section("default limit")
        b = budget()
        check(b.get("on") is True, "the budget is on by default")
        check(b.get("bound") is True, "Storm's filter takes the budget")
        check(int(b.get("limit", 0)) == DEFAULT_LIMIT, "the limit is the default %d (%r)" % (DEFAULT_LIMIT, b.get("limit")))
        check(int(b.get("demoted", -1)) == 0 and int(b.get("culled", -1)) == 0,
              "nothing is demoted or culled under the default limit (%r, %r)" % (b.get("demoted"), b.get("culled")))
        check(budget_boxes() == [], "no budget box is drawn (%r)" % budget_boxes()[:3])
        save("scene-budget-none")

        section("limit")
        limit = TOTAL_FACES // 2
        yield from resolve("Polygon Limit %d" % limit, set_knob("polygonLimit", limit))
        b = budget()
        check(int(b.get("drawn", TOTAL_FACES)) <= limit, "at most the limit is drawn (%r)" % b.get("drawn"))
        check(int(b.get("demoted", 0)) == len(paths("demotedPaths")), "the demoted count matches the demoted units")
        check(int(b.get("hidden", 0)) + int(b.get("masked", 0)) > 0, "the filter hides the demoted units (%r, %r)" %
              (b.get("hidden"), b.get("masked")))
        check_farthest_first("Polygon Limit %d" % limit)
        drawn_paths = set(p for p, _, _, _ in budget_boxes())
        demoted = paths("demotedPaths")
        check(all((p in drawn_paths) or p.startswith(INSTANCER + "[") for p in demoted),
              "every demoted mesh draws a budget box (%r missing)" % [p for p in demoted if p not in drawn_paths][:3])

        section("hud")
        b = budget()
        expected = "Budget: %d / %d polys, %d demoted, %d culled" % (int(b["drawn"]), int(b["limit"]), int(b["demoted"]),
                                                                    int(b["culled"]))
        check(hud() == expected, "the HUD line reads %r (%r)" % (expected, hud()))
        yield from resolve("Polygon Limit 100", set_knob("polygonLimit", 100))
        check("alone exceeds the limit" in hud(), "the HUD names a unit that alone exceeds the limit (%r)" % hud())
        alone = str(budget().get("aloneOverLimit", ""))
        check(alone != "" and alone in hud(), "the HUD names %r (%r)" % (alone, hud()))

        section("picking")
        boxes = budget_boxes()
        if check(len(boxes) > 0, "budget boxes are drawn with every unit over the limit"):
            budget_box_paths = set(p for p, _, _, _ in boxes)
            pickable = set(p for p in budget_box_paths if not p.startswith(INSTANCER + "["))
            pickable.add(PROTOTYPE)
            target = next(((p, x, y) for p, kind, x, y in boxes if kind == "budgetBox"), None)
            if check(target is not None, "a mesh budget box is drawn"):
                click(view(), QPoint(int(round(target[1])), int(round(target[2]))))
                yield settle()
                picked = selection()
                check(len(picked) == 1 and picked[0] in pickable,
                      "a click on %s's budget box selects a budget box's prim (%r)" % (target[0], picked))
            target = next(((p, x, y) for p, kind, x, y in boxes if kind == "budgetBoxInstance"), None)
            if check(target is not None, "an instance budget box is drawn"):
                QMetaObject.invokeMethod(view(), "selectPaths", Q_ARG("QStringList", []))
                click(view(), QPoint(int(round(target[1])), int(round(target[2]))))
                yield settle()
                picked = selection()
                check(len(picked) == 1 and picked[0] in pickable,
                      "a click on %s's budget box selects a budget box's prim (%r)" % (target[0], picked))
            QMetaObject.invokeMethod(view(), "selectPaths", Q_ARG("QStringList", []))

        section("explicit full row")
        yield from resolve("Polygon Limit %d" % limit, set_knob("polygonLimit", limit))
        e = eye()
        farthest = max((p for p in CENTRES if not p.startswith(INSTANCER + "[")), key=lambda p: distance(p, e))
        require(QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", farthest), Q_ARG("QString", "full")),
                "set %s to Full" % farthest)
        yield from resolve("Full on %s" % farthest, lambda: viewer.getParam("polygonLimit").setValue(limit - 1))
        check(farthest in paths("demotedPaths"), "%s, set to Full, is still demoted (%r)" % (farthest, paths("demotedPaths")[:5]))
        QMetaObject.invokeMethod(view(), "clearDrawState", Q_ARG("QString", farthest))

        section("budget boxes")
        for label, value in BUDGET_BOXES:
            yield from resolve("Budget Boxes %s" % label, lambda value=value: (
                viewer.getParam("budgetBoxes").setValue(value), viewer.getParam("polygonLimit").setValue(limit + value)))
            b = budget()
            boxes_drawn, instances, markers = (int(b.get("budgetBoxCount", 0)), int(b.get("budgetBoxInstances", 0)),
                                               int(b.get("budgetBoxMarkers", 0)))
            if label == "off":
                check(boxes_drawn == 0 and instances == 0 and markers == 0, "Off draws no budget box (%r, %r, %r)" %
                      (boxes_drawn, instances, markers))
            elif label == "marker":
                check(boxes_drawn + instances > 0 and markers == boxes_drawn + instances,
                      "Marker draws every budget box as a marker (%r, %r, %r)" % (boxes_drawn, instances, markers))
            else:
                check(boxes_drawn + instances > 0 and markers == 0, "%s draws boxes and no marker (%r, %r, %r)" %
                      (label, boxes_drawn, instances, markers))
            save("scene-budget-boxes-" + label)
        viewer.getParam("budgetBoxes").setValue(0)

        section("count whole scene")
        # Each notch closes 15% of the distance; the framed scene sits well inside the view until the
        # camera is near its middle, so dolly in far enough that part of it leaves the frustum.
        for _ in range(20):
            wheel(view())
        yield from resolve("Count Whole Scene off", set_knob("polygonLimit", DEFAULT_LIMIT))
        in_view = int(budget().get("counted", 0))
        yield from resolve("Count Whole Scene on", set_knob("countWholeScene", True))
        whole = int(budget().get("counted", 0))
        check(whole == TOTAL_FACES, "Count Whole Scene counts all %d faces (%d)" % (TOTAL_FACES, whole))
        check(in_view < whole, "without it only the faces in view count (%d < %d)" % (in_view, whole))
        yield from resolve("Count Whole Scene off again", set_knob("countWholeScene", False))
        frame_all()
        ok = yield ("until", lambda: solved() and not budget().get("idlePending", True), POLL_TIMEOUT_S)
        check(ok, "the framed camera's budget is solved")

        section("cull distance")
        e = eye()
        ordered = sorted(distance(p, e) for p in CENTRES)
        cull = ordered[len(ordered) // 2]
        yield from resolve("Cull Distance %.2f" % cull, set_knob("cullDistance", cull))
        e = eye()
        culled = paths("culledPaths")
        check(len(culled) > 0 and int(budget().get("culled", 0)) == len(culled),
              "units beyond %.2f are culled (%d, count %r)" % (cull, len(culled), budget().get("culled")))
        check(all(distance(p, e) + UNIT_RADIUS >= cull for p in culled), "no culled unit is nearer than the distance")
        check(all(distance(p, e) - UNIT_RADIUS <= cull for p in CENTRES if p not in culled),
              "every unit well beyond the distance is culled")
        drawn_paths = set(p for p, _, _, _ in budget_boxes())
        check(not any(p in drawn_paths for p in culled), "no budget box is drawn for a culled unit")
        yield from resolve("Cull Distance off", set_knob("cullDistance", 0.0))

        section("fraction")
        yield from resolve("Fraction Distance 0.001", set_knob("fractionDistance", 0.001))
        first = paths("fractionedPaths")
        expected_kept = INSTANCES * 25 // 100
        check(abs((INSTANCES - len(first)) - expected_kept) <= INSTANCES // 10,
              "Fraction Percent 25 keeps about %d of %d instances (%d)" % (expected_kept, INSTANCES, INSTANCES - len(first)))
        check(int(budget().get("facedMeshes", 0)) > 0, "the meshes beyond the distance drop faces (%r)" %
              budget().get("facedMeshes"))
        before = eye()

        def move_back():
            for _ in range(3):
                wheel(view(), -1)

        yield from resolve("Fraction from a second camera", move_back)
        second = paths("fractionedPaths")
        check(eye() != before, "the second camera is another one (%r, %r)" % (before, eye()))
        check(first == second, "the fractioned instances are the same from both cameras (%d vs %d, %r)" %
              (len(first), len(second), sorted(set(first) ^ set(second))[:5]))
        yield from resolve("Fraction Distance off", set_knob("fractionDistance", 0.0))

        section("camera moves")
        yield from resolve("Polygon Limit %d" % limit, set_knob("polygonLimit", limit))
        held = int(budget().get("serial", 0))
        changed_mid_move = False
        for _ in range(12):
            wheel(view())
            yield ("sleep", 100)
            if int(budget().get("serial", 0)) != held:
                changed_mid_move = True
        check(not changed_mid_move, "the applied budget holds while the camera moves")
        check(budget().get("idlePending") is True, "a solve waits for the camera to rest")
        ok = yield ("until", lambda: int(budget().get("serial", 0)) > held and solved(), POLL_TIMEOUT_S)
        check(ok, "a new budget is applied once the camera rests (%r -> %r)" % (held, budget().get("serial")))

        section("menu")

        def edit_in_menu():
            menu = open_budget_submenu()
            if not check(menu is not None, "the context menu has a Polygon Budget submenu"):
                close_menus()
                return
            names = set(a.objectName() for a in menu.actions())
            check({"polygonBudget", "countWholeScene"} <= names, "the submenu has the toggles (%r)" % sorted(names))
            box = menu.findChild(QSpinBox, "polygonLimit")
            if check(box is not None and box.value() == limit, "the submenu's Polygon Limit shows the knob (%r)" %
                     (box.value() if box is not None else None)):
                shot("scene-budget-menu.png")
                # The spin box passes Return on to the menu, which triggers its active action: make
                # that the box's row, as hovering it does, not the Polygon Budget toggle the Right key highlighted.
                row = box.parentWidget()
                menu.setActiveAction(next((a for a in menu.actions()
                                           if isinstance(a, QWidgetAction) and a.defaultWidget() == row), None))
                QApplication.processEvents()
                box.setFocus()
                box.selectAll()
                QTest.keyClicks(box, "1234")
                QTest.keyClick(box, Qt.Key_Return)
                QApplication.processEvents()
            close_menus()

        state = open_menu(edit_in_menu)
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield settle()
        check(viewer.getParam("polygonLimit").getValue() == 1234, "the menu edit sets the knob (%r)" %
              viewer.getParam("polygonLimit").getValue())
        check(viewer.getParam("polygonBudget").getValue(), "the menu edit leaves the budget on")
        undo = edit_menu_action("undo")
        if check(undo is not None and undo.isEnabled(), "Edit > Undo is enabled after the menu edit"):
            undo.trigger()
            QApplication.processEvents()
            yield settle()
            check(viewer.getParam("polygonLimit").getValue() == limit, "Undo puts the limit back (%r)" %
                  viewer.getParam("polygonLimit").getValue())

        section("budget off")
        viewer.getParam("polygonBudget").setValue(False)
        ok = yield ("until", lambda: int(budget().get("serial", -1)) == 0 and budget().get("on") is False, POLL_TIMEOUT_S)
        check(ok, "turning the budget off clears the applied budget (%r)" % budget().get("serial"))
        yield settle()
        b = budget()
        check(int(b.get("hidden", -1)) == 0 and int(b.get("masked", -1)) == 0 and int(b.get("facedMeshes", -1)) == 0,
              "the filter hides nothing (%r, %r, %r)" % (b.get("hidden"), b.get("masked"), b.get("facedMeshes")))
        check(budget_boxes() == [], "no budget box is drawn (%r)" % budget_boxes()[:3])
        check(hud() == "", "the HUD line is hidden (%r)" % hud())
        save("scene-budget-off")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
