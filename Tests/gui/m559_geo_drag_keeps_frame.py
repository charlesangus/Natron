"""A knob drag upstream of viewed geometry keeps the last frame up, with no composing status over it.

Graph: GeoPrimitive (sphere) -> GeoTransform -> Viewer1.

A drag is a burst of value changes on GeoTransform's translate, each followed by a short wait so
that the viewer evaluates and the 3D view starts a frame for it, as a slider drag does.

Checks:
  - the 3D view shows the geometry before the drag;
  - every step of the drag refreshes the viewer's geometry in place (natronGeoRefreshes rises);
  - the view never paints the composing status over the frame on screen while the drag goes on
    (natronSceneComposingPaints stays put), and it keeps showing the scene (natronSceneShown);
  - after the drag the view settles with no error.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STEP_MS = 40
STEPS = 30
STARTUP_TIMEOUT_S = 60.0

try:
    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    primitive = app.createNode("GeoPrimitive")
    require(primitive is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    primitive.getParam("type").setValue(3)
    transform = app.createNode("GeoTransform")
    require(transform is not None, "create GeoTransform", plugin_problem("GeoTransform"))
    translate = transform.getParam("translate")
    require(translate is not None, "GeoTransform has a translate parameter")
    require(transform.connectInput(0, primitive), "connect the GeoTransform to the GeoPrimitive")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    primitive.setPosition(0, 0)
    transform.setPosition(0, 100)
    viewer.setPosition(0, 220)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def steps():
        require(viewer.connectInput(0, transform), "connect the Viewer to the GeoTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the geometry (%r)" % (prop("natronSceneError"),))
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: not prop("natronSceneComposing", False), POLL_TIMEOUT_S)
        check(ok, "the first frame has been composed before the drag")
        shot("m559-drag-before.png")

        composing = int(prop("natronSceneComposingPaints", 0) or 0)
        refreshes = int(prop("natronGeoRefreshes", 0) or 0)
        report("NOTE before the drag: %d composing paints, %d refreshes" % (composing, refreshes))
        lost_scene = 0
        for i in range(1, STEPS + 1):
            translate.set(0.05 * i, 0.02 * i, 0.)
            yield ("sleep", STEP_MS)
            v = view()
            if v is None or not bool(v.property("natronSceneShown")):
                lost_scene += 1
            if i == STEPS // 2:
                shot("m559-drag-during.png")
        check(lost_scene == 0, "the view keeps showing the scene during the drag (%d steps without it)" % lost_scene)
        ok = yield ("until", lambda: int(prop("natronGeoRefreshes", 0) or 0) > refreshes, POLL_TIMEOUT_S)
        check(ok, "the drag refreshes the viewer's geometry in place (%r)" % (prop("natronGeoRefreshes"),))
        check(int(prop("natronSceneComposingPaints", 0) or 0) == composing,
              "no composing status is painted over the frame during the drag (%r, was %d)"
              % (prop("natronSceneComposingPaints"), composing))

        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the view settles on the dragged geometry with no error (%r)" % (prop("natronSceneError"),))
        yield ("sleep", SETTLE_MS)
        check(int(prop("natronSceneComposingPaints", 0) or 0) == composing,
              "settling paints no composing status either (%r)" % (prop("natronSceneComposingPaints"),))
        shot("m559-drag-after.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
