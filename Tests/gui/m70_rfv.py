"""Declaring Reverse-Flow Variables from the GUI: the user-parameter dialog on a
ReverseFlowVariableSet node and the project settings' "Edit variables..." dialog.

Sections:
  - set node: the node's RFV group and its page are pinned (no Delete, Edit, Up or Down); a Double
    and a String added with the group (or one of its variables) selected become variables; a
    Python keyword is refused as a variable name;
  - project: "Edit variables..." on the project settings' Variables page opens a dialog listing
    only the project's RFV group, without Pick; a Double added there is a project variable, shows
    on the panel, and app.<name> resolves in the Script Editor at once; a name that is an App
    attribute is refused;
  - reload: after a save, a reset (which drops app.<name>) and a load, both nodes' variables and
    the project variable are back with their values, and app.<name> resolves again;
  - delete: deleting the project variable from the dialog drops app.<name> too.

Every modal dialog is driven from QTimer callbacks (see run() in guitest.py): whatever raises it is
fired from a single-shot timer or a queued call, and the script waits for QApplication's active
modal widget. Widget wrappers are looked up again after every yield, never kept across one.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m70-rfv/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m70_rfv.py build/fast/App/Natron
"""
import builtins
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
DIALOG_TIMEOUT_S = 20.0
LOAD_TIMEOUT_S = 60.0
SETTLE_MS = 500

SET_PLUGIN = "fr.natron.ReverseFlowVariableSet"
RFV_GROUP = "rfvVariables"
RFV_PAGE = "rfvPage"
TYPE_DOUBLE = 3  # AddKnobDialog::eParamDataTypeFloatingPoint
TYPE_TEXT = 11   # AddKnobDialog::eParamDataTypeTextInput

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, Q_ARG, Qt, QTimer
    from PySide6.QtWidgets import QAbstractButton, QApplication, QGraphicsView, QLabel, QLineEdit, QPlainTextEdit, QTabWidget, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    PROJECT_PATH = os.path.join(gt.OUT, "m70_rfv.ntpz")

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        return guis[0] if guis else None

    def graph_up():
        gui = main_gui()
        return gui is not None and any(gt.class_name(g) == "NodeGraph" and g.isVisible()
                                       for g in gui.findChildren(QGraphicsView))

    def modal():
        return QApplication.activeModalWidget()

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
                return b
        return None

    def click_later(dialog_class, *texts):
        """Clicks the button labelled with one of texts in the modal dialog_class, from a timer."""
        def fn():
            dialog = modal()
            if dialog is None or gt.class_name(dialog) != dialog_class:
                check(False, "%s is the modal dialog to click %r in (modal is %r)" % (dialog_class, texts, modal_class()))
                return
            b = button_in(dialog, *texts)
            if b is None:
                check(False, "%s has a %r button" % (dialog_class, texts))
                return
            b.click()
        later(fn)

    def tree_item(dialog, text):
        tree = dialog.findChildren(QTreeWidget)[0]
        found = tree.findItems(text, Qt.MatchExactly | Qt.MatchRecursive, 0)
        return tree, (found[0] if found else None)

    def select(text):
        dialog = modal()
        tree, item = tree_item(dialog, text)
        if item is None:
            return False
        tree.setCurrentItem(item)
        QApplication.processEvents()
        return True

    def enabled(dialog_class, text):
        dialog = modal()
        if dialog is None or gt.class_name(dialog) != dialog_class:
            return None
        b = button_in(dialog, text)
        return b.isEnabled() if b is not None else None

    def by_tooltip(dialog, cls, needle):
        for w in dialog.findChildren(cls):
            if needle in w.toolTip():
                return w
        return None

    def combo_by_tooltip(dialog, needle):
        for w in dialog.findChildren(QWidget):
            if gt.class_name(w) == "ComboBox" and needle in w.toolTip():
                return w
        return None

    def add_param(owner_dialog, name, label, type_index):
        """From the modal owner_dialog, presses Add..., fills the add dialog and presses OK.
        Returns the class of the modal widget once the add dialog has handled OK."""
        click_later(owner_dialog, "Add...")
        ok = yield ("until", lambda: modal_class() == "AddKnobDialog", DIALOG_TIMEOUT_S)
        require(ok, "the add dialog opened for " + name, "modal is %r" % modal_class())
        dialog = modal()
        name_edit = by_tooltip(dialog, QLineEdit, "used in Python scripts")
        label_edit = by_tooltip(dialog, QLineEdit, "displayed on the graphical user interface")
        type_combo = combo_by_tooltip(dialog, "data type of the parameter")
        require(name_edit is not None and label_edit is not None and type_combo is not None,
                "found the add dialog's name, label and type fields")
        name_edit.setText(name)
        label_edit.setText(label)
        QMetaObject.invokeMethod(type_combo, "setCurrentIndex", Q_ARG(int, type_index))
        QApplication.processEvents()
        click_later("AddKnobDialog", "OK", "Ok")
        ok = yield ("until", lambda: modal_class() != "AddKnobDialog", DIALOG_TIMEOUT_S)
        check(ok, "the add dialog handled OK for " + name)
        return modal_class()

    def add_refused(owner_dialog, name, type_index):
        """Adds name, expects an error dialog, closes it and cancels the add dialog."""
        result = yield from add_param(owner_dialog, name, name, type_index)
        refused = result not in (None, owner_dialog, "AddKnobDialog")
        check(refused, "adding %r raises an error dialog (modal is %r)" % (name, result))
        if refused:
            QMetaObject.invokeMethod(modal(), "reject")
            ok = yield ("until", lambda: modal_class() == "AddKnobDialog", DIALOG_TIMEOUT_S)
            check(ok, "the add dialog is back after the error for " + name)
        if modal_class() == "AddKnobDialog":
            QMetaObject.invokeMethod(modal(), "reject")
        ok = yield ("until", lambda: modal_class() == owner_dialog, DIALOG_TIMEOUT_S)
        require(ok, "the manage dialog is back after refusing " + name, "modal is %r" % modal_class())

    def close_dialog(dialog_class):
        click_later(dialog_class, "Close")
        ok = yield ("until", lambda: modal() is None, DIALOG_TIMEOUT_S)
        require(ok, "closed the " + dialog_class, "modal is %r" % modal_class())

    def parent_name(param):
        parent = param.getParent() if param is not None else None
        return parent.getScriptName() if parent is not None else None

    def node_panel(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                if w.text() == label:
                    return panel
        return None

    def project_panel():
        for panel in gt.widgets_of("DockablePanel"):
            if panel.isVisible() and button_in(panel, "Edit variables...") is not None:
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

    def run_in_script_editor(script):
        """Runs script in the Script Editor the way its Execute button does."""
        editors = gt.widgets_of("ScriptEditor")
        if not check(len(editors) > 0, "found a Script Editor"):
            return False
        inputs = [w for w in editors[0].findChildren(QPlainTextEdit) if gt.class_name(w) == "InputScriptTextEdit"]
        if not check(len(inputs) == 1, "the Script Editor has one input field (got %d)" % len(inputs)):
            return False
        inputs[0].setPlainText(script)
        QMetaObject.invokeMethod(editors[0], "onExecScriptClicked")
        QApplication.processEvents()
        return True

    def editor_probe(expression):
        """Evaluates expression in the Script Editor's globals; None if it raised there."""
        builtins.m70_probe = None
        run_in_script_editor("import builtins\nbuiltins.m70_probe = (%s,)\n" % expression)
        probe = getattr(builtins, "m70_probe", None)
        return probe[0] if probe is not None else None

    def open_project_variables():
        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        require(ok, "the project settings panel is open")
        require(show_tab(project_panel(), "Variables"), "the project settings panel has a Variables tab")
        yield ("sleep", SETTLE_MS)
        button = button_in(project_panel(), "Edit variables...")
        require(button is not None and button.isVisible(), "the Variables tab shows an Edit variables... button")
        button.click()  # the dialog opens from a queued call, after this step
        ok = yield ("until", lambda: modal_class() == "ManageUserParamsDialog", DIALOG_TIMEOUT_S)
        require(ok, "Edit variables... opened the variables dialog", "modal is %r" % modal_class())

    def steps():
        ok = yield ("until", graph_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        yield ("sleep", SETTLE_MS)

        gt.section("set node")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(SET_PLUGIN, -1, None, props)
        require(node is not None, "created a ReverseFlowVariableSet node", gt.plugin_problem(SET_PLUGIN))
        node_name = node.getScriptName()
        ok = yield ("until", lambda: node_panel(app.getNode(node_name)) is not None, DIALOG_TIMEOUT_S)
        require(ok, "the Set node's panel is open")
        QMetaObject.invokeMethod(node_panel(app.getNode(node_name)), "onManageUserParametersActionTriggered", Qt.QueuedConnection)
        ok = yield ("until", lambda: modal_class() == "ManageUserParamsDialog", DIALOG_TIMEOUT_S)
        require(ok, "the manage user parameters dialog opened", "modal is %r" % modal_class())

        require(select(RFV_GROUP), "the dialog lists the RFV group " + RFV_GROUP)
        for text in ("Delete", "Edit...", "Up", "Down"):
            check(enabled("ManageUserParamsDialog", text) is False, "the RFV group is pinned: %s is disabled" % text)
        require(select(RFV_PAGE), "the dialog lists the page " + RFV_PAGE)
        check(enabled("ManageUserParamsDialog", "Delete") is False, "the page holding the RFV group cannot be deleted")
        check(enabled("ManageUserParamsDialog", "Edit...") is False, "the page holding the RFV group cannot be edited")
        check(button_in(modal(), "Pick...").isVisible(), "Pick... is offered on a node")

        select(RFV_GROUP)
        yield from add_param("ManageUserParamsDialog", "setGain", "Set gain", TYPE_DOUBLE)
        gain = app.getNode(node_name).getParam("setGain")
        check(gain is not None and parent_name(gain) == RFV_GROUP,
              "a Double added on the selected RFV group is a variable (parent %r)" % parent_name(gain))
        require(select("setGain"), "the dialog lists the new variable setGain")
        check(enabled("ManageUserParamsDialog", "Delete") is True, "a variable can be deleted")
        yield from add_param("ManageUserParamsDialog", "setName", "Set name", TYPE_TEXT)
        name = app.getNode(node_name).getParam("setName")
        check(name is not None and parent_name(name) == RFV_GROUP,
              "a String added with a variable selected is a variable too (parent %r)" % parent_name(name))
        select(RFV_GROUP)
        yield from add_refused("ManageUserParamsDialog", "class", TYPE_DOUBLE)
        check(app.getNode(node_name).getParam("class") is None, "the keyword name was not added")
        gt.shot("m70-set-node-dialog.png")
        yield from close_dialog("ManageUserParamsDialog")
        yield ("sleep", SETTLE_MS)
        panel = node_panel(app.getNode(node_name))
        check(panel is not None and panel_has_label(panel, "Set gain") and panel_has_label(panel, "Set name"),
              "the Set node's panel shows both variables")
        gt.shot("m70-set-node-panel.png")

        gt.section("project")
        yield from open_project_variables()
        check(modal().windowTitle() == "Project Variables", "the dialog is titled Project Variables (got %r)" % modal().windowTitle())
        tree = modal().findChildren(QTreeWidget)[0]
        check(tree.topLevelItemCount() == 1 and tree.topLevelItem(0).text(0) == RFV_GROUP,
              "the dialog lists only the project's RFV group")
        check(not button_in(modal(), "Pick...").isVisible(), "Pick... is not offered for the project")
        require(select(RFV_GROUP), "selected the project's RFV group")
        check(enabled("ManageUserParamsDialog", "Delete") is False, "the project's RFV group is pinned")
        yield from add_param("ManageUserParamsDialog", "projGain", "Project gain", TYPE_DOUBLE)
        proj_gain = app.getProjectParam("projGain")
        check(proj_gain is not None and parent_name(proj_gain) == RFV_GROUP,
              "a Double added in the variables dialog is a project variable (parent %r)" % parent_name(proj_gain))
        yield from add_refused("ManageUserParamsDialog", "render", TYPE_DOUBLE)
        check(app.getProjectParam("render") is None, "the App attribute name was not added")
        gt.shot("m70-project-dialog.png")
        yield from close_dialog("ManageUserParamsDialog")
        yield ("sleep", SETTLE_MS)
        check(panel_has_label(project_panel(), "Project gain"), "the project panel's Variables tab shows the new variable")
        gt.shot("m70-project-panel.png")
        app.getProjectParam("projGain").setValue(2.5)
        check(editor_probe("app.projGain.getValue()") == 2.5,
              "app.projGain resolves in the Script Editor right after it is added in the dialog")

        gt.section("reload")
        app.getNode(node_name).getParam("setGain").setValue(1.5)
        app.getNode(node_name).getParam("setName").setValue("hello")
        require(app.saveProjectAs(PROJECT_PATH), "saved the project to " + PROJECT_PATH)
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        check(editor_probe("hasattr(app, 'projGain')") is False, "a reset drops app.projGain")
        require(app.loadProject(PROJECT_PATH) is not None, "loaded " + PROJECT_PATH)
        ok = yield ("until", lambda: app.getNode(node_name) is not None, LOAD_TIMEOUT_S)
        require(ok, "the Set node is back after loading")
        yield ("sleep", SETTLE_MS)
        reloaded = app.getNode(node_name)
        gain = reloaded.getParam("setGain")
        name = reloaded.getParam("setName")
        check(gain is not None and parent_name(gain) == RFV_GROUP and gain.getValue() == 1.5,
              "the Set node's Double variable is back with its value")
        check(name is not None and parent_name(name) == RFV_GROUP and name.getValue() == "hello",
              "the Set node's String variable is back with its value")
        proj_gain = app.getProjectParam("projGain")
        check(proj_gain is not None and proj_gain.getValue() == 2.5, "the project variable is back with its value")
        check(editor_probe("app.projGain.getValue()") == 2.5, "app.projGain resolves in the Script Editor after loading")
        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        if check(ok, "the project settings panel is open after loading"):
            show_tab(project_panel(), "Variables")
            yield ("sleep", SETTLE_MS)
            check(panel_has_label(project_panel(), "Project gain"), "the project panel shows the loaded variable")
            gt.shot("m70-project-panel-reloaded.png")

        gt.section("delete")
        yield from open_project_variables()
        require(select("projGain"), "the variables dialog lists projGain")
        click_later("ManageUserParamsDialog", "Delete")
        ok = yield ("until", lambda: modal_class() == "QMessageBox", DIALOG_TIMEOUT_S)
        require(ok, "Delete asks for confirmation", "modal is %r" % modal_class())
        click_later("QMessageBox", "Yes")
        ok = yield ("until", lambda: modal_class() == "ManageUserParamsDialog", DIALOG_TIMEOUT_S)
        require(ok, "the variables dialog is back after confirming")
        check(app.getProjectParam("projGain") is None, "the project variable is deleted")
        yield from close_dialog("ManageUserParamsDialog")
        check(editor_probe("hasattr(app, 'projGain')") is False, "deleting the variable drops app.projGain")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
