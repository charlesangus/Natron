"""The viewer tab shows the view that matches the kind of its active input: the image viewer for
a 2D input, the scene view for a scene input, and flips each time the input changes kind.

Graph: Read(flat-rgba-pattern.exr) and USDRead(scene-reference.usda), with Viewer1's input 0
connected to the Read, then to the USDRead, then to the Read again, twice. After each change it
polls, with a timeout, for exactly one of the ViewerGL and SceneViewport widgets to be visible,
and for the scene view to show a scene and no error.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0

try:
    from guitest import FIXTURES, check, finish, plugin_problem, require, run, shot, viewer_up, widgets_of

    image_path = os.path.join(FIXTURES, "flat-rgba-pattern.exr")
    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(image_path), "fixture exists: " + image_path)
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    reader = app.createReader(image_path)
    require(reader is not None, "create a Read for " + os.path.basename(image_path))
    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    filename = usd.getParam("filename")
    require(filename is not None, "USDRead has a filename parameter")
    filename.setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    reader.setPosition(0, 0)
    usd.setPosition(200, 0)
    viewer.setPosition(100, 150)

    def visible(class_name):
        return [w for w in widgets_of(class_name) if w.isVisible()]

    def showing_image():
        return bool(visible("ViewerGL")) and not visible("SceneViewport")

    def showing_scene():
        return bool(visible("SceneViewport")) and not visible("ViewerGL")

    def scene_drawn():
        views = visible("SceneViewport")
        return bool(views) and bool(views[0].property("natronSceneShown")) and not views[0].property("natronSceneError")

    def describe():
        return "visible ViewerGL=%d SceneViewport=%d" % (len(visible("ViewerGL")), len(visible("SceneViewport")))

    def connect(node):
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def steps():
        require(connect(reader), "connect the Viewer to the Read")
        ok = yield ("until", viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the image viewer is up")
        yield ("sleep", SETTLE_MS)
        check(showing_image(), "a 2D input shows the image viewer (%s)" % describe())
        image_heights = [w.height() for w in visible("ViewerGL")]
        image_height = image_heights[0] if image_heights else 0

        for n in range(2):
            require(connect(usd), "connect the Viewer to the USDRead")
            ok = yield ("until", showing_scene, POLL_TIMEOUT_S)
            check(ok, "pass %d: a scene input shows the scene view (%s)" % (n + 1, describe()))
            ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
            views = visible("SceneViewport")
            error = views[0].property("natronSceneError") if views else None
            check(ok, "pass %d: the scene view shows the scene, no error (%r)" % (n + 1, error))
            yield ("sleep", SETTLE_MS)
            scene_heights = [w.height() for w in visible("SceneViewport")]
            scene_height = scene_heights[0] if scene_heights else 0
            check(image_height > 0 and scene_height >= 0.8 * image_height,
                  "pass %d: the scene view fills the slot of the image viewer (height %d, image viewer %d)"
                  % (n + 1, scene_height, image_height))
            shot("scene-%d.png" % (n + 1))

            require(connect(reader), "connect the Viewer back to the Read")
            ok = yield ("until", showing_image, POLL_TIMEOUT_S)
            check(ok, "pass %d: a 2D input again shows the image viewer (%s)" % (n + 1, describe()))
            yield ("sleep", SETTLE_MS)
            shot("image-%d.png" % (n + 1))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
