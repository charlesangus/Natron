"""GUI check of the viewer's stale cue: each style shows while a slow render runs, and its timing.

Graph: CheckerBoard -> IDefocus (white Map) -> Viewer. For each of Subtle Outline and Corner Dot, the preference staleCueStyle is set and the IDefocus size raised to a
new value so a render runs for seconds. The cue must show once the viewer has been stale for the
delay, and a screenshot is saved while it shows. Once the image is current the cue must clear within
delay + hold + slack.

A short render must never show the cue: the delay is raised far above how long a small IDefocus
takes, the viewer is watched until its image is current, and the cue must stay hidden throughout.

Visibility is read from the GL viewer's natronStaleCueVisible property; whether the image is stale
from natronViewerStale.

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots (m451-stale-cue-*.png) and
results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 120.0
WATCHDOG_S = 280
DELAY_MS = 400
HOLD_MS = 500
SLACK_MS = 500
SHORT_RENDER_DELAY_MS = 10000
SHORT_DEFOCUS_SIZE = 2.0
# Each style gets its own size so no render is served from the cache; at these sizes the IDefocus
# keeps the viewer stale for seconds.
SLOW_DEFOCUS_SIZE = 500.0
SLOW_DEFOCUS_STEP = 10.0
INPUT_MAP = 3
STYLES = [
    (1, "subtle-outline"),
    (2, "corner-dot"),
]
DEFAULT_STYLE = 2
STYLE_CHOICE_IDS = ["off", "subtleOutline", "cornerDot"]

try:
    import NatronEngine
    
    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    NEVER_STALE_GRACE_S = 3.0

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def viewer_stale():
        w = viewer_gl()
        return w is not None and bool(w.property("natronViewerStale"))

    def stale_transitions():
        w = viewer_gl()
        return int(w.property("natronViewerStaleTransitions") or 0) if w is not None else 0

    def gl_cue_visible():
        w = viewer_gl()
        return w is not None and bool(w.property("natronStaleCueVisible"))

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()

        gt.section("preferences")
        settings = NatronEngine.natron.getSettings()
        style = settings.getParam("staleCueStyle")
        delay = settings.getParam("staleCueDelay")
        hold = settings.getParam("staleCueHold")
        require(style is not None, "Settings has a parameter named staleCueStyle")
        require(delay is not None, "Settings has a parameter named staleCueDelay")
        require(hold is not None, "Settings has a parameter named staleCueHold")
        check(style.getValue() == DEFAULT_STYLE, "the style defaults to Corner Dot (got %r)" % style.getValue())
        check(delay.getValue() == DELAY_MS, "the delay defaults to %d ms (got %r)" % (DELAY_MS, delay.getValue()))
        options = [str(o) for o in style.getOptions()]
        check(options == STYLE_CHOICE_IDS, "the style choices are %r (got %r)" % (STYLE_CHOICE_IDS, options))
        check(hold.getValue() == HOLD_MS, "the hold defaults to %d ms (got %r)" % (HOLD_MS, hold.getValue()))

        gt.section("setup")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        checker = app.createNode("CheckerBoard", -1, None, props)
        require(checker is not None, "create CheckerBoard", gt.plugin_problem("CheckerBoard"))
        white = app.createNode("Constant", -1, None, props)
        require(white is not None, "create Constant", gt.plugin_problem("Constant"))
        white.setPosition(200, 0)
        white.getParam("color").set(1.0, 1.0, 1.0, 1.0)
        defocus = app.createNode("IDefocus", -1, None, props)
        require(defocus is not None, "create IDefocus", gt.plugin_problem("IDefocus"))
        defocus.setPosition(0, 200)
        require(defocus.connectInput(0, checker), "the CheckerBoard feeds the IDefocus")
        require(defocus.connectInput(INPUT_MAP, white), "the Constant is the IDefocus Map")
        size = defocus.getParam("size")
        require(size is not None, "the IDefocus has a size parameter")
        size.set(1.0)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        require(viewer.connectInput(0, defocus), "the Viewer shows the IDefocus")
        gt.close_all_panels()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is visible")
        ok = yield ("until", lambda: not viewer_stale() and not gl_cue_visible(), RENDER_TIMEOUT_S)
        require(ok, "the first image settles with no cue")
        yield ("sleep", 1000)

        gt.section("short render")
        style.setValue(DEFAULT_STYLE)
        delay.setValue(SHORT_RENDER_DELAY_MS)
        seen = {"cue": False, "stale_at": None, "done_at": None}

        def watch_short():
            if gl_cue_visible():
                seen["cue"] = True
            stale = viewer_stale()
            if stale and seen["stale_at"] is None:
                seen["stale_at"] = time.time()
            now = time.time()
            if seen["stale_at"] is not None and not stale:
                seen["done_at"] = now
            elif seen["stale_at"] is None and now - started > NEVER_STALE_GRACE_S:
                seen["done_at"] = now
            return seen["done_at"] is not None

        transitions_before = stale_transitions()
        started = time.time()
        size.set(SHORT_DEFOCUS_SIZE)
        ok = yield ("until", watch_short, RENDER_TIMEOUT_S)
        went_stale = stale_transitions() - transitions_before
        check(went_stale > 0,
              "the viewer went stale during the short render (%d stale transitions); "
              "the render is too fast to exercise the cue, raise SHORT_DEFOCUS_SIZE" % went_stale)
        if seen["stale_at"] is not None:
            check(ok, "the short render completes")
        elapsed_ms = ((seen["done_at"] or time.time()) - started) * 1000.0
        check(elapsed_ms < SHORT_RENDER_DELAY_MS,
              "the short render is shorter than the delay (%.0f ms < %d ms)" % (elapsed_ms, SHORT_RENDER_DELAY_MS))
        yield ("sleep", HOLD_MS + SLACK_MS)
        if gl_cue_visible():
            seen["cue"] = True
        check(not seen["cue"], "a render shorter than the delay never shows the cue")
        delay.setValue(DELAY_MS)

        gt.section("styles")
        next_size = SLOW_DEFOCUS_SIZE
        for index, name in STYLES:
            style.setValue(index)
            yield ("sleep", 200)
            check(not gl_cue_visible(), "%s: no cue while the image is current" % name)
            size.set(next_size)
            next_size += SLOW_DEFOCUS_STEP
            ok = yield ("until", lambda: gl_cue_visible(), DELAY_MS / 1000.0 + 10.0)
            check(ok, "%s: the cue shows once the image has been stale for the delay" % name)
            if not ok:
                continue
            check(viewer_stale(), "%s: the image is still stale while the cue shows" % name)
            gt.shot("m451-stale-cue-%s.png" % name)

            ok = yield ("until", lambda: not viewer_stale(), RENDER_TIMEOUT_S)
            check(ok, "%s: the slow render completes" % name)
            current_at = time.time()
            limit_s = (DELAY_MS + HOLD_MS + SLACK_MS) / 1000.0
            ok = yield ("until", lambda: not gl_cue_visible(), limit_s + 5.0)
            cleared_ms = (time.time() - current_at) * 1000.0
            check(ok and cleared_ms <= limit_s * 1000.0,
                  "%s: the cue clears within delay + hold + slack of the image becoming current (%.0f ms <= %.0f ms)"
                  % (name, cleared_ms, limit_s * 1000.0))
            yield ("sleep", 300)

        gt.shot("m451-stale-cue-cleared.png")
        style.setValue(DEFAULT_STYLE)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
