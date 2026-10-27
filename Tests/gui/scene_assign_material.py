"""A shared material assigned by pattern stays bound when the asset is moved under a USDXformPrim.

Graph: USDRead(asset.usda, a box bound to a grey material of its own) and a red USDMaterial into a
USDAssignMaterial (Prim Pattern /Asset, the material on its Material input, Stronger Than
Descendants) into Viewer1. A probe USDWrite flattens the scene for pxr to check: the material
lands at /materials/Red and the box is bound to it. Then a USDXformPrim /T1 is inserted between
the assign and the Viewer, and the box at /T1/Asset/Box is still bound to /materials/Red. Material
Path is greyed while the Material input is connected.

Screenshots (full window, each 3D view one with a _viewer.png crop of the scene view):
scene_assign_material_before.png, scene_assign_material_moved.png, and
scene_assign_material_panel.png of the USDAssignMaterial panel.

The asset is written under $NATRON_GUI_TEST_OUT/work. Run through Tests/gui/run-gui-test.sh.
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status
0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0

ASSET = """#usda 1.0
def Xform "Asset"
{
    def Cube "Box" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        double size = 2
        rel material:binding = </Asset/Looks/Grey>
    }
    def Scope "Looks"
    {
        def Material "Grey"
        {
            token outputs:surface.connect = </Asset/Looks/Grey/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
                token outputs:surface
            }
        }
    }
}
"""

try:
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea
    from pxr import Usd, UsdShade

    from guitest import OUT, check, close_all_panels, finish, plugin_problem, report, require, run, shot, widgets_of

    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)
    asset_path = os.path.join(work_dir, "assign_material_asset.usda")
    with open(asset_path, "w") as f:
        f.write(ASSET)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    read = make("USDRead", 0, 0)
    read.getParam("filename").setValue(asset_path)
    red = make("USDMaterial", 250, 0)
    red.getParam("primPath").setValue("/Red")
    red.getParam("diffuseColor").set(0.8, 0.1, 0.1)
    assign = make("USDAssignMaterial", 0, 120)
    assign.getParam("primPattern").setValue("/Asset")
    material_path = assign.getParam("materialPath")
    require(material_path is not None, "USDAssignMaterial has a materialPath knob")
    for name in ("primPattern", "materialPath", "purpose", "strength"):
        check(assign.getParam(name) is not None, "USDAssignMaterial has a %s knob" % name)
    check(material_path.getIsEnabled(), "Material Path is enabled with no Material input")
    require(assign.connectInput(0, read), "connect the USDRead to the assign's Scene input")
    require(assign.connectInput(1, red), "connect the USDMaterial to the assign's Material input")
    check(not material_path.getIsEnabled(), "Material Path is greyed while the Material input is connected")
    moved = make("USDXformPrim", 0, 240)
    moved.getParam("primPath").setValue("/T1")
    moved.getParam("translate").set(1.5, 0.0, 0.0)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 360)

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 300, 240)
    probe.getParam("exportMode").setValue(1)  # flattened
    counter = [0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def save_shots(name):
        shot(name + ".png")
        v = view()
        if v is None:
            return
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        path = os.path.join(OUT, name + "_viewer.png")
        if image.copy(top_left.x(), top_left.y(), v.width(), v.height()).save(path):
            report("SHOT " + path)

    def read_stage(source):
        """Flattens source to a fresh .usda under the work directory and opens it with pxr."""
        counter[0] += 1
        path = os.path.join(work_dir, "assign_probe%d.usda" % counter[0])
        probe.disconnectInput(0)
        require(probe.connectInput(0, source), "connect the probe writer to %s" % source.getScriptName())
        probe.getParam("filename").setValue(path)
        if os.path.exists(path):
            os.remove(path)
        app.renderBlocking(probe, 1, 1)
        return Usd.Stage.Open(path) if os.path.isfile(path) else None

    def bound(stage, path):
        prim = stage.GetPrimAtPath(path)
        if not prim:
            return None
        material = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
        return str(material.GetPath()) if material else ""

    def node_graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
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

    def steps():
        require(viewer.connectInput(0, assign), "connect the Viewer to the assign")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the assigned scene")
        yield ("sleep", SETTLE_MS)
        stage = read_stage(assign)
        require(stage is not None, "the probe writer flattened the assigned scene")
        check(stage.GetPrimAtPath("/materials/Red").IsA(UsdShade.Material), "the material landed at /materials/Red")
        check(not stage.GetPrimAtPath("/Red"), "no material is left at /Red")
        check(bound(stage, "/Asset/Box") == "/materials/Red",
              "the box is bound to /materials/Red over its own grey (%r)" % bound(stage, "/Asset/Box"))
        save_shots("scene_assign_material_before")

        require(moved.connectInput(0, assign), "connect the assign into the USDXformPrim")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, moved), "connect the Viewer to the USDXformPrim")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the scene view shows the moved scene")
        yield ("sleep", SETTLE_MS)
        stage = read_stage(moved)
        require(stage is not None, "the probe writer flattened the moved scene")
        check(not stage.GetPrimAtPath("/T1/materials"), "the Materials Scope is not carried under /T1")
        check(bound(stage, "/T1/Asset/Box") == "/materials/Red",
              "the moved box is still bound to /materials/Red (%r)" % bound(stage, "/T1/Asset/Box"))
        save_shots("scene_assign_material_moved")

        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        close_all_panels()
        app.selectNode(assign, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(assign) is not None, POLL_TIMEOUT_S)
        require(ok, "open the USDAssignMaterial's settings panel")
        yield ("sleep", SETTLE_MS)
        panel = panel_of(assign)
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(content.sizeHint().height() + 4)
        yield ("sleep", SETTLE_MS)
        labels = [w.text().rstrip(":") for w in panel.findChildren(QLabel) if w.isVisible()]
        report("NOTE panel labels %r" % labels)
        for label in ("Prim Pattern", "Material Path", "Purpose", "Strength"):
            check(label in labels, "the panel shows %s" % label)
        check(not material_path.getIsEnabled(), "Material Path is still greyed in the panel")
        shot("scene_assign_material_panel.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
