"""The scene hierarchy follows the graph topology: a scene node that takes a scene input parents it
under its own prim, and the project's Up Axis drives the viewport's grid.

Graph: USDRead1 and USDRead2 (both scene-reference.usda, root prim World) each into its own
USDXformPrim (Transform1 at translate x -5, Transform2 at x +5), both into USDMerge1, into Viewer1.
The composed stage is read by flattening the merge to a .usda through a probe USDWrite and opening
it with pxr.

Checks:
 1. the stage has /Transform1/World and /Transform2/World and no /World at the root;
 2. each Transform prim's local transform carries its translate knob;
 3. changing Transform1's translate re-renders: the viewport differs, /Transform1's xform follows,
    /Transform2's does not move;
 4. a Y-up file's USDRead warns naming both axes under a Z project and keeps its axis (not converted),
    so the file-fed stage and grid stay Y; with no file loaded (a lone USDXformPrim) the Up Axis
    flips the stage's upAxis and changes the viewport grid;
 5. USDWrite in layered mode writes the merge under build/, and a fresh USDRead of that file
    composes the same prim paths.

Screenshots: m226_hierarchy.png, m226_hierarchy_moved.png, m226_upaxis_z.png (full window), each
with a _viewer.png crop of the scene view, and m226_empty_y.png / m226_empty_z.png for the empty case.

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
OFFSET = 5.0

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication
    from pxr import Usd, UsdGeom

    from guitest import FIXTURES, OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)
    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    reads = [make("USDRead", 0, 0), make("USDRead", 300, 0)]
    for r in reads:
        r.getParam("filename").setValue(scene_path)
    xforms = [make("USDXformPrim", 0, 100), make("USDXformPrim", 300, 100)]
    merge = make("USDMerge", 150, 200)
    for i in range(2):
        require(xforms[i].connectInput(0, reads[i]), "connect USDRead%d into %s" % (i + 1, xforms[i].getScriptName()))
        require(merge.connectInput(i, xforms[i]), "connect %s into the merge" % xforms[i].getScriptName())
    for i, x in enumerate(xforms):
        x.setScriptName("Transform%d" % (i + 1))
    names = [x.getScriptName() for x in xforms]
    report("NOTE transform node names %r" % names)
    translates = [x.getParam("translate") for x in xforms]
    require(all(translates), "USDXformPrim has a translate knob")

    def set_translate(knob, x, y, z):
        knob.set(x, y, z)

    set_translate(translates[0], -OFFSET, 0.0, 0.0)
    set_translate(translates[1], OFFSET, 0.0, 0.0)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(150, 300)

    # Probe writers kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 450, 200)
    probe.getParam("exportMode").setValue(1)  # flattened
    layered = make("USDWrite", 450, 300)
    layered.getParam("exportMode").setValue(0)  # layered
    project_up_axis = app.getProjectParam("upAxis")
    require(project_up_axis is not None, "the project has an upAxis knob")
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

    def write_stage(source, writer, path, mode):
        writer.disconnectInput(0)
        require(writer.connectInput(0, source), "connect the writer to %s" % source.getScriptName())
        writer.getParam("filename").setValue(path)
        writer.getParam("exportMode").setValue(mode)
        if os.path.exists(path):
            os.remove(path)
        app.renderBlocking(writer, 1, 1)
        return os.path.isfile(path)

    def read_stage(source):
        """Flattens source to a fresh .usda and opens it with pxr."""
        counter[0] += 1
        path = os.path.join(work_dir, "probe%d.usda" % counter[0])
        if not write_stage(source, probe, path, 1):
            return None
        return Usd.Stage.Open(path)

    def prim_paths(stage):
        return sorted(str(p.GetPath()) for p in stage.Traverse())

    def local_translation(stage, path):
        prim = stage.GetPrimAtPath(path)
        if not prim:
            return None
        matrix = UsdGeom.Xformable(prim).GetLocalTransformation()
        t = matrix.ExtractTranslation()
        return (t[0], t[1], t[2])

    def near(a, b):
        return a is not None and all(abs(x - y) < 1e-6 for x, y in zip(a, b))

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the USDMerge")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the merged scene", repr(v.property("natronSceneError")) if v else "no scene view")
        yield ("sleep", SETTLE_MS)
        default_image = save_shots("m226_hierarchy")

        # 1. hierarchy
        stage = read_stage(merge)
        require(stage is not None, "the merge flattens to a file")
        paths = prim_paths(stage)
        report("NOTE composed prims %r" % paths)
        for name in names:
            check("/%s/World" % name in paths, "the stage has /%s/World" % name)
            check("/%s/World/Box" % name in paths, "the stage has /%s/World/Box" % name)
        check("/World" not in paths, "no /World at the root")

        # 2. local transforms
        t1 = local_translation(stage, "/" + names[0])
        t2 = local_translation(stage, "/" + names[1])
        check(near(t1, (-OFFSET, 0, 0)), "/%s local translation follows its knob (%r)" % (names[0], t1))
        check(near(t2, (OFFSET, 0, 0)), "/%s local translation follows its knob (%r)" % (names[1], t2))

        # 3. translate edit re-renders
        set_translate(translates[0], -OFFSET - 4.0, 3.0, 0.0)
        before = default_image
        changed = lambda: (crop() is not None and before is not None and crop() != before)
        ok = yield ("until", changed, POLL_TIMEOUT_S)
        check(ok, "changing %s's translate changes the viewport" % names[0])
        yield ("sleep", SETTLE_MS)
        moved_image = save_shots("m226_hierarchy_moved")
        stage = read_stage(merge)
        t1b = local_translation(stage, "/" + names[0])
        t2b = local_translation(stage, "/" + names[1])
        check(near(t1b, (-OFFSET - 4.0, 3.0, 0)), "/%s's xform updated (%r)" % (names[0], t1b))
        check(near(t2b, (OFFSET, 0, 0)), "/%s's xform is untouched (%r)" % (names[1], t2b))

        # 5. layered write and re-read (before the axis changes, with the scene as shown above)
        out_path = os.path.join(work_dir, "m226_layered.usda")
        wrote = write_stage(merge, layered, out_path, 0)
        check(wrote, "USDWrite (layered) wrote " + out_path)
        if wrote:
            fresh = make("USDRead", 600, 0)
            fresh.getParam("filename").setValue(out_path)
            fresh_stage = read_stage(fresh)
            fresh_paths = prim_paths(fresh_stage) if fresh_stage is not None else None
            report("NOTE re-read prims %r" % (fresh_paths,))
            check(fresh_paths == prim_paths(stage), "a fresh USDRead of the layered file composes the same prim paths")
            fresh_stage = None

        # 4. up axis
        check(UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.y, "the stage's upAxis is Y by default (%s)" % UsdGeom.GetStageUpAxis(stage))
        project_up_axis.setValue(1)
        yield ("sleep", 2000)
        save_shots("m226_upaxis_z")
        z_image = crop()
        stage = read_stage(merge)
        # A USDRead keeps its file's axis ("The scene is not converted"), and the strongest layer's
        # upAxis wins in a merge, so a graph fed by Y-up files stays Y under a Z project.
        check(UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.y,
              "file-fed stage keeps the Y-up file's axis under a Z project, unconverted (%s)" % UsdGeom.GetStageUpAxis(stage))
        check(z_image is not None and z_image == moved_image,
              "file-fed viewport is unchanged under a Z project: the file's Y axis still shows")
        ok = yield ("until", lambda: all("up axis is Y" in r.getPersistentMessage() and "are Z" in r.getPersistentMessage() for r in reads), POLL_TIMEOUT_S)
        messages = [r.getPersistentMessage() for r in reads]
        report("NOTE USDRead messages %r" % (messages,))
        check(ok, "a Y-up USDRead warns naming both axes")

        # 4 (cont.). no file loaded
        lone = make("USDXformPrim", 600, 300)
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, lone), "connect the Viewer to a lone USDXformPrim")
        project_up_axis.setValue(0)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        empty_y = save_shots("m226_empty_y")
        lone_stage = read_stage(lone)
        check(UsdGeom.GetStageUpAxis(lone_stage) == UsdGeom.Tokens.y, "no file loaded: the stage's upAxis is Y (%s)" % UsdGeom.GetStageUpAxis(lone_stage))
        project_up_axis.setValue(1)
        ok = yield ("until", lambda: crop() is not None and empty_y is not None and crop() != empty_y, POLL_TIMEOUT_S)
        check(ok, "with no file loaded, switching the Up Axis changes the viewport grid")
        yield ("sleep", SETTLE_MS)
        save_shots("m226_empty_z")
        lone_stage = read_stage(lone)
        check(UsdGeom.GetStageUpAxis(lone_stage) == UsdGeom.Tokens.z, "no file loaded: switching to Z flips the stage's upAxis (%s)" % UsdGeom.GetStageUpAxis(lone_stage))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
