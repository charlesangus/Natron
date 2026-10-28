"""USDCamera reading its camera from a file: the knobs show the file camera's values at the current
frame and follow the timeline.

Graph: USDCamera1 with Read From File on, reading Tests/fixtures/scene-camera-tracked.usda
(/Track/Cam: focal length 0.35 at frame 1 and 0.5 at frame 10, in meters, so 35 mm and 50 mm),
into Viewer1. Its settings panel is open: the node refreshes its knobs on a time change only while
it is.

Checks:
 1. the Camera Prim menu lists the file's cameras, /Track/Cam selected;
 2. at frame 10 Focal Length shows 50 and is disabled, and Translate shows the camera's world
    position there;
 3. scrubbing to frame 1 shows 35, and back to 10 shows 50 again;
 4. Edit Lens enables Focal Length;
 5. the scene view draws the camera with no error.

Screenshots: scene_camera_from_file_frame10.png and scene_camera_from_file_frame1.png (full
window), and scene_camera_from_file_edit_lens.png.

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

try:
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QLabel, QLineEdit
    from pxr import Usd, UsdGeom

    from guitest import FIXTURES, check, finish, plugin_problem, report, require, run, shot, widgets_of

    FIXTURE = os.path.join(FIXTURES, "scene-camera-tracked.usda")
    require(os.path.isfile(FIXTURE), "fixture exists: " + FIXTURE)

    file_stage = Usd.Stage.Open(FIXTURE)
    require(file_stage is not None, "pxr opens the fixture")
    file_camera = UsdGeom.Camera(file_stage.GetPrimAtPath("/Track/Cam"))
    require(bool(file_camera), "the fixture holds /Track/Cam")

    def file_position(frame):
        return file_camera.ComputeLocalToWorldTransform(Usd.TimeCode(frame)).ExtractTranslation()

    camera = app.createNode("USDCamera")
    require(camera is not None, "create USDCamera", plugin_problem("USDCamera"))
    camera.setPosition(0, 0)
    camera.getParam("file").setValue(FIXTURE)
    camera.getParam("readFromFile").setValue(True)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    focal = camera.getParam("focalLength")
    translate = camera.getParam("translate")
    edit_lens = camera.getParam("editLens")
    camera_prim = camera.getParam("cameraPrim")
    require(focal is not None and translate is not None and edit_lens is not None and camera_prim is not None,
            "USDCamera has its focalLength, translate, editLens and cameraPrim knobs")
    py_viewer = {}

    def seek(frame):
        if "v" not in py_viewer:
            py_viewer["v"] = app.getViewer(viewer.getScriptName())
        py_viewer["v"].seek(frame)

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

    def scene_view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = scene_view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def shows_focal(mm):
        return abs(focal.getValue() - mm) < 1e-3

    def steps():
        entries = [camera_prim.getOption(i) for i in range(camera_prim.getNumOptions())]
        check(entries == ["/Track/Cam", "/Witness"], "Camera Prim lists the file's cameras (%r)" % (entries,))
        selected = camera_prim.getOption(camera_prim.getValue())
        check(selected == "/Track/Cam", "the first camera is selected (%r)" % selected)

        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(camera, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(camera) is not None, POLL_TIMEOUT_S)
        require(ok, "open USDCamera1's settings panel")

        require(viewer.connectInput(0, camera), "connect the Viewer to USDCamera1")
        ok = yield ("until", lambda: scene_view() is not None, STARTUP_TIMEOUT_S)
        check(ok, "the scene view is up")

        seek(10)
        ok = yield ("until", lambda: shows_focal(50.), POLL_TIMEOUT_S)
        check(ok, "at frame 10 Focal Length shows the file's 50 mm (%r)" % focal.getValue())
        check(not focal.getIsEnabled(0), "Focal Length is disabled while the file drives it")
        position = file_position(10)
        check(all(abs(translate.getValue(i) - position[i]) < 1e-4 for i in range(3)),
              "at frame 10 Translate shows the file camera's world position (%r, file %r)" %
              ([translate.getValue(i) for i in range(3)], tuple(position)))
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = scene_view()
        check(ok, "the scene view draws the file camera with no error (%r)" % (v.property("natronSceneError") if v else None,))
        yield ("sleep", SETTLE_MS)
        shot("scene_camera_from_file_frame10.png")

        seek(1)
        ok = yield ("until", lambda: shows_focal(35.), POLL_TIMEOUT_S)
        check(ok, "scrubbing to frame 1 shows the file's 35 mm (%r)" % focal.getValue())
        yield ("sleep", SETTLE_MS)
        shot("scene_camera_from_file_frame1.png")

        seek(10)
        ok = yield ("until", lambda: shows_focal(50.), POLL_TIMEOUT_S)
        check(ok, "scrubbing back to frame 10 shows 50 mm again (%r)" % focal.getValue())

        edit_lens.setValue(True)
        ok = yield ("until", lambda: focal.getIsEnabled(0), POLL_TIMEOUT_S)
        check(ok, "Edit Lens enables Focal Length")
        check(shows_focal(50.), "Edit Lens starts from the file's value (%r)" % focal.getValue())
        yield ("sleep", SETTLE_MS)
        shot("scene_camera_from_file_edit_lens.png")
        report("NOTE done")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
