"""Template versions, end to end in the real application.

The supervisor selects Blur1 -> Blur2, chooses File > Define Template..., names it grade, sets its
publish folder and presses Publish: grade_v001.ntpz is written. The artist imports version 1 into a
second project: the Template node is there at version 1, and clicking inside its hull selects the
members and the Template node. The artist sets Blur2's size to 2. In a third project, version 1 is
imported again, Blur1's size set to 4, Blur3 added below Blur2 and put in the template with the
context menu's Add to Template, and Publish writes grade_v002.ntpz. Another template, other, is
defined and published from Python in its own folder.

Reopening the artist's project, the Template node shows version 2 as newer. Update to Latest takes
Blur1's size and Blur3 from version 2, keeps the artist's Blur2 size and raises no conflict; running
it again says the template is up to date. Update from File... with other's file warns that it is
another template, and Cancel leaves the graph as it was. Remove from Template takes Blur3 out of the
hull, and Add to Template puts it back. Deleting the Template node detaches the template: the hull
and the template actions are gone and the members stay. One undo brings the Template node, the hull
and the template actions back.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m646_template_versions.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
DIALOG_TIMEOUT_S = 20.0
SETTLE_MS = 400
BLUR = "Blur"
SIZE = "size"
V2_SIZE = 4.0
ARTIST_SIZE = 2.0
TEMPLATE_ACTIONS = ("actionGraphUpdateToLatest", "actionGraphUpdateFromFile", "actionGraphCompareWithTemplateBase",
                    "actionGraphAddToTemplate", "actionGraphRemoveFromTemplate")

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QAction, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMainWindow, QMenu, QMessageBox, QTreeWidget

    import guitest as gt
    from guitest import check, require

    _held = []

    def hold(obj):
        """Keeps a wrapper alive for the script's lifetime: child wrappers die with their parent's."""
        if obj is not None and not any(obj is h for h in _held):
            _held.append(obj)
        return obj

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return hold(w)
        return None

    def status_message():
        win = main_window()
        return win.statusBar().currentMessage() if win is not None and win.statusBar() is not None else ""

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = hold(gt.menu_of(act))
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def undo(label):
        a = edit_menu_action("undo")
        if not check(a is not None and a.isEnabled(), "Edit > Undo is enabled to undo " + label):
            return False
        a.trigger()
        QApplication.processEvents()
        return True

    def conflict_rows():
        found = 0
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QTreeWidget) and w.objectName() == "mergeConflictTree":
                    for g in range(w.topLevelItemCount()):
                        found += w.topLevelItem(g).childCount()
            except RuntimeError:
                continue
        return found

    def blur(name, x, y, source=None):
        node = app.createNode(BLUR)
        require(node is not None, "create " + name, gt.plugin_problem(BLUR))
        node.setScriptName(name)
        node.setPosition(x, y)
        if source is not None:
            require(node.connectInput(0, source), "connect %s to %s" % (source.getScriptName(), name))
        return node

    def size_of(node):
        param = node.getParam(SIZE)
        return (param.getValue(0), param.getValue(1))

    def source_of(node):
        source = node.getInput(0)
        return source.getScriptName() if source is not None else None

    def template_nodes():
        return [n for n in app.getChildren() if n.getPluginID() == "Template"]

    def template_node():
        found = template_nodes()
        return found[0] if len(found) == 1 else None

    def snapshot():
        """script name -> (plugin, size or None, input's script name) for every node."""
        state = {}
        for node in app.getChildren():
            size = size_of(node) if node.getParam(SIZE) is not None else None
            state[node.getScriptName()] = (node.getPluginID(), size, source_of(node) if node.getMaxInputCount() else None)
        return state

    def selected_names():
        return sorted(n.getScriptName() for n in app.getSelectedNodes())

    def modal_class():
        w = QApplication.activeModalWidget()
        return gt.class_name(w) if w is not None else None

    def later(fn, flag):
        """Runs fn from a timer, so this generator keeps running while fn blocks in a modal loop."""
        flag["done"] = False

        def fire():
            try:
                fn()
            finally:
                flag["done"] = True

        QTimer.singleShot(0, fire)

    def submit_open_dialog(path):
        dialog = QApplication.activeModalWidget()
        if dialog is None or gt.class_name(dialog) != "SequenceFileDialog":
            return False
        # The selection edit is created before the filter edit.
        edits = [w for w in dialog.findChildren(QLineEdit) if gt.class_name(w) == "LineEdit"]
        buttons = [b for b in dialog.findChildren(QAbstractButton) if b.text().replace("&", "") == "Open"]
        if not edits or not buttons:
            return False
        edits[0].setText(path)
        buttons[0].click()
        return True

    def wait_returned(flag, label):
        ok = yield ("until", lambda: flag["done"], DIALOG_TIMEOUT_S)
        require(ok, label + " returned", "modal is %r" % modal_class())
        yield ("sleep", SETTLE_MS)
        check(modal_class() is None, label + " leaves no dialog open (modal is %r)" % modal_class())

    def file_action(name):
        win = main_window()
        return win.findChild(QAction, name) if win is not None else None

    def through_file_dialog(name, path, label):
        """Triggers the File menu action called name and chooses path in its open dialog."""
        action = file_action(name)
        require(action is not None and action.isEnabled(), "the File menu has an enabled " + label)
        flag = {}
        later(action.trigger, flag)
        ok = yield ("until", lambda: modal_class() == "SequenceFileDialog", DIALOG_TIMEOUT_S)
        require(ok, label + " opens a file dialog", "modal is %r" % modal_class())
        require(submit_open_dialog(path), label + ": chose " + os.path.basename(path))
        yield from wait_returned(flag, label)

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def scene_click(view, scene_pos, button):
        view.centerOn(scene_pos)
        QApplication.processEvents()
        pos = QPointF(view.mapFromScene(scene_pos))
        port = view.viewport()
        glob = QPointF(port.mapToGlobal(pos))
        QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonPress, pos, glob, button, button, Qt.NoModifier))
        QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonRelease, pos, glob, button, Qt.NoButton, Qt.NoModifier))

    def centre_of(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return QPointF(x + w / 2.0, y + h / 2.0)

    def open_graph_menu(name, label, select):
        """Right-clicks node name from a timer and returns (flag, menu) once the context menu is up;
        the flag is set when the menu's action has returned. With select False the selection is
        kept, and must hold name."""
        node = app.getNode(name)
        require(node is not None, label + ": " + name + " exists")
        if select:
            app.selectNode(node, True)
        view = graph_view()
        require(view is not None, label + ": the node graph is up")
        flag = {}
        later(lambda: scene_click(view, centre_of(node), Qt.RightButton), flag)
        ok = yield ("until", lambda: isinstance(QApplication.activePopupWidget(), QMenu), DIALOG_TIMEOUT_S)
        require(ok, label + ": right-clicking " + name + " opens the node graph's menu")
        return flag, hold(QApplication.activePopupWidget())

    def menu_names(name, label):
        """The object names of the context menu's actions on node name, closing the menu."""
        flag, menu = yield from open_graph_menu(name, label, True)
        names = [a.objectName() for a in menu.actions() if a.objectName()]
        menu.close()
        yield from wait_returned(flag, label)
        return names

    def pick(menu, action_name, label):
        found = [a for a in menu.actions() if a.objectName() == action_name]
        if not found:
            menu.close()
        require(len(found) == 1 and found[0].isEnabled(), label + ": the menu has an enabled " + action_name,
                "items %r" % [a.text() for a in menu.actions()])
        menu.setActiveAction(found[0])
        QApplication.sendEvent(menu, QKeyEvent(QEvent.KeyPress, Qt.Key_Return, Qt.NoModifier))

    def graph_menu(name, action_name, label, select=True):
        """Right-clicks node name in the node graph and picks the context-menu action action_name."""
        flag, menu = yield from open_graph_menu(name, label, select)
        pick(menu, action_name, label)
        yield from wait_returned(flag, label)

    def click_hull(label):
        """Left-clicks just right of Blur1, inside the template's hull but on no node."""
        view = graph_view()
        require(view is not None, label + ": the node graph is up")
        blur1 = app.getNode("Blur1")
        x, y = blur1.getPosition()
        w, h = blur1.getSize()
        app.clearSelection()
        QApplication.processEvents()
        scene_click(view, QPointF(x + w + 9.0, y + h / 2.0), Qt.LeftButton)
        QApplication.processEvents()

    def hull_selection(label):
        click_hull(label)
        yield ("sleep", SETTLE_MS)
        return selected_names()

    def press(param_name, label):
        tpl = template_node()
        require(tpl is not None, label + ": the project has one Template node")
        button = tpl.getParam(param_name)
        require(button is not None, label + ": the Template node has " + param_name)
        button.trigger()
        QApplication.processEvents()

    def send_key(widget, key):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def save_and_reset(path, label):
        require(app.saveProjectAs(path), "save " + label)
        require(app.resetProject(), "reset the project after " + label)

    def steps():
        gt.section("templates")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        publish_dir = os.path.join(gt.OUT, "m646_templates")
        other_dir = os.path.join(gt.OUT, "m646_other")
        v1 = os.path.join(publish_dir, "grade_v001.ntpz")
        v2 = os.path.join(publish_dir, "grade_v002.ntpz")
        other_v1 = os.path.join(other_dir, "other_v001.ntpz")
        artist = os.path.join(gt.OUT, "m646_artist.ntpz")
        for folder in (publish_dir, other_dir):
            os.makedirs(folder, exist_ok=True)
        # Publish never overwrites, so versions left by an earlier run would be numbered past.
        for path in (v1, v2, other_v1):
            if os.path.exists(path):
                os.remove(path)

        gt.section("define and publish version 1")
        first = blur("Blur1", 0, 0)
        blur("Blur2", 0, 200, first)
        app.setSelection([app.getNode("Blur1"), app.getNode("Blur2")])
        action = file_action("actionDefineTemplate")
        require(action is not None and action.isEnabled(), "the File menu has an enabled Define Template...")
        action.trigger()
        ok = yield ("until", lambda: template_node() is not None, POLL_TIMEOUT_S)
        require(ok, "File > Define Template... makes a Template node")
        check(modal_class() is None, "Define Template opens no dialog (modal is %r)" % modal_class())
        tpl = template_node()
        check(tpl.getParam("version").getValue() == 0, "the defined template is unpublished (%r)" % tpl.getParam("version").getValue())
        tpl.getParam("templateName").setValue("grade")
        tpl.getParam("publishDir").setValue(publish_dir)
        press("publish", "Publish")
        ok = yield ("until", lambda: os.path.exists(v1), POLL_TIMEOUT_S)
        require(ok, "Publish writes grade_v001.ntpz", "status %r" % status_message())
        check(modal_class() is None, "Publish opens no dialog")
        check("grade_v001.ntpz" in status_message(), "the status bar names the published file (%r)" % status_message())
        save_and_reset(os.path.join(gt.OUT, "m646_supervisor.ntpz"), "the supervisor's script")
        yield ("sleep", SETTLE_MS)

        gt.section("import into the artist's project")
        yield from through_file_dialog("actionImportTemplate", v1, "File > Import Template...")
        require(app.getNode("Blur1") is not None and app.getNode("Blur2") is not None, "the import created Blur1 and Blur2")
        tpl = template_node()
        require(tpl is not None, "the import created the Template node")
        check(tpl.getParam("templateName").getValue() == "grade", "the Template node is grade (%r)" % tpl.getParam("templateName").getValue())
        check(tpl.getParam("version").getValue() == 1, "the import is at version 1 (%r)" % tpl.getParam("version").getValue())
        check(tpl.getParam("publishDir").getValue() == publish_dir, "the publish folder is the one the file is in (%r)" % tpl.getParam("publishDir").getValue())
        tpl_name = tpl.getScriptName()
        members = sorted(["Blur1", "Blur2", tpl_name])
        got = yield from hull_selection("the artist's hull")
        check(got == members, "clicking the hull selects the members and the Template node (%r)" % got)
        app.getNode("Blur2").getParam(SIZE).set(ARTIST_SIZE, ARTIST_SIZE)
        save_and_reset(artist, "the artist's project")
        yield ("sleep", SETTLE_MS)

        gt.section("publish version 2 from a fresh import")
        yield from through_file_dialog("actionImportTemplate", v1, "File > Import Template... (third project)")
        app.getNode("Blur1").getParam(SIZE).set(V2_SIZE, V2_SIZE)
        blur("Blur3", 0, 400, app.getNode("Blur2"))
        yield ("sleep", SETTLE_MS)
        app.setSelection([app.getNode("Blur3"), template_node()])
        yield from graph_menu("Blur3", "actionGraphAddToTemplate", "Add to Template (third project)", select=False)
        press("publish", "Publish version 2")
        ok = yield ("until", lambda: os.path.exists(v2), POLL_TIMEOUT_S)
        require(ok, "Publish from the fresh import writes grade_v002.ntpz", "status %r" % status_message())
        save_and_reset(os.path.join(gt.OUT, "m646_v2_source.ntpz"), "the version 2 script")
        yield ("sleep", SETTLE_MS)

        other = blur("Other1", 0, 0)
        other_tpl = app.defineTemplate([other], "other")
        require(other_tpl is not None, "app.defineTemplate() makes the other template")
        other_tpl.getParam("publishDir").setValue(other_dir)
        path = app.publishTemplate(other_tpl.getParam("templateKey").getValue())
        check(path == other_v1, "app.publishTemplate() returns the written path (%r)" % path)
        save_and_reset(os.path.join(gt.OUT, "m646_other.ntpz"), "the other template's script")
        yield ("sleep", SETTLE_MS)

        gt.section("update to latest")
        require(app.loadProject(artist) is not None, "reopen the artist's project")
        ok = yield ("until", lambda: template_node() is not None and template_node().getParam("newerVersion").getValue() == v2,
                    POLL_TIMEOUT_S)
        check(ok, "the reopened Template node shows version 2 as newer (%r)"
              % (template_node().getParam("newerVersion").getValue() if template_node() is not None else None))
        press("updateToLatest", "Update to Latest")
        ok = yield ("until", lambda: app.getNode("Blur3") is not None, POLL_TIMEOUT_S)
        require(ok, "Update to Latest adds Blur3", "status %r" % status_message())
        yield ("sleep", SETTLE_MS)
        check(modal_class() is None, "Update to Latest opens no dialog")
        check(size_of(app.getNode("Blur1")) == (V2_SIZE, V2_SIZE), "the update takes version 2's Blur1 size (%r)" % (size_of(app.getNode("Blur1")),))
        check(size_of(app.getNode("Blur2")) == (ARTIST_SIZE, ARTIST_SIZE), "the update keeps the artist's Blur2 size (%r)" % (size_of(app.getNode("Blur2")),))
        check(source_of(app.getNode("Blur3")) == "Blur2", "Blur3 is fed by Blur2 (%r)" % source_of(app.getNode("Blur3")))
        check(conflict_rows() == 0, "the clean update raises no conflict (%d)" % conflict_rows())
        check(not any(app.isMergeConflictBadgeShown(app.getNode(n)) for n in ("Blur1", "Blur2", "Blur3")), "no node carries a conflict badge")
        check(template_node().getParam("version").getValue() == 2, "the Template node is at version 2 (%r)" % template_node().getParam("version").getValue())
        check(template_node().getParam("newerVersion").getValue() == "", "no newer version is shown after the update")

        gt.section("up to date")
        before = snapshot()
        press("updateToLatest", "Update to Latest again")
        ok = yield ("until", lambda: "up to date" in status_message(), POLL_TIMEOUT_S)
        check(ok, "a second Update to Latest says the template is up to date (%r)" % status_message())
        check(modal_class() is None, "the up-to-date message is not a dialog")
        check(snapshot() == before, "the second update changes nothing")

        gt.section("update from another template")
        before = snapshot()
        flag, menu = yield from open_graph_menu("Blur1", "Update from File...", True)
        pick(menu, "actionGraphUpdateFromFile", "Update from File...")
        ok = yield ("until", lambda: modal_class() == "SequenceFileDialog", DIALOG_TIMEOUT_S)
        require(ok, "Update from File... opens a file dialog", "modal is %r" % modal_class())
        require(submit_open_dialog(other_v1), "Update from File...: chose " + os.path.basename(other_v1))
        ok = yield ("until", lambda: QApplication.activeModalWidget() is not None
                    and QApplication.activeModalWidget().objectName() == "templateMismatchDialog", DIALOG_TIMEOUT_S)
        require(ok, "another template's file raises the mismatch warning", "modal is %r" % modal_class())
        box = QApplication.activeModalWidget()
        check(box.findChild(QAbstractButton, "mergeAnywayButton") is not None, "the warning offers Merge Anyway")
        cancel = box.button(QMessageBox.Cancel) if isinstance(box, QMessageBox) else None
        if cancel is None:
            cancel = [b for b in box.findChildren(QAbstractButton) if b.text().replace("&", "") == "Cancel"][0]
        cancel.click()
        yield from wait_returned(flag, "Update from File... with another template")
        check(snapshot() == before, "Cancel leaves the graph unchanged")
        check(template_node().getParam("version").getValue() == 2, "Cancel leaves the import at version 2")

        gt.section("membership")
        got = yield from hull_selection("the updated hull")
        check(got == sorted(members + ["Blur3"]), "the hull holds Blur3 after the update (%r)" % got)
        yield from graph_menu("Blur3", "actionGraphRemoveFromTemplate", "Remove from Template")
        got = yield from hull_selection("the hull after Remove from Template")
        check(got == members, "Remove from Template takes Blur3 out of the hull (%r)" % got)
        check(app.getNode("Blur3") is not None, "the removed Blur3 stays in the project")
        app.setSelection([app.getNode("Blur3"), template_node()])
        yield from graph_menu("Blur3", "actionGraphAddToTemplate", "Add to Template", select=False)
        got = yield from hull_selection("the hull after Add to Template")
        check(got == sorted(members + ["Blur3"]), "Add to Template puts Blur3 back in the hull (%r)" % got)

        gt.section("delete the template node")
        view = graph_view()
        require(view is not None, "the node graph is up to delete in")
        app.setSelection([template_node()])
        QApplication.processEvents()
        send_key(view, Qt.Key_Backspace)
        ok = yield ("until", lambda: not template_nodes(), POLL_TIMEOUT_S)
        require(ok, "Backspace deletes the Template node")
        check(all(app.getNode(n) is not None for n in ("Blur1", "Blur2", "Blur3")), "the members stay")
        got = yield from hull_selection("the detached graph")
        check(got == [], "no hull is left to click (%r)" % got)
        names = yield from menu_names("Blur1", "the detached Blur1's menu")
        check(not any(a in names for a in TEMPLATE_ACTIONS), "the detached Blur1 has no origin: no template action (%r)" % names)
        if undo("the delete"):
            ok = yield ("until", lambda: template_node() is not None, POLL_TIMEOUT_S)
            check(ok, "one undo brings the Template node back")
            yield ("sleep", SETTLE_MS)
            names = yield from menu_names("Blur1", "the restored Blur1's menu")
            check("actionGraphUpdateToLatest" in names, "one undo restores Blur1's origin (%r)" % names)
            got = yield from hull_selection("the restored hull")
            check(got == sorted(members + ["Blur3"]), "one undo restores the hull (%r)" % got)
            check(template_node().getParam("version").getValue() == 2, "one undo restores the import's version")
        gt.shot("m646_template_versions.png")

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
