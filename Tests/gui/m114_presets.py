"""A GUI test that importing node presets resets the node to the preset and is one undoable edit.

A preset is exported from a mostly default node through the panel's Export as presets action and
imported into a modified node of the same plug-in through Import presets. The import must clear what
the preset does not hold (values, keyframes, an expression, a link to another node) while keeping the
label, the disable switch, a writer's file and the node's own user parameters. Ctrl+Z over the node
graph must restore the node exactly, Ctrl+Shift+Z re-apply the preset, and the panel's undo and
redo buttons must do the same. It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m114-presets/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m114_presets.py <AppImage or build/fast/App/Natron>'

The panel's settings menu is a modal QMenu, so its two actions are invoked as the panel's slots,
which is what the menu's actions are connected to. The file dialogs they open are filled in and
accepted the way a user does. The target's modifications are made from Python, which records no
undo history, so the import is the only edit undone.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 400
DIALOG_APPEAR_S = 30.0
DIALOG_CLOSE_S = 15.0
DIALOG_TOTAL_S = 60.0
BLUR = "net.sf.cimg.CImgBlur"
WRITE = "fr.inria.built-in.Write"
LABEL = "kept label"
EXPRESSION = "3.5"
PRESET_USER = "presetUser"
OWN_USER = "ownUser"

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, Qt, QTimer
    from PySide6.QtGui import QCursor
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    PRESET_DIR = os.path.join(gt.OUT, "presets")
    os.makedirs(PRESET_DIR, exist_ok=True)

    # ---- plumbing --------------------------------------------------------------------------

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

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

    def activate_window(widget):
        """Window shortcuts such as Edit > Undo only fire in the active window, and a modal file
        dialog leaves the main window inactive under Xvfb."""
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
        check(activate_window(widget), "the main window is active before the shortcut")
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

    def click_panel_button(name, what):
        p = panel_of(name)
        if p is None:
            return False
        for b in p.findChildren(QAbstractButton):
            if ("%s the last change" % what) in b.toolTip():
                if not (b.isVisible() and b.isEnabled()):
                    return False
                b.click()
                QApplication.processEvents()
                return True
        return False

    def through_file_dialog(name, slot, path):
        """Invokes the panel slot, which runs a modal file dialog, and accepts the dialog with
        path typed in from timers. The slot blocks until the dialog closes, so everything that
        can end the wait is a timer: it gives up on a dialog that never shows, rejects one that
        will not accept, and rejects any dialog still open at the hard deadline. Returns whether
        the dialog was seen and accepted."""
        state = {"seen": False, "accepted": False, "done": False, "accepted_at": None}
        start = time.monotonic()

        def visible_dialogs():
            return [d for d in gt.widgets_of("SequenceFileDialog") if d.isVisible()]

        def fill():
            if state["done"]:
                return
            elapsed = time.monotonic() - start
            dialogs = visible_dialogs()
            if not dialogs:
                if state["seen"]:
                    state["done"] = True
                    return
                if elapsed > DIALOG_APPEAR_S:
                    report("NOTE no file dialog appeared within %gs" % DIALOG_APPEAR_S)
                    state["done"] = True
                    return
                QTimer.singleShot(200, fill)
                return
            dialog = dialogs[0]
            state["seen"] = True
            if elapsed > DIALOG_TOTAL_S or (state["accepted_at"] is not None
                                            and time.monotonic() - state["accepted_at"] > DIALOG_CLOSE_S):
                report("NOTE the file dialog did not close in time; rejecting it")
                state["accepted"] = False
                dialog.reject()
                QTimer.singleShot(200, fill)
                return
            if state["accepted_at"] is None:
                dialog.activateWindow()
                dialog.raise_()
                # The selection edit is created before the filter edit.
                edits = [w for w in dialog.findChildren(QLineEdit) if gt.class_name(w) == "LineEdit"]
                if not edits:
                    report("NOTE dialog line edits: %r" % [gt.class_name(w) for w in dialog.findChildren(QLineEdit)])
                    dialog.reject()
                    QTimer.singleShot(200, fill)
                    return
                edits[0].setText(path)
                state["accepted_at"] = time.monotonic()
                state["accepted"] = True
                QMetaObject.invokeMethod(dialog, "openSelectedFiles")
            QTimer.singleShot(200, fill)

        p = panel_of(name)
        if p is None:
            return False
        QTimer.singleShot(300, fill)
        QMetaObject.invokeMethod(p, slot)
        state["done"] = True
        QApplication.processEvents()
        return state["seen"] and state["accepted"]

    def export_preset(name, file_name):
        path = os.path.join(PRESET_DIR, file_name)
        if os.path.isfile(path):
            os.remove(path)
        if not check(through_file_dialog(name, "onExportPresetsActionTriggered", path),
                     "Export as presets on %s opened a file dialog" % name):
            return None
        if not check(os.path.isfile(path), "the preset %s was written" % file_name):
            return None
        return path

    def import_preset(name, path):
        if path is None:
            check(False, "Import presets on %s skipped: its preset was not exported" % name)
            return
        check(through_file_dialog(name, "onImportPresetsActionTriggered", path),
              "Import presets on %s opened a file dialog" % name)

    # ---- state -----------------------------------------------------------------------------

    def param(name, script_name):
        node = app.getNode(name)
        return node.getParam(script_name) if node is not None else None

    def dims(p):
        try:
            return p.getNumDimensions()
        except Exception:
            return 1

    def value(name, script_name, dim=0):
        p = param(name, script_name)
        if p is None:
            return None
        try:
            return p.getValue(dim)
        except TypeError:
            return p.getValue()

    def default(name, script_name, dim=0):
        p = param(name, script_name)
        try:
            return p.getDefaultValue(dim)
        except TypeError:
            return p.getDefaultValue()

    def expression(name, script_name, dim):
        p = param(name, script_name)
        return str(p.getExpression(dim)[0]) if p is not None else None

    def knob_state(name, script_name):
        p = param(name, script_name)
        if p is None:
            return None
        out = []
        for d in range(dims(p)):
            keys = p.getNumKeys(d) if hasattr(p, "getNumKeys") else 0
            expr = str(p.getExpression(d)[0]) if hasattr(p, "getExpression") else ""
            out.append((value(name, script_name, d), keys, expr))
        return tuple(out)

    def node_state(name, script_names):
        return dict((s, knob_state(name, s)) for s in script_names)

    def create(plugin_id):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        return node

    def create_writer(file_name):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createWriter(os.path.join(PRESET_DIR, file_name), None, props)
        require(node is not None and node.getPluginID() == WRITE, "create a native Write", gt.plugin_problem(WRITE))
        return node

    def undo_redo_round(target, before, after, describe):
        """Ctrl+Z over the graph, Ctrl+Shift+Z, then the panel's undo and redo buttons, checking
        the node against before/after each time."""
        graph = root_graph()
        require(graph is not None, "the node graph is visible")
        check(undo_enabled(), "%s: the import is in the history" % describe)
        shortcut(graph, redo=False)
        yield ("sleep", SETTLE_MS)
        now = before()
        check(now is True, "%s: Ctrl+Z over the graph restores the node (%r)" % (describe, now))
        shortcut(graph, redo=True)
        yield ("sleep", SETTLE_MS)
        now = after()
        check(now is True, "%s: Ctrl+Shift+Z re-applies the preset (%r)" % (describe, now))
        check(not redo_enabled(), "%s: nothing is left to redo" % describe)
        check(click_panel_button(target, "Undo"), "%s: clicked the panel's undo button" % describe)
        yield ("sleep", SETTLE_MS)
        now = before()
        check(now is True, "%s: the panel's undo button restores the node (%r)" % (describe, now))
        check(click_panel_button(target, "Redo"), "%s: clicked the panel's redo button" % describe)
        yield ("sleep", SETTLE_MS)
        now = after()
        check(now is True, "%s: the panel's redo button re-applies the preset (%r)" % (describe, now))

    # ---- sections --------------------------------------------------------------------------

    def blur_section():
        gt.section("blur")
        source = create(BLUR)
        link_source = create(BLUR)
        target = create(BLUR)
        src, lnk, tgt = source.getScriptName(), link_source.getScriptName(), target.getScriptName()
        yield ("sleep", SETTLE_MS)
        path = export_preset(src, "blur_default.nps")
        if path is None:
            return

        size = target.getParam("size")
        size.setValueAtTime(5.0, 1, 0)
        size.setValueAtTime(20.0, 10, 0)
        require(size.setExpression(EXPRESSION, False, 1), "set an expression on %s.size" % tgt)
        crop_default = default(tgt, "cropToFormat")
        link_source.getParam("cropToFormat").setValue(not crop_default)
        require(target.getParam("cropToFormat").slaveTo(link_source.getParam("cropToFormat"), 0, 0),
                "linked %s.cropToFormat to %s" % (tgt, lnk))
        filt = target.getParam("filter")
        filt.setValue((default(tgt, "filter") + 1) % filt.getNumOptions())
        target.getParam("disableNode").setValue(True)
        target.getParam("userTextArea").setValue(LABEL)
        yield ("sleep", SETTLE_MS)
        require(value(tgt, "cropToFormat") == (not crop_default), "the link carries %s's value" % lnk)
        watched = ("size", "cropToFormat", "filter", "disableNode", "userTextArea")
        modified = node_state(tgt, watched)

        import_preset(tgt, path)
        yield ("sleep", SETTLE_MS)
        for d in range(2):
            check(param(tgt, "size").getNumKeys(d) == 0, "the import removed size[%d]'s keys" % d)
            check(expression(tgt, "size", d) == "", "the import removed size[%d]'s expression" % d)
            check(value(tgt, "size", d) == default(tgt, "size", d), "size[%d] is at its default" % d)
        check(value(tgt, "cropToFormat") == crop_default, "the import removed the link to %s" % lnk)
        check(value(tgt, "filter") == default(tgt, "filter"), "filter is at its default")
        check(value(tgt, "disableNode") is True, "the import kept the disable switch")
        check(str(value(tgt, "userTextArea")) == LABEL, "the import kept the label")
        applied = node_state(tgt, watched)
        gt.shot("m114-presets-blur.png")

        def restored():
            now = node_state(tgt, watched)
            return True if now == modified else now

        def reapplied():
            now = node_state(tgt, watched)
            return True if now == applied else now

        yield from undo_redo_round(tgt, restored, reapplied, "blur")
        # The link is back, not just its value: the source's value still reaches the target.
        shortcut(root_graph(), redo=False)
        yield ("sleep", SETTLE_MS)
        link_source.getParam("cropToFormat").setValue(crop_default)
        yield ("sleep", SETTLE_MS)
        check(value(tgt, "cropToFormat") == crop_default, "after undo the target follows %s again" % lnk)

    def write_section():
        gt.section("write")
        source = create_writer("source.####.exr")
        target = create_writer("target.####.exr")
        src, tgt = source.getScriptName(), target.getScriptName()
        yield ("sleep", SETTLE_MS)
        path = export_preset(src, "write_default.nps")
        if path is None:
            return
        frame_range = target.getParam("frameRange")
        frame_range.setValue((default(tgt, "frameRange") + 1) % frame_range.getNumOptions())
        target_file = str(value(tgt, "filename"))
        watched = ("filename", "frameRange")
        modified = node_state(tgt, watched)

        import_preset(tgt, path)
        yield ("sleep", SETTLE_MS)
        check(str(value(tgt, "filename")) == target_file, "the import kept the Write's file (%r)" % value(tgt, "filename"))
        check(value(tgt, "frameRange") == default(tgt, "frameRange"), "frameRange is at its default")
        applied = node_state(tgt, watched)

        def restored():
            now = node_state(tgt, watched)
            return True if now == modified else now

        def reapplied():
            now = node_state(tgt, watched)
            return True if now == applied else now

        yield from undo_redo_round(tgt, restored, reapplied, "write")

    def user_param_section():
        gt.section("user parameters")
        source = create(BLUR)
        target = create(BLUR)
        src, tgt = source.getScriptName(), target.getScriptName()
        added = source.createDoubleParam(PRESET_USER, "Preset User")
        own = target.createDoubleParam(OWN_USER, "Own User")
        require(added is not None and own is not None, "created a user parameter on each Blur")
        added.setValue(6.0)
        own.setValue(2.0)
        source.refreshUserParamsGUI()
        target.refreshUserParamsGUI()
        yield ("sleep", SETTLE_MS)
        path = export_preset(src, "blur_user.nps")
        if path is None:
            return

        import_preset(tgt, path)
        yield ("sleep", SETTLE_MS)
        check(value(tgt, PRESET_USER) == 6.0, "the import added %s (%r)" % (PRESET_USER, value(tgt, PRESET_USER)))
        check(value(tgt, OWN_USER) == 2.0, "the import kept %s (%r)" % (OWN_USER, value(tgt, OWN_USER)))

        def restored():
            now = (param(tgt, PRESET_USER) is None, value(tgt, OWN_USER))
            return True if now == (True, 2.0) else now

        def reapplied():
            now = (value(tgt, PRESET_USER), value(tgt, OWN_USER))
            return True if now == (6.0, 2.0) else now

        yield from undo_redo_round(tgt, restored, reapplied, "user parameters")

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        viewers = [n for n in app.getChildren() if n.getPluginID() == "fr.inria.built-in.Viewer"]
        if not viewers:
            require(app.createNode("fr.inria.built-in.Viewer") is not None, "created a viewer for the empty project")

        yield from blur_section()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from write_section()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from user_param_section()

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
