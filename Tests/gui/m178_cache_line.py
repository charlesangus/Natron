"""GUI check that the timeline's cached-frames line shows what playback cached, in the cached-frame
colour, and follows edits and undo.

Graph: Constant -> Blur -> Viewer, project range 1-6. The script reads the line from the TimeLineGui
properties natronCachedFrames (every frame the line marks) and natronCachedFramesInRAM (the frames
drawn in the bright cached-frame colour rather than the dark disk-cached one), and counts pixels of
the cached-frame colour in the timeline on screen:
  - while playback runs, the line already marks frames;
  - after playing 1-6, it marks 1-6, all in the cached-frame colour, and that colour is on screen;
  - after an undoable Blur size edit made through the Script Editor, the frames cached under the
    old state leave the line (only frames rendered since may remain);
  - after Edit > Undo, it marks 1-6 again, served from the cache;
  - after Cache > Clear All Caches, it marks at most the current frame, which the clear renders again.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 280
SETTLE_MS = 1500
FIRST, LAST = 1, 6
ALL_FRAMES = list(range(FIRST, LAST + 1))
BLUR = "Blur"
# The default "Cached frame" colour of the Appearance settings, 0.56 0.79 0.4.
CACHED_RGB = (143, 201, 102)
COLOUR_TOLERANCE = 16

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QAction, QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow, QPlainTextEdit

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def timeline():
        visible = [w for w in gt.widgets_of("TimeLineGui") if w.isVisible()]
        return visible[0] if visible else None

    def frames_property(name):
        w = timeline()
        if w is None:
            return None
        value = w.property(name)
        if value is None:
            return None
        return sorted(int(t) for t in value)

    def cached_frames():
        """The frames the cached line marks, sorted, or None when there is no timeline or property."""
        return frames_property("natronCachedFrames")

    def ram_frames():
        """The frames the cached line draws in the cached-frame colour."""
        return frames_property("natronCachedFramesInRAM")

    def covers(frames):
        got = cached_frames() or []
        return all(f in got for f in frames)

    def cached_colour_pixels():
        """Pixels of the cached-frame colour inside the timeline, as on screen."""
        w = timeline()
        if w is None:
            return 0
        QApplication.processEvents()
        top_left = w.mapToGlobal(w.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        count = 0
        for y in range(max(0, top_left.y()), min(image.height(), top_left.y() + w.height())):
            for x in range(max(0, top_left.x()), min(image.width(), top_left.x() + w.width())):
                c = image.pixelColor(x, y)
                if (abs(c.red() - CACHED_RGB[0]) <= COLOUR_TOLERANCE and abs(c.green() - CACHED_RGB[1]) <= COLOUR_TOLERANCE
                        and abs(c.blue() - CACHED_RGB[2]) <= COLOUR_TOLERANCE):
                    count += 1
        return count

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def app_var():
        """The name the Script Editor knows this project by."""
        return "app%d" % (app.getAppID() + 1)

    def run_in_script_editor(script):
        """Puts script into the Script Editor and runs it the way its Execute button does, so the
        edit lands in the undo history."""
        editors = gt.widgets_of("ScriptEditor")
        editor = next((e for e in editors if e.isVisible()), editors[0] if editors else None)
        if editor is None:
            return False
        inputs = [w for w in editor.findChildren(QPlainTextEdit) if gt.class_name(w) == "InputScriptTextEdit"]
        if len(inputs) != 1:
            return False
        inputs[0].setPlainText(script)
        QMetaObject.invokeMethod(editor, "onExecScriptClicked")
        QApplication.processEvents()
        return True

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        gt.close_all_panels()

        gt.section("setup")
        app.getProjectParam("frameRange").set(FIRST, LAST)
        constant = app.createNode("Constant")
        require(constant is not None, "create a Constant", gt.plugin_problem("Constant"))
        constant.getParam("color").set(0.25, 0.5, 0.75, 1.0)
        blur = app.createNode(BLUR)
        require(blur is not None, "create a Blur", gt.plugin_problem(BLUR))
        require(blur.connectInput(0, constant), "the Blur reads the Constant")
        size = blur.getParam("size")
        require(size is not None, "the Blur has a size parameter")
        size.set(3.0, 3.0)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        require(viewer.connectInput(0, blur), "the Viewer shows the Blur")
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", lambda: gt.viewer_up() and timeline() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer and its timeline are visible")
        ok = yield ("until", lambda: cached_frames() is not None and ram_frames() is not None, POLL_TIMEOUT_S)
        require(ok, "the timeline publishes natronCachedFrames and natronCachedFramesInRAM")
        py_viewer.seek(FIRST)
        yield ("sleep", SETTLE_MS)

        gt.section("play")
        py_viewer.startForward()
        live = yield ("until", lambda: len(cached_frames() or []) > 0, POLL_TIMEOUT_S)
        check(live, "the line marks frames while playback runs (got %r)" % cached_frames())
        ok = yield ("until", lambda: covers(ALL_FRAMES), POLL_TIMEOUT_S)
        py_viewer.pause()
        yield ("sleep", SETTLE_MS)
        played = cached_frames()
        check(ok and covers(ALL_FRAMES), "after playing %d-%d the line marks them all (got %r)" % (FIRST, LAST, played))
        in_ram = ram_frames() or []
        check(all(f in in_ram for f in ALL_FRAMES),
              "the played frames are drawn in the cached-frame colour, not the disk-cached one (got %r)" % in_ram)
        pixels = cached_colour_pixels()
        check(pixels > 0, "the timeline on screen shows the cached-frame colour (%d pixels)" % pixels)
        gt.shot("m178-cache-line-played.png")

        gt.section("edit")
        current = py_viewer.getCurrentFrame()
        require(run_in_script_editor("%s.%s.size.set(9.0, 9.0)\n" % (app_var(), blur.getScriptName())),
                "ran the Blur size edit in the Script Editor")
        require(abs(size.getValue(0) - 9.0) < 1e-9, "the Blur size is 9")

        def old_frames_gone():
            got = cached_frames()
            return got is not None and len(got) <= 1

        ok = yield ("until", old_frames_gone, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        got = cached_frames()
        check(ok and got is not None and len(got) <= 1,
              "after the edit the line drops the frames cached before it, keeping at most the frame on screen "
              "(got %r, current frame %r)" % (got, current))
        gt.shot("m178-cache-line-edited.png")

        gt.section("undo")
        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()
        ok = yield ("until", lambda: covers(ALL_FRAMES), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        got = cached_frames()
        check(ok and covers(ALL_FRAMES), "after undo the line marks %d-%d again (got %r)" % (FIRST, LAST, got))
        in_ram = ram_frames() or []
        check(all(f in in_ram for f in ALL_FRAMES), "after undo they are drawn in the cached-frame colour (got %r)" % in_ram)
        gt.shot("m178-cache-line-undone.png")

        gt.section("clear all")
        actions = [a for a in win.findChildren(QAction) if "Clear All Caches" in a.text().replace("&", "")]
        require(bool(actions), "found the Clear All Caches action")
        actions[0].trigger()
        ok = yield ("until", lambda: len(cached_frames() or []) <= 1, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        got = cached_frames()
        check(ok and got is not None and len(got) <= 1, "after Clear All Caches the line marks at most the current frame (got %r)" % got)
        gt.shot("m178-cache-line-cleared.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
