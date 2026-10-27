"""The four primitive nodes compose meshes the Viewer's 3D view draws, shaded and in wireframe.

Graph: USDCard1, USDSphere1, USDCube1 and USDCylinder1, each into its own USDXformPrim
(Card, Sphere, Cube and Cylinder, at translate x -4.5, -1.5, 1.5 and 4.5), all into USDMerge1, into
Viewer1. The composed stage is read by flattening the merge to a .usda through a probe USDWrite
and opening it with pxr.

Checks:
 1. the stage has a Mesh under each USDXformPrim, at /Card/USDCard1 and so on, with an st primvar
    and the face count of the node's defaults (card 8 x 8, sphere 30 x 30, cube 6, cylinder 30 x 30
    sides plus two 30-triangle caps);
 2. each mesh's world bounds are centred on its USDXformPrim's translate;
 3. the scene view shows the scene with no error in Shaded and in Wireframe draw modes, chosen
    from its context menu, and the two differ.

Screenshots: scene_primitives_shaded.png and scene_primitives_wireframe.png (full window), each
with a _viewer.png crop of the scene view.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0

# Plugin ID, the USDXformPrim's name, its x translate and the mesh's face count at the defaults.
PRIMITIVES = [
    ("USDCard", "Card", -4.5, 8 * 8),
    ("USDSphere", "Sphere", -1.5, 30 * 30),
    ("USDCube", "Cube", 1.5, 6),
    ("USDCylinder", "Cylinder", 4.5, 30 * 30 + 2 * 30),
]

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication
    from pxr import Usd, UsdGeom

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    merge = make("USDMerge", 450, 200)
    shapes = []
    xforms = []
    for i, (plugin_id, name, offset, _) in enumerate(PRIMITIVES):
        shape = make(plugin_id, 300 * i, 0)
        xform = make("USDXformPrim", 300 * i, 100)
        require(xform.connectInput(0, shape), "connect %s into its USDXformPrim" % shape.getScriptName())
        require(merge.connectInput(i, xform), "connect the %s USDXformPrim into the merge" % name)
        xform.setScriptName(name)
        translate = xform.getParam("translate")
        require(translate is not None, "USDXformPrim has a translate knob")
        translate.set(offset, 0.0, 0.0)
        shapes.append(shape)
        xforms.append(xform)
    report("NOTE primitive node names %r" % [s.getScriptName() for s in shapes])

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(450, 300)

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 750, 200)
    counter = [0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def crop():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def save_shots(name):
        shot(name + ".png")
        image = crop()
        if image is not None:
            image.save(os.path.join(OUT, name + "_viewer.png"))
            report("SHOT " + os.path.join(OUT, name + "_viewer.png"))
        return image

    def read_stage(source):
        """Flattens source to a fresh .usda and opens it with pxr."""
        counter[0] += 1
        path = os.path.join(work_dir, "primitives%d.usda" % counter[0])
        probe.disconnectInput(0)
        require(probe.connectInput(0, source), "connect the probe writer to %s" % source.getScriptName())
        probe.getParam("filename").setValue(path)
        probe.getParam("exportMode").setValue(1)  # flattened
        if os.path.exists(path):
            os.remove(path)
        app.renderBlocking(probe, 1, 1)
        return Usd.Stage.Open(path) if os.path.isfile(path) else None

    def open_context_menu():
        """Right-clicks the scene view's centre on the next event loop turn, as the window system
        would; the returned dict gets "closed" once the menu's exec returns."""
        v = view()
        outcome = {}
        if v is None:
            return outcome
        centre = v.rect().center()

        def open_menu():
            press = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(v.mapToGlobal(centre)),
                                Qt.RightButton, Qt.RightButton, Qt.NoModifier)
            QApplication.sendEvent(v, press)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            outcome["closed"] = True

        QTimer.singleShot(0, open_menu)
        return outcome

    def steps():
        stage = read_stage(merge)
        require(stage is not None, "the merge flattens to a stage")
        for (plugin_id, name, offset, faces), shape in zip(PRIMITIVES, shapes):
            path = "/%s/%s" % (name, shape.getScriptName())
            mesh = UsdGeom.Mesh(stage.GetPrimAtPath(path))
            check(bool(mesh), "%s composes a Mesh at %s" % (plugin_id, path))
            if not mesh:
                continue
            counts = mesh.GetFaceVertexCountsAttr().Get() or []
            check(len(counts) == faces, "%s has %d faces (%d)" % (plugin_id, faces, len(counts)))
            st = UsdGeom.PrimvarsAPI(mesh.GetPrim()).GetPrimvar("st")
            check(st.IsDefined() and len(st.Get() or []) > 0, "%s has an st primvar" % plugin_id)
            box = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_]).ComputeWorldBound(mesh.GetPrim())
            mid = box.ComputeAlignedRange().GetMidpoint()
            check(abs(mid[0] - offset) < 1e-4 and abs(mid[1]) < 1e-4 and abs(mid[2]) < 1e-4,
                  "%s is centred on its transform (%r)" % (plugin_id, tuple(mid)))

        require(viewer.connectInput(0, merge), "connect the Viewer to the USDMerge")
        ok = yield ("until", lambda: view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the primitives (%r)" % (v.property("natronSceneError") if v else None,))
        QTest.keyClick(v, Qt.Key_F)
        yield ("sleep", SETTLE_MS)

        images = {}
        for label in ("Shaded", "Wireframe"):
            outcome = open_context_menu()
            ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
            check(ok, "a right-click opens the context menu for %s" % label)
            if not ok:
                continue
            popup = QApplication.activePopupWidget()
            actions = [a for a in popup.actions() if a.text().replace("&", "") == label]
            check(len(actions) == 1, "the context menu offers %s" % label)
            if actions:
                actions[0].trigger()
            QTest.keyClick(popup, Qt.Key_Escape)
            ok = yield ("until", lambda: outcome.get("closed", False) and QApplication.activePopupWidget() is None, POLL_TIMEOUT_S)
            require(ok, "the context menu closes")
            view().update()
            ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
            check(ok, "%s: the scene view shows the primitives with no error" % label)
            yield ("sleep", SETTLE_MS)
            images[label] = save_shots("scene_primitives_" + label.lower())

        shaded = images.get("Shaded")
        wireframe = images.get("Wireframe")
        check(shaded is not None and wireframe is not None and shaded != wireframe,
              "the wireframe view differs from the shaded one")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
