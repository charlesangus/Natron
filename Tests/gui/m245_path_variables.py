"""Project paths are string variables of the project, created from a dialog that allows nesting,
refuses duplicates and is reachable from a file knob.

Sections:
  - setup: the project is saved under the test output folder, so [Project] names that folder;
  - nested: "Add Path Variable..." on the project settings' Variables tab opens the dialog; shots is
    made from [Project], inserted from the dialog's drop-down, then shot as [shots]/s01, and the
    dialog previews both fully expanded; both are string variables of the project;
  - duplicates: a value naming shot's folder offers "Use [shot]" and "Create Anyway" and keeps OK
    disabled until "Create Anyway" is ticked; a taken name keeps OK disabled; Cancel creates nothing;
  - write: a Write's file right-click menu offers "Add Project Path..."; for a file in shot's
    folder the dialog starts at [shot] and "Use [shot]" spells the file as [shot]/...; for a new
    folder it starts at [Project]/renders, and OK creates renders and spells the file through it.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m245-path-variables/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m245_path_variables.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 240
DIALOG_TIMEOUT_S = 20.0
SETTLE_MS = 500

RFV_GROUP = "rfvVariables"
DIALOG = "AddPathVariableDialog"
ADD_BUTTON = "Add Path Variable..."
ADD_ACTION = "Add Project Path..."

try:
    from PySide6.QtCore import QMetaObject, QTimer
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QCheckBox, QComboBox, QDialogButtonBox, QGraphicsView,
                                   QLabel, QLineEdit, QTabWidget, QWidget)

    import guitest as gt
    from guitest import check, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures", "m245")
    _held = []

    def hold(w):
        if w is not None and not any(w is h for h in _held):
            _held.append(w)
        return w

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        return guis[0] if guis else None

    def graph_up():
        gui = main_gui()
        return gui is not None and any(gt.class_name(g) == "NodeGraph" and g.isVisible()
                                       for g in gui.findChildren(QGraphicsView))

    def modal():
        return hold(QApplication.activeModalWidget())

    def modal_class():
        w = modal()
        return gt.class_name(w) if w is not None else None

    def later(fn):
        """Runs fn from a timer, so a modal loop it raises does not block this generator."""
        def fire():
            try:
                fn()
            except Exception:
                check(False, "timer callback error: " + traceback.format_exc())
        QTimer.singleShot(0, fire)

    def button_in(widget, *texts):
        for b in widget.findChildren(QAbstractButton):
            if b.text().replace("&", "") in texts:
                return hold(b)
        return None

    def child(dialog, cls, name):
        return hold(dialog.findChild(cls, name))

    def project_panel():
        for panel in gt.widgets_of("DockablePanel"):
            if panel.isVisible() and button_in(panel, ADD_BUTTON) is not None:
                return hold(panel)
        return None

    def show_tab(panel, text):
        for tabs in panel.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == text:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def project_dir():
        path = app.getProjectParam("projectPath").getValue()
        return path[:-1] if path.endswith("/") else path

    def dialog_state():
        d = modal()
        if d is None or gt.class_name(d) != DIALOG:
            return None
        ok = d.findChild(QDialogButtonBox).button(QDialogButtonBox.Ok)
        use = child(d, QAbstractButton, "pathVariableUseExisting")
        allow = child(d, QCheckBox, "pathVariableAllowDuplicate")
        return {
            "dialog": d,
            "name": child(d, QLineEdit, "pathVariableName"),
            "value": child(d, QLineEdit, "pathVariableValue"),
            "insert": child(d, QComboBox, "pathVariableInsert"),
            "preview": child(d, QLabel, "pathVariablePreview").text(),
            "status": child(d, QLabel, "pathVariableStatus").text(),
            "ok": hold(ok),
            "okEnabled": ok.isEnabled(),
            "use": use,
            "useVisible": use.isVisible(),
            "allow": allow,
            "allowVisible": allow.isVisible(),
        }

    def open_add_dialog():
        panel = project_panel()
        require(panel is not None, "the project settings panel is open")
        button = button_in(panel, ADD_BUTTON)
        require(button is not None and button.isVisible(), "the Variables tab shows " + ADD_BUTTON)
        button.click()  # the dialog opens from a queued call, after this step
        ok = yield ("until", lambda: modal_class() == DIALOG, DIALOG_TIMEOUT_S)
        require(ok, ADD_BUTTON + " opened the dialog", "modal is %r" % modal_class())
        yield ("sleep", SETTLE_MS)

    def project_param(name):
        return app.getProjectParam(name)

    def is_path_variable(name, value):
        p = project_param(name)
        return (p is not None and p.getParent() is not None and p.getParent().getScriptName() == RFV_GROUP
                and p.getValue() == value)

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
        return None

    def open_panel(node):
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, DIALOG_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())

    def file_field(node):
        panel = panel_of(node)
        if panel is None:
            return None
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and "<b>filename</b>" in w.toolTip():
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def right_click_file(node, trigger):
        """Opens the file knob's right-click menu, returns its entry texts and triggers the entry
        named trigger from a timer, so the dialog it opens does not block the menu."""
        seen = {"entries": None}
        field = file_field(node)
        if not check(field is not None, "the panel of %s shows its file field" % node.getLabel()):
            return []

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the file field's right-click menu is open")
                return
            hold(popup)
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            action = next((a for a in popup.actions() if a.text() == trigger), None)
            popup.close()
            if action is not None and action.isEnabled():
                hold(action)
                later(action.trigger)

        QTimer.singleShot(800, inspect)
        try:
            # the menu runs a nested loop until inspect() closes it
            field.customContextMenuRequested.emit(field.rect().center())
        except RuntimeError as e:
            check(False, "the right-click menu call completed: " + str(e))
        return seen["entries"] or []

    def steps():
        ok = yield ("until", graph_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        os.makedirs(FIXTURE_DIR, exist_ok=True)
        require(app.saveProjectAs(os.path.join(FIXTURE_DIR, "m245.ntpz")), "save the project under the test output")
        root = project_dir()
        require(bool(root) and os.path.isdir(root), "the project has a folder (%r)" % root)
        os.makedirs(os.path.join(root, "shots", "s01"), exist_ok=True)

        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        require(ok, "the project settings panel is open")
        require(show_tab(project_panel(), "Variables"), "the project settings panel has a Variables tab")
        yield ("sleep", SETTLE_MS)

        gt.section("nested")
        yield from open_add_dialog()
        s = dialog_state()
        check(not s["okEnabled"], "OK is disabled while the dialog is empty")
        items = [s["insert"].itemText(i) for i in range(s["insert"].count())]
        check("[Project]" in items, "the drop-down offers [Project] (got %r)" % items)
        s["name"].setText("shots")
        s["insert"].activated.emit(items.index("[Project]"))
        s["value"].insert("/shots")
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check(s["value"].text() == "[Project]/shots", "the drop-down inserted [Project] (value %r)" % s["value"].text())
        check(s["preview"].startswith(os.path.join(root, "shots")), "the preview expands [Project]/shots (%r)" % s["preview"])
        check(s["okEnabled"], "OK is enabled for a new name and folder (status %r)" % s["status"])
        gt.shot("m245-add-shots.png")
        s["ok"].click()
        ok = yield ("until", lambda: modal_class() != DIALOG, DIALOG_TIMEOUT_S)
        check(ok, "OK closes the dialog")
        check(is_path_variable("shots", "[Project]/shots"), "shots is a project variable holding [Project]/shots")

        yield from open_add_dialog()
        s = dialog_state()
        s["name"].setText("shot")
        s["value"].setText("[shots]/s01")
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check(s["preview"] == os.path.join(root, "shots", "s01"), "the preview expands the nested [shots]/s01 (%r)" % s["preview"])
        check(s["okEnabled"], "OK is enabled for a nested value (status %r)" % s["status"])
        gt.shot("m245-add-nested.png")
        s["ok"].click()
        ok = yield ("until", lambda: modal_class() != DIALOG, DIALOG_TIMEOUT_S)
        check(ok, "OK closes the dialog")
        check(is_path_variable("shot", "[shots]/s01"), "shot is a project variable holding [shots]/s01")

        gt.section("duplicates")
        yield from open_add_dialog()
        s = dialog_state()
        s["name"].setText("again")
        s["value"].setText(os.path.join(root, "shots", "s01"))
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check("[shot]" in s["status"], "the dialog says [shot] already names the folder (%r)" % s["status"])
        check(not s["okEnabled"], "OK is disabled for a duplicate folder")
        check(s["useVisible"] and s["use"].text() == "Use [shot]", "the dialog offers Use [shot] (%r)" % s["use"].text())
        check(s["allowVisible"], "the dialog offers Create Anyway")
        gt.shot("m245-duplicate.png")
        s["allow"].setChecked(True)
        yield ("sleep", SETTLE_MS)
        check(dialog_state()["okEnabled"], "Create Anyway enables OK")
        s["allow"].setChecked(False)
        s["value"].setText("[Project]/elsewhere")
        s["name"].setText("shots")
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check(not s["okEnabled"], "OK is disabled for a taken name (status %r)" % s["status"])
        s["name"].setText("Project")
        yield ("sleep", SETTLE_MS)
        check(not dialog_state()["okEnabled"], "OK is disabled for the built-in name Project")
        button_in(s["dialog"], "Cancel").click()
        ok = yield ("until", lambda: modal_class() != DIALOG, DIALOG_TIMEOUT_S)
        check(ok, "Cancel closes the dialog")
        check(project_param("again") is None, "Cancel created nothing")

        gt.section("write")
        write = app.createWriter(os.path.join(root, "shots", "s01", "w.####.exr"))
        require(write is not None, "create a Write in shot's folder")
        yield from open_panel(write)
        yield ("sleep", SETTLE_MS)
        entries = right_click_file(write, ADD_ACTION)
        check(ADD_ACTION in entries, "the file's right-click menu offers %s (got %r)" % (ADD_ACTION, entries))
        ok = yield ("until", lambda: modal_class() == DIALOG, DIALOG_TIMEOUT_S)
        require(ok, ADD_ACTION + " opened the dialog", "modal is %r" % modal_class())
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check(s["value"].text() == "[shot]", "the dialog starts at the folder spelled [shot] (%r)" % s["value"].text())
        check(s["useVisible"] and not s["okEnabled"], "the folder is shot's, so Use [shot] is offered and OK is disabled")
        gt.shot("m245-write-duplicate.png")
        s["use"].click()
        ok = yield ("until", lambda: modal_class() != DIALOG, DIALOG_TIMEOUT_S)
        check(ok, "Use [shot] closes the dialog")
        ok = yield ("until", lambda: write.getParam("filename").getValue() == "[shot]/w.####.exr", DIALOG_TIMEOUT_S)
        check(ok, "the Write's file is spelled through [shot] (%r)" % write.getParam("filename").getValue())
        check(project_param("s01") is None, "Use [shot] created no variable")

        write2 = app.createWriter(os.path.join(root, "renders", "out.####.exr"))
        require(write2 is not None, "create a Write in a new folder")
        yield from open_panel(write2)
        yield ("sleep", SETTLE_MS)
        right_click_file(write2, ADD_ACTION)
        ok = yield ("until", lambda: modal_class() == DIALOG, DIALOG_TIMEOUT_S)
        require(ok, ADD_ACTION + " opened the dialog for the second Write", "modal is %r" % modal_class())
        yield ("sleep", SETTLE_MS)
        s = dialog_state()
        check(s["value"].text() == "[Project]/renders", "the dialog starts at [Project]/renders (%r)" % s["value"].text())
        check(s["name"].text() == "renders", "the dialog names it after the folder (%r)" % s["name"].text())
        check(s["okEnabled"], "OK is enabled (status %r)" % s["status"])
        s["ok"].click()
        ok = yield ("until", lambda: modal_class() != DIALOG, DIALOG_TIMEOUT_S)
        check(ok, "OK closes the dialog")
        check(is_path_variable("renders", "[Project]/renders"), "renders is a project variable holding [Project]/renders")
        ok = yield ("until", lambda: write2.getParam("filename").getValue() == "[renders]/out.####.exr", DIALOG_TIMEOUT_S)
        check(ok, "the second Write's file is spelled through [renders] (%r)" % write2.getParam("filename").getValue())
        gt.shot("m245-write-renders.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
