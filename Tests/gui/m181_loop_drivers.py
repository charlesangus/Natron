"""Loop drivers from the GUI: Constant -> LoopBegin -> Blur -> LoopEnd.

Sections:
  - drive: Blur's size, set to 10, has "Drive from loop" in its right-click menu; choosing it
    drives both dimensions from loop.t, default to 10, and the knob's row shows the driver row instead
    of its fields; Ctrl+Z takes the driver off and shows the fields again; redo puts it back;
  - preview: the row shows the value at LoopEnd's Single Pass Iteration: the default at iteration 0 and 10
    at iteration 3 of 4;
  - table: LoopEnd's Driven Knobs table lists size.x and size.y; setting End to 7 in the first
    row changes the driver and the inline row shows it.
Screenshots: loopdriver_menu.png, loopdriver_row.png, loopdriver_row_iteration3.png,
loopdriver_table.png, loopdriver_table_edited.png in $NATRON_GUI_TEST_OUT.

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

try:
    from PySide6.QtCore import QMetaObject, QRect, Qt, QTimer
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTableWidget, QWidget

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
        gt.close_all_panels()
        app.selectNode(node, False)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")

    def knob_field(panel, script_name):
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and script_name in w.toolTip():
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
        open_panel(blur)
        ok = yield ("until", lambda: panel_of(blur) is not None, POLL_TIMEOUT_S)
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
        open_panel(end)
        ok = yield ("until", lambda: panel_of(end) is not None, POLL_TIMEOUT_S)
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

        open_panel(blur)
        ok = yield ("until", lambda: driver_row(panel_of(blur)) is not None, POLL_TIMEOUT_S)
        require(ok, "Blur's driver row shows again")
        ok = yield ("until", lambda: preview_text(panel_of(blur)).replace(" ", "") == "=7,10", POLL_TIMEOUT_S)
        check(ok, "the inline row shows the table's edit (got %r)" % preview_text(panel_of(blur)))
        trigger_edit("undo")
        ok = yield ("until", lambda: size.getDriverEnd(0) == 10, POLL_TIMEOUT_S)
        check(ok, "undo restores the end the table changed")

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
