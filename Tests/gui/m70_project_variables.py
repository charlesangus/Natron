"""Project variables animate like a node's knobs: keyframes set from the project settings panel and
expressions follow the timeline, in the engine and on the panel.

Sections:
  - add: a Double made with app.createDoubleParam and passed to app.addRFVKnob is a project
    variable, app.<name>, and shows on the project settings panel's Variables tab;
  - keyframes: the variable's right-click menu offers Set Key; Set Key at frames 1 and 11 makes two
    keyframes; with the second moved to 10, seeking to frame 6 shows an interpolated value on the
    panel and through app.<name>; Remove Key at frame 11 removes that keyframe only;
  - expression: a second variable with the expression "frame" shows the timeline's frame on the
    panel and through app.<name> at frames 1, 10 and 25.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m70-project-variables/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m70_project_variables.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 180
DIALOG_TIMEOUT_S = 20.0
SETTLE_MS = 500

RFV_GROUP = "rfvVariables"
RAMP = "m70Ramp"
FRAME = "m70Frame"

try:
    from PySide6.QtCore import QMetaObject, QTimer
    from PySide6.QtWidgets import QAbstractButton, QApplication, QGraphicsView, QLabel, QTabWidget, QWidget

    import guitest as gt
    from guitest import check, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        return guis[0] if guis else None

    def graph_up():
        gui = main_gui()
        return gui is not None and any(gt.class_name(g) == "NodeGraph" and g.isVisible()
                                       for g in gui.findChildren(QGraphicsView))

    def button_in(widget, *texts):
        for b in widget.findChildren(QAbstractButton):
            if b.text().replace("&", "") in texts:
                return b
        return None

    def project_panel():
        for panel in gt.widgets_of("DockablePanel"):
            if panel.isVisible() and button_in(panel, "Edit variables...") is not None:
                _held.append(panel)
                return panel
        return None

    def show_tab(panel, text):
        for tabs in panel.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == text:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def panel_has_label(panel, text):
        return any(w.isVisible() and w.text().startswith(text) for w in panel.findChildren(QLabel))

    def knob_field(script_name):
        """The first-dimension field of the project knob named script_name: the widget the knob's
        right-click menu is attached to, whose tooltip holds the knob's script name."""
        panel = project_panel()
        if panel is None:
            return None
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and script_name in w.toolTip():
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

    def field_value(script_name):
        """The number the panel's field for script_name shows, or None."""
        field = knob_field(script_name)
        if field is None or not hasattr(field, "text"):
            return None
        try:
            return float(field.text())
        except ValueError:
            return None

    def field_shows(script_name, value):
        shown = field_value(script_name)
        return shown is not None and abs(shown - value) < 1e-6

    def right_click(script_name, trigger=None, shot_name=None):
        """Opens the right-click menu of the knob's field the way a right click does, returns the
        texts of its entries and triggers the entry named trigger."""
        seen = {"entries": None}
        field = knob_field(script_name)
        if not check(field is not None, "the project panel shows the field of " + script_name):
            return []

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the right-click menu of %s is open" % script_name)
                return
            _held.append(popup)
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            if shot_name:
                gt.shot(shot_name)
            action = next((a for a in popup.actions() if a.text() == trigger), None) if trigger else None
            if action is not None and action.isEnabled():
                action.trigger()
            popup.close()

        QTimer.singleShot(800, inspect)
        try:
            # the menu runs a nested loop until inspect() closes it
            field.customContextMenuRequested.emit(field.rect().center())
        except RuntimeError as e:
            check(False, "the right-click menu call completed: " + str(e))
        return seen["entries"] or []

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    _py_viewer = {}

    def seek(frame):
        if "v" not in _py_viewer:
            _py_viewer["v"] = app.getViewer(viewer.getScriptName())
        _py_viewer["v"].seek(frame)

    def steps():
        ok = yield ("until", graph_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        require(viewer is not None, "a Viewer node exists")
        yield ("sleep", SETTLE_MS)

        gt.section("add")
        seek(1)
        ramp = app.createDoubleParam(RAMP, "Ramp")
        require(ramp is not None, "app.createDoubleParam made a project parameter")
        ramp.setValue(0.0)
        app.addRFVKnob(ramp)
        ramp = app.getProjectParam(RAMP)
        require(ramp is not None and ramp.getParent() is not None and ramp.getParent().getScriptName() == RFV_GROUP,
                "app.addRFVKnob moved %s into the project's variables" % RAMP)
        check(getattr(app, RAMP, None) is not None, "app.%s resolves" % RAMP)

        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        require(ok, "the project settings panel is open")
        require(show_tab(project_panel(), "Variables"), "the project settings panel has a Variables tab")
        ok = yield ("until", lambda: knob_field(RAMP) is not None, DIALOG_TIMEOUT_S)
        require(ok, "the Variables tab shows the field of " + RAMP)
        check(panel_has_label(project_panel(), "Ramp"), "the Variables tab shows the variable's label")

        gt.section("keyframes")
        entries = right_click(RAMP, "Set Key", "m70pv-set-key-menu.png")
        check("Set Key" in entries, "the variable's right-click menu offers Set Key (got %r)" % entries)
        ok = yield ("until", lambda: app.getProjectParam(RAMP).getNumKeys() == 1, DIALOG_TIMEOUT_S)
        check(ok, "Set Key at frame 1 made a keyframe (keys: %d)" % app.getProjectParam(RAMP).getNumKeys())

        seek(11)
        yield ("sleep", SETTLE_MS)
        right_click(RAMP, "Set Key")
        ok = yield ("until", lambda: app.getProjectParam(RAMP).getNumKeys() == 2, DIALOG_TIMEOUT_S)
        check(ok, "Set Key at frame 11 made a second keyframe (keys: %d)" % app.getProjectParam(RAMP).getNumKeys())
        app.getProjectParam(RAMP).setValueAtTime(10.0, 11)

        seek(6)
        ok = yield ("until", lambda: field_value(RAMP) is not None and 0.0 < field_value(RAMP) < 10.0, DIALOG_TIMEOUT_S)
        check(ok, "at frame 6 the panel shows a value between the keyframes (shows %r)" % field_value(RAMP))
        value = getattr(app, RAMP).get()
        check(0.0 < value < 10.0, "at frame 6 app.%s.get() is between the keyframes (got %r)" % (RAMP, value))
        check(field_shows(RAMP, value), "the panel shows app.%s.get() at frame 6 (%r vs %r)" % (RAMP, field_value(RAMP), value))
        gt.shot("m70pv-interpolated.png")

        seek(11)
        ok = yield ("until", lambda: field_shows(RAMP, 10.0), DIALOG_TIMEOUT_S)
        check(ok, "at frame 11 the panel shows the second keyframe's value (shows %r)" % field_value(RAMP))
        entries = right_click(RAMP, "Remove Key")
        check("Remove Key" in entries, "on a keyframe the menu offers Remove Key (got %r)" % entries)
        ok = yield ("until", lambda: app.getProjectParam(RAMP).getNumKeys() == 1, DIALOG_TIMEOUT_S)
        check(ok, "Remove Key at frame 11 removed only that keyframe (keys: %d)" % app.getProjectParam(RAMP).getNumKeys())

        gt.section("expression")
        frame = app.createDoubleParam(FRAME, "Frame")
        require(frame is not None, "app.createDoubleParam made a second project parameter")
        app.addRFVKnob(frame)
        require(app.getProjectParam(FRAME).setExpression("frame", False), "set the expression frame on " + FRAME)
        ok = yield ("until", lambda: knob_field(FRAME) is not None, DIALOG_TIMEOUT_S)
        require(ok, "the Variables tab shows the field of " + FRAME)
        for f in (1, 10, 25):
            seek(f)
            ok = yield ("until", lambda f=f: field_shows(FRAME, float(f)), DIALOG_TIMEOUT_S)
            check(ok, "at frame %d the panel shows %d (shows %r)" % (f, f, field_value(FRAME)))
            value = getattr(app, FRAME).get()
            check(value == float(f), "at frame %d app.%s.get() is %d (got %r)" % (f, FRAME, f, value))
        gt.shot("m70pv-expression.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
