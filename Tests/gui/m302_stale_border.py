"""GUI check that the viewer draws an amber border while its image is stale and clears it when current.

Graph: CheckerBoard -> IDefocus (white Map) -> Viewer. After the first image settles the border must
be absent. Raising the IDefocus size starts a slow render: the border must turn amber once the viewer
has been stale for the delay (preference viewerStaleBorderDelayMs), still be there while the render
runs, and be gone after the new image lands. Pixels are read from a screen grab of the GL viewer's
four edges; a screenshot is saved for each check.

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots (m302-stale-border-*.png) and
results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 120.0
BORDER_APPEARS_S = 5.0
WATCHDOG_S = 280
DELAY_MS = 150
# The render must outlast the delay plus two screen grabs, and a grab takes about 300 ms while the
# render holds every core. A Blur of size 400 is stale for about 250 ms; this IDefocus for seconds.
SLOW_DEFOCUS_SIZE = 500.0
INPUT_MAP = 3
BORDER_PX = 2

try:
    import NatronEngine
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def is_amber(c):
        return c[0] > 200 and 110 < c[1] < 230 and c[2] < 60

    def border_samples():
        """Screen colours on the middle of each edge of the first visible viewer, on the outermost
        BORDER_PX pixels; None when there is no viewer. Read from a screen grab since a widget grab
        of the GL viewer can come out blank under llvmpipe."""
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        if not visible:
            return None
        w = visible[0]
        QApplication.processEvents()
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        origin = w.mapToGlobal(w.rect().topLeft())
        points = []
        for d in range(BORDER_PX):
            points += [(w.width() // 2, d), (w.width() // 2, w.height() - 1 - d),
                       (d, w.height() // 2), (w.width() - 1 - d, w.height() // 2)]
        samples = []
        for x, y in points:
            c = image.pixelColor(origin.x() + x, origin.y() + y)
            samples.append((c.red(), c.green(), c.blue()))
        return samples

    def border_amber():
        s = border_samples()
        return s is not None and all(is_amber(c) for c in s)

    def border_clear():
        s = border_samples()
        return s is not None and not any(is_amber(c) for c in s)

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()

        gt.section("setup")
        settings = NatronEngine.natron.getSettings()
        delay = settings.getParam("viewerStaleBorderDelayMs")
        require(delay is not None, "Settings has a parameter named viewerStaleBorderDelayMs")
        check(delay.get() == DELAY_MS, "the delay defaults to %d ms (got %r)" % (DELAY_MS, delay.get()))

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
        yield ("sleep", 3000)

        gt.section("border")
        ok = yield ("until", border_clear, RENDER_TIMEOUT_S)
        check(ok, "no border while the viewer image is current")
        gt.shot("m302-stale-border-idle.png")

        size.set(SLOW_DEFOCUS_SIZE)
        ok = yield ("until", border_amber, BORDER_APPEARS_S)
        check(ok, "the border is amber once the image has been stale for the delay")
        gt.shot("m302-stale-border-stale.png")
        yield ("sleep", 500)
        still = border_amber()
        check(still, "the border stays amber while the slow render runs")
        gt.shot("m302-stale-border-held.png")

        ok = yield ("until", border_clear, RENDER_TIMEOUT_S)
        check(ok, "the border clears after the render completes")
        gt.shot("m302-stale-border-completed.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
