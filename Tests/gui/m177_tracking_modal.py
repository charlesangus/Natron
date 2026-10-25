"""Tracking blocks the interface like a foreground render. A CheckerBoard moved by a Transform feeds
a Tracker with one track; the Transform's translation is an expression that sleeps, so each frame
takes long enough to act on. The track is tracked forward over 30 frames: the "ForegroundTaskDialog"
is the active modal widget, and the viewer's track-forward (V) and stop (Escape) shortcuts, sent
through the window system with QTest, do nothing. Cancel stops tracking, and no marker key appears
past the frame it stopped at.

Before that, a second Tracker whose settings panel is never opened is tracked through the Python
API, and the same modal comes up and cancels.

The tracking starts from the V shortcut, sent the same way with no dialog up, which shows that
input reaches the viewer; Python starts it if the shortcut does not.

Screenshots: m177-track-panelless-modal.png, m177-track-modal.png (the dialog over the main window
while tracking) and m177-track-stopped.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m177_tracking_modal.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
FIRST = 1
LAST = 30
DIALOG_NAME = "ForegroundTaskDialog"
TRACKER_ID = "fr.inria.built-in.Tracker"

try:
    from PySide6.QtCore import QMetaObject, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QPushButton

    import guitest as gt
    from guitest import check, report, require

    state = {}

    def task_dialog():
        for w in QApplication.topLevelWidgets():
            try:
                if w.objectName() == DIALOG_NAME and w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def viewer_widget():
        widgets = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return widgets[0] if widgets else None

    def viewer_key(key):
        """key through the viewer's window to the viewer, as a key press from a person arrives."""
        viewer = viewer_widget()
        viewer.setFocus(Qt.OtherFocusReason)
        QTest.keyClick(viewer.window().windowHandle(), key)
        QApplication.processEvents()

    def key_times():
        param = state["track"].getParam("centerPoint")
        times = []
        for i in range(param.getNumKeys(0)):
            value = param.getKeyTime(i, 0)
            if isinstance(value, (tuple, list)):
                if value[0]:
                    times.append(value[1])
            else:
                times.append(value)
        return sorted(times)

    def last_key():
        times = key_times()
        return times[-1] if times else None

    def build_graph():
        gt.section("setup")
        app.getProjectParam("frameRange").set(FIRST, LAST + 10)
        checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
        require(checker is not None, "create a CheckerBoard", gt.plugin_problem("net.sf.openfx.CheckerBoardPlugin"))
        checker.setPosition(200, 100)
        transform = app.createNode("net.sf.openfx.TransformPlugin")
        require(transform is not None, "create a Transform", gt.plugin_problem("net.sf.openfx.TransformPlugin"))
        transform.setPosition(200, 200)
        transform.connectInput(0, checker)
        translate = transform.getParam("translate")
        translate.setExpression("__import__('time').sleep(0.2) or frame * 2", False, 0)
        tracker = app.createNode(TRACKER_ID)
        require(tracker is not None, "create a Tracker", gt.plugin_problem(TRACKER_ID))
        tracker.setPosition(200, 300)
        tracker.connectInput(0, transform)
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        viewer.setPosition(500, 400)
        viewer.connectInput(0, tracker)
        context = tracker.getTrackerContext()
        require(context is not None, "the Tracker has a tracker context")
        track = context.createTrack()
        require(track is not None, "create a track")
        track.getParam("centerPoint").setValue(960 + 20, 0)
        track.getParam("centerPoint").setValue(540 + 20, 1)
        state.update(checker=checker, transform=transform, tracker=tracker, context=context, track=track, viewer=viewer)
        yield ("sleep", 300)

    def panelless_section():
        """A second Tracker whose settings panel is never opened, tracked through the Python API."""
        gt.section("tracking from Python with no panel")
        gt.close_all_panels()
        tracker = app.createNode(TRACKER_ID)
        require(tracker is not None, "create a second Tracker", gt.plugin_problem(TRACKER_ID))
        tracker.setPosition(400, 300)
        tracker.connectInput(0, state["transform"])
        context = tracker.getTrackerContext()
        require(context is not None, "the second Tracker has a tracker context")
        track = context.createTrack()
        require(track is not None, "create a track on the second Tracker")
        track.getParam("centerPoint").setValue(960 + 20, 0)
        track.getParam("centerPoint").setValue(540 + 20, 1)
        yield ("sleep", 300)
        check(panel_of(tracker) is None, "the second Tracker has no settings panel open")
        check(not [w for w in gt.widgets_of("TrackerPanel") if w.isVisible()], "no Tracker panel is showing")

        context.startTracking([track], FIRST, LAST, True)
        up = yield ("until", task_dialog, POLL_TIMEOUT_S)
        if not check(up, "the foreground task dialog is up while the panel-less Tracker tracks"):
            context.stopTracking()
            yield ("sleep", 2000)
            return
        check(QApplication.activeModalWidget() is task_dialog(), "it is the active modal widget")
        gt.shot("m177-track-panelless-modal.png")
        dialog = task_dialog()
        buttons = [b for b in dialog.findChildren(QPushButton) if b.text() == "Cancel"]
        if check(len(buttons) == 1, "found the dialog's Cancel button"):
            buttons[0].click()
        closed = yield ("until", lambda: task_dialog() is None, POLL_TIMEOUT_S)
        check(closed, "the dialog closed once the panel-less tracking stopped")
        check(QApplication.activeModalWidget() is None, "no modal widget is left")
        check(panel_of(tracker) is None, "tracking did not open the second Tracker's panel")
        yield ("sleep", 500)

    def open_tracker_panel():
        # The Tracker's panel puts its tools in the viewer, so its shortcuts can start tracking.
        tracker = state["tracker"]
        gt.close_all_panels()
        app.selectNode(tracker, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(tracker) is not None, POLL_TIMEOUT_S)
        require(ok, "open the Tracker's settings panel")
        app.getViewer("Viewer1").seek(FIRST)
        yield ("sleep", 1500)

    def tracking_section():
        gt.section("tracking")
        viewer_key(Qt.Key_V)
        up = yield ("until", task_dialog, 5.0)
        if not check(up, "the viewer's track-forward key through the window starts tracking"):
            report("NOTE starting the track from Python instead")
            state["context"].startTracking([state["track"]], FIRST, LAST, True)
            up = yield ("until", task_dialog, POLL_TIMEOUT_S)
        require(up, "the foreground task dialog is up while tracking")
        check(QApplication.activeModalWidget() is task_dialog(), "it is the active modal widget")
        ok = yield ("until", lambda: (last_key() or FIRST) >= FIRST + 3, POLL_TIMEOUT_S)
        check(ok, "tracking is writing keys (%r)" % key_times())
        gt.shot("m177-track-modal.png")

        # Stop, then track forward again: both must be blocked by the dialog.
        viewer_key(Qt.Key_Escape)
        viewer_key(Qt.Key_V)
        before = last_key()
        progressed = yield ("until", lambda: (last_key() or FIRST) > (before or FIRST) + 1, POLL_TIMEOUT_S)
        check(progressed, "tracking went on after the stop key (%r -> %r)" % (before, last_key()))
        check(task_dialog() is not None, "the dialog is still up after the keys")

        dialog = task_dialog()
        buttons = [b for b in dialog.findChildren(QPushButton) if b.text() == "Cancel"] if dialog else []
        require(len(buttons) == 1, "found the dialog's Cancel button")
        buttons[0].click()
        at_cancel = last_key()
        closed = yield ("until", lambda: task_dialog() is None, POLL_TIMEOUT_S)
        check(closed, "the dialog closed once tracking stopped")
        stopped_at = last_key()
        report("NOTE last key at Cancel %r, once stopped %r" % (at_cancel, stopped_at))
        check(stopped_at is not None and stopped_at < LAST, "Cancel stopped tracking before frame %d (%r)" % (LAST, stopped_at))
        check(stopped_at is not None and at_cancel is not None and stopped_at <= at_cancel + 2,
              "at most the frame in flight was tracked after Cancel (%r -> %r)" % (at_cancel, stopped_at))
        yield ("sleep", 2000)
        check(last_key() == stopped_at, "no marker key appeared past the stop frame (%r -> %r)" % (stopped_at, last_key()))
        check(QApplication.activeModalWidget() is None, "no modal widget is left")
        gt.shot("m177-track-stopped.png")

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        yield from build_graph()
        yield from panelless_section()
        yield from open_tracker_panel()
        yield from tracking_section()

    if app.getNode("Viewer1") is None:
        app.createNode("fr.inria.built-in.Viewer")
    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
