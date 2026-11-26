"""The 3D view keeps its last frame through edits, plays animated geometry smoothly and shows the stale cue.

Graph: GeoPrimitive (sphere) -> GeoTransform -> Viewer1, then GeoPrimitive -> GeoScatter -> Viewer1.

Checks:
  - drag: a burst of GeoTransform translate changes, 40 ms apart, never leaves the view without
    its scene, never paints the composing status, never shows a frame of one flat colour, and
    the view ends on the geometry of the last value (natronGeoBounds);
  - nudge: single steps of translate, some only a few milliseconds apart and some after the view
    settled, end on the geometry of the last value with no error;
  - stale cue: with the Subtle Outline style and no delay, the 3D view shows the cue while a slow
    scatter is evaluated (natronStaleCueVisible) and hides it once the view has caught up;
  - playback: GeoScatter with seed = frame plays frames 1-24 twice; the per-frame time between
    shown frames is reported. On the second pass every frame is shown, none of them resyncs a
    prim (each frame's points are edited in place), the frames past the first (which also waits
    for playback to start) come at the project rate on average (one frame plus the sampling
    interval), and none takes as long as two. A single frame over one frame's time is not a
    failure: the timeline keeps the rate and shows the next frame early to make up for it, and
    a late paint under Xvfb with software GL says nothing about the product.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 700
DRAG_STEP_MS = 40
DRAG_STEPS = 30
QUICK_NUDGE_MS = 5
SLOW_NUDGE_MS = 150
NUDGES = 10
NUDGE = 0.1
SAMPLE_MS = 4
FIRST = 1
FRAMES = 24
LAST = FIRST + FRAMES - 1
PLAY_TIMEOUT_S = 30.0
SCATTER_COUNT = 20000
SLOW_SCATTER_COUNT = 2000000
STYLE_SUBTLE_OUTLINE = 1
TOLERANCE = 1e-4

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, QTimer
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QWidget

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up

    try:
        app.getProjectParam("frameRange").set(FIRST, LAST)
    except Exception:
        report("NOTE could not set the project frame range: " + traceback.format_exc().splitlines()[-1])

    sphere = app.createNode("GeoPrimitive")
    require(sphere is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    sphere.getParam("type").setValue(3)
    transform = app.createNode("GeoTransform")
    require(transform is not None, "create GeoTransform", plugin_problem("GeoTransform"))
    translate = transform.getParam("translate")
    require(translate is not None, "GeoTransform has a translate parameter")
    require(transform.connectInput(0, sphere), "connect the GeoTransform to the GeoPrimitive")

    source = app.createNode("GeoPrimitive")
    require(source is not None, "create the GeoScatter source")
    scatter = app.createNode("GeoScatter")
    require(scatter is not None, "create GeoScatter", plugin_problem("GeoScatter"))
    require(scatter.connectInput(0, source), "connect the GeoScatter to its source")
    count = scatter.getParam("count")
    seed = scatter.getParam("seed")
    require(count is not None and seed is not None, "GeoScatter has count and seed parameters")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    sphere.setPosition(0, 0)
    transform.setPosition(0, 100)
    source.setPosition(300, 0)
    scatter.setPosition(300, 100)
    viewer.setPosition(150, 240)

    keep = {}

    def view():
        # findChildren searches in C++: walking every widget from Python would hold the main
        # thread longer than a played frame.
        for top in QApplication.topLevelWidgets():
            if not top.isVisible():
                continue
            for v in top.findChildren(QWidget, "SceneViewport"):
                if v.isVisible():
                    return v
        return None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError") and not v.property("natronSceneComposing"))

    def centre():
        bounds = prop("natronGeoBounds")
        if not bounds or len(bounds) != 6:
            return None
        return [0.5 * (float(bounds[i]) + float(bounds[i + 3])) for i in range(3)]

    def centred_on(expected):
        c = centre()
        return c is not None and all(abs(c[i] - expected[i]) < TOLERANCE for i in range(3))

    def flat_frame():
        # Grey is the background with nothing on it: one colour over a coarse grid of the frame.
        # Read from the screen: the bundle has no QOpenGLWidget wrapper, and a widget grab of the
        # GL view can come out blank under llvmpipe.
        v = view()
        if v is None:
            return True
        QApplication.processEvents()
        origin = v.mapToGlobal(QPoint(0, 0))
        image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(), v.height()).toImage()
        if image.isNull() or image.width() < 2 or image.height() < 2:
            return True
        colours = set()
        for j in range(1, 16):
            for i in range(1, 16):
                colours.add(image.pixel(i * image.width() // 16, j * image.height() // 16))
        return len(colours) <= 1

    def start_sampling(samples, started):
        def sample():
            v = view()
            if v is None:
                return
            frame = v.property("natronGeoFrame")
            samples.append((time.time() - started, app.timelineGetTime(), None if frame is None else int(frame),
                            bool(v.property("natronStaleCueVisible")), bool(v.property("natronSceneShown"))))

        timer = QTimer()
        timer.setInterval(SAMPLE_MS)
        timer.timeout.connect(sample)
        timer.start()
        keep["timer"] = timer

    def stop_sampling():
        timer = keep.pop("timer", None)
        if timer is not None:
            timer.stop()

    def pv():
        if "v" not in keep:
            keep["v"] = app.getViewer(viewer.getScriptName())
        return keep["v"]

    def drag_and_nudge():
        require(viewer.connectInput(0, transform), "connect the Viewer to the GeoTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the sphere (%r)" % (prop("natronSceneError"),))
        view().setProperty("natronGeoPublishBounds", True)
        view().update()
        ok = yield ("until", lambda: centre() is not None, POLL_TIMEOUT_S)
        require(ok, "the 3D view publishes the bounds of the geometry it shows")
        yield ("sleep", SETTLE_MS)
        rest = centre()
        report("NOTE sphere centre at rest: %r" % (rest,))
        shot("m600-drag-before.png")

        composing = int(prop("natronSceneComposingPaints", 0) or 0)
        lost_scene = 0
        flat = 0
        for i in range(1, DRAG_STEPS + 1):
            translate.set(0.05 * i, 0.02 * i, 0.)
            yield ("sleep", DRAG_STEP_MS)
            v = view()
            if v is None or not bool(v.property("natronSceneShown")):
                lost_scene += 1
            if i % 5 == 0 and flat_frame():
                flat += 1
            if i == DRAG_STEPS // 2:
                shot("m600-drag-during.png")
        check(lost_scene == 0, "drag: the view keeps showing the scene (%d steps without it)" % lost_scene)
        check(flat == 0, "drag: no sampled frame is one flat colour (%d were)" % flat)
        check(int(prop("natronSceneComposingPaints", 0) or 0) == composing,
              "drag: no composing status is painted over the frame (%r, was %d)" % (prop("natronSceneComposingPaints"), composing))
        dragged = [rest[0] + 0.05 * DRAG_STEPS, rest[1] + 0.02 * DRAG_STEPS, rest[2]]
        ok = yield ("until", lambda: scene_drawn() and centred_on(dragged), POLL_TIMEOUT_S)
        check(ok, "drag: the view ends on the last value's geometry (centre %r, expected %r)" % (centre(), dragged))
        check(not flat_frame(), "drag: the settled frame is not one flat colour")
        shot("m600-drag-after.png")

        x = 0.05 * DRAG_STEPS
        lost_scene = 0
        for wait in (QUICK_NUDGE_MS, SLOW_NUDGE_MS):
            for _ in range(NUDGES):
                x += NUDGE
                translate.set(x, 0.02 * DRAG_STEPS, 0.)
                yield ("sleep", wait)
                v = view()
                if v is None or not bool(v.property("natronSceneShown")):
                    lost_scene += 1
            nudged = [rest[0] + x, rest[1] + 0.02 * DRAG_STEPS, rest[2]]
            ok = yield ("until", lambda: scene_drawn() and centred_on(nudged), POLL_TIMEOUT_S)
            check(ok, "nudges %d ms apart: the view ends on the last value's geometry (centre %r, expected %r, error %r)"
                  % (wait, centre(), nudged, prop("natronSceneError")))
            check(not flat_frame(), "nudges %d ms apart: the settled frame is not one flat colour" % wait)
        check(lost_scene == 0, "nudges: the view keeps showing the scene (%d nudges without it)" % lost_scene)
        check(int(prop("natronSceneComposingPaints", 0) or 0) == composing,
              "nudges: no composing status is painted (%r, was %d)" % (prop("natronSceneComposingPaints"), composing))
        shot("m600-nudge-after.png")

    def stale_cue():
        settings = NatronEngine.natron.getSettings()
        style = settings.getParam("staleCueStyle")
        delay = settings.getParam("staleCueDelay")
        require(style is not None and delay is not None, "Settings has staleCueStyle and staleCueDelay")
        saved = (style.getValue(), delay.getValue())
        style.setValue(STYLE_SUBTLE_OUTLINE)
        delay.setValue(0)
        try:
            count.setValue(SCATTER_COUNT)
            # connectInput refuses an input that is already connected.
            viewer.disconnectInput(0)
            require(viewer.connectInput(0, scatter), "connect the Viewer to the GeoScatter")
            ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
            require(ok, "the 3D view shows the scatter (%r)" % (prop("natronSceneError"),))
            yield ("sleep", SETTLE_MS)
            check(not prop("natronStaleCueVisible", False), "stale cue: no cue on a settled view")
            samples = []
            start_sampling(samples, time.time())
            count.setValue(SLOW_SCATTER_COUNT)
            ok = yield ("until", lambda: any(s[3] for s in samples), POLL_TIMEOUT_S)
            check(ok, "stale cue: the 3D view shows the cue while the scatter is out of date")
            if ok:
                shot("m600-stale-cue.png")
            ok = yield ("until", lambda: scene_drawn() and not prop("natronStaleCueVisible", False), POLL_TIMEOUT_S)
            stop_sampling()
            check(ok, "stale cue: the cue goes once the view has caught up")
            check(all(s[4] for s in samples), "stale cue: the view keeps the last scatter up while out of date")
        finally:
            stop_sampling()
            style.setValue(saved[0])
            delay.setValue(saved[1])
            count.setValue(SCATTER_COUNT)

    def playback():
        require(seed.setExpression("frame", False, 0), "seed = frame")
        pv().seek(FIRST)
        ok = yield ("until", lambda: scene_drawn() and prop("natronGeoFrame") == FIRST, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows frame %d of the scatter (%r)" % (FIRST, prop("natronSceneError")))
        fps = float(app.getProjectParam("frameRate").getValue()) if app.getProjectParam("frameRate") else 24.
        period_ms = 1000. / fps
        budget_ms = period_ms + SAMPLE_MS
        stall_ms = 2. * period_ms
        for pass_index in range(2):
            pv().seek(FIRST)
            ok = yield ("until", lambda: scene_drawn() and prop("natronGeoFrame") == FIRST, POLL_TIMEOUT_S)
            check(ok, "pass %d: the view is on frame %d before playing" % (pass_index, FIRST))
            yield ("sleep", SETTLE_MS)
            resyncs = int(prop("natronGeoRefreshResyncs", 0) or 0)
            samples = []
            started = time.time()
            start_sampling(samples, started)
            pv().startForward()

            def at_end():
                shown = [s[2] for s in samples if s[2] is not None]
                wrapped = any(b < a for a, b in zip(shown, shown[1:]))
                return wrapped or (shown and shown[-1] >= LAST) or time.time() - started > PLAY_TIMEOUT_S

            yield ("until", at_end, PLAY_TIMEOUT_S + 5)
            pv().pause()
            stop_sampling()
            shown_at = []
            last = None
            for t, _, frame, _, _ in samples:
                if frame is not None and frame != last:
                    if last is not None and frame < last:
                        break
                    shown_at.append((t, frame))
                    last = frame
            per_frame = [(b[1], 1000. * (b[0] - a[0])) for a, b in zip(shown_at, shown_at[1:])]
            report("NOTE pass %d per-frame ms: %s" % (pass_index, ", ".join("%d:%.0f" % f for f in per_frame)))
            frames_shown = [f for _, f in shown_at]
            skipped = [f for f in range(FIRST, LAST + 1) if f not in frames_shown]
            report("NOTE pass %d: %d frames shown, skipped %r" % (pass_index, len(frames_shown), skipped))
            check(len(frames_shown) >= FRAMES // 2, "pass %d: playback shows the scatter's frames (%d)" % (pass_index, len(frames_shown)))
            # The first frame shown after the one playback starts on also waits for the start of
            # playback, which is not the rate frames come at.
            paced = per_frame[1:]
            mean_ms = sum(ms for _, ms in paced) / len(paced) if paced else 0.
            over = [(f, round(ms)) for f, ms in paced if ms > budget_ms]
            report("NOTE pass %d: mean %.1f ms per frame, over %.0f ms: %r" % (pass_index, mean_ms, budget_ms, over))
            if pass_index == 1:
                check(not skipped, "pass 1: no frame skipped (%r)" % (skipped,))
                check(int(prop("natronGeoRefreshResyncs", 0) or 0) == resyncs,
                      "pass 1: no frame resyncs a prim (%r, was %d)" % (prop("natronGeoRefreshResyncs"), resyncs))
                check(paced and mean_ms <= budget_ms, "pass 1: frames come at the project rate (mean %.1f ms, budget %.0f ms)" % (mean_ms, budget_ms))
                stalls = [(f, round(ms)) for f, ms in paced if ms >= stall_ms]
                check(not stalls, "pass 1: no frame takes two frames' time, %.0f ms (%r)" % (stall_ms, stalls))
            yield ("sleep", SETTLE_MS)
        check(not prop("natronSceneError"), "no error after playback (%r)" % (prop("natronSceneError"),))

    def all_steps():
        yield from drag_and_nudge()
        yield from stale_cue()
        yield from playback()

    run(all_steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
