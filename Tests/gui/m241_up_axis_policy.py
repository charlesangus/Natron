"""USDRead's up axis and units policy in the Viewer: a Z-up centimetre file read into a Y-up metre
project is shown as it is, with a warning, until Match Project Up Axis and Match Project Units
convert it; Use File's Metrics For Project adopts the file's metrics instead, as one undo step.

Graph: USDRead1 (a Z-up, 0.01 meters-per-unit box 100 x 100 x 300 units, tall along Z, generated
under $NATRON_GUI_TEST_OUT/work) into Viewer1. The composed stage is read by flattening USDRead1 to
a .usda through a probe USDWrite and opening it with pxr.

Checks:
 1. both toggles off: the warning names the up axis and the units and the toggle for each; the stage
    stays Z-up, 0.01, and the box is 300 units tall along Z;
 2. both toggles on: no warning; the stage is Y-up, 1 meter per unit, and the box stands 3 units
    tall along Y, 1 x 1 across; the viewport changes;
 3. toggles off, then Use File's Metrics For Project: the project is Z / 0.01, the warning clears and
    the viewport grid changes; Edit > Undo puts the project back to Y / 1.

Screenshots: m241_uncorrected.png, m241_corrected.png, m241_file_metrics.png (full window), each with
a _viewer.png crop of the scene view.

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

# A closed box from (0, 0, 0) to (100, 100, 300), Z up, in centimetres.
BOX_USDA = """#usda 1.0
(
    defaultPrim = "Tower"
    metersPerUnit = 0.01
    upAxis = "Z"
)
def Xform "Tower"
{
    def Mesh "Box"
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7]
        point3f[] points = [(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0), (0, 0, 300), (100, 0, 300), (100, 100, 300), (0, 100, 300)]
    }
}
"""

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication
    from pxr import Usd, UsdGeom

    import guitest as gt
    from guitest import OUT, check, plugin_problem, report, require, run, shot, widgets_of

    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)
    box_path = os.path.join(work_dir, "m241_zup_cm.usda")
    with open(box_path, "w") as f:
        f.write(BOX_USDA)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    project_up_axis = app.getProjectParam("upAxis")
    project_meters_per_unit = app.getProjectParam("metersPerUnit")
    require(project_up_axis is not None and project_meters_per_unit is not None, "the project has upAxis and metersPerUnit knobs")
    project_up_axis.setValue(0)
    project_meters_per_unit.setValue(1.0)

    reader = make("USDRead", 0, 0)
    reader.getParam("filename").setValue(box_path)
    match_up_axis = reader.getParam("matchUpAxis")
    match_units = reader.getParam("matchUnits")
    use_file_metrics = reader.getParam("useFileMetrics")
    require(match_up_axis is not None and match_units is not None and use_file_metrics is not None,
            "USDRead has matchUpAxis, matchUnits and useFileMetrics")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 300, 100)
    probe.getParam("exportMode").setValue(1)  # flattened
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
        v.update()
        v.repaint()
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def differing(a, b):
        """Number of differing pixels between two crops, or None when they cannot be compared."""
        if a is None or b is None or a.size() != b.size():
            return None
        count = 0
        for y in range(0, a.height(), 2):
            for x in range(0, a.width(), 2):
                if a.pixel(x, y) != b.pixel(x, y):
                    count += 1
        return count

    def save_failure_crops(name, before_image, after_image):
        for suffix, image in (("before", before_image), ("after", after_image)):
            if image is not None:
                path = os.path.join(OUT, "%s_%s_viewer.png" % (name, suffix))
                image.save(path)
                report("SHOT " + path)

    def save_shots(name):
        shot(name + ".png")
        image = crop()
        if image is not None:
            image.save(os.path.join(OUT, name + "_viewer.png"))
            report("SHOT " + os.path.join(OUT, name + "_viewer.png"))
        return image

    def read_stage():
        """Flattens USDRead1 to a fresh .usda and opens it with pxr."""
        counter[0] += 1
        path = os.path.join(work_dir, "m241_probe%d.usda" % counter[0])
        if os.path.exists(path):
            os.remove(path)
        probe.disconnectInput(0)
        require(probe.connectInput(0, reader), "connect the probe writer to USDRead1")
        probe.getParam("filename").setValue(path)
        app.renderBlocking(probe, 1, 1)
        require(os.path.isfile(path), "the probe writer wrote " + path)
        return Usd.Stage.Open(path)

    def box_bounds(stage):
        cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_])
        box = cache.ComputeWorldBound(stage.GetPrimAtPath("/Tower/Box")).ComputeAlignedRange()
        return tuple(box.GetMin()), tuple(box.GetMax())

    def near(a, b, tolerance=1e-4):
        return a is not None and all(abs(x - y) < tolerance for x, y in zip(a, b))

    def message():
        return reader.getPersistentMessage() or ""

    def edit_undo():
        win = gt._find_main_window()
        if win is None:
            return False
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith("undo") and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        return False

    def steps():
        require(viewer.connectInput(0, reader), "connect the Viewer to USDRead1")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the file", repr(v.property("natronSceneError")) if v else "no scene view")
        yield ("sleep", SETTLE_MS)

        # 1. Uncorrected.
        uncorrected = save_shots("m241_uncorrected")
        text = message()
        report("NOTE uncorrected message %r" % text)
        check("up axis is Z" in text and "Match Project Up Axis" in text, "the warning names the up axis and its toggle")
        check("meters per unit is 0.01" in text and "Match Project Units" in text, "the warning names the units and their toggle")
        stage = read_stage()
        check(UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z, "uncorrected: the stage stays Z-up (%s)" % UsdGeom.GetStageUpAxis(stage))
        check(abs(UsdGeom.GetStageMetersPerUnit(stage) - 0.01) < 1e-9, "uncorrected: the stage stays 0.01 (%r)" % UsdGeom.GetStageMetersPerUnit(stage))
        bounds = box_bounds(stage)
        check(near(bounds[0], (0, 0, 0)) and near(bounds[1], (100, 100, 300)), "uncorrected: the box is 300 units tall along Z (%r)" % (bounds,))

        # 2. Corrected.
        match_up_axis.setValue(True)
        match_units.setValue(True)
        ok = yield ("until", lambda: message() == "" and crop() is not None and uncorrected is not None and crop() != uncorrected, POLL_TIMEOUT_S)
        check(ok, "both toggles on: the warning clears and the viewport changes (%r)" % message())
        yield ("sleep", SETTLE_MS)
        corrected = save_shots("m241_corrected")
        stage = read_stage()
        check(UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.y, "corrected: the stage is Y-up (%s)" % UsdGeom.GetStageUpAxis(stage))
        check(abs(UsdGeom.GetStageMetersPerUnit(stage) - 1.0) < 1e-9, "corrected: the stage is in metres (%r)" % UsdGeom.GetStageMetersPerUnit(stage))
        bounds = box_bounds(stage)
        report("NOTE corrected bounds %r" % (bounds,))
        check(near(bounds[0], (0, 0, -1)) and near(bounds[1], (1, 3, 0)), "corrected: the box stands 3 m tall along Y, 1 m across (%r)" % (bounds,))

        # 3. Use File's Metrics For Project, then undo.
        match_up_axis.setValue(False)
        match_units.setValue(False)
        ok = yield ("until", lambda: "up axis is Z" in message(), POLL_TIMEOUT_S)
        check(ok, "toggles off: the warning comes back")
        yield ("sleep", SETTLE_MS)
        before = crop()
        use_file_metrics.trigger()
        QApplication.processEvents()
        check(project_up_axis.getValue() == 1, "Use File's Metrics For Project sets the project to Z (%r)" % project_up_axis.getValue())
        check(abs(project_meters_per_unit.getValue() - 0.01) < 1e-12,
              "Use File's Metrics For Project sets the project to 0.01 (%r)" % project_meters_per_unit.getValue())
        ok = yield ("until", lambda: message() == "" and crop() is not None and corrected is not None and crop() != corrected, POLL_TIMEOUT_S)
        after = crop()
        metric = differing(corrected, after)
        report("NOTE viewport comparison against the corrected crop: %r of the sampled pixels differ (corrected size %r, after size %r)"
               % (metric, corrected.size() if corrected is not None else None, after.size() if after is not None else None))
        if not ok:
            save_failure_crops("m241_file_metrics_failed", corrected, after)
        check(message() == "", "Use File's Metrics For Project clears the warning (%r)" % message())
        check(ok, "Use File's Metrics For Project flips the grid to Z: the viewport differs from the corrected Y-up crop (differing pixels %r)" % metric)
        yield ("sleep", SETTLE_MS)
        save_shots("m241_file_metrics")
        check(edit_undo(), "Edit > Undo is enabled after the button")
        ok = yield ("until", lambda: project_up_axis.getValue() == 0 and abs(project_meters_per_unit.getValue() - 1.0) < 1e-12, POLL_TIMEOUT_S)
        check(ok, "one undo puts the project back to Y / 1 (%r, %r)" % (project_up_axis.getValue(), project_meters_per_unit.getValue()))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
