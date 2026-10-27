"""GUI check that the viewer never shows a frame cached before the Map of an STMap was disconnected.

Graph: CheckerBoard (Source) and a UV ramp Read (UV) into an STMap, with a half-strength Constant on
its Map input, into the Viewer. The UV ramp is written by the script into $NATRON_GUI_TEST_OUT
(cache-invalidation-uv-ramp.png, red across, green up, the project format's size), since no
generator in the tree draws one.

First the two references are taken from empty caches (Cache > Clear All Caches): the viewer with the
Map connected (A) and without it (B), read back as a grid of screen pixels over the viewer; they
must differ. Then, with the caches left alone, the Map is disconnected and reconnected:
  - while idle, after visiting frames 1-5: B on screen, the cached-frames line marks only the
    current frame, revisiting a frame cached with the Map shows B, reconnecting shows A again;
  - during a scrub (seeks to 6, 7 and 8 without waiting, disconnecting between 7 and 8): B at 8,
    and at 6, which was cached with the Map; reconnecting shows A;
  - during playback (disconnect a second after startForward, pause a second later): B at the
    frame the playback stopped on and at frame 1, cached with the Map; reconnecting shows A.

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots (cache-invalidation-*.png) and
results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
RENDER_MS = 1500
PLAY_MS = 1000
EDIT_APPLIES_S = 2.0
# Screen pixels are 8-bit and pass through the viewer's display transform.
COLOUR_SLACK = 3
GRID = 5

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_STMAP = "STMap"
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
STMAP_INPUT_UV = 0
STMAP_INPUT_SOURCE = 1
STMAP_INPUT_MAP = 3

try:
    import NatronEngine
    from PySide6.QtGui import QAction, QColor, QGuiApplication, QImage, QLinearGradient, QPainter
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def timeline():
        visible = [w for w in gt.widgets_of("TimeLineGui") if w.isVisible()]
        return visible[0] if visible else None

    def cached_frames():
        """The frames the cached line marks, sorted, or None when there is no timeline or property."""
        w = timeline()
        if w is None:
            return None
        value = w.property("natronCachedFrames")
        if value is None:
            return None
        return sorted(int(t) for t in value)

    def viewer_samples():
        """A GRID x GRID grid of screen colours over the middle of the first visible viewer, read from a
        screen grab since a widget grab of the GL viewer can come out blank under llvmpipe."""
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        if not visible:
            return None
        w = visible[0]
        QApplication.processEvents()
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        samples = []
        for j in range(GRID):
            for i in range(GRID):
                x = int(w.width() * (0.3 + 0.4 * i / (GRID - 1)))
                y = int(w.height() * (0.3 + 0.4 * j / (GRID - 1)))
                p = w.mapToGlobal(w.rect().topLeft())
                c = image.pixelColor(p.x() + x, p.y() + y)
                samples.append((c.red(), c.green(), c.blue()))
        return samples

    def same(a, b):
        if a is None or b is None or len(a) != len(b):
            return False
        return all(abs(x - y) <= COLOUR_SLACK for pa, pb in zip(a, b) for x, y in zip(pa, pb))

    def write_uv_ramp(path, width, height):
        image = QImage(width, height, QImage.Format_RGB32)
        image.fill(QColor(0, 0, 0))
        painter = QPainter(image)
        across = QLinearGradient(0, 0, width, 0)
        across.setColorAt(0.0, QColor(0, 0, 0))
        across.setColorAt(1.0, QColor(255, 0, 0))
        painter.fillRect(0, 0, width, height, across)
        painter.setCompositionMode(QPainter.CompositionMode_Plus)
        # Image rows run down, Natron's y runs up.
        up = QLinearGradient(0, height, 0, 0)
        up.setColorAt(0.0, QColor(0, 0, 0))
        up.setColorAt(1.0, QColor(0, 255, 0))
        painter.fillRect(0, 0, width, height, up)
        painter.end()
        return image.save(path)

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getPluginID(), name))
        return p

    def clear_all_caches(win):
        actions = [a for a in win.findChildren(QAction) if "Clear All Caches" in a.text().replace("&", "")]
        require(bool(actions), "found the Clear All Caches action")
        actions[0].trigger()

    def settle():
        """Waits until two grabs RENDER_MS apart agree, and returns the last one."""
        previous = None
        for _ in range(int(POLL_TIMEOUT_S * 1000 / RENDER_MS)):
            yield ("sleep", RENDER_MS)
            now = viewer_samples()
            if now is not None and same(now, previous):
                return now
            previous = now
        return previous

    def expect_shown(label, reference, name):
        ok = yield ("until", lambda: same(viewer_samples(), reference), POLL_TIMEOUT_S)
        # A late refresh from a frame cached before the edit must not replace it.
        yield ("sleep", RENDER_MS)
        check(ok and same(viewer_samples(), reference), "%s: the viewer shows the %s image" % (label, name))
        gt.shot("cache-invalidation-%s.png" % label.replace(" ", "-"))

    def expect_line(label, frames):
        ok = yield ("until", lambda: cached_frames() == frames, POLL_TIMEOUT_S)
        yield ("sleep", RENDER_MS)
        got = cached_frames()
        check(ok and got == frames, "%s: the cached line marks %r (got %r)" % (label, frames, got))

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        gt.close_all_panels()

        gt.section("setup")
        app.getProjectParam("frameRange").set(1, 30)
        ramp_path = os.path.join(gt.OUT, "cache-invalidation-uv-ramp.png")
        require(write_uv_ramp(ramp_path, 1920, 1080), "write the UV ramp " + ramp_path)
        checker = create(PLUGIN_CHECKER, 0, 0)
        uv = app.createReader(ramp_path)
        require(uv is not None, "create a Read for the UV ramp")
        uv.setPosition(200, 0)
        mapping = create(PLUGIN_CONSTANT, 400, 0)
        param(mapping, "color").set(0.5, 0.5, 0.5, 0.5)
        stmap = create(PLUGIN_STMAP, 200, 200)
        param(stmap, "uvOffset").set(0.1, 0.05)
        require(stmap.connectInput(STMAP_INPUT_SOURCE, checker), "the CheckerBoard feeds the STMap's Source")
        require(stmap.connectInput(STMAP_INPUT_UV, uv), "the ramp feeds the STMap's UV")
        require(stmap.connectInput(STMAP_INPUT_MAP, mapping), "the Constant feeds the STMap's Map")
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer exists")
        require(viewer.connectInput(0, stmap), "the Viewer shows the STMap")
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", lambda: gt.viewer_up() and timeline() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer and its timeline are visible")
        ok = yield ("until", lambda: cached_frames() is not None, POLL_TIMEOUT_S)
        require(ok, "the timeline publishes natronCachedFrames")

        # An edit made while the STMap renders (during playback) is queued and applies once the
        # renders it aborts have ended, so getInput (the rendered inputs) changes a moment later.
        def connect_map():
            require(stmap.connectInput(STMAP_INPUT_MAP, mapping), "reconnect the Map")
            ok = yield ("until", lambda: stmap.getInput(STMAP_INPUT_MAP) is not None, EDIT_APPLIES_S)
            require(ok, "the reconnected Map applies within %g s" % EDIT_APPLIES_S)

        def disconnect_map():
            stmap.disconnectInput(STMAP_INPUT_MAP)
            ok = yield ("until", lambda: stmap.getInput(STMAP_INPUT_MAP) is None, EDIT_APPLIES_S)
            require(ok, "the Map disconnect applies within %g s" % EDIT_APPLIES_S)

        gt.section("references")
        py_viewer.seek(1)
        clear_all_caches(win)
        with_map = yield from settle()
        require(with_map is not None, "read the viewer with the Map")
        gt.shot("cache-invalidation-reference-with-map.png")
        yield from disconnect_map()
        clear_all_caches(win)
        without_map = yield from settle()
        require(without_map is not None, "read the viewer without the Map")
        gt.shot("cache-invalidation-reference-without-map.png")
        require(not same(with_map, without_map), "the Map changes what the viewer shows, so a stale frame is visible")
        yield from connect_map()
        clear_all_caches(win)
        yield from expect_shown("reference restored", with_map, "with-Map")

        gt.section("idle")
        for frame in (1, 2, 3, 4, 5):
            py_viewer.seek(frame)
            yield ("until", lambda f=frame: f in (cached_frames() or []), POLL_TIMEOUT_S)
        yield from expect_line("idle visited", [1, 2, 3, 4, 5])
        yield from disconnect_map()
        yield from expect_shown("idle disconnected", without_map, "without-Map")
        yield from expect_line("idle disconnected", [5])
        py_viewer.seek(2)
        yield from expect_shown("idle revisit", without_map, "without-Map")
        yield from connect_map()
        yield from expect_shown("idle reconnected", with_map, "with-Map")

        gt.section("scrub")
        py_viewer.seek(6)
        py_viewer.seek(7)
        yield from disconnect_map()
        py_viewer.seek(8)
        yield from expect_shown("scrub disconnected", without_map, "without-Map")
        py_viewer.seek(6)
        yield from expect_shown("scrub revisit", without_map, "without-Map")
        yield from connect_map()
        yield from expect_shown("scrub reconnected", with_map, "with-Map")

        gt.section("playback")
        py_viewer.seek(1)
        yield from expect_shown("before playback", with_map, "with-Map")
        py_viewer.startForward()
        yield ("sleep", PLAY_MS)
        yield from disconnect_map()
        yield ("sleep", PLAY_MS)
        py_viewer.pause()
        report("NOTE playback paused at frame %d" % py_viewer.getCurrentFrame())
        yield from expect_shown("playback disconnected", without_map, "without-Map")
        py_viewer.seek(1)
        yield from expect_shown("playback revisit", without_map, "without-Map")
        yield from connect_map()
        yield from expect_shown("playback reconnected", with_map, "with-Map")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
