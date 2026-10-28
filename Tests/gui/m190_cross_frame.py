"""GUI check of a Cross Frame loop, its Bake and Load From Cache.

Graph: Constant (0.05) -> LoopBegin "trail" -> Grade (multiply 0.9, offset 0.01) -> LoopEnd
(Feedback, 4 iterations, Cross Frame from frame 1) -> Viewer. Each frame runs four substeps from
the frame before, so the image brightens from frame 1 towards 0.1.
  - labels: the cross-frame knobs carry the readable form of their script names;
  - bake: Bake writes frames 1 to BAKE_END to an EXR sequence under $NATRON_GUI_TEST_OUT, in the
    background, and the Progress panel shows it with a progress bar;
  - playback: the trail brightens from frame 1 to frame 10; with Load From Cache on and the Grade
    edited so a render would go dark, frame 10 still shows the baked trail, the LoopEnd warns that
    the frames predate the edit, and a frame past the bake continues, darker, from the last one.
Screenshots: m190-cross-frame-knobs.png, m190-bake-progress.png, m190-trail-baked.png,
m190-trail-loaded.png in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m190_cross_frame.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
BAKE_TIMEOUT_S = 150.0
WATCHDOG_S = 280
SETTLE_MS = 1500
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
GRADE_ID = "net.sf.openfx.GradePlugin"
BEGIN_ID = "fr.natron.LoopBegin"
END_ID = "fr.natron.LoopEnd"
# Long enough at the project format for the Progress panel to show the task: it shows a task
# without a frame range once it has run a second with more than four seconds left.
BAKE_END = 80
PAST_BAKE = BAKE_END + 2
LABELS = {
    "crossFrame": "Cross Frame",
    "startFrame": "Start Frame",
    "cacheFile": "Cache File",
    "bakeEnd": "Bake End",
    "bake": "Bake",
    "loadFromCache": "Load From Cache",
    "clearCache": "Clear Cache",
}
# Screen levels: the centre colour is read from a screen grab, after the viewer's display transform.
SAME_TOLERANCE = 3
BRIGHTER_BY = 4

try:
    from PySide6.QtCore import QMetaObject, QTimer
    from PySide6.QtWidgets import QApplication, QProgressBar

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def set_all(param, value):
        for d in range(param.getNumDimensions()):
            param.setValue(value, d)

    cache_dir = os.path.join(gt.OUT, "m190-bake")
    cache_pattern = os.path.join(cache_dir, "trail.####.exr")

    def baked_path(frame):
        return os.path.join(cache_dir, "trail.%04d.exr" % frame)

    def baked_count():
        return sum(1 for f in range(1, BAKE_END + 1) if os.path.isfile(baked_path(f)))

    progress = {"bars": 0, "peak": 0, "visible": False}

    def sample_progress():
        for panel in gt.widgets_of("ProgressPanel"):
            try:
                if not panel.isVisible():
                    continue
                progress["visible"] = True
                _held.append(panel)
                for bar in panel.findChildren(QProgressBar):
                    if bar.isVisible():
                        progress["bars"] += 1
                        progress["peak"] = max(progress["peak"], bar.value())
            except RuntimeError:
                continue

    def centre_red():
        c = gt.viewer_centre_colour()
        return c[0] if c is not None else None

    def show(py_viewer, frame):
        """Steps that show frame and settle; returns the centre's red level."""
        py_viewer.seek(frame)
        yield ("sleep", SETTLE_MS)
        QApplication.processEvents()
        return centre_red()

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        gt.close_all_panels()

        gt.section("setup")
        for f in range(1, BAKE_END + 1):
            if os.path.isfile(baked_path(f)):
                os.remove(baked_path(f))
        app.getProjectParam("frameRange").set(1, PAST_BAKE + 2)
        constant = app.createNode(CONSTANT_ID)
        require(constant is not None, "create a Constant", gt.plugin_problem(CONSTANT_ID))
        constant.getParam("color").set(0.05, 0.05, 0.05, 1.0)
        begin = app.createNode(BEGIN_ID)
        grade = app.createNode(GRADE_ID)
        end = app.createNode(END_ID)
        require(begin is not None and grade is not None and end is not None, "create the loop's nodes")
        begin.getParam("name").setValue("trail")
        set_all(grade.getParam("multiply"), 0.9)
        set_all(grade.getParam("offset"), 0.01)
        chain = (constant, begin, grade, end)
        for i, node in enumerate(chain):
            node.setPosition(0, i * 110)
            if i > 0:
                require(node.connectInput(0, chain[i - 1]), "connect %s" % node.getScriptName())
        end.getParam("mode").set("feedback")
        end.getParam("iterations").setValue(4)
        end.getParam("crossFrame").setValue(True)
        end.getParam("startFrame").setValue(1)
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None and viewer.connectInput(0, end), "the Viewer shows the LoopEnd")
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", gt.viewer_up, POLL_TIMEOUT_S)
        require(ok, "the viewer is visible")

        gt.section("labels")
        for name, label in LABELS.items():
            param = end.getParam(name)
            check(param is not None and param.getLabel() == label,
                  "%s is labelled %r (got %r)" % (name, label, param.getLabel() if param is not None else None))
        app.clearSelection()
        app.selectNode(end, True)
        view = node_graph_view()
        if view is not None:
            QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        yield ("sleep", 800)
        gt.shot("m190-cross-frame-knobs.png")

        gt.section("bake")
        end.getParam("cacheFile").setValue(cache_pattern)
        end.getParam("bakeEnd").setValue(BAKE_END)
        sampler = QTimer()
        sampler.setInterval(20)
        sampler.timeout.connect(sample_progress)
        sampler.start()
        end.getParam("bake").trigger()
        ok = yield ("until", lambda: progress["bars"] > 0 or baked_count() == BAKE_END, BAKE_TIMEOUT_S)
        if progress["bars"] > 0:
            gt.shot("m190-bake-progress.png")
        ok = yield ("until", lambda: baked_count() == BAKE_END, BAKE_TIMEOUT_S)
        check(ok, "Bake writes frames 1 to %d (got %d)" % (BAKE_END, baked_count()))
        check(not os.path.isfile(baked_path(BAKE_END + 1)), "Bake stops at Bake End")
        # The last file appears as it is written; the thread then ends and the task closes.
        yield ("sleep", SETTLE_MS)
        sampler.stop()
        check(progress["visible"] and progress["bars"] > 0,
              "the Progress panel shows the bake with a progress bar (bars seen %d, peak %d%%)"
              % (progress["bars"], progress["peak"]))
        message = end.getPersistentMessage() or ""
        check(message == "", "a bake that ran to the end leaves no message on the LoopEnd (got %r)" % message)
        sizes = sum(os.path.getsize(baked_path(f)) for f in range(1, BAKE_END + 1) if os.path.isfile(baked_path(f)))
        report("NOTE baked sequence: %d bytes" % sizes)

        gt.section("playback")
        gt.close_all_panels()
        first = yield from show(py_viewer, 1)
        tenth = yield from show(py_viewer, 10)
        check(first is not None and tenth is not None and tenth >= first + BRIGHTER_BY,
              "the trail brightens from frame 1 to frame 10 (red %r then %r)" % (first, tenth))
        gt.shot("m190-trail-baked.png")
        errors = [m for m in gt.viewer_messages() if "error" in m.lower()]
        check(not errors, "the viewer shows no error (got %r)" % errors)

        end.getParam("loadFromCache").setValue(True)
        end.getParam("clearCache").trigger()
        # A render with this Grade would fade to black; the frames baked before the edit load as baked.
        set_all(grade.getParam("offset"), 0.)
        played = []
        for frame in (1, 5, 10):
            red = yield from show(py_viewer, frame)
            played.append(red)
        check(None not in played and played == sorted(played),
              "frames 1, 5 and 10 play back the baked trail, brightening (red %r)" % played)
        check(played[-1] is not None and tenth is not None and abs(played[-1] - tenth) <= SAME_TOLERANCE,
              "frame 10 loads as baked after the Grade edit (red %r, baked %r)" % (played[-1], tenth))
        message = end.getPersistentMessage() or ""
        check("baked before" in message,
              "the LoopEnd warns that the frames predate the edit (got %r)" % message)
        gt.shot("m190-trail-loaded.png")

        last = yield from show(py_viewer, BAKE_END)
        past = yield from show(py_viewer, PAST_BAKE)
        check(last is not None and past is not None and past + BRIGHTER_BY <= last,
              "frame %d continues from the last frame baked under the edited Grade, darker (red %r then %r)"
              % (PAST_BAKE, last, past))
        errors = [m for m in gt.viewer_messages() if "error" in m.lower()]
        check(not errors, "the viewer shows no error after loading (got %r)" % errors)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    import traceback
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
            f.write("FAIL script error: " + traceback.format_exc() + "\n")
        os._exit(1)
