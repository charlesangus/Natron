"""GUI check of the viewer's stale cue: each style shows while a slow render runs, and its timing.

Graph: CheckerBoard -> IDefocus (white Map) -> Viewer. For each of Subtle Outline, Corner Dot, Edge Line
and Toolbar Spinner, the preference staleCueStyle is set and the IDefocus size raised to a
new value so a render runs for seconds. The cue must show once the viewer has been stale for the
delay, and a screenshot is saved while it shows. Once the image is current the cue must clear within
delay + hold + slack.

A short render must never show the cue: the delay is raised far above how long a small IDefocus
takes, the viewer is watched until its image is current, and the cue must stay hidden throughout.

The Toolbar Spinner must keep the toolbar's layout: the spinner's geometry and the toolbar row's
size hint are the same shown and hidden. While it shows, the spinner must be on screen: non-zero size,
wholly inside every ancestor widget and the main window, and not clipped (its visible region is its
whole rect). isVisible() alone passes for a spinner laid out past the clipped end of a full row.

Visibility is read from the GL viewer's natronStaleCueVisible property and from the isVisible() of
the toolbar widget named viewerStaleSpinner; whether the image is stale from natronViewerStale.

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
    (3, "edge-line"),
    (4, "toolbar-spinner"),
]
SPINNER_STYLE = 4
DEFAULT_STYLE = 2
STYLE_CHOICE_IDS = ["off", "subtleOutline", "cornerDot", "edgeLine", "toolbarSpinner"]

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, QRect
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def spinner():
        for w in QApplication.allWidgets():
            try:
                if w.objectName() == "viewerStaleSpinner":
                    return w
            except RuntimeError:
                continue
        return None

    def viewer_stale():
        w = viewer_gl()
        return w is not None and bool(w.property("natronViewerStale"))

    def gl_cue_visible():
        w = viewer_gl()
        return w is not None and bool(w.property("natronStaleCueVisible"))

    def spinner_visible():
        s = spinner()
        return s is not None and s.isVisible()

    def spinner_on_screen():
        """(ok, detail): the spinner shows, has a size, and no ancestor clips any of it."""
        s = spinner()
        if s is None or not s.isVisible():
            return False, "not visible"
        if s.width() <= 0 or s.height() <= 0:
            return False, "size %dx%d" % (s.width(), s.height())
        unclipped = s.visibleRegion().boundingRect()
        if unclipped != s.rect():
            return False, "visible region %r of rect %r" % (unclipped, s.rect())
        top = s.window()
        rect = QRect(s.mapTo(top, QPoint(0, 0)), s.size())
        lay = s.parentWidget().layout()
        last = lay.count() - 1 if lay is not None else -1
        if last < 1 or lay.itemAt(last).widget() is not s:
            return False, "the spinner is not the last widget of its toolbar row"
        picker = lay.itemAt(last - 1).widget()
        if picker is None or picker.mapTo(top, QPoint(0, 0)).x() + picker.width() > rect.x():
            return False, "the spinner is not to the right of the colour-picker button"
        w = s.parentWidget()
        while w is not None:
            area = QRect(w.mapTo(top, QPoint(0, 0)), w.size()) if w is not top else top.rect()
            if not area.contains(rect):
                return False, "%r in the window lies outside %s %r" % (rect, w.metaObject().className(), area)
            if w is top:
                break
            w = w.parentWidget()
        return True, "at %r in a %dx%d window" % (rect, top.width(), top.height())

    def any_cue_visible():
        return gl_cue_visible() or spinner_visible()

    def cue_visible(style):
        return spinner_visible() if style == SPINNER_STYLE else gl_cue_visible()

    def toolbar_layout():
        s = spinner()
        if s is None or s.parentWidget() is None:
            return None
        g = s.geometry()
        return (g.x(), g.y(), g.width(), g.height(), s.parentWidget().sizeHint().width())

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
        require(spinner() is not None, "the viewer toolbar has the stale spinner")
        ok = yield ("until", lambda: not viewer_stale() and not any_cue_visible(), RENDER_TIMEOUT_S)
        require(ok, "the first image settles with no cue")
        yield ("sleep", 1000)

        gt.section("short render")
        style.setValue(DEFAULT_STYLE)
        delay.setValue(SHORT_RENDER_DELAY_MS)
        seen = {"cue": False, "stale_at": None}

        def watch_short():
            if any_cue_visible():
                seen["cue"] = True
            stale = viewer_stale()
            if stale and seen["stale_at"] is None:
                seen["stale_at"] = time.time()
            return seen["stale_at"] is not None and not stale

        started = time.time()
        size.set(SHORT_DEFOCUS_SIZE)
        ok = yield ("until", watch_short, RENDER_TIMEOUT_S)
        if seen["stale_at"] is None:
            report("NOTE the short render finished before the viewer was seen stale")
        else:
            check(ok, "the short render completes")
        elapsed_ms = (time.time() - started) * 1000.0
        check(elapsed_ms < SHORT_RENDER_DELAY_MS,
              "the short render is shorter than the delay (%.0f ms < %d ms)" % (elapsed_ms, SHORT_RENDER_DELAY_MS))
        yield ("sleep", HOLD_MS + SLACK_MS)
        if any_cue_visible():
            seen["cue"] = True
        check(not seen["cue"], "a render shorter than the delay never shows the cue")
        delay.setValue(DELAY_MS)

        gt.section("styles")
        next_size = SLOW_DEFOCUS_SIZE
        layout_hidden = toolbar_layout()
        for index, name in STYLES:
            style.setValue(index)
            yield ("sleep", 200)
            check(not any_cue_visible(), "%s: no cue while the image is current" % name)
            size.set(next_size)
            next_size += SLOW_DEFOCUS_STEP
            ok = yield ("until", lambda: cue_visible(index), DELAY_MS / 1000.0 + 10.0)
            check(ok, "%s: the cue shows once the image has been stale for the delay" % name)
            if not ok:
                continue
            check(viewer_stale(), "%s: the image is still stale while the cue shows" % name)
            if index == SPINNER_STYLE:
                check(not gl_cue_visible(), "%s: the viewer draws no GL cue" % name)
                on_screen, detail = spinner_on_screen()
                check(on_screen, "%s: the spinner is on screen, sized and unclipped (%s)" % (name, detail))
                check(toolbar_layout() == layout_hidden,
                      "%s: the toolbar keeps its layout with the spinner shown (%r, hidden %r)"
                      % (name, toolbar_layout(), layout_hidden))
            else:
                check(not spinner_visible(), "%s: the toolbar spinner stays hidden" % name)
            gt.shot("m451-stale-cue-%s.png" % name)

            ok = yield ("until", lambda: not viewer_stale(), RENDER_TIMEOUT_S)
            check(ok, "%s: the slow render completes" % name)
            current_at = time.time()
            limit_s = (DELAY_MS + HOLD_MS + SLACK_MS) / 1000.0
            ok = yield ("until", lambda: not cue_visible(index), limit_s + 5.0)
            cleared_ms = (time.time() - current_at) * 1000.0
            check(ok and cleared_ms <= limit_s * 1000.0,
                  "%s: the cue clears within delay + hold + slack of the image becoming current (%.0f ms <= %.0f ms)"
                  % (name, cleared_ms, limit_s * 1000.0))
            yield ("sleep", 300)

        gt.shot("m451-stale-cue-cleared.png")
        check(toolbar_layout() == layout_hidden, "the toolbar layout is unchanged after the spinner hides")
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
