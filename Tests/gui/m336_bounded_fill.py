"""The bounded fill: Storm's geometry comes into Hydra a paced batch a frame, and the view stays live.

Graph: USDRead of a heavy_city.usda (tools/bench/heavy_scene_make.py) into Viewer1, Full + Load. Once
every payload has landed and the first fill is complete, the script hides /World, which takes the city
out of Hydra, and clears that, so the whole city comes in again in bounded passes. It checks:
  - the fill takes more than one presented frame, and natronSceneFillProgress never falls while the
    camera rests;
  - a camera nudge mid-fill gets a frame new to the screen within a few of the fill's passes;
  - the fill completes: natronSceneFillComplete is set and natronSceneFillProgress is 1.

File: $NATRON_CITY_FILE, by default a city of $NATRON_FILL_BLOCKS blocks (40) the script writes under
$NATRON_GUI_TEST_OUT; a fill too fast to nudge mid-way needs a bigger one.

Screenshots for sign-off: bounded-fill-mid (the boxes of rows still to come over the frame),
bounded-fill-complete.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 1000
POLL_MS = 5
BLOCKS = int(os.environ.get("NATRON_FILL_BLOCKS", "40"))
BUILDINGS_PER_BLOCK = 20
CITY_FLOORS = 16
CITY_PRIMS = BLOCKS * BUILDINGS_PER_BLOCK * 40 + BLOCKS + 1
INITIAL_DRAW_FULL_LOAD = 4
# Frames of the fill a nudge may take to reach the screen in.
NUDGE_PASSES = 4
NUDGE_FLOOR_MS = 500.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    city_path = os.environ.get("NATRON_CITY_FILE")
    if not city_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "m336_bounded_fill"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, BLOCKS, BUILDINGS_PER_BLOCK, CITY_FLOORS)
        city_path = os.path.join(fixture_dir, "heavy_city.usda")
    require(os.path.isfile(city_path), "fixture exists: %s" % city_path)

    settings = NatronEngine.natron.getSettings()

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(INITIAL_DRAW_FULL_LOAD)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(city_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def presented():
        return int(prop("natronScenePresentedFrames", 0))

    def fill_complete():
        return bool(prop("natronSceneFillComplete"))

    def settled():
        return scene_drawn() and not prop("natronSceneLoadsPending") and fill_complete()

    def save(name):
        shot(name + ".png")
        v = view()
        if v is None:
            return
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        image = image.copy(top_left.x(), top_left.y(), v.width(), v.height()).convertToFormat(QImage.Format_RGBX8888)
        path = os.path.join(OUT, name + "_view.png")
        image.save(path)
        report("SHOT " + path)

    def call(method, *args):
        require(QMetaObject.invokeMethod(view(), method, *args), "the scene view has %s" % method)

    def nudge():
        """One small tumble of the free camera."""
        navigation = settings.getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        navigation.setValue(1)
        v = view()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier))

        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        mouse(QEvent.MouseMove, centre + QPoint(12, 4), Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, centre + QPoint(12, 4), Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        navigation.setValue(original)

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", lambda: prop("natronSceneFillProgress") is not None, POLL_TIMEOUT_S)
        require(ok, "the scene view publishes natronSceneFillProgress")
        ok = yield ("until", settled, LOAD_TIMEOUT_S)
        require(ok, "the city loads and its first fill completes (progress %r, complete %r)"
                % (prop("natronSceneFillProgress"), prop("natronSceneFillComplete")))

        section("The city out of Hydra")
        call("setDrawState", Q_ARG("QString", "/World"), Q_ARG("QString", "hidden"))
        ok = yield ("until", lambda: scene_drawn() and fill_complete(), POLL_TIMEOUT_S)
        require(ok, "hiding /World settles")
        yield ("sleep", SETTLE_MS)

        section("Bounded fill")
        frames_before = presented()
        call("clearDrawState", Q_ARG("QString", "/World"))
        ok = yield ("until", lambda: not fill_complete() and float(prop("natronSceneFillProgress", 1.0)) < 1.0, POLL_TIMEOUT_S)
        require(ok, "clearing /World starts a fill (progress %r)" % (prop("natronSceneFillProgress"),))
        progress = [float(prop("natronSceneFillProgress", 0.0))]
        frame_times = []
        last_frames = presented()
        fell = None
        # Watches the fill for two of its frames before the nudge.
        while not fill_complete() and len(frame_times) < 2:
            yield ("sleep", POLL_MS)
            value = float(prop("natronSceneFillProgress", 0.0))
            if value < progress[-1] - 1e-9 and fell is None:
                fell = (progress[-1], value)
            progress.append(value)
            if presented() != last_frames:
                last_frames = presented()
                frame_times.append(time.time() * 1000.0)
        mid_fill = not fill_complete()
        check(mid_fill, "the fill takes more than two frames (%d frames, progress %.3f); a bigger city is needed otherwise"
              % (presented() - frames_before, progress[-1]))
        if mid_fill:
            save("bounded-fill-mid")
            gaps = [b - a for a, b in zip(frame_times, frame_times[1:])]
            allowed_ms = max(NUDGE_FLOOR_MS, NUDGE_PASSES * (max(gaps) if gaps else 0.0))
            frames_at_nudge = presented()
            nudged_at = time.time()
            nudge()
            ok = yield ("until", lambda: presented() > frames_at_nudge, allowed_ms / 1000.0)
            check(ok, "a camera nudge mid-fill gets a new frame within %.0f ms (%.0f ms, %d frames)"
                  % (allowed_ms, (time.time() - nudged_at) * 1000.0, presented() - frames_at_nudge))
            # The fill waits for the camera to rest; its progress is watched again from then.
            yield ("sleep", 300)
            progress.append(float(prop("natronSceneFillProgress", 0.0)))
        fill_deadline = time.time() + LOAD_TIMEOUT_S
        while not fill_complete() and time.time() < fill_deadline:
            yield ("sleep", POLL_MS)
            value = float(prop("natronSceneFillProgress", 0.0))
            if value < progress[-1] - 1e-9 and fell is None:
                fell = (progress[-1], value)
            progress.append(value)
        check(fell is None, "the fill's progress never falls while the camera rests (fell %r)" % (fell,))
        ok = yield ("until", fill_complete, LOAD_TIMEOUT_S)
        check(ok, "the fill completes (natronSceneFillComplete %r)" % (prop("natronSceneFillComplete"),))
        check(float(prop("natronSceneFillProgress", 0.0)) == 1.0, "the fill's progress ends at 1 (%r)" % (prop("natronSceneFillProgress"),))
        check(presented() - frames_before > 1, "the fill came in over more than one frame (%d)" % (presented() - frames_before))
        yield ("sleep", SETTLE_MS)
        save("bounded-fill-complete")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
