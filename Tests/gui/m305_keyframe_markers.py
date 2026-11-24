"""GUI check that the timeline's keyframe markers always match the keys of the nodes with open panels.

Graph: Constant -> Blur A -> Blur B -> Viewer, project range 1-100. Both Blurs have a keyed size.
The script reads the markers through TimeLineGui::keyframeMarkerTimes() and, after every step,
compares them with the keys read back from Python for the Blurs whose panels are open:
  - panels: opening and closing each panel adds and drops that node's keys;
  - closed: keys edited while a panel is closed leave no marker, and no stale entry when it reopens;
  - shared-frame: a frame keyed on both nodes keeps its marker until both keys are gone;
  - panel: a key typed into a spin box and a Set Key / Remove Key from the knob's menu, with
    undo and redo of the typed key;
  - dope-sheet and curve-editor: deleting keys, then undo and redo, which add them back;
  - node-delete: a deleted node's markers go, and undoing the delete restores them;
  - python-loop: 10000 keys set from Python on an open node, the elapsed time as a note, all shown
    at once, removed with removeAnimation;
  - project-close: resetting the project clears everything.

Run through Tests/gui/run-gui-test.sh. Screenshots of the timeline and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 15.0
WATCHDOG_S = 280
SETTLE_MS = 300
KEYED_PARAM = "size"
BULK_KEYS = 10000
BULK_FIRST_FRAME = 1000

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMetaObject, QTimer, Q_ARG, Q_RETURN_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent
    from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QTreeWidget, QWidget
    from shiboken6 import isValid

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def timeline():
        visible = [w for w in gt.widgets_of("TimeLineGui") if w.isVisible()]
        return visible[0] if visible else None

    def markers():
        """The frames that show a marker, or None when there is no timeline to ask."""
        w = timeline()
        if w is None:
            return None
        return [int(t) for t in QMetaObject.invokeMethod(w, "keyframeMarkerTimes", Qt.DirectConnection,
                                                         Q_RETURN_ARG("QVariantList"))]

    def timeline_shot(name):
        """Grabs the screen area of the timeline, whose markers are too small to read in a full shot."""
        w = timeline()
        if w is None:
            report("NOTE no timeline for screenshot " + name)
            return
        QApplication.processEvents()
        origin = w.mapToGlobal(w.rect().topLeft())
        path = os.path.join(gt.OUT, name)
        if QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), w.width(), w.height()).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def trigger_edit(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return False
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is None or not act.text().lower().startswith("edit"):
                continue
            for a in menu.actions():
                if a.text().lower().startswith(prefix) and a.isEnabled():
                    a.trigger()
                    QApplication.processEvents()
                    return True
        return False

    def ancestor_of_class(widget, name):
        p = widget.parentWidget()
        while p is not None:
            if gt.class_name(p) == name:
                return p
            p = p.parentWidget()
        return None

    def make_tab_current(editor, word):
        pane = ancestor_of_class(editor, "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if word in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def panel_of(name):
        """The node's open settings panel, found by the label in its header's name field."""
        node = app.getNode(name)
        if node is None:
            return None
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for le in panel.findChildren(QLineEdit):
                    if gt.class_name(le) == "LineEdit" and le.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def open_panel(name):
        graphs = gt.widgets_of("NodeGraph")
        require(graphs, "found the node graph")
        app.setSelection([app.getNode(name)])
        QMetaObject.invokeMethod(graphs[0], "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(name) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the settings panel of " + name)
        app.clearSelection()

    def close_panel(name):
        panel = panel_of(name)
        require(panel is not None, "the settings panel of %s is open" % name)
        QMetaObject.invokeMethod(panel, "closePanel")
        ok = yield ("until", lambda: panel_of(name) is None, POLL_TIMEOUT_S)
        require(ok, "closed the settings panel of " + name)

    def key_param(name):
        return app.getNode(name).getParam(KEYED_PARAM)

    def key_times(name):
        node = app.getNode(name)
        if node is None:
            return set()
        param = node.getParam(KEYED_PARAM)
        times = set()
        for d in range(param.getNumDimensions()):
            for i in range(param.getNumKeys(d)):
                found, t = param.getKeyTime(i, d)
                if found:
                    times.add(int(t))
        return times

    def type_into_panel(name, value):
        """Types value into the first visible, enabled spin box of the node's panel and presses
        Return, which is how a user keys an animated knob at the current frame."""
        panel = panel_of(name)
        require(panel is not None, "found the settings panel of " + name)
        boxes = [w for w in panel.findChildren(QLineEdit)
                 if gt.class_name(w) == "SpinBox" and w.isEnabled() and w.isVisible()]
        require(boxes, "the settings panel of %s has a visible, enabled spin box" % name)
        boxes[0].setFocus()
        boxes[0].setText(str(value))
        send_key(boxes[0], Qt.Key_Return)

    def knob_menu(name, entry):
        """Opens the right-click menu of the knob's first field the way a right click does and
        triggers the entry with that exact text."""
        panel = panel_of(name)
        require(panel is not None, "found the settings panel of " + name)
        field = None
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and "<b>%s</b>" % KEYED_PARAM in w.toolTip():
                    field = w
                    break
            except RuntimeError:
                continue
        require(field is not None, "the panel of %s shows the field of %s" % (name, KEYED_PARAM))
        _held.append(field)
        seen = {"triggered": False}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            _held.append(popup)
            action = next((a for a in popup.actions() if a.text() == entry), None)
            if action is not None and action.isEnabled():
                action.trigger()
                seen["triggered"] = True
            popup.close()

        QTimer.singleShot(800, inspect)
        # The menu runs a nested loop until inspect() closes it.
        field.customContextMenuRequested.emit(field.rect().center())
        require(seen["triggered"], "the knob menu of %s offers %s" % (name, entry))

    def dope_sheet_view():
        views = gt.widgets_of("DopeSheetView")
        require(views, "found the dope sheet view")
        return views[0]

    def curve_editor_widget():
        for ce in gt.widgets_of("CurveEditor"):
            for w in gt.widgets_of("CurveWidget"):
                if isValid(w) and ce.isAncestorOf(w):
                    return w
        return None

    def select_curve_editor_rows(label):
        """Selects only the node's row and those under it, so Ctrl+A acts on that node's curves."""
        for ce in gt.widgets_of("CurveEditor"):
            for tree in ce.findChildren(QTreeWidget):
                if not (isValid(tree) and tree.objectName() == "tree"):
                    continue
                tree.expandAll()
                model = tree.model()
                selection = tree.selectionModel()
                selection.clear()

                def walk(parent):
                    for r in range(model.rowCount(parent)):
                        index = model.index(r, 0, parent)
                        yield index
                        for sub in walk(index):
                            yield sub

                for top in range(model.rowCount()):
                    top_index = model.index(top, 0)
                    if label in str(model.data(top_index)):
                        flags = QItemSelectionModel.Select | QItemSelectionModel.Rows
                        selection.select(top_index, flags)
                        for index in walk(top_index):
                            selection.select(index, flags)
                        QApplication.processEvents()
                        return True
        return False

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")) and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        win.activateWindow()
        win.raise_()
        gt.close_all_panels()

        gt.section("setup")
        app.getProjectParam("frameRange").set(1, 100)
        constant = app.createNode("Constant")
        require(constant is not None, "create a Constant", gt.plugin_problem("Constant"))

        def create_closed(plugin_id):
            props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
            node = app.createNode(plugin_id, -1, None, props)
            require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
            return node

        blur_a = create_closed("Blur")
        blur_b = create_closed("Blur")
        a = blur_a.getScriptName()
        b = blur_b.getScriptName()
        require(blur_a.connectInput(0, constant), "connect %s to the Constant" % a)
        require(blur_b.connectInput(0, blur_a), "connect %s to %s" % (b, a))
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        require(viewer.connectInput(0, blur_b), "the Viewer shows " + b)
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", lambda: gt.viewer_up() and timeline() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer and its timeline are visible")

        watched = (a, b)

        def expected():
            times = set()
            for name in watched:
                if panel_of(name) is not None:
                    times |= key_times(name)
            return sorted(times)

        def expect(label, frames=None):
            """Markers match the open panels' keys, and the literal frames when the step knows them."""
            ok = yield ("until", lambda: markers() == expected() and (frames is None or markers() == frames),
                        POLL_TIMEOUT_S)
            # A late refresh must not bring back frames the timeline has dropped.
            yield ("sleep", SETTLE_MS)
            got = markers()
            want = expected()
            check(ok and got == want and (frames is None or got == frames),
                  "%s: markers %r match the open panels' keys %r%s" %
                  (label, got, want, "" if frames is None else " and %r" % (frames,)))

        key_param(a).setValueAtTime(2.0, 5, 0)
        key_param(a).setValueAtTime(4.0, 15, 0)
        key_param(b).setValueAtTime(3.0, 15, 0)
        key_param(b).setValueAtTime(5.0, 25, 0)
        # The Viewer just started rendering, and keys set during a render reach the curve Python reads after it.
        ok = yield ("until", lambda: key_times(a) == {5, 15} and key_times(b) == {15, 25}, POLL_TIMEOUT_S)
        require(ok, "the Blurs have their keys", "%r, %r" % (key_times(a), key_times(b)))
        yield ("sleep", SETTLE_MS)

        gt.section("panels")
        yield from expect("every panel closed", [])
        timeline_shot("m305-timeline-all-closed.png")
        yield from open_panel(a)
        yield from expect("A open", [5, 15])
        yield from open_panel(b)
        yield from expect("A and B open", [5, 15, 25])
        timeline_shot("m305-timeline-both-open.png")
        yield from close_panel(a)
        yield from expect("B open", [15, 25])
        yield from close_panel(b)
        yield from expect("both closed again", [])

        gt.section("closed")
        key_param(a).setValueAtTime(6.0, 35, 0)
        yield from expect("key added while closed", [])
        key_param(a).deleteValueAtTime(5, 0)
        yield from expect("key deleted while closed", [])
        yield from open_panel(a)
        yield from expect("A reopened after edits", [15, 35])
        yield from close_panel(a)
        key_param(a).deleteValueAtTime(35, 0)
        key_param(a).setValueAtTime(2.0, 5, 0)
        yield from open_panel(a)
        yield from expect("A reopened after a hide and more edits", [5, 15])

        gt.section("python")
        key_param(a).setValueAtTime(7.0, 45, 0)
        yield from expect("Python set a key", [5, 15, 45])
        key_param(a).deleteValueAtTime(45, 0)
        yield from expect("Python deleted a key", [5, 15])

        gt.section("shared-frame")
        yield from open_panel(b)
        yield from expect("frame 15 keyed on both", [5, 15, 25])
        key_param(b).deleteValueAtTime(15, 0)
        yield from expect("B dropped frame 15, A still has it", [5, 15, 25])
        key_param(a).deleteValueAtTime(15, 0)
        yield from expect("both dropped frame 15", [5, 25])
        key_param(a).setValueAtTime(4.0, 15, 0)
        key_param(b).setValueAtTime(3.0, 15, 0)
        yield from expect("frame 15 keyed on both again", [5, 15, 25])

        gt.section("panel")
        py_viewer.seek(40)
        yield ("sleep", SETTLE_MS)
        before = key_times(a)
        type_into_panel(a, 8)
        ok = yield ("until", lambda: 40 in key_times(a), POLL_TIMEOUT_S)
        require(ok, "typing into the panel keys the animated size at frame 40", repr(key_times(a)))
        yield from expect("typed key", [5, 15, 25, 40])
        timeline_shot("m305-timeline-typed-key.png")
        require(trigger_edit("undo"), "undo the typed key")
        ok = yield ("until", lambda: key_times(a) == before, POLL_TIMEOUT_S)
        require(ok, "undo removed the typed key", repr(key_times(a)))
        yield from expect("typed key undone", [5, 15, 25])
        timeline_shot("m305-timeline-typed-key-undone.png")
        require(trigger_edit("redo"), "redo the typed key")
        ok = yield ("until", lambda: 40 in key_times(a), POLL_TIMEOUT_S)
        require(ok, "redo put the typed key back", repr(key_times(a)))
        yield from expect("typed key redone", [5, 15, 25, 40])
        require(trigger_edit("undo"), "undo the typed key again")
        ok = yield ("until", lambda: key_times(a) == before, POLL_TIMEOUT_S)
        require(ok, "undo removed the typed key again", repr(key_times(a)))

        py_viewer.seek(50)
        yield ("sleep", SETTLE_MS)
        # The size field shows its two dimensions as one until they differ, so the menu offers only the whole knob.
        knob_menu(a, "Set Key (all dimensions)")
        ok = yield ("until", lambda: 50 in key_times(a), POLL_TIMEOUT_S)
        require(ok, "Set Key keys the size at frame 50", repr(key_times(a)))
        yield from expect("Set Key", [5, 15, 25, 50])
        knob_menu(a, "Remove Key (all dimensions)")
        ok = yield ("until", lambda: 50 not in key_times(a), POLL_TIMEOUT_S)
        require(ok, "Remove Key removes the key at frame 50", repr(key_times(a)))
        yield from expect("Remove Key", [5, 15, 25])

        gt.section("dope-sheet")
        # DopeSheetEditor has no Q_OBJECT, so its class name never reaches Python; its view does.
        require(make_tab_current(dope_sheet_view(), "dope"), "brought the dope sheet's tab to the front")
        yield ("sleep", SETTLE_MS)
        view = dope_sheet_view()
        QMetaObject.invokeMethod(view, "onSelectedAllTriggered")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        QMetaObject.invokeMethod(view, "deleteSelectedKeyframes")
        ok = yield ("until", lambda: not key_times(a) and not key_times(b), POLL_TIMEOUT_S)
        check(ok, "deleting every key in the dope sheet empties both Blurs (%r, %r)" % (key_times(a), key_times(b)))
        yield from expect("dope sheet deleted every key", [])
        timeline_shot("m305-timeline-dope-sheet-deleted.png")
        require(trigger_edit("undo"), "undo the dope sheet delete")
        ok = yield ("until", lambda: key_times(a) and key_times(b), POLL_TIMEOUT_S)
        require(ok, "undo gave the Blurs their keys back")
        yield from expect("dope sheet delete undone", [5, 15, 25])
        timeline_shot("m305-timeline-dope-sheet-undone.png")
        require(trigger_edit("redo"), "redo the dope sheet delete")
        ok = yield ("until", lambda: not key_times(a) and not key_times(b), POLL_TIMEOUT_S)
        check(ok, "redo deleted the keys again")
        yield from expect("dope sheet delete redone", [])
        require(trigger_edit("undo"), "undo the dope sheet delete again")
        ok = yield ("until", lambda: key_times(a) and key_times(b), POLL_TIMEOUT_S)
        require(ok, "undo gave the Blurs their keys back again")
        yield from expect("dope sheet delete undone again", [5, 15, 25])

        gt.section("curve-editor")
        editors = gt.widgets_of("CurveEditor")
        require(editors, "found the curve editor")
        require(make_tab_current(editors[0], "curve"), "brought the curve editor's tab to the front")
        yield ("sleep", SETTLE_MS)
        require(select_curve_editor_rows(app.getNode(b).getLabel()), "selected B's rows in the curve editor")
        yield ("sleep", SETTLE_MS)
        curve_widget = curve_editor_widget()
        require(curve_widget is not None, "found the curve widget")
        send_key(curve_widget, Qt.Key_A, Qt.ControlModifier)
        send_key(curve_widget, Qt.Key_Backspace)
        ok = yield ("until", lambda: not key_times(b), POLL_TIMEOUT_S)
        check(ok, "deleting B's keys in the curve editor empties it (%r)" % (key_times(b),))
        check(key_times(a) == {5, 15}, "the curve editor edit left A's keys alone (%r)" % (key_times(a),))
        yield from expect("curve editor deleted B's keys", [5, 15])
        timeline_shot("m305-timeline-curve-editor-deleted.png")
        require(trigger_edit("undo"), "undo the curve editor delete")
        ok = yield ("until", lambda: key_times(b) == {15, 25}, POLL_TIMEOUT_S)
        require(ok, "undo gave B its keys back", repr(key_times(b)))
        yield from expect("curve editor delete undone", [5, 15, 25])
        timeline_shot("m305-timeline-curve-editor-undone.png")

        gt.section("node-delete")
        graphs = gt.widgets_of("NodeGraph")
        require(graphs, "found the node graph")
        require(make_tab_current(graphs[0], "graph"), "brought the node graph's tab to the front")
        yield ("sleep", SETTLE_MS)
        app.setSelection([app.getNode(b)])
        QApplication.processEvents()
        send_key(gt.widgets_of("NodeGraph")[0], Qt.Key_Backspace)
        ok = yield ("until", lambda: app.getNode(b) is None, POLL_TIMEOUT_S)
        require(ok, "Backspace deleted " + b)
        yield from expect("B deleted", [5, 15])
        timeline_shot("m305-timeline-node-deleted.png")
        require(trigger_edit("undo"), "undo the node delete")
        ok = yield ("until", lambda: app.getNode(b) is not None, POLL_TIMEOUT_S)
        require(ok, "undo restored " + b)
        ok = yield ("until", lambda: panel_of(b) is not None, POLL_TIMEOUT_S)
        check(ok, "undo reopened the panel of " + b)
        yield from expect("B restored", [5, 15, 25])
        timeline_shot("m305-timeline-node-restored.png")

        gt.section("python-loop")
        app.clearSelection()
        bulk = create_closed("Blur")
        bulk_name = bulk.getScriptName()
        yield from open_panel(bulk_name)
        bulk_param = bulk.getParam(KEYED_PARAM)
        started = time.time()
        for i in range(BULK_KEYS):
            bulk_param.setValueAtTime(float(i % 7), BULK_FIRST_FRAME + i, 0)
        report("NOTE set %d keys from Python in %.2f s" % (BULK_KEYS, time.time() - started))
        watched = (a, b, bulk_name)
        started = time.time()
        got = markers()
        report("NOTE read %d markers in %.3f s" % (len(got or []), time.time() - started))
        check(len(got or []) == BULK_KEYS + 3,
              "all %d keys show as markers right after the loop (%d)" % (BULK_KEYS, len(got or [])))
        yield from expect("after the Python loop")
        bulk_param.removeAnimation(0)
        yield from expect("after removeAnimation", [5, 15, 25])
        yield from close_panel(bulk_name)
        watched = (a, b)

        gt.section("project-close")
        path = os.path.join(gt.OUT, "m305_keyframe_markers.ntpz")
        require(app.saveProjectAs(path), "save the project to " + path)
        require(app.resetProject(), "reset the project")
        ok = yield ("until", lambda: markers() is None, POLL_TIMEOUT_S)
        check(ok, "resetting the project takes the Viewer and its timeline away (%r)" % (markers(),))
        require(app.createNode("Viewer") is not None, "a new Viewer in the reset project")
        ok = yield ("until", lambda: markers() == [], POLL_TIMEOUT_S)
        check(ok, "the new Viewer's timeline has no markers left from the old project (%r)" % (markers(),))
        gt.shot("m305-after-project-reset.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
