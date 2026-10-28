"""Loop drivers from the GUI: Constant -> LoopBegin -> Blur -> LoopEnd.

Sections:
  - drive: Blur's size, set to 10, has "Drive from loop" in its right-click menu; choosing it
    drives both dimensions from loop.t, default to 10, and the knob's row shows the driver row instead
    of its fields; Ctrl+Z takes the driver off and shows the fields again; redo puts it back;
  - preview: the row shows the value at LoopEnd's Single Pass Iteration: the default at iteration 0 and 10
    at iteration 3 of 4;
  - table: LoopEnd's Driven Knobs table lists size.x and size.y; setting End to 7 in the first
    row changes the driver and the inline row shows it.
  - keys: at frame 1 the row's Start menu's Set Key keys Start; at frame 10 typing 4 into Start
    keys it again (auto-key); the Start field shows keyed at a key's frame and animated between;
    undo takes the second key off.
  - editors: with one Start key on both dimensions, the curve editor and the dope sheet show a
    Driver Start row under size and hide Driver End and the per-dimension rows; Ctrl+A, Backspace
    in either deletes the keys and hides the row, and undo brings both back.
Screenshots: loopdriver_menu.png, loopdriver_row.png, loopdriver_row_iteration3.png,
loopdriver_table.png, loopdriver_table_edited.png, loopdriver_start_key_menu.png,
loopdriver_row_keyed.png, loopdriver_curve_editor.png, loopdriver_dope_sheet.png in
$NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m181_loop_drivers.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
BLUR_ID = "net.sf.cimg.CImgBlur"
BEGIN_ID = "fr.natron.LoopBegin"
END_ID = "fr.natron.LoopEnd"
DRIVE_ITEM = "Drive from loop"
SET_KEY_ITEM = "Set Key"
# SpinBox's animation property: 0 constant, 1 animated, 2 on a key.
ANIMATED = 1
ON_KEY = 2

try:
    from PySide6.QtCore import Q_ARG, QEvent, QItemSelectionModel, QMetaObject, QModelIndex, QRect, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QKeyEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTableWidget, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report, require

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        return guis[0] if guis else None

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
                        _held.append(panel)
                        return panel
                except RuntimeError:
                    continue
        return None

    def open_panel(node):
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            app.selectNode(node, True)
            QApplication.processEvents()
            view = node_graph_view()
            invoked = view is not None and QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            if not invoked:
                report("NOTE showSelectedNodeSettingsPanel was not invoked for %s (attempt %d)"
                       % (node.getScriptName(), attempt + 1))
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def knob_field(panel, script_name):
        """The first-dimension field of the knob named script_name: its tool tip opens with the script
        name in bold. A bare substring test also matches other knobs whose help text mentions it."""
        tag = "<b>%s</b>" % script_name
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and tag in w.toolTip()[:60]:
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

    def child(panel, name):
        if panel is None:
            return None
        for w in panel.findChildren(QWidget, name):
            try:
                if w.isVisible():
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

    def driver_row(panel):
        return child(panel, "KnobDriverRow")

    def preview_text(panel):
        row = driver_row(panel)
        label = child(row, "driverPreview") if row is not None else None
        return label.text() if label is not None else ""

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def right_click(field, trigger, shot_name):
        """Opens field's right-click menu and triggers the item named trigger; returns
        {"entries": [...], "item": (enabled, tooltip) or None}."""
        seen = {"entries": None, "item": None}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the right-click menu is open")
                return
            _held.append(popup)
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            action = next((a for a in popup.actions() if a.text() == trigger), None)
            if action is not None:
                seen["item"] = (action.isEnabled(), action.toolTip())
            gt.shot(shot_name)
            popup.close()
            if action is not None and action.isEnabled():
                action.trigger()

        QTimer.singleShot(800, inspect)
        field.customContextMenuRequested.emit(field.rect().center())
        return seen

    def edit_menu_action(prefix):
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

    def trigger_edit(prefix):
        a = edit_menu_action(prefix)
        if check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
            a.trigger()
        QApplication.processEvents()

    def commit_spin(spin, text):
        """Types text into a SpinBox and presses Return, which commits it as a user edit."""
        spin.setFocus()
        spin.selectAll()
        if QTest is not None:
            QTest.keyClicks(spin, text)
            QTest.keyClick(spin, Qt.Key_Return)
        else:
            spin.setText(text)
            spin.returnPressed.emit()
        QApplication.processEvents()

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def make_tab_current(widget, word):
        """Brings the tab whose title contains word, in widget's pane, to the front."""
        w = widget
        while w is not None and gt.class_name(w) != "TabWidget":
            w = w.parentWidget()
        if w is None:
            return False
        _held.append(w)
        bars = [b for b in w.findChildren(QWidget) if gt.class_name(b) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if word in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(w, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def curve_editor():
        editors = gt.widgets_of("CurveEditor")
        visible = [e for e in editors if e.isVisible()]
        editor = (visible or editors or [None])[0]
        if editor is not None:
            _held.append(editor)
        return editor

    def curve_editor_tree():
        editor = curve_editor()
        trees = editor.findChildren(QTreeWidget) if editor is not None else []
        trees = [t for t in trees if t.objectName() == "tree"] or trees
        return trees[0] if trees else None

    def curve_widget():
        editor = curve_editor()
        widgets = [w for w in gt.widgets_of("CurveWidget") if editor is not None and editor.isAncestorOf(w)]
        return widgets[0] if widgets else None

    def hierarchy_view():
        views = [v for v in gt.widgets_of("HierarchyView") if v.isVisible()] or gt.widgets_of("HierarchyView")
        return views[0] if views else None

    def dope_sheet_view():
        views = [v for v in gt.widgets_of("DopeSheetView") if v.isVisible()] or gt.widgets_of("DopeSheetView")
        return views[0] if views else None

    def driver_rows(tree, node_label):
        """{row text: hidden} for the Driver rows under node_label's top row, by a QModelIndex walk;
        a row counts as hidden when it or one of its parents is."""
        rows = {}
        if tree is None:
            return rows
        _held.append(tree)
        model = tree.model()

        def walk(parent, hidden):
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                h = hidden or tree.isRowHidden(r, parent)
                text = str(model.data(idx))
                if text.startswith("Driver"):
                    rows[text] = h
                walk(idx, h)

        for top in range(model.rowCount(QModelIndex())):
            top_idx = model.index(top, 0, QModelIndex())
            if str(model.data(top_idx)) == node_label:
                walk(top_idx, tree.isRowHidden(top, QModelIndex()))
        return rows

    def select_row(tree, node_label, text):
        """Selects only the row named text under node_label's top row."""
        if tree is None:
            return False
        model = tree.model()
        sel = tree.selectionModel()

        def find(parent):
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                if str(model.data(idx)) == text:
                    return idx
                sub = find(idx)
                if sub is not None:
                    return sub
            return None

        for top in range(model.rowCount(QModelIndex())):
            top_idx = model.index(top, 0, QModelIndex())
            if str(model.data(top_idx)) != node_label:
                continue
            idx = find(top_idx)
            if idx is not None:
                tree.expandAll()
                sel.clear()
                sel.select(idx, QItemSelectionModel.Select | QItemSelectionModel.Rows)
                QApplication.processEvents()
                return True
        return False

    def one_start_key_rows_ok(rows):
        """The layout for one shared Start key: Driver Start shown, every other Driver row hidden."""
        per_dim = [k for k in rows if k.startswith("Driver Start.")]
        return (rows.get("Driver Start") is False and len(per_dim) == 2
                and all(rows[k] for k in per_dim)
                and all(h for k, h in rows.items() if k.startswith("Driver End")))

    def start_key_counts(size):
        return (size.getDriverStartKeyCount(0), size.getDriverStartKeyCount(1))

    def steps():
        gt.section("setup")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        constant = app.createNode(CONSTANT_ID)
        begin = app.createNode(BEGIN_ID)
        blur = app.createNode(BLUR_ID)
        end = app.createNode(END_ID)
        for plugin_id, node in ((CONSTANT_ID, constant), (BEGIN_ID, begin), (BLUR_ID, blur), (END_ID, end)):
            require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        for i, node in enumerate((constant, begin, blur, end)):
            node.setPosition(0, i * 120)
        require(begin.connectInput(0, constant), "connect the LoopBegin")
        require(blur.connectInput(0, begin), "connect the Blur")
        require(end.connectInput(0, blur), "connect the LoopEnd")
        size = blur.getParam("size")
        require(size is not None, "Blur has a size knob")
        size.setValue(10, 0)
        size.setValue(10, 1)

        gt.section("drive")
        ok = yield from open_panel(blur)
        require(ok, "open Blur's settings panel")
        yield ("sleep", 600)
        field = knob_field(panel_of(blur), "size")
        require(field is not None, "found the size field")
        check(driver_row(panel_of(blur)) is None, "an undriven knob shows no driver row")
        seen = right_click(field, DRIVE_ITEM, "loopdriver_menu.png")
        ok = yield ("until", lambda: seen["entries"] is not None, POLL_TIMEOUT_S)
        require(ok, "the size menu opened")
        check(seen["item"] is not None and seen["item"][0], "the menu has an enabled \"%s\" (got %r)" % (DRIVE_ITEM, seen["entries"]))
        ok = yield ("until", lambda: size.hasDriver(0) and size.hasDriver(1), POLL_TIMEOUT_S)
        require(ok, "Drive from loop drives both dimensions")
        check(size.getDriverVariable(0) == "loop.t", "the driver reads loop.t (got %s)" % size.getDriverVariable(0))
        default = size.getDefaultValue(0)
        check(size.getDriverStart(0) == default and size.getDriverEnd(0) == 10,
              "the driver goes from the default %g to the value 10 (got %s to %s)" % (default, size.getDriverStart(0), size.getDriverEnd(0)))
        ok = yield ("until", lambda: driver_row(panel_of(blur)) is not None, POLL_TIMEOUT_S)
        check(ok, "the driver row shows")
        check(knob_field(panel_of(blur), "size") is None, "the size fields are hidden while driven")
        if ok:
            screen_crop("loopdriver_row", panel_of(blur))

        yield ("sleep", 300)
        trigger_edit("undo")
        ok = yield ("until", lambda: not size.hasDriver(0) and not size.hasDriver(1), POLL_TIMEOUT_S)
        check(ok, "undo takes the driver off")
        ok = yield ("until", lambda: driver_row(panel_of(blur)) is None and knob_field(panel_of(blur), "size") is not None, POLL_TIMEOUT_S)
        check(ok, "after undo the size fields show again")
        trigger_edit("redo")
        ok = yield ("until", lambda: size.hasDriver(0) and size.hasDriver(1), POLL_TIMEOUT_S)
        check(ok, "redo drives the knob again")
        ok = yield ("until", lambda: driver_row(panel_of(blur)) is not None, POLL_TIMEOUT_S)
        check(ok, "after redo the driver row shows again")

        gt.section("preview")
        iteration = end.getParam("singlePassIteration")
        require(iteration is not None, "LoopEnd has singlePassIteration")
        iteration.setValue(0)
        first = "=%g,%g" % (default, default)
        ok = yield ("until", lambda: preview_text(panel_of(blur)).replace(" ", "") == first, POLL_TIMEOUT_S)
        check(ok, "at iteration 0 the row previews the default (got %r)" % preview_text(panel_of(blur)))
        iteration.setValue(3)
        ok = yield ("until", lambda: preview_text(panel_of(blur)).replace(" ", "") == "=10,10", POLL_TIMEOUT_S)
        check(ok, "at iteration 3 of 4 the row previews 10 (got %r)" % preview_text(panel_of(blur)))
        if driver_row(panel_of(blur)) is not None:
            screen_crop("loopdriver_row_iteration3", panel_of(blur))

        gt.section("table")
        ok = yield from open_panel(end)
        require(ok, "open LoopEnd's settings panel")
        yield ("sleep", 600)
        table = child(panel_of(end), "LoopDriverTable")
        require(table is not None, "LoopEnd's panel shows the driven-knob table")
        ok = yield ("until", lambda: child(panel_of(end), "LoopDriverTable").rowCount() == 2, POLL_TIMEOUT_S)
        table = child(panel_of(end), "LoopDriverTable")
        check(ok, "the table lists size's two dimensions (got %d rows)" % table.rowCount())
        params = sorted(table.item(r, 1).text() for r in range(table.rowCount()) if table.item(r, 1) is not None)
        check(params == ["size.x", "size.y"], "the rows are size.x and size.y (got %r)" % params)
        values = [table.item(r, 6).text() for r in range(table.rowCount()) if table.item(r, 6) is not None]
        check(values == ["10", "10"], "the Value column shows the value at iteration 3 (got %r)" % values)
        screen_crop("loopdriver_table", panel_of(end))

        row = next(r for r in range(table.rowCount()) if table.item(r, 1).text() == "size.x")
        dim = 0
        spin = table.cellWidget(row, 4)
        require(spin is not None, "the End cell holds a spin box")
        _held.append(spin)
        commit_spin(spin, "7")
        ok = yield ("until", lambda: size.getDriverEnd(dim) == 7, POLL_TIMEOUT_S)
        check(ok, "editing End in the table sets the driver's end (got %s)" % size.getDriverEnd(dim))
        check(size.getDriverEnd(1) == 10, "the other dimension keeps its end")
        yield ("sleep", 300)
        screen_crop("loopdriver_table_edited", panel_of(end))

        yield from open_panel(blur)
        ok = yield ("until", lambda: driver_row(panel_of(blur)) is not None, POLL_TIMEOUT_S)
        require(ok, "Blur's driver row shows again")
        ok = yield ("until", lambda: preview_text(panel_of(blur)).replace(" ", "") == "=7,10", POLL_TIMEOUT_S)
        check(ok, "the inline row shows the table's edit (got %r)" % preview_text(panel_of(blur)))
        trigger_edit("undo")
        ok = yield ("until", lambda: size.getDriverEnd(0) == 10, POLL_TIMEOUT_S)
        check(ok, "undo restores the end the table changed")

        gt.section("keys")

        def start_spin():
            return child(driver_row(panel_of(blur)), "driverStart")

        def start_animation():
            spin = start_spin()
            return spin.property("animation") if spin is not None else None

        require(start_spin() is not None, "the driver row has a Start field")
        check(start_animation() == 0, "an unkeyed Start shows constant (got %r)" % start_animation())
        app.seek(1)
        ok = yield ("until", lambda: app.timelineGetTime() == 1, POLL_TIMEOUT_S)
        require(ok, "the timeline is at frame 1")
        seen = right_click(start_spin(), SET_KEY_ITEM, "loopdriver_start_key_menu.png")
        ok = yield ("until", lambda: seen["entries"] is not None, POLL_TIMEOUT_S)
        require(ok, "the Start menu opened")
        check(seen["item"] is not None and seen["item"][0], "the Start menu has an enabled \"%s\" (got %r)" % (SET_KEY_ITEM, seen["entries"]))
        ok = yield ("until", lambda: size.getDriverStartKeyCount(0) == 1 and size.getDriverStartKeyCount(1) == 1, POLL_TIMEOUT_S)
        check(ok, "Set Key keys Start of both dimensions (got %d, %d)" % (size.getDriverStartKeyCount(0), size.getDriverStartKeyCount(1)))
        ok = yield ("until", lambda: start_animation() == ON_KEY, POLL_TIMEOUT_S)
        check(ok, "at the key's frame Start shows keyed (got %r)" % start_animation())

        app.seek(10)
        ok = yield ("until", lambda: start_animation() == ANIMATED, POLL_TIMEOUT_S)
        check(ok, "off a key Start shows animated (got %r)" % start_animation())
        commit_spin(start_spin(), "4")
        ok = yield ("until", lambda: size.getDriverStartKeyCount(0) == 2, POLL_TIMEOUT_S)
        check(ok, "typing a value into an animated Start keys it (got %d keys)" % size.getDriverStartKeyCount(0))
        check(size.getDriverStartAtTime(10, 0) == 4 and size.getDriverStartAtTime(1, 0) == default,
              "Start is 4 at frame 10 and %g at frame 1 (got %s, %s)" % (default, size.getDriverStartAtTime(10, 0), size.getDriverStartAtTime(1, 0)))
        ok = yield ("until", lambda: start_animation() == ON_KEY, POLL_TIMEOUT_S)
        check(ok, "at the new key's frame Start shows keyed (got %r)" % start_animation())
        yield ("sleep", 300)
        screen_crop("loopdriver_row_keyed", panel_of(blur))

        app.seek(5)
        ok = yield ("until", lambda: start_animation() == ANIMATED, POLL_TIMEOUT_S)
        check(ok, "between the keys Start shows animated (got %r)" % start_animation())
        trigger_edit("undo")
        ok = yield ("until", lambda: size.getDriverStartKeyCount(0) == 1, POLL_TIMEOUT_S)
        check(ok, "undo takes the second key off (got %d keys)" % size.getDriverStartKeyCount(0))

        gt.section("editors")
        ok = yield ("until", lambda: start_key_counts(size) == (1, 1), POLL_TIMEOUT_S)
        require(ok, "Start has one key on each dimension (got %r)" % (start_key_counts(size),))
        check(size.getDriverEndKeyCount(0) == 0 and size.getDriverEndKeyCount(1) == 0, "End has no keys")
        blur_label = blur.getLabel()

        require(curve_editor() is not None, "found the curve editor")
        check(make_tab_current(curve_editor(), "curve"), "brought the curve editor's tab to the front")
        ok = yield ("until", lambda: one_start_key_rows_ok(driver_rows(curve_editor_tree(), blur_label)), POLL_TIMEOUT_S)
        check(ok, "the curve editor shows Driver Start, with one key on both dimensions, and hides the other driver rows (got %r)"
              % driver_rows(curve_editor_tree(), blur_label))
        check(select_row(curve_editor_tree(), blur_label, "Driver Start"), "selected the curve editor's Driver Start row")
        yield ("sleep", 400)
        gt.shot("loopdriver_curve_editor.png")
        require(curve_widget() is not None, "found the curve widget")
        send_key(curve_widget(), Qt.Key_A, Qt.ControlModifier)
        send_key(curve_widget(), Qt.Key_Backspace)
        ok = yield ("until", lambda: start_key_counts(size) == (0, 0), POLL_TIMEOUT_S)
        check(ok, "Ctrl+A, Backspace in the curve editor deletes the Start key of both dimensions (got %r)" % (start_key_counts(size),))
        ok = yield ("until", lambda: driver_rows(curve_editor_tree(), blur_label).get("Driver Start") is True, POLL_TIMEOUT_S)
        check(ok, "without keys the curve editor hides Driver Start")
        trigger_edit("undo")
        ok = yield ("until", lambda: start_key_counts(size) == (1, 1), POLL_TIMEOUT_S)
        check(ok, "undo puts the curve editor's deleted keys back (got %r)" % (start_key_counts(size),))
        check(size.getDriverStartAtTime(1, 0) == default, "the restored key holds %g (got %s)" % (default, size.getDriverStartAtTime(1, 0)))
        ok = yield ("until", lambda: one_start_key_rows_ok(driver_rows(curve_editor_tree(), blur_label)), POLL_TIMEOUT_S)
        check(ok, "after undo the curve editor shows Driver Start again")

        require(hierarchy_view() is not None and dope_sheet_view() is not None, "found the dope sheet")
        check(make_tab_current(hierarchy_view(), "dope"), "brought the dope sheet's tab to the front")
        ok = yield ("until", lambda: one_start_key_rows_ok(driver_rows(hierarchy_view(), blur_label)), POLL_TIMEOUT_S)
        check(ok, "the dope sheet shows Driver Start and hides the other driver rows (got %r)"
              % driver_rows(hierarchy_view(), blur_label))
        hierarchy_view().expandAll()
        yield ("sleep", 400)
        gt.shot("loopdriver_dope_sheet.png")
        send_key(dope_sheet_view(), Qt.Key_A, Qt.ControlModifier)
        send_key(dope_sheet_view(), Qt.Key_Backspace)
        ok = yield ("until", lambda: start_key_counts(size) == (0, 0), POLL_TIMEOUT_S)
        check(ok, "Ctrl+A, Backspace in the dope sheet deletes the Start key of both dimensions (got %r)" % (start_key_counts(size),))
        ok = yield ("until", lambda: driver_rows(hierarchy_view(), blur_label).get("Driver Start") is True, POLL_TIMEOUT_S)
        check(ok, "without keys the dope sheet hides Driver Start")
        trigger_edit("undo")
        ok = yield ("until", lambda: start_key_counts(size) == (1, 1), POLL_TIMEOUT_S)
        check(ok, "undo puts the dope sheet's deleted keys back (got %r)" % (start_key_counts(size),))
        ok = yield ("until", lambda: one_start_key_rows_ok(driver_rows(hierarchy_view(), blur_label)), POLL_TIMEOUT_S)
        check(ok, "after undo the dope sheet shows Driver Start again")

        for node in (end, blur, begin, constant):
            node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
