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
  - promote: a Set-node variable's right-click menu has "Add to project variables", which copies
    it into the project's RFV group with its value and declares app.<name>; once the project
    declares the name the item is disabled with a tooltip, and a project variable's own menu does
    not have it; the node's variable stays enabled and editable, unlinked from the project's copy;
    the node's Variables tab shows its Variables and Global Variables groups, and the promote
    moves the variable from the first to the second; Ctrl+Z, with the main window active, takes
    the variable back out of the project's Variables and moves the node's back to its Variables
    group;
  - existing: on a third Set node's Variables tab, the Global Variables group starts with an
    "Existing variable" drop-down listing the project's and other Set nodes' variables; choosing
    projGain adds it to the node's Global Variables group, since the project declares the name,
    with the project's value, resets the drop-down to its prompt and stops offering the name;
    Ctrl+Z takes it back off and keeps the drop-down in its group;
  - right-click manage: a real right mouse press on a knob row of the Set node's panel opens the
    panel's menu inside the row's press handler; "Manage user parameters..." opens the dialog, a
    3-dimension Double added to the RFV group and an Int added outside it (no selection) go through
    the dialog's Add flow, and a real mouse click on Close leaves Natron running with both knobs on
    the node and its panel;
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

SET_PLUGIN = "ReverseFlowVariableSet"
RFV_GROUP = "rfvVariables"
RFV_PAGE = "rfvPage"
GLOBAL_LABEL = "Global Variables"
GLOBAL_GROUP = "rfvGlobalVariables"
EXISTING_CHOICE = "rfvExistingVariable"
EXISTING_PROMPT = "Add existing variable..."
TYPE_INT = 0     # AddKnobDialog::eParamDataTypeInteger
TYPE_DOUBLE_3D = 5  # AddKnobDialog::eParamDataTypeFloatingPoint3D
TYPE_DOUBLE = 3  # AddKnobDialog::eParamDataTypeFloatingPoint
TYPE_TEXT = 11   # AddKnobDialog::eParamDataTypeTextInput

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, Q_ARG, QPoint, Qt, QTimer
    from PySide6.QtWidgets import QAbstractButton, QApplication, QGraphicsView, QLabel, QLineEdit, QPlainTextEdit, QTabWidget, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report, require

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

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

    _held = []

    def modal():
        """The active modal widget; its wrapper is kept alive for the run, because PySide marks the
        wrappers of its children deleted once a temporary wrapper of it is collected."""
        w = QApplication.activeModalWidget()
        if w is not None and not any(w is h for h in _held):
            _held.append(w)
        return w

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

    # A group's title may be drawn by its fold button rather than by a label.
    def panel_has_text(panel, text):
        return panel_has_label(panel, text) or any(
            w.isVisible() and w.text().replace("&", "").startswith(text) for w in panel.findChildren(QAbstractButton))

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

    PROMOTE_ITEM = "Add to project variables"

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def open_node_panel(node_name):
        view = graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(app.getNode(node_name), True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: node_panel(app.getNode(node_name)) is not None, DIALOG_TIMEOUT_S)
        require(ok, "the settings panel of %s is open" % node_name)

    def knob_field(panel, script_name):
        """The first-dimension field of the knob named script_name on panel: the widget the knob's
        right-click menu is attached to, whose tooltip starts with the knob's script name."""
        _held.append(panel)
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and script_name in w.toolTip():
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

    def right_click_menu(field, shot_name=None, trigger=None):
        """Opens field's right-click menu the way a right click does and returns
        {"entries": [...], "item": (enabled, tooltip) or None}; triggers the item named trigger."""
        seen = {"entries": None, "item": None}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is not None:
                _held.append(popup)
            if popup is None:
                check(False, "the right-click menu is open")
                return
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            action = next((a for a in popup.actions() if a.text() == PROMOTE_ITEM), None)
            if action is not None:
                seen["item"] = (action.isEnabled(), action.toolTip())
            if shot_name:
                gt.shot(shot_name)
            if trigger and action is not None and action.text() == trigger and action.isEnabled():
                action.trigger()
            popup.close()

        QTimer.singleShot(800, inspect)
        try:
            # the menu runs a nested loop; a triggered item may rebuild the panel and delete field
            field.customContextMenuRequested.emit(field.rect().center())
        except RuntimeError as e:
            check(False, "the right-click menu call completed: " + str(e))
        return seen

    def edit_menu_action(prefix):
        """The Edit menu's action whose text starts with prefix, e.g. "undo"."""
        win = main_gui()
        if win is None or win.menuBar() is None:
            return None
        _held.append(win)
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().replace("&", "").lower().startswith(prefix):
                        return a
        return None

    def activate_main_window():
        """Raises the main window and waits until it is active: an inactive window drops shortcuts."""
        win = main_gui()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: main_gui() is not None and main_gui().isActiveWindow(), DIALOG_TIMEOUT_S)
        require(ok, "the main window is active", "synthetic key presses are ignored by an inactive window")

    def press_undo():
        """Ctrl+Z on the main window, through QTest so that the shortcut map sees it."""
        win = main_gui()
        require(win is not None, "found the main window for Ctrl+Z")
        if QTest is None:
            report("NOTE PySide6.QtTest is unavailable: triggering the Edit menu's undo instead of Ctrl+Z")
            a = edit_menu_action("undo")
            if check(a is not None and a.isEnabled(), "an enabled undo action is in the Edit menu"):
                a.trigger()
        else:
            QTest.keyClick(win, Qt.Key_Z, Qt.ControlModifier)
        QApplication.processEvents()

    def combo_items(combo):
        """The entries of a Natron ComboBox, which PySide sees only as a QFrame."""
        items = combo.property("natronItems")
        return list(items) if items is not None else []

    def existing_combo(panel):
        """The ComboBox of the existing-variable drop-down on panel, found by its prompt entry."""
        if panel is None:
            return None
        _held.append(panel)
        for w in panel.findChildren(QWidget):
            try:
                if gt.class_name(w) == "ComboBox" and w.isVisible() and EXISTING_PROMPT in combo_items(w):
                    _held.append(w)
                    return w
            except (RuntimeError, AttributeError):
                continue
        return None

    def pick_from_combo(combo, text, shot_name=None):
        """Opens combo's menu with a click and chooses the entry text in the popup, the way a user
        does. Returns whether the entry was there."""
        seen = {"picked": False}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the drop-down's menu is open")
                return
            _held.append(popup)
            if shot_name:
                gt.shot(shot_name)
            action = next((a for a in popup.actions() if a.text() == text), None)
            if action is None:
                popup.close()
                return
            seen["picked"] = True
            popup.setActiveAction(action)
            if QTest is not None:
                QTest.keyClick(popup, Qt.Key_Return)
            else:
                report("NOTE PySide6.QtTest is unavailable: triggering the drop-down's action directly")
                action.trigger()
                popup.close()

        QTimer.singleShot(800, inspect)
        try:
            # the menu runs a nested loop; the choice may rebuild the panel's user pages
            if QTest is not None:
                QTest.mouseClick(combo, Qt.LeftButton)
            else:
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, combo_items(combo).index(text)))
                seen["picked"] = True
        except (RuntimeError, ValueError) as e:
            check(False, "the drop-down call completed: " + str(e))
        return seen["picked"]

    def open_project_variables():
        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        require(ok, "the project settings panel is open")
        require(show_tab(project_panel(), "Variables"), "the project settings panel has a Variables tab")
        yield ("sleep", SETTLE_MS)
        panel = project_panel()
        button = button_in(panel, "Edit variables...")
        require(button is not None and button.isVisible(), "the Variables tab shows an Edit variables... button")
        button.click()  # the dialog opens from a queued call, after this step
        ok = yield ("until", lambda: modal_class() == "ManageUserParamsDialog", DIALOG_TIMEOUT_S)
        require(ok, "Edit variables... opened the variables dialog", "modal is %r" % modal_class())

    MANAGE_ITEM = "Manage user parameters..."

    def real_click_later(dialog_class, *texts):
        """Like click_later, but with a real left mouse press and release on the button."""
        def fn():
            dialog = modal()
            b = button_in(dialog, *texts) if dialog is not None and gt.class_name(dialog) == dialog_class else None
            if b is None:
                check(False, "%s is the modal dialog with a %r button to click (modal is %r)" % (dialog_class, texts, modal_class()))
                return
            QTest.mouseClick(b, Qt.LeftButton)
        later(fn)

    def row_container(panel, label_text):
        """The right-clickable row or page widget holding the visible label that starts with label_text."""
        _held.append(panel)
        for w in panel.findChildren(QLabel):
            try:
                if not (w.isVisible() and w.text().startswith(label_text)):
                    continue
                p = w.parentWidget()
                while p is not None and gt.class_name(p) != "RightClickableWidget":
                    p = p.parentWidget()
                if p is not None:
                    _held.append(p)
                    return p
            except RuntimeError:
                continue
        return None

    def bare_point(row):
        """A point of row that no child widget covers. The panel only opens its menu when the
        widget under the cursor is the row itself."""
        for fy in (0.5, 0.25, 0.75, 0.1, 0.9):
            for x in range(row.width() - 3, 2, -4):
                pt = QPoint(x, int(row.height() * fy))
                if QApplication.widgetAt(row.mapToGlobal(pt)) is row:
                    return pt
        return None

    def press_manage_from_menu(row):
        """Right-clicks row with a real mouse press, so the panel's menu runs inside the press
        handler, and chooses the manage-parameters item with the keyboard. The click returns once
        the menu has closed; the dialog opens from a queued call afterwards."""
        seen = {"entries": None, "chosen": False, "failed": False}
        pt = bare_point(row)
        if pt is None:
            check(False, "a point on the row not covered by a child widget (row %s)" % row.size())
            seen["failed"] = True
            return seen

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the panel's right-click menu is open")
                seen["failed"] = True
                return
            _held.append(popup)
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            gt.shot("m70-panel-menu.png")
            action = next((a for a in popup.actions() if a.text() == MANAGE_ITEM), None)
            if action is None or not action.isEnabled():
                popup.close()
                return
            seen["chosen"] = True
            popup.setActiveAction(action)
            QTest.keyClick(popup, Qt.Key_Return)

        def press():
            try:
                QTest.mouseClick(row, Qt.RightButton, Qt.NoModifier, pt)
            except RuntimeError as e:
                check(False, "the right click completed: " + str(e))

        QTimer.singleShot(800, inspect)
        later(press)  # the press may not return until the dialog it opens is closed
        return seen

    def manage_from_right_click(node_name, label_text):
        """Opens the manage dialog on node_name's panel from a real right click on the row labelled label_text."""
        yield from open_node_panel(node_name)
        require(show_tab(node_panel(app.getNode(node_name)), "Variables"), "the panel has a Variables tab")
        yield ("sleep", SETTLE_MS)
        row = row_container(node_panel(app.getNode(node_name)), label_text)
        require(row is not None, "found the right-clickable row of %r on the panel" % label_text)
        seen = press_manage_from_menu(row)
        row = None
        ok = yield ("until", lambda: seen["entries"] is not None or seen["failed"], DIALOG_TIMEOUT_S)
        require(ok and not seen["failed"], "the panel's right-click menu opened")
        check(MANAGE_ITEM in seen["entries"],
              "the panel's menu has %r (entries %r)" % (MANAGE_ITEM, seen["entries"]))
        require(seen["chosen"], "chose %r from the menu" % MANAGE_ITEM)
        ok = yield ("until", lambda: modal_class() == "ManageUserParamsDialog", DIALOG_TIMEOUT_S)
        require(ok, "the manage dialog opened", "modal is %r" % modal_class())

    def close_with_mouse(dialog_class):
        real_click_later(dialog_class, "Close")
        ok = yield ("until", lambda: modal() is None, DIALOG_TIMEOUT_S)
        require(ok, "a mouse click on Close closed the " + dialog_class, "modal is %r" % modal_class())

    def clear_selection():
        tree = modal().findChildren(QTreeWidget)[0]
        tree.clearSelection()
        tree.setCurrentItem(None)
        QApplication.processEvents()

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
        panel = project_panel()
        tabs = [t for t in panel.findChildren(QTabWidget) if t.isVisible()]
        check(any(t.tabText(t.currentIndex()).replace("&", "") == "Variables" for t in tabs),
              "the project panel stayed on its Variables tab after the dialog closed")
        check(panel_has_label(panel, "Project gain"), "the project panel's Variables tab shows the new variable")
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

        gt.section("promote")
        yield from open_node_panel(node_name)
        show_tab(node_panel(app.getNode(node_name)), "Variables")
        yield ("sleep", SETTLE_MS)
        check(panel_has_text(node_panel(app.getNode(node_name)), GLOBAL_LABEL),
              "the Set node's Variables tab shows the %s group" % GLOBAL_LABEL)
        check(parent_name(app.getNode(node_name).getParam("setGain")) == RFV_GROUP,
              "before the promote, setGain is in the node's Variables group")
        gt.shot("m70-promote-groups-before.png")
        field = knob_field(node_panel(app.getNode(node_name)), "setGain")
        require(field is not None, "found the field of the Set node's variable setGain")
        seen = right_click_menu(field, "m70-promote-menu.png", PROMOTE_ITEM)
        check(seen["item"] is not None and seen["item"][0],
              "a Set-node variable's right-click menu has %r, enabled (entries %r)" % (PROMOTE_ITEM, seen["entries"]))
        yield ("sleep", SETTLE_MS)
        promoted = app.getProjectParam("setGain")
        check(promoted is not None and parent_name(promoted) == RFV_GROUP and promoted.getValue() == 1.5,
              "the variable is now a project variable with the node's value")
        check(app.getNode(node_name).getParam("setGain") is not None, "the Set node keeps its own variable")
        check(parent_name(app.getNode(node_name).getParam("setGain")) == GLOBAL_GROUP,
              "the promote moves the node's setGain to its Global Variables group (parent %r)"
              % parent_name(app.getNode(node_name).getParam("setGain")))
        show_tab(node_panel(app.getNode(node_name)), "Variables")
        yield ("sleep", SETTLE_MS)
        check(panel_has_label(node_panel(app.getNode(node_name)), "Set gain"),
              "the Set node's Variables tab still shows setGain after the promote")
        gt.shot("m70-promote-groups-after.png")
        check(editor_probe("app.setGain.getValue()") == 1.5, "app.setGain resolves in the Script Editor after the promote")

        field = knob_field(node_panel(app.getNode(node_name)), "setGain")
        require(field is not None, "found the field of setGain again")
        seen = right_click_menu(field, "m70-promote-menu-disabled.png")
        check(seen["item"] is not None and not seen["item"][0] and bool(seen["item"][1]),
              "once the project declares the name, %r is disabled with a tooltip (got %r)" % (PROMOTE_ITEM, seen["item"]))

        # Both copies stay active: only the menu item is disabled, never the node's variable.
        yield ("sleep", SETTLE_MS)
        node_param = app.getNode(node_name).getParam("setGain")
        check(node_param is not None and node_param.getIsEnabled(0), "the node's setGain is still enabled after the promote")
        field = knob_field(node_panel(app.getNode(node_name)), "setGain")
        require(field is not None, "found the field of the node's setGain after the promote")
        read_only = field.isReadOnly() if hasattr(field, "isReadOnly") else False
        check(field.isEnabled() and not read_only,
              "the field of the node's setGain is enabled and editable (enabled %r, read-only %r)" % (field.isEnabled(), read_only))
        node_param.setValue(2.25)
        yield ("sleep", SETTLE_MS)
        check(app.getNode(node_name).getParam("setGain").getValue() == 2.25, "a value set on the node's setGain reads back")
        check(app.getProjectParam("setGain").getValue() == 1.5, "the project's copy keeps its own value: the copies are not linked")
        gt.shot("m70-promote-node-variable-editable.png")

        QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        require(ok, "the project settings panel is open for the promote check")
        show_tab(project_panel(), "Variables")
        yield ("sleep", SETTLE_MS)
        check(panel_has_label(project_panel(), "Set gain"), "the project panel's Variables tab shows the promoted variable")
        gt.shot("m70-project-panel-promoted.png")
        field = knob_field(project_panel(), "setGain")
        if check(field is not None, "found the field of the project variable setGain"):
            seen = right_click_menu(field)
            check(seen["entries"] is not None and PROMOTE_ITEM not in seen["entries"],
                  "a project variable's right-click menu has no %r (entries %r)" % (PROMOTE_ITEM, seen["entries"]))

        gt.section("promote-undo")
        yield ("sleep", SETTLE_MS)
        yield from activate_main_window()
        undo = edit_menu_action("undo")
        check(undo is not None and undo.isEnabled() and PROMOTE_ITEM.lower() in undo.text().lower(),
              "the newest undo step is the promote (Edit menu has %r)" % (undo.text() if undo is not None else None))
        undo = None
        press_undo()
        yield ("sleep", SETTLE_MS)
        check(app.getProjectParam("setGain") is None, "Ctrl+Z removes the promoted variable from the project")
        node_param = app.getNode(node_name).getParam("setGain")
        check(node_param is not None and node_param.getIsEnabled(0) and node_param.getValue() == 2.25,
              "the node's setGain is untouched by the undo")
        check(editor_probe("hasattr(app, 'setGain')") is False, "the undo drops app.setGain")
        check(parent_name(app.getNode(node_name).getParam("setGain")) == RFV_GROUP,
              "the undo moves the node's setGain back to its Variables group (parent %r)"
              % parent_name(app.getNode(node_name).getParam("setGain")))
        if node_panel(app.getNode(node_name)) is not None:
            show_tab(node_panel(app.getNode(node_name)), "Variables")
            yield ("sleep", SETTLE_MS)
            gt.shot("m70-promote-groups-undone.png")
        if project_panel() is None:
            QMetaObject.invokeMethod(main_gui(), "setVisibleProjectSettingsPanel")
        ok = yield ("until", lambda: project_panel() is not None, DIALOG_TIMEOUT_S)
        if check(ok, "the project settings panel is open after the undo"):
            show_tab(project_panel(), "Variables")
            yield ("sleep", SETTLE_MS)
            check(not panel_has_label(project_panel(), "Set gain"), "the project panel's Variables tab no longer shows setGain")
            check(panel_has_label(project_panel(), "Project gain"), "the project panel's Variables tab still shows projGain")
            gt.shot("m70-project-panel-promote-undone.png")

        gt.section("existing")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        third = app.createNode(SET_PLUGIN, -1, None, props)
        require(third is not None, "created a third ReverseFlowVariableSet node")
        third_name = third.getScriptName()
        third = None
        ok = yield ("until", lambda: node_panel(app.getNode(third_name)) is not None, DIALOG_TIMEOUT_S)
        require(ok, "the third Set node's panel is open")
        # Opening the panel lists the variables; reopening it here lists those declared since.
        yield from open_node_panel(third_name)
        check(app.getNode(third_name).getParam(EXISTING_CHOICE) is not None, "the Set node has the existing-variable drop-down")
        check(parent_name(app.getNode(third_name).getParam(EXISTING_CHOICE)) == GLOBAL_GROUP,
              "the drop-down is in the %s group, not the node's variables group" % GLOBAL_GROUP)
        require(show_tab(node_panel(app.getNode(third_name)), "Variables"), "the Set node's panel has a Variables tab")
        yield ("sleep", SETTLE_MS)
        check(panel_has_text(node_panel(app.getNode(third_name)), GLOBAL_LABEL),
              "the drop-down's %s group is on the Variables tab" % GLOBAL_LABEL)
        combo = existing_combo(node_panel(app.getNode(third_name)))
        require(combo is not None, "found the existing-variable drop-down on the panel")
        items = combo_items(combo)
        check(len(items) > 0 and items[0] == EXISTING_PROMPT, "the drop-down's first entry is %r (got %r)" % (EXISTING_PROMPT, items[:1]))
        check("projGain" in items and "setGain" in items,
              "the drop-down offers the project's and the other Set node's variables (got %r)" % items)
        picked = pick_from_combo(combo, "projGain", "m70-existing-menu.png")
        combo = None
        check(picked, "chose projGain from the drop-down's menu")
        yield ("sleep", SETTLE_MS)
        added = app.getNode(third_name).getParam("projGain")
        check(added is not None and parent_name(added) == GLOBAL_GROUP,
              "choosing projGain adds a variable to the node's Global Variables group (parent %r)" % parent_name(added))
        check(added is not None and isinstance(added, NatronEngine.DoubleParam) and added.getValue() == app.getProjectParam("projGain").getValue(),
              "the added variable is a Double starting from the project's value")
        added = None
        combo = existing_combo(node_panel(app.getNode(third_name)))
        if check(combo is not None, "found the drop-down again after adding"):
            current = combo.property("natronCurrentText")
            check(current == EXISTING_PROMPT, "the drop-down is back on its prompt (shows %r)" % current)
            check("projGain" not in combo_items(combo), "projGain is no longer offered (got %r)" % combo_items(combo))
        combo = None
        show_tab(node_panel(app.getNode(third_name)), "Variables")
        yield ("sleep", SETTLE_MS)
        check(panel_has_label(node_panel(app.getNode(third_name)), "Project gain"),
              "the Set node's Variables tab shows the added variable")
        gt.shot("m70-existing-added.png")

        gt.section("existing-undo")
        yield from activate_main_window()
        press_undo()
        yield ("sleep", SETTLE_MS)
        check(app.getNode(third_name).getParam("projGain") is None, "Ctrl+Z takes the added variable back off the node")
        check(app.getNode(third_name).getParam(EXISTING_CHOICE) is not None, "the drop-down survives the undo")
        check(parent_name(app.getNode(third_name).getParam(EXISTING_CHOICE)) == GLOBAL_GROUP,
              "the drop-down stays in the %s group after the undo" % GLOBAL_GROUP)
        check(app.getProjectParam("projGain") is not None, "the project's variable is untouched by the undo")

        gt.section("right-click manage")
        if QTest is None:
            report("NOTE PySide6.QtTest is unavailable: skipping the real right-click and Close click checks")
        else:
            yield from activate_main_window()
            # Inside the RFV group: the user's report. The press that opened the menu is still on
            # the stack of the row it hit while the dialog rebuilds the user pages.
            yield from manage_from_right_click(node_name, "Set gain")
            require(select(RFV_GROUP), "the dialog lists the RFV group " + RFV_GROUP)
            yield from add_param("ManageUserParamsDialog", "gainVec", "Gain vec", TYPE_DOUBLE_3D)
            gt.shot("m70-right-click-manage-added.png")
            yield from close_with_mouse("ManageUserParamsDialog")
            yield ("sleep", SETTLE_MS)
            check(main_gui() is not None and graph_up(), "Natron is still running after Close")
            vec = app.getNode(node_name).getParam("gainVec")
            check(vec is not None and vec.getNumDimensions() == 3, "the 3-dimension gainVec exists on the node")
            check(vec is not None and parent_name(vec) == RFV_GROUP, "gainVec is in the RFV group (parent %r)" % parent_name(vec))
            vec = None
            show_tab(node_panel(app.getNode(node_name)), "Variables")
            yield ("sleep", SETTLE_MS)
            check(node_panel(app.getNode(node_name)) is not None and panel_has_label(node_panel(app.getNode(node_name)), "Gain vec"),
                  "the panel shows gainVec")
            gt.shot("m70-right-click-manage-closed.png")

            # Outside the group: a plain user page.
            yield from manage_from_right_click(node_name, "Gain vec")
            clear_selection()
            yield from add_param("ManageUserParamsDialog", "plainCount", "Plain count", TYPE_INT)
            yield from close_with_mouse("ManageUserParamsDialog")
            yield ("sleep", SETTLE_MS)
            check(main_gui() is not None and graph_up(), "Natron is still running after Close with a plain user knob")
            plain = app.getNode(node_name).getParam("plainCount")
            check(plain is not None and isinstance(plain, NatronEngine.IntParam), "the Int plainCount exists on the node")
            check(plain is not None and parent_name(plain) != RFV_GROUP,
                  "plainCount is outside the RFV group (parent %r)" % parent_name(plain))
            plain = None
            panel = node_panel(app.getNode(node_name))
            if panel is not None:
                for tabs in panel.findChildren(QTabWidget):
                    for i in range(tabs.count()):
                        if tabs.tabText(i).replace("&", "") == "User":
                            tabs.setCurrentIndex(i)
                QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            check(node_panel(app.getNode(node_name)) is not None and panel_has_label(node_panel(app.getNode(node_name)), "Plain count"),
                  "the panel shows plainCount")
            gt.shot("m70-right-click-manage-plain.png")

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
