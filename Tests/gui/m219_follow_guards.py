"""While a vector follow runs in Roto, an application-modal progress window blocks the interface, in the app.

The follow writes the shape as it goes, so an edit made meanwhile would be overwritten by the frames
still to come, or would fight them. The progress window, which has only a Cancel button, keeps the user
out until the follow ends or is cancelled. A ctest cannot open it (there is no Gui headless), so this
runs the real app. Plate, scene and follow helpers are in m219_follow_common.py.

Input is delivered through the window system (QTest on the top-level window), as a user's would be, so
Qt's modal blocking applies; QApplication.sendEvent would bypass it. An application event filter counts
the mouse and key events that reach the viewer, the node graph and the Roto panel.

A TV-L1 follow of the whole plate is started with one control point selected by a real click. Once the
first frames have been baked, and with the follow still running:

  modal     the window "ForegroundTaskDialog" is shown, application modal, and its only button is Cancel
  viewer    a press and drag on the selected point and Delete reach no viewer widget and move nothing
  graph     a click and Delete in the node graph reach no graph widget; the Roto node is still there
  panel     a click in the Roto panel reaches no panel widget
  cancel    pressing Cancel ends the follow within ABORT_END_S as a user stop, opening no dialog, and
            the progress window goes away
  control   afterwards the same clicks reach the graph and the panel, and the drag moves the point, and
            only that point

A screenshot is taken mid-follow (m219-follow-guard.png in the output directory).

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m219-curves-follow-vectors/out-guards -e GUI_TEST_TIMEOUT=900 \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m219_follow_guards.py <Natron AppImage>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 3000
GUARD_AFTER_KEYS = 2
DRAG_PX = 40.0
DRAG_STEPS = 8
MOVED_MIN = 5.0
DIALOG_NAME = "ForegroundTaskDialog"

try:
    import faulthandler

    from PySide6.QtCore import QEvent, QObject, QPoint, QPointF, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QPushButton

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require
    from m219_follow_common import *
    import m219_follow_common

    m219_follow_common.bind(app)

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    INPUT_EVENTS = (QEvent.MouseButtonPress, QEvent.MouseButtonRelease, QEvent.MouseButtonDblClick,
                    QEvent.MouseMove, QEvent.KeyPress, QEvent.KeyRelease)

    class InputCounter(QObject):
        """Counts, per target, the input events delivered to that widget or one of its children."""

        def __init__(self, targets):
            QObject.__init__(self)
            self.targets = targets
            self.counts = dict((name, 0) for name in targets)

        def reset(self):
            for name in self.counts:
                self.counts[name] = 0

        def eventFilter(self, obj, event):
            try:
                if event.type() in INPUT_EVENTS and obj.isWidgetType():
                    for name, target in self.targets.items():
                        if obj is target or target.isAncestorOf(obj):
                            self.counts[name] += 1
            except RuntimeError:
                pass
            return False

    def corner_xy(bez, i, t):
        p = pos(bez, i, t)
        return (p[0], p[1])

    def widget_of(vmap, bez, i, t):
        x, y = corner_xy(bez, i, t)
        return vmap.to_widget(x, y)

    def moved(a, b):
        return max(abs(a[0] - b[0]), abs(a[1] - b[1]))

    def in_window(widget, p):
        """The top-level window of widget and p, a point in widget, in that window's coordinates."""
        top = widget.window()
        return top.windowHandle(), widget.mapTo(top, QPoint(int(round(p.x())), int(round(p.y()))))

    def os_drag(widget, start, end):
        window, a = in_window(widget, start)
        _, b = in_window(widget, end)
        QTest.mouseMove(window, a)
        QTest.mousePress(window, Qt.LeftButton, Qt.NoModifier, a)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            QTest.mouseMove(window, QPoint(int(round(a.x() + (b.x() - a.x()) * f)), int(round(a.y() + (b.y() - a.y()) * f))))
        QTest.mouseRelease(window, Qt.LeftButton, Qt.NoModifier, b)
        QApplication.processEvents()

    def os_click(widget, p):
        window, at = in_window(widget, p)
        QTest.mouseClick(window, Qt.LeftButton, Qt.NoModifier, at)
        QApplication.processEvents()

    def os_key(widget, key):
        widget.setFocus()
        QTest.keyClick(widget.window().windowHandle(), key)
        QApplication.processEvents()

    def roto_panel(scene):
        for w in gt.widgets_of("NodeSettingsPanel"):
            try:
                if w.isVisible() and scene.roto.getScriptName() in w.objectName() + " " + w.windowTitle():
                    return w
            except RuntimeError:
                continue
        shown = [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]
        return shown[0] if len(shown) == 1 else None

    def progress_window():
        w = QApplication.activeModalWidget()
        if w is None or not w.isVisible() or w.objectName() != DIALOG_NAME:
            return None
        return w

    def section_guards(scene):
        set_project_range(START, PLATE_FRAMES)
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.slow_gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        select_shapes([bez])
        yield from go_to(START)

        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        keep(gl)
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        require(graphs != [], "the node graph is visible")
        graph = keep(graphs[0])
        panel = roto_panel(scene)
        require(panel is not None, "the Roto panel is open")
        keep(panel)
        rg.activate()
        yield ("sleep", SETTLE_MS)
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")
        rg.select_tool(gl)

        # A real click selects point 0 alone, so a leaked select-all or a leaked Delete shows up later.
        grab = widget_of(vmap, bez, 0, START)
        rg.click(gl, grab)
        yield ("sleep", SEEK_MS)
        before = [corner_xy(bez, i, START) for i in range(POINTS)]
        scale = vmap.pixel_scale()
        target = QPointF(grab.x() + DRAG_PX, grab.y())
        graph_at = QPointF(graph.width() * 0.9, graph.height() * 0.9)
        panel_at = QPointF(4.0, 4.0)
        counter = InputCounter({"viewer": gl, "graph": graph, "panel": panel})
        state = {"done": False, "keys": 0, "cancelled": False}
        seen = {}

        def probe():
            return [corner_xy(bez, i, START) for i in range(POINTS)]

        def blocked_input(dialog):
            seen["modality"] = dialog.windowModality()
            seen["buttons"] = [str(b.text()) for b in dialog.findChildren(QPushButton) if b.isVisible()]
            counter.reset()
            QApplication.instance().installEventFilter(counter)
            try:
                os_drag(gl, grab, target)
                os_key(gl, Qt.Key_Delete)
                seen["viewer"] = probe()
                seen["viewer_count"] = bez.getNumControlPoints()
                os_click(graph, graph_at)
                os_key(graph, Qt.Key_Delete)
                os_click(panel, panel_at)
            finally:
                QApplication.instance().removeEventFilter(counter)
            seen["counts"] = dict(counter.counts)
            seen["roto_alive"] = app.getNode(scene.roto.getScriptName()) is not None
            seen["running"] = scene.ctx.isFollowingVectors()

        def tick():
            if state["cancelled"]:
                return None
            if not scene.ctx.isFollowingVectors():
                return None
            keys = len(keys_of(bez))
            dialog = progress_window()
            if keys < GUARD_AFTER_KEYS or dialog is None:
                return None
            state["done"] = True
            state["keys"] = keys
            blocked_input(dialog)
            shot("m219-follow-guard.png", "Mid-follow (%d keys baked): the progress window is up; a drag and Delete in the viewer, a click and Delete in the graph and a click in the panel reached %r"
                 % (keys, seen["counts"]))
            cancel = [b for b in dialog.findChildren(QPushButton) if b.isVisible()]
            if cancel:
                QTest.mouseClick(cancel[0], Qt.LeftButton)
            state["cancelled"] = True
            return (ABORT_END_S, "guards: the follow ends within %.0f s of Cancel" % ABORT_END_S)

        press(scene.roto, "followForwardToEnd")
        outcome, stats = yield from wait_follow(scene, "guards", ABORT_TIMEOUT_S, tick)
        require(state["done"], "guards: the progress window was up while the follow ran, and the follow was still running when %d keys had been baked (report %r)"
                % (GUARD_AFTER_KEYS, outcome["message"]))
        report("NOTE guards: input sent after %d keys; report %r" % (state["keys"], outcome["message"]))

        check(seen["modality"] == Qt.ApplicationModal, "guards modal: the progress window is application modal (%r)" % seen["modality"])
        check(seen["buttons"] == ["Cancel"], "guards modal: the progress window's only button is Cancel (%r)" % seen["buttons"])
        check(seen["running"], "guards: the follow was still running after the input was sent")
        check(all(n == 0 for n in seen["counts"].values()),
              "guards input: no mouse or key event reached the viewer, the graph or the panel (%r)" % seen["counts"])
        viewer_moved = max(moved(seen["viewer"][i], before[i]) for i in range(POINTS))
        check(viewer_moved <= UNCHANGED_TOL and seen["viewer_count"] == POINTS,
              "guards viewer: a %.0f px drag on the selected point and Delete moved and removed nothing (largest move %.3e, %d points)"
              % (DRAG_PX * abs(scale), viewer_moved, seen["viewer_count"]))
        check(seen["roto_alive"], "guards graph: Delete in the node graph left the Roto node in place")
        check(is_user_stop(outcome), "guards cancel: Cancel stops the follow as a user stop (status %r, %r)" % (outcome["status"], outcome["message"]))
        yield ("sleep", SETTLE_MS)
        check(progress_window() is None and QApplication.activeModalWidget() is None,
              "guards cancel: the progress window goes away and no other modal window is open (%r)" % QApplication.activeModalWidget())

        yield from go_to(START)
        yield from settle()
        counter.reset()
        QApplication.instance().installEventFilter(counter)
        try:
            os_click(graph, graph_at)
            os_click(panel, panel_at)
            # No click first: the drag grabs the point with whatever selection the blocked input left.
            grab_now = widget_of(vmap, bez, 0, START)
            os_drag(gl, grab_now, QPointF(grab_now.x() + DRAG_PX, grab_now.y()))
        finally:
            QApplication.instance().removeEventFilter(counter)
        yield ("sleep", SEEK_MS)
        after_counts = dict(counter.counts)
        check(all(n > 0 for n in after_counts.values()),
              "guards control: with no follow running the same input reaches the viewer, the graph and the panel (%r)" % after_counts)
        after = [corner_xy(bez, i, START) for i in range(POINTS)]
        point0 = moved(after[0], before[0])
        others = max(moved(after[i], before[i]) for i in range(1, POINTS))
        check(point0 >= MOVED_MIN and others <= KEY_TOL,
              "guards control: the same press and drag moves the point and only that point (point 0 moved %.2f px, the others %.3e)"
              % (point0, others))
        set_project_range(START, MAIN_LAST)


    def steps():
        gt.section("guards")
        scene = yield from start_scene()
        yield from open_scene_roto(scene)
        yield from section_guards(scene)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
