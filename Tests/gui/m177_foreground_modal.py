"""Foreground renders block the interface. For each way of starting one (the Render dialog, the
Write's Render button triggered from Python, and app.render), the modal "ForegroundTaskDialog" is the
active modal widget while the render runs; a key edit to an open Grade spinbox and a drag in the
node graph, both sent through the window system the way a person's input arrives, change nothing;
Cancel stops the render early and ends its Progress panel task as Canceled, not Paused, since a
restart would render the project as edited since. Then, with no render running, an edit while
the viewer plays restarts the viewer's render, so the viewer shows the edit.

Input goes through QTest's QWindow overloads, which enter Qt where the window system's events do
and so meet the modal blocking; widget-targeted QTest events would skip it. A control run with no
dialog up shows the same input does edit.

Screenshots: m177-fg-<trigger>-modal.png (the dialog over the main window) and
m177-fg-viewer-restart.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m177_foreground_modal.py
"""
import glob
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
FRAMES = 60
DIALOG_NAME = "ForegroundTaskDialog"

try:
    from PySide6.QtCore import QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QCheckBox, QDialogButtonBox, QLabel, QLineEdit, QPushButton, QWidget

    import guitest as gt
    from guitest import check, report, require

    OUT_DIR = os.path.join(gt.OUT, "m177")
    state = {}

    def task_dialog():
        for w in QApplication.topLevelWidgets():
            try:
                if w.objectName() == DIALOG_NAME and w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def dialog_is_active_modal():
        dialog = task_dialog()
        return dialog is not None and QApplication.activeModalWidget() is dialog

    def task_status(node):
        """The Status cell of node's row in the Progress panel, or None without a row."""
        label = node.getLabel()
        for panel in gt.widgets_of("ProgressPanel"):
            for view in panel.findChildren(QWidget):
                try:
                    if gt.class_name(view) != "TableView":
                        continue
                    model = view.model()
                    for row in range(model.rowCount()):
                        if model.index(row, 0).data() == label:
                            return model.index(row, 2).data()
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

    def open_panel(node):
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())

    def knob_widget(panel, script_name, widget_class):
        """The visible widget_class widget of the knob script_name on panel, found by the knob's
        script name in its tooltip. A panel shows its header before its knob widgets, so callers
        wait for this rather than look once."""
        if panel is None:
            return None
        tag = "<b>%s</b>" % script_name
        for w in panel.findChildren(QWidget):
            try:
                if gt.class_name(w) == widget_class and w.isVisible() and tag in w.toolTip():
                    return w
            except RuntimeError:
                continue
        return None

    def window_handle(widget):
        """widget's top-level QWindow. The top-level widget's wrapper is kept in state: the
        handle's wrapper is its child and is marked deleted once that wrapper is collected."""
        top = widget.window()
        state.setdefault("top_levels", {})[id(top)] = top
        return top.windowHandle()

    def window_key_edit(spinbox, key):
        """Selects the spinbox's text, then types key and Return into its window."""
        spinbox.setFocus(Qt.OtherFocusReason)
        spinbox.selectAll()
        window = window_handle(spinbox)
        QTest.keyClick(window, key)
        QTest.keyClick(window, Qt.Key_Return)
        QApplication.processEvents()

    def node_centre_in_window(node):
        view = node_graph_view()
        x, y = node.getPosition()
        w, h = node.getSize()
        centre = QPointF(x + w / 2.0, y + h / 2.0)
        view.centerOn(centre)
        QApplication.processEvents()
        viewport = view.viewport()
        return viewport.mapTo(viewport.window(), view.mapFromScene(centre)), window_handle(viewport)

    def window_drag(node, dx, dy):
        start, window = node_centre_in_window(node)
        end = QPoint(start.x() + dx, start.y() + dy)
        QTest.mousePress(window, Qt.LeftButton, Qt.NoModifier, start)
        for i in range(1, 6):
            QTest.mouseMove(window, QPoint(start.x() + dx * i // 5, start.y() + dy * i // 5))
        QTest.mouseRelease(window, Qt.LeftButton, Qt.NoModifier, end)
        QApplication.processEvents()

    def frames_written(tag):
        return sorted(glob.glob(os.path.join(OUT_DIR, "fg_%s.*.png" % tag)))

    def drive_render_dialog(seen):
        """Accepts the RenderDialog as a foreground render; it runs its own event loop inside the
        click that opened it, so a repeating timer works from within that loop."""
        timer = QTimer()
        timer.setInterval(100)

        def poll():
            dialogs = [d for d in gt.widgets_of("RenderDialog") if d.isVisible()]
            if not dialogs:
                return
            timer.stop()
            dialog = dialogs[0]
            seen["dialog"] = True
            for box in dialog.findChildren(QCheckBox):
                if "background" in box.text().lower():
                    box.setChecked(False)
            boxes = [b for b in dialog.findChildren(QDialogButtonBox) if b.isVisible()]
            if boxes and boxes[0].button(QDialogButtonBox.Ok) is not None:
                boxes[0].button(QDialogButtonBox.Ok).click()
            else:
                dialog.reject()

        timer.timeout.connect(poll)
        timer.start()
        state.setdefault("timers", []).append(timer)
        return timer

    def build_graph():
        gt.section("setup")
        os.makedirs(OUT_DIR, exist_ok=True)
        for stale in glob.glob(os.path.join(OUT_DIR, "fg_*.png")):
            os.remove(stale)
        constant = app.createNode("net.sf.openfx.ConstantPlugin")
        require(constant is not None, "create a Constant", gt.plugin_problem("net.sf.openfx.ConstantPlugin"))
        constant.setPosition(200, 100)
        constant.getParam("color").set(0.1, 0.8, 0.1, 1.0)
        blur = app.createNode("net.sf.cimg.CImgBlur")
        require(blur is not None, "create a Blur", gt.plugin_problem("net.sf.cimg.CImgBlur"))
        blur.setPosition(200, 200)
        blur.connectInput(0, constant)
        size = blur.getParam("size")
        if size is not None:
            size.set(150, 150)
        grade = app.createNode("net.sf.openfx.GradePlugin")
        require(grade is not None, "create a Grade", gt.plugin_problem("net.sf.openfx.GradePlugin"))
        grade.setPosition(200, 300)
        grade.connectInput(0, blur)
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        viewer.setPosition(500, 400)
        viewer.connectInput(0, grade)
        state.update(constant=constant, blur=blur, grade=grade, viewer=viewer)
        yield ("sleep", 500)

    def make_writer(tag, y):
        write = app.createWriter(os.path.join(OUT_DIR, "fg_%s.####.png" % tag))
        require(write is not None, "create a Write for %s" % tag)
        write.setPosition(200, y)
        require(write.connectInput(0, state["grade"]), "connect the Grade to the %s Write" % tag)
        write.getParam("frameRange").setValue(2)
        write.getParam("firstFrame").setValue(1)
        write.getParam("lastFrame").setValue(FRAMES)
        state["write_" + tag] = write
        return write

    def control_section():
        """With no dialog up, the same window-system input edits the Grade and moves a node."""
        gt.section("control")
        # In an inactive window QGraphicsItem::isActive() is false and a press misses every node,
        # so with no window manager to activate it the main window is activated here.
        mains = [w for w in QApplication.topLevelWidgets() if gt.class_name(w) == "Gui" and w.isVisible()]
        require(len(mains) == 1, "found the main window")
        main = mains[0]
        state["main_window"] = main
        main.activateWindow()
        main.raise_()
        ok = yield ("until", main.isActiveWindow, 10.0)
        require(ok, "the main window is active")
        grade = state["grade"]
        yield from open_panel(grade)
        panel = panel_of(grade)
        state["grade_panel"] = panel
        found = yield ("until", lambda: knob_widget(panel, "mix", "SpinBox") is not None, POLL_TIMEOUT_S)
        require(found, "found the Grade's Mix spinbox")
        spin = knob_widget(panel, "mix", "SpinBox")
        mix = grade.getParam("mix")
        window_key_edit(spin, Qt.Key_0)
        ok = yield ("until", lambda: abs(mix.getValue() - 0.0) < 1e-6, 5.0)
        check(ok, "a key edit through the window sets Mix to 0 (%r)" % mix.getValue())
        mix.setValue(1.0)
        node = state["blur"]
        before = node.getPosition()
        window_drag(node, 60, 40)
        yield ("sleep", 200)
        after = node.getPosition()
        check(after != before, "a drag through the window moves the Blur (%r -> %r)" % (before, after))
        node.setPosition(before[0], before[1])
        yield ("sleep", 200)

    def modal_section(tag, start):
        gt.section("render via " + tag)
        write = make_writer(tag, 400 + 100 * len([k for k in state if k.startswith("write_")]))
        yield ("sleep", 300)
        yield from start(write)
        up = yield ("until", task_dialog, POLL_TIMEOUT_S)
        if not check(up, "the foreground task dialog is up"):
            return
        check(dialog_is_active_modal(), "it is the active modal widget")
        check(task_dialog().windowModality() == Qt.ApplicationModal, "it is application modal")
        first = yield ("until", lambda: len(frames_written(tag)) >= 1, POLL_TIMEOUT_S)
        check(first, "the render is writing frames")

        # Opening a panel is programmatic, so the dialog does not stop it; the input below is
        # what it must stop.
        grade = state["grade"]
        if panel_of(grade) is None:
            app.selectNode(grade, True)
            QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
            yield ("until", lambda: panel_of(grade) is not None, 5.0)
        panel = panel_of(grade)
        state["grade_panel"] = panel
        yield ("until", lambda: knob_widget(panel, "mix", "SpinBox") is not None, 5.0)
        spin = knob_widget(panel, "mix", "SpinBox")
        gt.shot("m177-fg-%s-modal.png" % tag)
        mix = grade.getParam("mix")
        if check(spin is not None, "found the Grade's Mix spinbox under the dialog"):
            window_key_edit(spin, Qt.Key_0)
        node = state["blur"]
        before = node.getPosition()
        window_drag(node, 60, 40)
        yield ("sleep", 300)
        check(abs(mix.getValue() - 1.0) < 1e-6, "the key edit left Mix at 1 (%r)" % mix.getValue())
        check(node.getPosition() == before, "the drag left the Blur where it was (%r -> %r)" % (before, node.getPosition()))
        check(task_dialog() is not None, "the render is still running after the input")

        dialog = task_dialog()
        buttons = [b for b in dialog.findChildren(QPushButton) if b.text() == "Cancel"] if dialog else []
        if check(len(buttons) == 1, "found the dialog's Cancel button"):
            buttons[0].click()
        closed = yield ("until", lambda: task_dialog() is None, POLL_TIMEOUT_S)
        check(closed, "Cancel closed the dialog")
        yield ("sleep", 1000)
        written = len(frames_written(tag))
        check(written < FRAMES, "Cancel stopped the render early (%d of %d frames)" % (written, FRAMES))
        status = task_status(write)
        check(status == "Canceled", "Cancel ended the task as Canceled, not Paused (%r)" % status)
        check(QApplication.activeModalWidget() is None, "no modal widget is left")

    def via_dialog(write):
        """Clicks the Write panel's Render button, which opens the Render dialog."""
        gt.close_all_panels()
        app.selectNode(write, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        opened = yield ("until", lambda: panel_of(write) is not None, POLL_TIMEOUT_S)
        if not check(opened, "open %s's settings panel" % write.getLabel()):
            return
        panel = panel_of(write)
        state["write_panel"] = panel
        found = yield ("until", lambda: knob_widget(panel, "startRender", "QPushButton") is not None, POLL_TIMEOUT_S)
        if not check(found, "found the Write panel's Render button"):
            return
        buttons = [knob_widget(panel, "startRender", "QPushButton")]
        seen = {}
        driver = drive_render_dialog(seen)
        buttons[0].click()
        driver.stop()
        check(seen.get("dialog", False), "the Render button opened the Render dialog")

    def via_trigger(write):
        write.getParam("startRender").trigger()
        yield ("sleep", 0)

    def via_app_render(write):
        app.render(write, 1, FRAMES)
        yield ("sleep", 0)

    def viewer_restart_section():
        gt.section("viewer restart")
        viewer = app.getViewer("Viewer1")
        require(viewer is not None, "the Viewer1 viewer exists")
        state["pyviewer"] = viewer
        app.getProjectParam("frameRange").set(1, 200)
        constant = state["constant"]
        constant.getParam("color").set(0.1, 0.8, 0.1, 1.0)
        state["blur"].getParam("size").set(0, 0)
        ok = yield ("until", lambda: dominant(gt.viewer_centre_colour()) == "green", POLL_TIMEOUT_S)
        check(ok, "the viewer shows the green Constant (%r)" % (gt.viewer_centre_colour(),))
        viewer.startForward()
        yield ("sleep", 800)
        start_frame = viewer.getCurrentFrame()
        constant.getParam("color").set(0.8, 0.1, 0.1, 1.0)
        ok = yield ("until", lambda: dominant(gt.viewer_centre_colour()) == "red", POLL_TIMEOUT_S)
        check(ok, "an edit during playback shows in the viewer (%r)" % (gt.viewer_centre_colour(),))
        check(viewer.getCurrentFrame() != start_frame, "the viewer kept playing (%d -> %d)" % (start_frame, viewer.getCurrentFrame()))
        check(task_dialog() is None, "playback shows no task dialog")
        gt.shot("m177-fg-viewer-restart.png")
        viewer.pause()

    def dominant(colour):
        if colour is None:
            return None
        order = sorted(range(3), key=lambda i: -colour[i])
        if colour[order[0]] - colour[order[1]] < 60:
            return None
        return ("red", "green", "blue")[order[0]]

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield from build_graph()
        yield from control_section()
        yield from modal_section("dialog", via_dialog)
        yield from modal_section("trigger", via_trigger)
        yield from modal_section("python", via_app_render)
        yield from viewer_restart_section()

    # The viewer widget only exists once a Viewer node does.
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
