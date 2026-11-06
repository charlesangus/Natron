"""A GUI test that the window's undo history survives a rebuild of a node panel's parameter widgets.

Adding a user parameter rebuilds the panel's user page and every curve of the node in the curve
editor (KnobGuiContainerHelper::recreateUserKnobs). Edits recorded before the rebuild must still
undo and redo afterwards, in order, together with edits made after it. It is driven by
Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m114-rebuild/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m114_rebuild_undo.py <AppImage or build/fast/App/Natron>'

The edits are made the way a user makes them: typing into spin boxes of the node's panel, and
deleting a parameter's keys with Ctrl+A/Backspace in the curve editor. The user parameter is added
from Python (createDoubleParam + refreshUserParamsGUI), which rebuilds the panel through the same
recreateUserKnobs call as the Manage User Params dialog's OK button; the dialog's nested modal
add-parameter dialog is not driven. Adding a parameter is not itself an undoable edit.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 400
BLUR = "Blur"
CURVE_PARAM = "mix"
TYPED_USER = "typedUser"
KEYED_USER = "keyedUser"
ADDED_USER = "addedUser"
USER_PARAMS = (TYPED_USER, KEYED_USER, ADDED_USER)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QCursor, QKeyEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMainWindow, QTabWidget, QTreeWidget
    from shiboken6 import isValid

    import guitest as gt
    from guitest import check, report

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- plumbing --------------------------------------------------------------------------

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

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

    def undo_enabled():
        a = edit_menu_action("undo")
        return a is not None and a.isEnabled()

    def redo_enabled():
        a = edit_menu_action("redo")
        return a is not None and a.isEnabled()

    def undo_text():
        a = edit_menu_action("undo")
        return a.text() if a is not None else None

    def activate_window(widget):
        """Window shortcuts such as Edit > Undo only fire in the active window, and typing into a
        panel leaves the main window inactive under Xvfb."""
        win = widget.window()
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)
        return win.isActiveWindow()

    def shortcut(widget, redo):
        """Ctrl+Z (or Ctrl+Shift+Z) with the mouse over widget, sent to widget."""
        activate_window(widget)
        QCursor.setPos(widget.mapToGlobal(widget.rect().center()))
        QApplication.processEvents()
        mods = Qt.ControlModifier | (Qt.ShiftModifier if redo else Qt.NoModifier)
        if QTest is None:
            report("NOTE PySide6.QtTest is unavailable: triggering the Edit menu action instead of the shortcut")
            a = edit_menu_action("redo" if redo else "undo")
            if check(a is not None and a.isEnabled(), "an enabled Edit menu action is there to trigger"):
                a.trigger()
                QApplication.processEvents()
            return
        QTest.keyClick(widget, Qt.Key_Z, mods)
        QApplication.processEvents()

    # ---- panel -----------------------------------------------------------------------------

    def panel_of(name):
        """The settings panel of the node with that script name, found by the label in its
        header's name field."""
        node = app.getNode(name)
        if node is None:
            return None
        label = node.getLabel()
        for p in gt.widgets_of("NodeSettingsPanel"):
            for le in p.findChildren(QLineEdit):
                if gt.class_name(le) == "LineEdit" and le.text() == label:
                    return p
        return None

    # PySide makes the wrappers p.findChildren returns children of p's wrapper and marks them
    # deleted once p's wrapper is collected, although the C++ widgets live on (see gt.menu_of).
    # Every lookup below therefore uses what it finds while the panel's wrapper is still held.

    def panel_button_enabled(name, what):
        p = panel_of(name)
        if p is None:
            return None
        for b in p.findChildren(QAbstractButton):
            if ("%s the last change" % what) in b.toolTip():
                return b.isEnabled()
        return None

    def show_page(name, page_label):
        """Makes page_label the current tab of the node's panel. Looked up afresh each time: a
        rebuild replaces the page widgets."""
        p = panel_of(name)
        if p is None:
            return False
        for tabs in p.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == page_label:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def spin_boxes(p):
        return [w for w in p.findChildren(QLineEdit)
                if gt.class_name(w) == "SpinBox" and w.isEnabled() and w.isVisible()]

    def has_spin_box(name):
        p = panel_of(name)
        return p is not None and len(spin_boxes(p)) > 0

    def type_into(name, value):
        """Types value into the first visible, enabled spin box of the node's panel and presses
        Return, which is how KnobGui pushes a value command."""
        p = panel_of(name)
        if p is None:
            return False
        boxes = spin_boxes(p)
        if not boxes:
            return False
        box = boxes[0]
        box.setFocus()
        box.setText(str(value))
        send_key(box, Qt.Key_Return)
        return True

    def plugin_values(name):
        """The node's plugin parameter values, without the user parameters and the keyed one."""
        node = app.getNode(name)
        out = {}
        if node is None:
            return out
        for p in node.getParams():
            script_name = p.getScriptName()
            if script_name in USER_PARAMS or script_name == CURVE_PARAM:
                continue
            getv = getattr(p, "getValue", None)
            if getv is None:
                continue
            try:
                n = p.getNumDimensions()
            except Exception:
                continue
            for d in range(n):
                try:
                    out[(script_name, d)] = getv(d)
                except Exception:
                    try:
                        out[(script_name, d)] = getv()
                    except Exception:
                        pass
        return out

    def value_of(name, param):
        node = app.getNode(name)
        p = node.getParam(param) if node is not None else None
        return p.getValue() if p is not None else None

    def num_keys(name, param):
        node = app.getNode(name)
        p = node.getParam(param) if node is not None else None
        return p.getNumKeys(0) if p is not None else -1

    # ---- curve editor ----------------------------------------------------------------------

    def curve_widget():
        # Several CurveEditor wrappers can exist (stale ones from closed panels); keep only live ones.
        best = None
        for ce in gt.widgets_of("CurveEditor"):
            if not isValid(ce):
                continue
            widgets = [w for w in gt.widgets_of("CurveWidget") if isValid(w) and ce.isAncestorOf(w)]
            if widgets and (best is None or ce.isVisible()):
                best = widgets[0]
        return best

    def tree_select_row(node_name, row_label):
        """Selects only the row labelled row_label under the named node's row, and its children,
        so that Ctrl+A acts on that parameter's curves. Never holds the tree wrapper across calls:
        the rebuild replaces the tree items."""
        from PySide6.QtCore import QItemSelectionModel
        flags = QItemSelectionModel.Select | QItemSelectionModel.Rows

        def walk(model, parent):
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                yield idx
                for sub in walk(model, idx):
                    yield sub

        for ce in gt.widgets_of("CurveEditor"):
            if not isValid(ce):
                continue
            for t in ce.findChildren(QTreeWidget):
                if not (isValid(t) and t.objectName() == "tree"):
                    continue
                t.expandAll()
                model = t.model()
                sel = t.selectionModel()
                for top in range(model.rowCount()):
                    top_idx = model.index(top, 0)
                    if node_name not in str(model.data(top_idx)):
                        continue
                    for idx in walk(model, top_idx):
                        if str(model.data(idx)) == row_label:
                            sel.clear()
                            sel.select(idx, flags)
                            for sub in walk(model, idx):
                                sel.select(sub, flags)
                            return True
        return False

    def delete_keys_in_curve_editor(node_name, row_label):
        if not check(tree_select_row(node_name, row_label),
                     "selected %s's %s row in the curve editor tree" % (node_name, row_label)):
            return False
        QApplication.processEvents()
        widget = curve_widget()
        if not check(widget is not None, "found the curve widget"):
            return False
        send_key(widget, Qt.Key_A, Qt.ControlModifier)
        send_key(widget, Qt.Key_Backspace)
        return True

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        viewers = [n for n in app.getChildren() if n.getPluginID() == "Viewer"]
        if not viewers:
            viewer = app.createNode("Viewer")
            gt.require(viewer is not None, "created a viewer for the empty project")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(BLUR, -1, None, props)
        gt.require(node is not None, "create " + BLUR, gt.plugin_problem(BLUR))
        blur = node.getScriptName()

        mix = node.getParam(CURVE_PARAM)
        gt.require(mix is not None and hasattr(mix, "setValueAtTime"), "%s has an animatable %s" % (blur, CURVE_PARAM))
        mix.setValueAtTime(0.5, 1, 0)
        mix.setValueAtTime(0.8, 10, 0)
        typed = node.createDoubleParam(TYPED_USER, "Typed User")
        keyed = node.createDoubleParam(KEYED_USER, "Keyed User")
        gt.require(typed is not None and keyed is not None, "created two user parameters")
        keyed.setValueAtTime(2.0, 1, 0)
        keyed.setValueAtTime(4.0, 10, 0)
        node.refreshUserParamsGUI()
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: curve_widget() is not None, 10.0)
        gt.require(ok, "found the curve editor's curve widget")
        check(not undo_enabled(), "the history is empty before any user edit")

        plugin_before = plugin_values(blur)
        typed_before = value_of(blur, TYPED_USER)

        # ---- edits before the rebuild.
        gt.section("edits")
        gt.require(show_page(blur, "Controls"), "showed the Controls page of " + blur)
        gt.require(has_spin_box(blur), "the Controls page of %s has a visible, enabled spin box" % blur)
        gt.require(type_into(blur, 7), "typed 7 into the Controls page's spin box")
        yield ("sleep", SETTLE_MS)
        gt.require(plugin_values(blur) != plugin_before, "1: typing into the Controls page changed a plugin parameter")
        plugin_after = plugin_values(blur)

        gt.require(show_page(blur, "User"), "showed the User page of " + blur)
        gt.require(has_spin_box(blur), "the User page of %s has a visible, enabled spin box" % blur)
        gt.require(type_into(blur, 3), "typed 3 into the User page's spin box")
        yield ("sleep", SETTLE_MS)
        gt.require(value_of(blur, TYPED_USER) == 3, "2: typing into the User page set %s" % TYPED_USER,
                   "value %r" % (value_of(blur, TYPED_USER),))

        delete_keys_in_curve_editor(blur, "Mix")
        yield ("sleep", SETTLE_MS)
        gt.require(num_keys(blur, CURVE_PARAM) == 0, "3: Ctrl+A, Backspace in the curve editor removed the %s keys" % CURVE_PARAM,
                   "%d keys left" % num_keys(blur, CURVE_PARAM))
        gt.require(num_keys(blur, KEYED_USER) == 2, "3: the curve editor edit left %s's keys alone" % KEYED_USER)

        # ---- the rebuild.
        gt.section("rebuild")
        text_before_rebuild = undo_text()
        added = node.createDoubleParam(ADDED_USER, "Added User")
        gt.require(added is not None, "added a user parameter")
        node.refreshUserParamsGUI()
        yield ("sleep", SETTLE_MS)
        check(undo_enabled() and undo_text() == text_before_rebuild,
              "the history is untouched by the rebuild (%r, was %r)" % (undo_text(), text_before_rebuild))
        check(panel_button_enabled(blur, "Undo") is True, "the rebuilt panel's undo button is enabled")

        # ---- an edit after the rebuild.
        gt.section("after-rebuild")
        delete_keys_in_curve_editor(blur, "Keyed User")
        yield ("sleep", SETTLE_MS)
        gt.require(num_keys(blur, KEYED_USER) == 0, "4: Ctrl+A, Backspace in the curve editor removed %s's keys" % KEYED_USER,
                   "%d keys left" % num_keys(blur, KEYED_USER))

        def state():
            return (plugin_values(blur) == plugin_after,
                    value_of(blur, TYPED_USER) == 3,
                    num_keys(blur, CURVE_PARAM) == 0,
                    num_keys(blur, KEYED_USER) == 0)

        def expected(done):
            return tuple(i < done for i in range(4))

        check(state() == expected(4), "all four edits are in place (state %r)" % (state(),))

        # ---- undo everything in order, then redo it.
        gt.section("undo-redo")
        graph = root_graph()
        for i in range(4):
            shortcut(graph, redo=False)
            yield ("sleep", SETTLE_MS)
            check(state() == expected(3 - i), "Ctrl+Z #%d undoes edit %d only (state %r)" % (i + 1, 4 - i, state()))
        check(plugin_values(blur) == plugin_before, "after undoing everything the plugin parameters are back")
        check(value_of(blur, TYPED_USER) == typed_before, "after undoing everything %s is back" % TYPED_USER)
        check(num_keys(blur, CURVE_PARAM) == 2 and num_keys(blur, KEYED_USER) == 2, "after undoing everything the keys are back")
        check(not undo_enabled(), "nothing is left to undo")
        for i in range(4):
            shortcut(graph, redo=True)
            yield ("sleep", SETTLE_MS)
            check(state() == expected(i + 1), "Ctrl+Shift+Z #%d redoes edit %d only (state %r)" % (i + 1, i + 1, state()))
        check(not redo_enabled(), "nothing is left to redo")
        check(app.getNode(blur).getParam(ADDED_USER) is not None, "the added user parameter is still there")
        gt.shot("m114-rebuild-undo.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
