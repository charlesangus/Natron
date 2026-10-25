"""A GUI test of Python edits in the window's undo history: what a Script Editor run, a parameter's
callback or a button's callback changes is undone with Edit > Undo, in order with the user's own
edits, one entry per run. It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m114-python-undo/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m114_python_undo.py <AppImage or build/fast/App/Natron>'

This script's own Python calls run outside any undo step, so they set the scene without adding to
the history. Scripts meant to be recorded are run through the Script Editor the way its Execute
button runs them. Undo and redo go through the Edit menu actions: Ctrl+Z over the Script Editor
steps its own script history instead.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 400
BLUR = "net.sf.cimg.CImgBlur"
SIZE = "size"
CROP = "cropToFormat"
BUTTON = "m114Button"
BUTTON_LABEL = "M114 Button"
MULTI = "m114Multi"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, Qt
    from PySide6.QtGui import QKeyEvent
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QLineEdit, QMainWindow, QPlainTextEdit,
                                   QTabWidget)

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- plumbing --------------------------------------------------------------------------

    def create(plugin_id, x, y, undoable=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        if undoable:
            props["CreateNodeArgsPropAddUndoRedoCommand"] = NatronEngine.BoolNodeCreationProperty(True)
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

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

    def trigger(prefix):
        a = edit_menu_action(prefix)
        if not check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
            return False
        a.trigger()
        QApplication.processEvents()
        return True

    def app_var():
        """The name the Script Editor knows this project by."""
        return "app%d" % (app.getAppID() + 1)

    def run_in_script_editor(script):
        """Puts script into the Script Editor and runs it the way its Execute button does."""
        editors = gt.widgets_of("ScriptEditor")
        editor = next((e for e in editors if e.isVisible()), editors[0] if editors else None)
        if not check(editor is not None, "found a Script Editor"):
            return False
        inputs = [w for w in editor.findChildren(QPlainTextEdit) if gt.class_name(w) == "InputScriptTextEdit"]
        if not check(len(inputs) == 1, "the Script Editor has one input field (got %d)" % len(inputs)):
            return False
        inputs[0].setPlainText(script)
        QMetaObject.invokeMethod(editor, "onExecScriptClicked")
        QApplication.processEvents()
        return True

    # ---- panels ----------------------------------------------------------------------------

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

    def type_into_panel(name, value):
        """Types value into the first visible, enabled spin box of the node's panel and presses
        Return, which is how KnobGui pushes a value command."""
        p = panel_of(name)
        if not check(p is not None, "found the settings panel of " + name):
            return False
        boxes = [w for w in p.findChildren(QLineEdit)
                 if gt.class_name(w) == "SpinBox" and w.isEnabled() and w.isVisible()]
        if not check(len(boxes) > 0, "the settings panel of %s has a visible, enabled spin box" % name):
            return False
        box = boxes[0]
        box.setFocus()
        box.setText(str(value))
        send_key(box, Qt.Key_Return)
        return True

    def show_page(name, page_label):
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

    def click_panel_button(name, text):
        """Clicks the visible, enabled button labelled text in the node's panel. The panel wrapper
        stays referenced while its children are used."""
        p = panel_of(name)
        if p is None:
            return False
        buttons = [b for b in p.findChildren(QAbstractButton)
                   if b.text().replace("&", "") == text and b.isVisible() and b.isEnabled()]
        if not buttons:
            return False
        buttons[0].click()
        QApplication.processEvents()
        return True

    # ---- state -----------------------------------------------------------------------------

    def snapshot(name):
        node = app.getNode(name)
        out = {}
        if node is None:
            return out
        for p in node.getParams():
            getv = getattr(p, "getValue", None)
            if getv is None:
                continue
            try:
                n = p.getNumDimensions()
            except Exception:
                continue
            for d in range(n):
                try:
                    out[(p.getScriptName(), d)] = getv(d)
                except Exception:
                    try:
                        out[(p.getScriptName(), d)] = getv()
                    except Exception:
                        pass
        return out

    def num_keys(name):
        node = app.getNode(name)
        p = node.getParam(SIZE) if node is not None else None
        return p.getNumKeys(0) if p is not None else -1

    def expression(name, dim):
        node = app.getNode(name)
        p = node.getParam(SIZE) if node is not None else None
        if p is None:
            return None
        e = p.getExpression(dim)
        return e[0] if isinstance(e, (tuple, list)) else e

    def input_name(name, nb=0):
        node = app.getNode(name)
        src = node.getInput(nb) if node is not None else None
        return src.getScriptName() if src is not None else None

    # ---- callbacks -------------------------------------------------------------------------

    _in_callback = [False]

    def m114_param_changed(thisParam, thisNode, thisGroup, app, userEdited):
        """A user edit flips the crop toggle; the button sets the size and flips the toggle."""
        if _in_callback[0]:
            return
        name = thisParam.getScriptName()
        _in_callback[0] = True
        try:
            if name == BUTTON:
                thisNode.getParam(SIZE).setValue(13.0, 0)
                crop = thisNode.getParam(CROP)
                crop.set(not crop.get())
            elif name == SIZE and userEdited:
                crop = thisNode.getParam(CROP)
                crop.set(not crop.get())
        finally:
            _in_callback[0] = False

    import __main__
    __main__.m114_param_changed = m114_param_changed

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        viewers = [n for n in app.getChildren() if n.getPluginID() == "fr.inria.built-in.Viewer"]
        if not viewers:
            viewers = [create("fr.inria.built-in.Viewer", 0, 400)]
        gt.require(len(viewers) > 0 and viewers[0] is not None, "the project has a viewer")
        check(not undo_enabled(), "the history is empty before any edit")

        # The user's example: a node created the way the node graph creates it, a value typed in
        # its panel, then the node destroyed from Python.
        blur = create(BLUR, 0, 0, undoable=True).getScriptName()
        out = create(BLUR, 300, 0).getScriptName()
        app.getNode(out).connectInput(0, app.getNode(blur))
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        gt.require(input_name(out) == blur, "%s is connected to %s" % (out, blur))
        check(undo_enabled(), "creating %s with an undo command put it in the history" % blur)

        gt.section("user-example")
        blur_before = snapshot(blur)
        type_into_panel(blur, 7)
        yield ("sleep", SETTLE_MS)
        blur_after = snapshot(blur)
        gt.require(blur_after != blur_before, "typing into %s's panel changed a parameter" % blur)

        gt.require(run_in_script_editor("%s.%s.destroy()\n" % (app_var(), blur)), "ran destroy() in the Script Editor")
        yield ("sleep", SETTLE_MS)
        gt.require(app.getNode(blur) is None, "the Script Editor destroyed " + blur)
        check(input_name(out) is None, "destroying %s disconnected %s" % (blur, out))

        trigger("undo")
        yield ("sleep", SETTLE_MS)
        gt.require(app.getNode(blur) is not None, "undo brought %s back" % blur)
        check(input_name(out) == blur, "the restored %s feeds %s again (input %r)" % (blur, out, input_name(out)))
        check(snapshot(blur) == blur_after, "the restored %s keeps the typed value" % blur)

        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(blur) == blur_before, "the second undo reverted the typed value")
        check(app.getNode(blur) is not None, "the second undo left %s in place" % blur)

        trigger("redo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(blur) == blur_after, "redo re-applied the typed value")
        trigger("redo")
        yield ("sleep", SETTLE_MS)
        check(app.getNode(blur) is None, "the second redo destroyed %s again" % blur)
        check(input_name(out) is None, "the second redo disconnected %s again" % out)

        # ---- one Script Editor run of many edits is one entry.
        gt.section("multi-edit")
        out_before = snapshot(out)
        v = app_var()
        script = "\n".join([
            "n = %s.createNode(%r)" % (v, BLUR),
            "n.setScriptName(%r)" % MULTI,
            "n.connectInput(0, %s.%s)" % (v, out),
            "p = %s.%s.%s" % (v, out, SIZE),
            "p.setValue(5.0, 1)",
            "p.setExpression('4.5', False, 1)",
            "p.setValueAtTime(1.0, 1, 0)",
            "p.setValueAtTime(2.0, 5, 0)",
            "p.setValueAtTime(3.0, 9, 0)",
            "",
        ])
        gt.require(run_in_script_editor(script), "ran the multi-edit in the Script Editor")
        yield ("sleep", SETTLE_MS)
        gt.require(app.getNode(MULTI) is not None, "the multi-edit created and renamed " + MULTI)
        check(input_name(MULTI) == out, "the multi-edit connected %s to %s" % (MULTI, out))
        check(num_keys(out) == 3, "the multi-edit set three keys on %s" % out)
        check(expression(out, 1) == "4.5", "the multi-edit set an expression on %s" % out)

        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(app.getNode(MULTI) is None, "one undo removed " + MULTI)
        check(num_keys(out) == 0, "one undo removed %s's keys (%d left)" % (out, num_keys(out)))
        check(not expression(out, 1), "one undo removed %s's expression" % out)
        check(snapshot(out) == out_before, "one undo restored every value of " + out)

        trigger("redo")
        yield ("sleep", SETTLE_MS)
        check(app.getNode(MULTI) is not None, "one redo brought back " + MULTI)
        check(input_name(MULTI) == out, "one redo reconnected %s to %s" % (MULTI, out))
        check(num_keys(out) == 3, "one redo restored %s's keys" % out)
        check(expression(out, 1) == "4.5", "one redo restored %s's expression" % out)
        gt.shot("m114-python-undo-multi.png")

        # ---- what a parameter's callback changes undoes with the edit that fired it.
        gt.section("callback-merge")
        cb = create(BLUR, 600, 0).getScriptName()
        cb_node = app.getNode(cb)
        cb_node.createButtonParam(BUTTON, BUTTON_LABEL)
        cb_node.refreshUserParamsGUI()
        cb_node.getParam("onParamChanged").setValue("m114_param_changed")
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        gt.require(cb_node.getParam(BUTTON) is not None, "%s has the %s button" % (cb, BUTTON))
        cb_node = None

        cb_before = snapshot(cb)
        crop_before = app.getNode(cb).getParam(CROP).get()
        type_into_panel(cb, 6)
        yield ("sleep", SETTLE_MS)
        cb_after = snapshot(cb)
        gt.require(cb_after != cb_before, "typing into %s's panel changed a parameter" % cb)
        gt.require(app.getNode(cb).getParam(CROP).get() != crop_before,
                   "the callback flipped %s's crop toggle" % cb)
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(cb) == cb_before, "one undo reverted the typed value and the callback's edit")
        trigger("redo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(cb) == cb_after, "one redo re-applied both")

        # ---- a button's callback is one step.
        gt.section("button")
        gt.require(show_page(cb, "User"), "showed the User page of " + cb)
        yield ("sleep", SETTLE_MS)
        before_button = snapshot(cb)
        gt.require(click_panel_button(cb, BUTTON_LABEL), "clicked %s on %s's panel" % (BUTTON_LABEL, cb))
        yield ("sleep", SETTLE_MS)
        after_button = snapshot(cb)
        gt.require(after_button.get((SIZE, 0)) == 13.0, "the button's callback set the size",
                   "size %r" % (after_button.get((SIZE, 0)),))
        gt.require(after_button.get((CROP, 0)) != before_button.get((CROP, 0)),
                   "the button's callback flipped the crop toggle")
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(cb) == before_button,
              "one undo reverted everything the button's callback did and nothing before it")
        trigger("redo")
        yield ("sleep", SETTLE_MS)
        check(snapshot(cb) == after_button, "one redo re-applied the button's callback edits")
        gt.shot("m114-python-undo.png")

        report("NOTE wrote the history through: user example, multi-edit, callback merge, button")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
