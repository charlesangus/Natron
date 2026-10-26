"""The scene view keeps one stage and one Storm engine while the scene it shows is edited.

Graph: USDRead(scene-reference.usda) into Viewer1. Once the scene view shows the scene and has
drawn it, the USDRead's frame offset is set ten times, and the file is reloaded twice, while the
view stays up. After each edit it polls, with a timeout, for the view to have synced the edit and
still show the scene with no error, and checks that the view never made a second engine
(natronSceneEngineCount stays 1).

Then it triggers Cache > Free GPU Memory: the view lets go of its engine at once
(natronSceneEngineHeld false), and the repaint that follows makes a second one and shows the scene.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 300
STARTUP_TIMEOUT_S = 60.0
EDITS = 10

try:
    from PySide6.QtGui import QAction
    from PySide6.QtWidgets import QApplication, QMainWindow

    from guitest import FIXTURES, check, finish, plugin_problem, require, run, shot, widgets_of

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    offset = usd.getParam("frameOffset")
    require(offset is not None, "USDRead has a frameOffset parameter")
    reload = usd.getParam("reload")
    require(reload is not None, "USDRead has a reload parameter")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    viewer.setPosition(0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def engines():
        v = view()
        return int(v.property("natronSceneEngineCount") or 0) if v is not None else -1

    def engine_held():
        v = view()
        return v is not None and bool(v.property("natronSceneEngineHeld"))

    def free_gpu_memory_action():
        for window in QApplication.topLevelWidgets():
            if not isinstance(window, QMainWindow):
                continue
            for a in window.findChildren(QAction):
                if a.text().replace("&", "") == "Free GPU Memory":
                    return a
        return None

    def synced_and_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", synced_and_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: engines() >= 1, POLL_TIMEOUT_S)
        check(ok, "the scene view made its engine at the first paint (count %d)" % engines())
        shot("live-edit-before.png")

        for i in range(EDITS + 2):
            if i < EDITS:
                offset.setValue(float(i + 1))
                what = "frame offset %d" % (i + 1)
            else:
                reload.trigger()
                what = "reload %d" % (i - EDITS + 1)
            ok = yield ("until", synced_and_drawn, POLL_TIMEOUT_S)
            v = view()
            error = v.property("natronSceneError") if v is not None else None
            check(ok, "%s: the scene view synced the edit and shows the scene (%r)" % (what, error))
            yield ("sleep", SETTLE_MS)
            check(engines() == 1, "%s: still one engine (count %d)" % (what, engines()))

        shot("live-edit-after.png")

        free = free_gpu_memory_action()
        require(free is not None, "the Cache menu has a Free GPU Memory action")
        check(engine_held(), "the scene view holds its engine before Free GPU Memory")
        # The release runs inside trigger(); the repaint it asks for comes later on the event loop.
        free.trigger()
        check(not engine_held(), "Free GPU Memory: the scene view let go of its engine")
        ok = yield ("until", lambda: engine_held() and engines() == 2, POLL_TIMEOUT_S)
        check(ok, "Free GPU Memory: the repaint made the view's second engine (count %d)" % engines())
        ok = yield ("until", synced_and_drawn, POLL_TIMEOUT_S)
        check(ok, "Free GPU Memory: the scene view still shows the scene")
        yield ("sleep", SETTLE_MS)
        check(engines() == 2 and engine_held(), "Free GPU Memory: one engine held, two made (count %d)" % engines())
        shot("live-edit-after-free-gpu-memory.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
