"""A background render works from the project as it was when it was submitted. A Constant (green)
feeds a Write rendering 10 frames in the background; right after submission the Constant turns red.
Once 3 frames are written the task is paused from the Progress panel, the Constant turns blue, and
the task is restarted. Every frame written, before and after the restart, is green.

Screenshots: m177-bg-paused.png (the Progress panel with the paused task) and m177-bg-done.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m177_background_frozen.py
"""
import glob
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 60.0
STARTUP_TIMEOUT_S = 60.0
FRAMES = 10
PAUSE_AFTER = 3
GREEN = (0.1, 0.8, 0.1)
RED = (0.8, 0.1, 0.1)
BLUE = (0.1, 0.1, 0.8)

try:
    from PySide6.QtCore import QMetaObject, QTimer
    from PySide6.QtGui import QImage
    from PySide6.QtWidgets import QApplication, QCheckBox, QDialogButtonBox, QLabel, QLineEdit, QWidget

    import guitest as gt
    from guitest import check, report, require

    OUT_DIR = os.path.join(gt.OUT, "m177")
    PATTERN = os.path.join(OUT_DIR, "bg_frozen.####.png")
    state = {}

    def frames_written():
        return sorted(glob.glob(os.path.join(OUT_DIR, "bg_frozen.*.png")))

    def complete_frames():
        """Frames that load as images: a file the render is still writing does not."""
        return [p for p in frames_written() if not QImage(p).isNull()]

    def dominant(path):
        image = QImage(path)
        if image.isNull():
            return None
        c = image.pixelColor(image.width() // 2, image.height() // 2)
        rgb = (c.red(), c.green(), c.blue())
        order = sorted(range(3), key=lambda i: -rgb[i])
        if rgb[order[0]] - rgb[order[1]] < 60:
            return None
        return ("red", "green", "blue")[order[0]]

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

    def progress_button(tooltip_start):
        """The enabled Progress panel task button whose tooltip starts with tooltip_start."""
        for panel in gt.widgets_of("ProgressPanel"):
            for w in panel.findChildren(QWidget):
                try:
                    if gt.class_name(w) == "Button" and w.isEnabled() and tooltip_start in w.toolTip():
                        return w
                except RuntimeError:
                    continue
        return None

    def drive_render_dialog(seen):
        """Accepts the RenderDialog with "Render in background" checked."""
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
                    box.setChecked(True)
                    seen["background"] = True
            boxes = [b for b in dialog.findChildren(QDialogButtonBox) if b.isVisible()]
            if boxes and boxes[0].button(QDialogButtonBox.Ok) is not None:
                boxes[0].button(QDialogButtonBox.Ok).click()
            else:
                dialog.reject()

        timer.timeout.connect(poll)
        timer.start()
        state["driver"] = timer
        return timer

    def build_graph():
        gt.section("setup")
        os.makedirs(OUT_DIR, exist_ok=True)
        for stale in frames_written():
            os.remove(stale)
        constant = app.createNode("net.sf.openfx.ConstantPlugin")
        require(constant is not None, "create a Constant", gt.plugin_problem("net.sf.openfx.ConstantPlugin"))
        constant.setPosition(200, 100)
        constant.getParam("color").set(GREEN[0], GREEN[1], GREEN[2], 1.0)
        # A slow node between the two keeps each frame long enough to pause after a few.
        blur = app.createNode("net.sf.cimg.CImgBlur")
        require(blur is not None, "create a Blur", gt.plugin_problem("net.sf.cimg.CImgBlur"))
        blur.setPosition(200, 200)
        blur.connectInput(0, constant)
        if blur.getParam("size") is not None:
            blur.getParam("size").set(150, 150)
        write = app.createWriter(PATTERN)
        require(write is not None, "create a Write")
        write.setPosition(200, 300)
        require(write.connectInput(0, blur), "connect the Blur to the Write")
        write.getParam("frameRange").setValue(2)
        write.getParam("firstFrame").setValue(1)
        write.getParam("lastFrame").setValue(FRAMES)
        state.update(constant=constant, blur=blur, write=write)
        yield ("sleep", 500)

    def render_section():
        gt.section("background render")
        constant, write = state["constant"], state["write"]
        gt.close_all_panels()
        app.selectNode(write, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(write) is not None, POLL_TIMEOUT_S)
        require(ok, "open the Write's settings panel")
        panel = panel_of(write)
        buttons = [w for w in gt.widgets_of("Button") + gt.widgets_of("QPushButton")
                   if w.isVisible() and w.text() == "Render" and panel.isAncestorOf(w)]
        require(bool(buttons), "found the Write panel's Render button")
        seen = {}
        driver = drive_render_dialog(seen)
        buttons[0].click()
        driver.stop()
        # The submission is saved before the dialog's click returns.
        constant.getParam("color").set(RED[0], RED[1], RED[2], 1.0)
        check(seen.get("dialog", False), "the Render button opened the Render dialog")
        check(seen.get("background", False), "the dialog's background box was checked")
        check(QApplication.activeModalWidget() is None, "a background render leaves the interface usable")

        ok = yield ("until", lambda: len(complete_frames()) >= PAUSE_AFTER, POLL_TIMEOUT_S)
        require(ok, "the render wrote %d frames (%d)" % (PAUSE_AFTER, len(complete_frames())))
        pause = progress_button("Pause the task")
        require(pause is not None, "found the task's Pause button")
        pause.click()
        restart = yield ("until", lambda: progress_button("Restart task"), POLL_TIMEOUT_S)
        require(bool(restart), "the paused task can restart")
        paused_at = len(frames_written())
        report("NOTE %d frame file(s) when paused" % paused_at)
        check(paused_at < FRAMES, "the pause stopped the render before the end (%d of %d)" % (paused_at, FRAMES))
        gt.shot("m177-bg-paused.png")

        constant.getParam("color").set(BLUE[0], BLUE[1], BLUE[2], 1.0)
        yield ("sleep", 300)
        progress_button("Restart task").click()
        ok = yield ("until", lambda: len(complete_frames()) == FRAMES, POLL_TIMEOUT_S * 2)
        check(ok, "the restarted task wrote every frame (%d of %d)" % (len(complete_frames()), FRAMES))
        yield ("sleep", 1000)
        colours = [(os.path.basename(p), dominant(p)) for p in frames_written()]
        report("NOTE frame colours: %r" % colours)
        check(len(colours) == FRAMES and all(c == "green" for _, c in colours),
              "every frame has the colour at submission, green (%r)" % colours)
        gt.shot("m177-bg-done.png")

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        yield from build_graph()
        yield from render_section()

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
