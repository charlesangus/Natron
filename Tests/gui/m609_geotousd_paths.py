"""GeoToUSD set to Overlay puts edited geometry back at its original path.

Graph: USDRead1 (a scene written to the work dir: Xform /World translated by (5, 0, 0) holding the
mesh /World/A) -> USDToGeo (prim pattern /World/A, Unpack on) -> GeoTransform (translate y +2) ->
GeoToUSD (Geometry input; USDRead1 on its Scene input; Other Primitives Overlay) -> USDMerge input 0,
with USDRead2 (a second file holding /Other) on input 1. The composed stage is read by flattening
the merge to a .usda through a probe USDWrite and opening it with pxr.

Checks:
 1. the merged stage holds /World, /World/A and /Other, and no prim under the GeoToUSD's Path
    Prefix (/<node name>);
 2. /World/A is still a Mesh and /World keeps its (5, 0, 0) translate;
 3. /World/A's world-space points are the originals moved by (0, 2, 0): the edit, and only the edit;
 4. the GeoToUSD's node message lists /World/A as overlaid.

Screenshot: m609_paths.png (full window).

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

SETTLE_MS = 700
LIFT = 2.0

SCENE = """#usda 1.0

def Xform "World"
{
    double3 xformOp:translate = (5, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Mesh "A"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    }
}
"""

OTHER = """#usda 1.0

def Xform "Other"
{
}
"""

try:
    from pxr import Gf, Usd, UsdGeom

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot

    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)
    scene_path = os.path.join(work_dir, "m609_scene.usda")
    other_path = os.path.join(work_dir, "m609_other.usda")
    for path, text in ((scene_path, SCENE), (other_path, OTHER)):
        with open(path, "w") as f:
            f.write(text)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    read = make("USDRead", 0, 0)
    read.getParam("filename").setValue(scene_path)
    other = make("USDRead", 300, 0)
    other.getParam("filename").setValue(other_path)
    to_geo = make("USDToGeo", 0, 100)
    to_geo.getParam("primPattern").setValue("/World/A")
    to_geo.getParam("unpack").setValue(True)
    transform = make("GeoTransform", 0, 200)
    transform.getParam("translate").set(0.0, LIFT, 0.0)
    to_usd = make("GeoToUSD", 0, 300)
    other_primitives = to_usd.getParam("otherPrimitives")
    require(other_primitives is not None, "GeoToUSD has an otherPrimitives knob")
    other_primitives.setValue(1)  # Overlay
    prefix = to_usd.getParam("pathPrefix").getValue()
    report("NOTE GeoToUSD path prefix %r" % prefix)
    merge = make("USDMerge", 150, 400)
    require(to_geo.connectInput(0, read), "connect the USDToGeo to USDRead1")
    require(transform.connectInput(0, to_geo), "connect the GeoTransform to the USDToGeo")
    require(to_usd.connectInput(0, transform), "connect the GeoTransform to the GeoToUSD Geometry input")
    require(to_usd.connectInput(1, read), "connect USDRead1 to the GeoToUSD Scene input")
    require(merge.connectInput(0, to_usd), "connect the GeoToUSD to the USDMerge")
    require(merge.connectInput(1, other), "connect USDRead2 to the USDMerge")

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 450, 400)
    probe.getParam("exportMode").setValue(1)  # flattened

    def read_stage(source):
        """Flattens source to a fresh .usda and opens it with pxr."""
        path = os.path.join(work_dir, "m609_probe.usda")
        probe.disconnectInput(0)
        require(probe.connectInput(0, source), "connect the probe writer to %s" % source.getScriptName())
        probe.getParam("filename").setValue(path)
        if os.path.exists(path):
            os.remove(path)
        app.renderBlocking(probe, 1, 1)
        return Usd.Stage.Open(path) if os.path.isfile(path) else None

    def world_points(stage, path):
        prim = stage.GetPrimAtPath(path)
        world = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
        return [world.Transform(Gf.Vec3d(p)) for p in UsdGeom.Mesh(prim).GetPointsAttr().Get()]

    def near(a, b):
        return all(abs(x - y) < 1e-5 for x, y in zip(a, b))

    def steps():
        yield ("sleep", SETTLE_MS)
        stage = read_stage(merge)
        require(stage is not None, "the merge flattens to a file")
        paths = set(str(p.GetPath()) for p in stage.Traverse())
        report("NOTE merged prims %r" % sorted(paths))
        for path in ("/World", "/World/A", "/Other"):
            check(path in paths, "the merged stage has " + path)
        check(not any(p == prefix or p.startswith(prefix + "/") for p in paths),
              "nothing is written under the Path Prefix %s" % prefix)

        mesh = stage.GetPrimAtPath("/World/A")
        check(mesh.GetTypeName() == "Mesh", "/World/A is still a Mesh (%s)" % mesh.GetTypeName())
        parent = UsdGeom.Xformable(stage.GetPrimAtPath("/World")).GetLocalTransformation().ExtractTranslation()
        check(near(parent, (5.0, 0.0, 0.0)), "/World keeps its (5, 0, 0) translate (%r)" % (tuple(parent),))

        original = Usd.Stage.Open(scene_path)
        before = world_points(original, "/World/A")
        after = world_points(stage, "/World/A")
        check(len(before) == len(after), "/World/A keeps its point count (%d, %d)" % (len(before), len(after)))
        moved = all(near(b + Gf.Vec3d(0.0, LIFT, 0.0), a) for b, a in zip(before, after))
        check(moved, "/World/A's points are moved by the edit only (%r -> %r)" % (before, after))

        message = to_usd.getPersistentMessage() or ""
        check("/World/A" in message and "overlaid" in message, "the node message lists /World/A as overlaid (%r)" % message)
        shot("m609_paths.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
