"""Make I-Node and Make Base Node from the GUI.

Sections:
  - convert: a lone Blur (size 6) between a Constant and a Grade becomes an IBlur under the same
    name, with its size and both connections; undo brings the Blur back; Make Base Node on the
    IBlur, whose Map input is empty so no confirmation shows, makes it a Blur again.
  - menu: the Edit menu's Make Base Node is disabled with a reason on a Grade, Make I-Node enabled.
  - wrap: a lone Grade, which has no I-variant, is wrapped: LoopBegin -> Grade -> LoopEnd with the
    LoopEnd in Map Blend mode with 8 iterations, selected, and its panel open on the empty Driven
    Knobs table; undo takes the loop out and reconnects the Grade.
  - warning: wrapping a Transform -> Grade selection shows a status bar warning naming the
    Transform; undo takes the loop out.
Screenshots: makeinode_converted.png, makeinode_wrapped.png, makeinode_warning.png in
$NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m188_make_inode.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 240
CONSTANT_ID = "Constant"
BLUR_ID = "Blur"
IBLUR_ID = "IBlur"
GRADE_ID = "Grade"
TRANSFORM_ID = "Transform"
BEGIN_ID = "LoopBegin"
END_ID = "LoopEnd"
ITERATIONS = 8
MAP_BLEND = "mapBlend"

try:
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        return guis[0] if guis else None

    def node_graph_view():
        visible = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return visible[0] if visible else None

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

    def driver_table_rows(node):
        """The row count of the Driven Knobs table on node's open panel, None without one."""
        panel = panel_of(node)
        if panel is None:
            return None
        for w in panel.findChildren(QWidget, "LoopDriverTable"):
            try:
                if w.isVisible():
                    _held.append(w)
                    return w.rowCount()
            except RuntimeError:
                continue
        return None

    def select_only(nodes):
        app.clearSelection()
        for node in nodes:
            app.selectNode(node, False)
        QApplication.processEvents()

    def invoke(slot):
        view = node_graph_view()
        ok = view is not None and QMetaObject.invokeMethod(view, slot)
        check(ok, "the node graph runs " + slot)
        QApplication.processEvents()

    def edit_menu():
        win = main_gui()
        if win is None or win.menuBar() is None:
            return None
        _held.append(win)
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                return menu
        return None

    def edit_menu_action(prefix):
        menu = edit_menu()
        if menu is None:
            return None
        for a in menu.actions():
            if a.text().replace("&", "").lower().startswith(prefix):
                return a
        return None

    def trigger_edit(prefix):
        a = edit_menu_action(prefix)
        if check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
            a.trigger()
        QApplication.processEvents()

    def status_message():
        win = main_gui()
        return win.statusBar().currentMessage() if win is not None and win.statusBar() is not None else ""

    def plugin_of(name):
        node = app.getNode(name)
        return node.getPluginID() if node is not None else None

    def input_name(node, i):
        inp = node.getInput(i) if node is not None else None
        return inp.getScriptName() if inp is not None else None

    def loop_end_reading(name):
        """The LoopEnd node whose Source input reads the node called name, or None."""
        for node in app.getChildren():
            if node.getPluginID() == END_ID and input_name(node, 0) == name:
                return node
        return None

    def steps():
        gt.section("setup")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        constant = app.createNode(CONSTANT_ID)
        blur = app.createNode(BLUR_ID)
        grade = app.createNode(GRADE_ID)
        for plugin_id, node in ((CONSTANT_ID, constant), (BLUR_ID, blur), (GRADE_ID, grade)):
            require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        for i, node in enumerate((constant, blur, grade)):
            node.setPosition(0, i * 120)
        require(blur.connectInput(0, constant), "connect the Blur")
        require(grade.connectInput(0, blur), "connect the Grade")
        blur.getParam("size").setValue(6, 0)
        blur_name = blur.getScriptName()
        grade_name = grade.getScriptName()
        constant_name = constant.getScriptName()

        gt.section("convert")
        select_only([blur])
        invoke("makeINodeFromSelection")
        ok = yield ("until", lambda: plugin_of(blur_name) == IBLUR_ID, POLL_TIMEOUT_S)
        check(ok, "Make I-Node replaces the Blur by an IBlur named %s (got %r)" % (blur_name, plugin_of(blur_name)))
        iblur = app.getNode(blur_name)
        if iblur is not None:
            check(abs(iblur.getParam("size").getValue(0) - 6) < 1e-9, "the IBlur keeps the size")
            check(input_name(iblur, 0) == constant_name, "the IBlur reads the Constant")
        check(input_name(app.getNode(grade_name), 0) == blur_name, "the Grade reads the IBlur")
        gt.shot("makeinode_converted.png")
        trigger_edit("undo")
        ok = yield ("until", lambda: plugin_of(blur_name) == BLUR_ID, POLL_TIMEOUT_S)
        check(ok, "undo brings the Blur back (got %r)" % plugin_of(blur_name))
        trigger_edit("redo")
        ok = yield ("until", lambda: plugin_of(blur_name) == IBLUR_ID, POLL_TIMEOUT_S)
        check(ok, "redo makes it an IBlur again")
        select_only([app.getNode(blur_name)])
        invoke("makeBaseNodeFromSelection")
        ok = yield ("until", lambda: plugin_of(blur_name) == BLUR_ID, POLL_TIMEOUT_S)
        check(ok, "Make Base Node with no Map connection makes it a Blur without asking")
        check(input_name(app.getNode(grade_name), 0) == blur_name, "the Grade reads the Blur again")

        gt.section("menu")
        grade = app.getNode(grade_name)
        select_only([grade])
        menu = edit_menu()
        require(menu is not None, "the main window has an Edit menu")
        menu.aboutToShow.emit()
        QApplication.processEvents()
        base = edit_menu_action("make base node")
        inode = edit_menu_action("make i-node")
        check(base is not None and not base.isEnabled() and bool(base.toolTip()),
              "Make Base Node is disabled with a reason on a Grade")
        check(inode is not None and inode.isEnabled(), "Make I-Node is enabled on a Grade")

        gt.section("wrap")
        select_only([grade])
        invoke("makeINodeFromSelection")
        ok = yield ("until", lambda: loop_end_reading(grade_name) is not None, POLL_TIMEOUT_S)
        check(ok, "Make I-Node on a Grade puts a LoopEnd after it")
        end = loop_end_reading(grade_name)
        begin = app.getNode(grade_name).getInput(0)
        check(begin is not None and begin.getPluginID() == BEGIN_ID, "a LoopBegin feeds the Grade")
        check(begin is not None and input_name(begin, 0) == blur_name, "the LoopBegin reads the Blur")
        if end is not None:
            check(end.getParam("iterations").getValue() == ITERATIONS, "the loop runs %d iterations" % ITERATIONS)
            mode = end.getParam("mode")
            check(mode is not None and mode.getOption(mode.getValue()) == MAP_BLEND, "the loop is in Map Blend mode")
            selected = [n.getScriptName() for n in app.getSelectedNodes()]
            check(selected == [end.getScriptName()], "the LoopEnd is selected (got %r)" % selected)
            ok = yield ("until", lambda: panel_of(end) is not None, POLL_TIMEOUT_S)
            check(ok, "the LoopEnd's panel opens")
            rows = driver_table_rows(end)
            check(rows == 0, "the Driven Knobs table is on the panel and empty (got %r rows)" % rows)
        gt.shot("makeinode_wrapped.png")
        trigger_edit("undo")
        ok = yield ("until", lambda: loop_end_reading(grade_name) is None, POLL_TIMEOUT_S)
        check(ok, "undo takes the loop out")
        check(input_name(app.getNode(grade_name), 0) == blur_name, "after undo the Grade reads the Blur")
        gt.close_all_panels()

        gt.section("warning")
        transform = app.createNode(TRANSFORM_ID)
        require(transform is not None, "create " + TRANSFORM_ID, gt.plugin_problem(TRANSFORM_ID))
        transform.setPosition(200, 0)
        grade2 = app.createNode(GRADE_ID)
        grade2.setPosition(200, 120)
        require(transform.connectInput(0, app.getNode(constant_name)), "connect the Transform")
        require(grade2.connectInput(0, transform), "connect the second Grade")
        select_only([transform, grade2])
        invoke("makeINodeFromSelection")
        ok = yield ("until", lambda: transform.getLabel() in status_message(), POLL_TIMEOUT_S)
        check(ok, "wrapping a Transform warns about it (got %r)" % status_message())
        gt.shot("makeinode_warning.png")
        check(loop_end_reading(grade2.getScriptName()) is not None, "the Transform and Grade are wrapped")
        trigger_edit("undo")
        ok = yield ("until", lambda: loop_end_reading(grade2.getScriptName()) is None, POLL_TIMEOUT_S)
        check(ok, "undo takes the Transform's loop out")
        check(input_name(grade2, 0) == transform.getScriptName(), "after undo the Grade reads the Transform")

        gt.close_all_panels()
        for name in (grade2.getScriptName(), transform.getScriptName(), grade_name, blur_name, constant_name):
            node = app.getNode(name)
            if node is not None:
                node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
