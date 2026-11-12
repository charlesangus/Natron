"""Playback of time-varying geometry in the 3D view: every played frame refreshes the viewer's copy of
the geometry once, and the view shows the frame the timeline is on.

Graph: GeoPrimitive (card) -> GeoScatter (count points) -> GeoCopyToPoints input 0, a second
GeoPrimitive (GEO_PLAYBACK_TARGET, default card) on its input 1 -> GeoWrangle moving @P with @Frame
-> Viewer1. GEO_PLAYBACK_COUNT sets the scatter count (default 1000), GEO_PLAYBACK_FRAMES the frames
played (default 60).

Checks:
  - played frames refresh the viewer's geometry once each (natronGeoRefreshes rises by one per
    timeline frame, give or take the frame playback stops on);
  - each refresh shows the frame the timeline was on when it landed (natronGeoFrame), so the
    view never shows a frame the timeline has not reached or one it left more than a frame ago;
  - playback resyncs no prim (natronGeoRefreshResyncs stays 0);
  - stepping frames one at a time still refreshes each frame, at that frame.

Reported, not checked: the timeline rate, the refresh rate, and the delay from a timeline frame
to the refresh that shows it. Run through Tests/gui/run-gui-test.sh; results.txt and samples.tsv go
to $NATRON_GUI_TEST_OUT. Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 700
SAMPLE_MS = 4
FIRST = 1
FRAMES = int(os.environ.get("GEO_PLAYBACK_FRAMES", "60"))
COUNT = int(os.environ.get("GEO_PLAYBACK_COUNT", "1000"))
TARGET = int(os.environ.get("GEO_PLAYBACK_TARGET", "0"))
PLAY_TIMEOUT_S = 120.0
STEPPED = 5

try:
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication, QWidget

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up

    LAST = FIRST + FRAMES - 1
    try:
        app.getProjectParam("frameRange").set(FIRST, LAST)
    except Exception:
        report("NOTE could not set the project frame range: " + traceback.format_exc().splitlines()[-1])

    card = app.createNode("GeoPrimitive")
    require(card is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    scatter = app.createNode("GeoScatter")
    require(scatter is not None, "create GeoScatter", plugin_problem("GeoScatter"))
    scatter.getParam("count").setValue(COUNT)
    target = app.createNode("GeoPrimitive")
    target.getParam("type").setValue(TARGET)
    copy = app.createNode("GeoCopyToPoints")
    require(copy is not None, "create GeoCopyToPoints", plugin_problem("GeoCopyToPoints"))
    wrangle = app.createNode("GeoWrangle")
    require(wrangle is not None, "create GeoWrangle", plugin_problem("GeoWrangle"))
    wrangle.getParam("snippet").setValue("@P.y += 0.25 * sin(@Frame * 0.3 + @P.x * 4.0);")
    require(scatter.connectInput(0, card), "connect the GeoScatter to the card")
    require(copy.connectInput(0, scatter), "connect the scattered points to GeoCopyToPoints input 0")
    require(copy.connectInput(1, target), "connect the second GeoPrimitive to GeoCopyToPoints input 1")
    require(wrangle.connectInput(0, copy), "connect the GeoWrangle to the GeoCopyToPoints")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    for i, node in enumerate((card, scatter, target, copy, wrangle, viewer)):
        node.setPosition(0, 100 * i)
    py_viewer = {}
    keep = {}

    def view_and_parent():
        # findChild searches in C++: walking every widget from Python would hold the main thread
        # longer than a played frame and slow the playback being measured.
        for top in QApplication.topLevelWidgets():
            if not top.isVisible():
                continue
            for v in top.findChildren(QWidget, "SceneViewport"):
                if v.isVisible():
                    return v, top
        return None, None

    def prop(name, default=None):
        v, top = view_and_parent()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def geo_frame():
        value = prop("natronGeoFrame")
        return None if value is None else int(round(float(value)))

    def scene_drawn():
        v, top = view_and_parent()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def pv():
        if "v" not in py_viewer:
            py_viewer["v"] = app.getViewer(viewer.getScriptName())
        return py_viewer["v"]

    def start_sampling(samples, started):
        def sample():
            v, top = view_and_parent()
            if v is None:
                return
            samples.append((time.time() - started, app.timelineGetTime(), int(v.property("natronGeoRefreshes") or 0),
                            int(v.property("natronSceneSyncedAtMs") or 0), int(v.property("natronSceneLastPaintMs") or 0),
                            geo_frame(), int(v.property("natronSceneSyncRequests") or 0)))

        timer = QTimer()
        timer.setInterval(SAMPLE_MS)
        timer.timeout.connect(sample)
        timer.start()
        keep["timer"] = timer

    def stop_sampling():
        timer = keep.pop("timer", None)
        if timer is not None:
            timer.stop()

    def write_samples(samples):
        out = os.environ.get("NATRON_GUI_TEST_OUT", "")
        if out:
            with open(os.path.join(out, "samples.tsv"), "w") as f:
                f.write("t\ttimeline\trefreshes\tsyncedAtMs\tlastPaintMs\tgeoFrame\tsyncRequests\n")
                for s in samples:
                    f.write("\t".join(str(x) for x in s) + "\n")

    def summarise(samples):
        frame_changes = []
        refresh_events = []
        last_frame = None
        last_refreshes = None
        for t, frame, refreshes, _, _, shown, _ in samples:
            if frame != last_frame:
                frame_changes.append((t, frame))
                last_frame = frame
            if last_refreshes is not None and refreshes != last_refreshes:
                refresh_events.append((t, frame, refreshes - last_refreshes, shown))
            last_refreshes = refreshes
        return frame_changes, refresh_events

    def steps():
        require(viewer.connectInput(0, wrangle), "connect the Viewer to the GeoWrangle")
        ok = yield ("until", lambda: viewer_up() or view_and_parent()[0] is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        pv().seek(FIRST)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the 3D view shows the geometry (%r)" % (prop("natronSceneError"),))
        yield ("sleep", SETTLE_MS)
        shot("m376-playback-start.png")
        require(prop("natronGeoFrame") is not None, "the 3D view publishes the frame its geometry was evaluated at")

        for run_index in range(2):
            pv().seek(FIRST)
            yield ("sleep", SETTLE_MS)
            resyncs0 = int(prop("natronGeoRefreshResyncs", 0))
            samples = []
            started = time.time()
            start_sampling(samples, started)
            pv().startForward()
            def at_end():
                frames = [s[1] for s in samples]
                wrapped = any(b < a for a, b in zip(frames, frames[1:]))
                return wrapped or (frames and frames[-1] >= LAST) or time.time() - started > PLAY_TIMEOUT_S

            yield ("until", at_end, PLAY_TIMEOUT_S + 5)
            window = list(samples)
            pv().pause()
            yield ("sleep", SETTLE_MS)
            stop_sampling()
            write_samples(samples)
            elapsed = samples[-1][0] if samples else 0.
            frame_changes, refresh_events = summarise(window)
            # Frames and refreshes are both counted between the first and last playback sample, so neither
            # includes the settle after pause(), where the view catches up to the stopped frame.
            refreshes = window[-1][2] - window[0][2] if window else 0
            played = len(frame_changes) - 1
            play_s = window[-1][0] - window[0][0] if window else 0.
            report("NOTE run %d: %d timeline frames in %.2fs (%.1f fps), %d refreshes (%.1f /s), %.2f refreshes per frame, %d samples over %.1fs"
                   % (run_index, played, play_s, played / play_s if play_s > 0 else 0., refreshes,
                      refreshes / play_s if play_s > 0 else 0., refreshes / float(max(played, 1)),
                      len(samples), elapsed))
            # Delay from each timeline frame to the first refresh after it.
            delays = []
            for t, frame in frame_changes[1:]:
                later = [e for e in refresh_events if e[0] >= t]
                if later:
                    delays.append(later[0][0] - t)
            if delays:
                delays.sort()
                report("NOTE run %d: frame-to-refresh delay median %.0f ms, p90 %.0f ms, max %.0f ms"
                       % (run_index, 1000 * delays[len(delays) // 2], 1000 * delays[int(len(delays) * 0.9)], 1000 * delays[-1]))
            gaps = [b[0] - a[0] for a, b in zip(refresh_events, refresh_events[1:])]
            if gaps:
                gaps.sort()
                report("NOTE run %d: refresh gap median %.0f ms, p90 %.0f ms, max %.0f ms"
                       % (run_index, 1000 * gaps[len(gaps) // 2], 1000 * gaps[int(len(gaps) * 0.9)], 1000 * gaps[-1]))
            check(played >= FRAMES // 2, "run %d: playback advances the timeline (%d frames)" % (run_index, played))
            check(abs(refreshes - played) <= 1,
                  "run %d: one refresh per played frame (%d refreshes, %d frames)" % (run_index, refreshes, played))
            check(int(prop("natronGeoRefreshResyncs", 0)) == resyncs0,
                  "run %d: playback resyncs no prim (%r)" % (run_index, prop("natronGeoRefreshResyncs")))
            off = [(round(t, 3), frame, shown) for t, frame, _, shown in refresh_events
                   if shown is None or shown > frame or shown < frame - 1]
            check(not off, "run %d: every refresh shows the timeline's frame or the one before it (%d off: %r)"
                  % (run_index, len(off), off[:8]))
            check(geo_frame() == app.timelineGetTime(),
                  "run %d: after playback stops the view shows the timeline's frame (%r, %d)"
                  % (run_index, geo_frame(), app.timelineGetTime()))
        shot("m376-playback-end.png")

        for frame in range(FIRST + 1, FIRST + 1 + STEPPED):
            refreshes = int(prop("natronGeoRefreshes", 0))
            pv().seek(frame)
            ok = yield ("until", lambda: int(prop("natronGeoRefreshes", 0)) > refreshes, POLL_TIMEOUT_S)
            check(ok, "stepping to frame %d refreshes the viewer's geometry" % frame)
            ok = yield ("until", lambda: geo_frame() == frame, POLL_TIMEOUT_S)
            check(ok, "stepping to frame %d shows frame %d (%r)" % (frame, frame, geo_frame()))
        check(not prop("natronSceneError"), "no error after playback (%r)" % (prop("natronSceneError"),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
