"""GUI check that the timeline's cached-frames line follows the viewer cache's current state.

Graph: Constant -> Viewer, project range 1-10. The script reads the line from the TimeLineGui
property natronCachedFrames (the frames the line marks), polling with a timeout:
  - after visiting frames 1-4, the line marks 1, 2, 3 and 4;
  - after a colour edit at frame 4, it marks only 4: the textures of 1-3 stay in the cache for
    undo, but the viewer would render those frames again;
  - after the colour is set back, it marks 1-4 again, since the original hash's textures are
    still cached and the frame shown comes from the cache;
  - after Cache > Clear All Caches, it marks nothing but the current frame, which the clear renders
    again.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
RENDER_MS = 1500

try:
    import NatronEngine
    from PySide6.QtGui import QAction
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

    def expect_line(label, frames):
        ok = yield ("until", lambda: cached_frames() == frames, POLL_TIMEOUT_S)
        # A late refresh must not bring back frames the line has dropped.
        yield ("sleep", RENDER_MS)
        got = cached_frames()
        check(ok and got == frames, "%s: the cached line marks %r (got %r)" % (label, frames, got))
        gt.shot("m178-timeline-%s.png" % label.replace(" ", "-"))

    def set_colour(param, rgba):
        param.set(rgba[0], rgba[1], rgba[2], rgba[3])

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        gt.close_all_panels()

        gt.section("setup")
        app.getProjectParam("frameRange").set(1, 10)
        constant = app.createNode("Constant")
        require(constant is not None, "create a Constant", gt.plugin_problem("Constant"))
        colour = constant.getParam("color")
        require(colour is not None, "the Constant has a color parameter")
        original = (0.25, 0.5, 0.75, 1.0)
        set_colour(colour, original)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        require(viewer.connectInput(0, constant), "the Viewer shows the Constant")
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", lambda: gt.viewer_up() and timeline() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer and its timeline are visible")
        ok = yield ("until", lambda: cached_frames() is not None, POLL_TIMEOUT_S)
        require(ok, "the timeline publishes natronCachedFrames")
        yield ("sleep", RENDER_MS)

        gt.section("visit")
        for frame in (1, 2, 3, 4):
            py_viewer.seek(frame)
            yield ("until", lambda f=frame: f in (cached_frames() or []), POLL_TIMEOUT_S)
        yield from expect_line("visited", [1, 2, 3, 4])

        gt.section("edit")
        set_colour(colour, (0.9, 0.1, 0.1, 1.0))
        yield from expect_line("after edit", [4])

        gt.section("revert")
        set_colour(colour, original)
        yield from expect_line("after revert", [1, 2, 3, 4])

        gt.section("clear all")
        actions = [a for a in win.findChildren(QAction) if "Clear All Caches" in a.text().replace("&", "")]
        require(bool(actions), "found the Clear All Caches action")
        actions[0].trigger()
        yield from expect_line("after clear all", [4])
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
