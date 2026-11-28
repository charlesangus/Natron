"""Template import, artist edit, template update and its merge conflicts, end to end in the real application.

Every import, comparison and update in the shot goes through the application's own actions: File >
Import Template..., File > Compare with File..., File > Update Template to Latest, and the node graph's
context-menu Compare with Base and Update to Latest on a selected template member. File dialogs and
menus are driven from QTimer callbacks (see run() in guitest.py), since their slots block in a modal
loop.

A template (Blur1 -> Blur2) is defined with app.defineTemplate() and published as version 1. Version 2
of the clean scenario sets Blur1's size to 4 and adds Blur3 fed by Blur2, added to the template from
the context menu, and is published from the same script. The shot imports version 1, the artist sets
Blur2's size to 2, and Compare with Base tints Blur2 as changed; Escape in the node graph clears it.
Compare with File against version 1 tints Blur2 again, and Escape in the Node Diff panel clears that.
File > Update Template to Latest raises no conflict, takes Blur1's size and Blur3 from version 2 and
keeps the artist's Blur2 size. Edit > Undo once restores exactly the graph before the update, so the
update is one undo step.

The conflicted scenario has a version 2 that sets both sizes (Blur1 to 4, Blur2 to 6) while the artist
set them to 2 and 3. Update to Latest on Blur1's context menu keeps the artist's values and raises
two conflicts, listed in the Merge Conflicts panel. Theirs resolves the first from the panel. The project is saved with the other still
unresolved, reset and reopened: the panel lists the one conflict again, with the badge on its node and
the artist's value kept, and resolving it from the reopened panel clears the badge.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m97_template_merge.py
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
CLEAN_V2_SIZE = 4.0
CLEAN_ARTIST_SIZE = 2.0
CONFLICT_V2 = {"Blur1": 4.0, "Blur2": 6.0}
CONFLICT_ARTIST = {"Blur1": 2.0, "Blur2": 3.0}

try:
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QAction, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QLineEdit, QMainWindow, QMenu, QPushButton, QTabBar,
                                   QTreeWidget)

    import guitest as gt
    from guitest import check, report, require

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

    def undo(label):
        a = edit_menu_action("undo")
        if not check(a is not None and a.isEnabled(), "Edit > Undo is enabled to undo " + label):
            return False
        a.trigger()
        QApplication.processEvents()
        return True

    def widget(cls, name):
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, cls) and w.objectName() == name:
                    return w
            except RuntimeError:
                continue
        return None

    def panel_shown():
        for w in gt.widgets_of("MergeConflictPanel"):
            return w.isVisible()
        return False

    def tree():
        return widget(QTreeWidget, "mergeConflictTree")

    def rows():
        """The conflict rows as (node row text, conflict row text); plain values only, since every
        refresh() deletes the items."""
        t = tree()
        found = []
        if t is None:
            return found
        for g in range(t.topLevelItemCount()):
            group = t.topLevelItem(g)
            for i in range(group.childCount()):
                found.append((group.text(0), group.child(i).text(0)))
        return found

    def tab_label():
        for bar in QApplication.allWidgets():
            try:
                if not isinstance(bar, QTabBar):
                    continue
                for i in range(bar.count()):
                    if bar.tabText(i).startswith("Merge Conflicts"):
                        return bar.tabText(i)
            except RuntimeError:
                continue
        return ""

    def check_panel(count, label):
        found = rows()
        check(len(found) == count, "%s: the panel lists %d conflicts (got %d)" % (label, count, len(found)))
        tab = tab_label()
        if count:
            check(("(%d)" % count) in tab, "%s: the panel's tab shows the count (%r)" % (label, tab))
        else:
            check("(" not in tab, "%s: the panel's tab shows no count (%r)" % (label, tab))

    def select_first():
        t = tree()
        found = rows()
        if not check(t is not None and len(found) > 0, "the panel lists a conflict to select"):
            return None
        group_text, text = found[0]
        for g in range(t.topLevelItemCount()):
            group = t.topLevelItem(g)
            if group.text(0) != group_text:
                continue
            for i in range(group.childCount()):
                row = group.child(i)
                if row.text(0) == text:
                    t.clearSelection()
                    t.setCurrentItem(row)
                    row.setSelected(True)
                    QApplication.processEvents()
                    return (group_text, text)
        return None

    def click(name):
        b = widget(QPushButton, name)
        if not check(b is not None and b.isEnabled(), "the %s button is enabled" % name):
            return False
        b.click()
        QApplication.processEvents()
        return True

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
            report("NOTE open dialog edits %r, buttons %r" % ([gt.class_name(w) for w in dialog.findChildren(QLineEdit)],
                                                          [b.text() for b in dialog.findChildren(QAbstractButton)]))
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

    def activate(menu, action, label):
        """Activates action in the open menu with Return, as a keyboard user would, so a blocking
        exec() returns it."""
        flag = {}

        def fire():
            menu.setActiveAction(action)
            QApplication.sendEvent(menu, QKeyEvent(QEvent.KeyPress, Qt.Key_Return, Qt.NoModifier))

        later(fire, flag)
        yield from wait_returned(flag, label)

    def update_from_file_menu():
        win = main_window()
        menu = win.findChild(QMenu, "menuUpdateFromTemplate") if win is not None else None
        require(menu is not None, "the File menu has Update Template to Latest")
        menu.popup(win.mapToGlobal(QPoint(40, 40)))
        ok = yield ("until", menu.isVisible, DIALOG_TIMEOUT_S)
        require(ok, "File > Update Template to Latest opens")
        entries = [a for a in menu.actions() if a.isEnabled() and a.data()]
        require(len(entries) == 1, "File > Update Template to Latest lists the one import (%r)" % [a.text() for a in menu.actions()])
        yield from activate(menu, entries[0], "File > Update Template to Latest > " + entries[0].text())

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def graph_menu(name, action_name, label, select=True):
        """Right-clicks node name in the node graph and picks the context-menu action action_name.
        With select False the selection is kept, and must hold name."""
        node = app.getNode(name)
        require(node is not None, label + ": " + name + " exists")
        if select:
            app.selectNode(node, True)
        view = graph_view()
        require(view is not None, label + ": the node graph is up")
        x, y = node.getPosition()
        w, h = node.getSize()
        centre = QPointF(x + w / 2.0, y + h / 2.0)
        view.centerOn(centre)
        QApplication.processEvents()
        pos = QPointF(view.mapFromScene(centre))
        port = view.viewport()
        flag = {}

        def press():
            QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonPress, pos, QPointF(port.mapToGlobal(pos)),
                                                     Qt.RightButton, Qt.RightButton, Qt.NoModifier))
            QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonRelease, pos, QPointF(port.mapToGlobal(pos)),
                                                     Qt.RightButton, Qt.NoButton, Qt.NoModifier))

        later(press, flag)
        ok = yield ("until", lambda: isinstance(QApplication.activePopupWidget(), QMenu), DIALOG_TIMEOUT_S)
        require(ok, label + ": right-clicking " + name + " opens the node graph's menu")
        menu = QApplication.activePopupWidget()
        found = [a for a in menu.actions() if a.objectName() == action_name]
        if not found:
            menu.close()
        require(len(found) == 1, label + ": the menu has " + action_name, "items %r" % [a.text() for a in menu.actions()])
        menu.setActiveAction(found[0])
        QApplication.sendEvent(menu, QKeyEvent(QEvent.KeyPress, Qt.Key_Return, Qt.NoModifier))
        yield from wait_returned(flag, label)

    def escape(widget):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, Qt.Key_Escape, Qt.NoModifier))
        QApplication.processEvents()

    def snapshot(names):
        """name -> (size, input's script name) for each node, None for a node that is not there."""
        state = {}
        for name in names:
            node = app.getNode(name)
            state[name] = None if node is None else (size_of(node), source_of(node))
        return state

    def template_node():
        found = [n for n in app.getChildren() if n.getPluginID() == "Template"]
        return found[0] if len(found) == 1 else None

    def publish_v1(name, folder):
        """Blur1 -> Blur2 defined as the template name and published as version 1 in folder; returns
        the template key."""
        first = blur("Blur1", 0, 0)
        second = blur("Blur2", 0, 200, first)
        os.makedirs(folder, exist_ok=True)
        tpl = app.defineTemplate([first, second], name)
        require(tpl is not None, "define the template " + name)
        key = tpl.getParam("templateKey").getValue()
        tpl.getParam("publishDir").setValue(folder)
        path = app.publishTemplate(key)
        require(os.path.basename(path) == name + "_v001.ntpz", "publish %s version 1 (%r)" % (name, path))
        return key

    def publish_v2(key, name, script):
        path = app.publishTemplate(key)
        require(os.path.basename(path) == name + "_v002.ntpz", "publish %s version 2 (%r)" % (name, path))
        require(app.saveProjectAs(script), "save the script %s was published from" % name)
        require(app.resetProject(), "reset the project")

    def steps():
        gt.section("templates")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        names = ["Blur1", "Blur2", "Blur3"]
        clean_dir = os.path.join(gt.OUT, "m97_clean")
        conflict_dir = os.path.join(gt.OUT, "m97_conflict")
        clean_v1 = os.path.join(clean_dir, "clean_v001.ntpz")
        conflict_v1 = os.path.join(conflict_dir, "conflict_v001.ntpz")
        clean_after = os.path.join(gt.OUT, "m97_clean_after.ntpz")
        shot = os.path.join(gt.OUT, "m97_shot.ntpz")
        # Publish refuses to write over or below the versions an earlier run left in the folder.
        for name, folder in (("clean", clean_dir), ("conflict", conflict_dir)):
            for version in ("_v001.ntpz", "_v002.ntpz"):
                path = os.path.join(folder, name + version)
                if os.path.exists(path):
                    os.remove(path)

        key = publish_v1("clean", clean_dir)
        app.getNode("Blur1").getParam(SIZE).set(CLEAN_V2_SIZE, CLEAN_V2_SIZE)
        blur("Blur3", 0, 400, app.getNode("Blur2"))
        yield ("sleep", SETTLE_MS)
        app.setSelection([app.getNode("Blur3"), template_node()])
        yield from graph_menu("Blur3", "actionGraphAddToTemplate", "Add to Template", select=False)
        publish_v2(key, "clean", os.path.join(gt.OUT, "m97_clean_src.ntpz"))
        yield ("sleep", SETTLE_MS)

        key = publish_v1("conflict", conflict_dir)
        app.getNode("Blur1").getParam(SIZE).set(CONFLICT_V2["Blur1"], CONFLICT_V2["Blur1"])
        app.getNode("Blur2").getParam(SIZE).set(CONFLICT_V2["Blur2"], CONFLICT_V2["Blur2"])
        publish_v2(key, "conflict", os.path.join(gt.OUT, "m97_conflict_src.ntpz"))
        yield ("sleep", SETTLE_MS)

        gt.section("import and edit")
        yield from through_file_dialog("actionImportTemplate", clean_v1, "File > Import Template...")
        blur1 = app.getNode("Blur1")
        blur2 = app.getNode("Blur2")
        require(blur1 is not None and blur2 is not None, "the import created Blur1 and Blur2")
        check(app.getNode("Blur3") is None, "version 1 has no Blur3")
        base_size = size_of(blur1)
        blur2.getParam(SIZE).set(CLEAN_ARTIST_SIZE, CLEAN_ARTIST_SIZE)
        yield ("sleep", SETTLE_MS)

        yield from graph_menu("Blur2", "actionGraphCompareWithTemplateBase", "Compare with Base")
        ok = yield ("until", lambda: app.getDiffTint(app.getNode("Blur2")) != "", POLL_TIMEOUT_S)
        check(ok, "the comparison with the template's base tints the edited Blur2")
        check(app.getDiffTint(app.getNode("Blur2")) == "changed", "Blur2 is tinted as changed (%r)" % app.getDiffTint(app.getNode("Blur2")))
        check(app.getDiffTint(app.getNode("Blur1")) == "", "the untouched Blur1 is not tinted (%r)" % app.getDiffTint(app.getNode("Blur1")))
        view = graph_view()
        require(view is not None, "the node graph is up for Escape")
        escape(view)
        yield ("sleep", SETTLE_MS)
        check(app.getDiffTint(app.getNode("Blur2")) == "", "Escape in the node graph clears the comparison")
        check(app.getDiffGhostCount() == 0, "no ghost is drawn after clearing the comparison")

        yield from through_file_dialog("actionCompareWithFile", clean_v1, "File > Compare with File...")
        ok = yield ("until", lambda: app.getDiffTint(app.getNode("Blur2")) == "changed", POLL_TIMEOUT_S)
        check(ok, "comparing with version 1's file tints the edited Blur2 as changed (%r)" % app.getDiffTint(app.getNode("Blur2")))
        check(app.getDiffTint(app.getNode("Blur1")) == "", "the file comparison leaves Blur1 untinted (%r)" % app.getDiffTint(app.getNode("Blur1")))
        panels = gt.widgets_of("NodeDiffPanel")
        require(len(panels) == 1, "the Gui has its Node Diff panel")
        escape(panels[0])
        yield ("sleep", SETTLE_MS)
        check(app.getDiffTint(app.getNode("Blur2")) == "", "Escape in the Node Diff panel clears the comparison")

        gt.section("clean update")
        before = snapshot(names)
        report("NOTE before the update %r" % (before,))
        check(before["Blur3"] is None, "Blur3 is absent before the update")
        yield from update_from_file_menu()
        blur1 = app.getNode("Blur1")
        blur2 = app.getNode("Blur2")
        blur3 = app.getNode("Blur3")
        require(blur1 is not None and blur2 is not None and blur3 is not None, "the update added Blur3")
        check(size_of(blur1) == (CLEAN_V2_SIZE, CLEAN_V2_SIZE), "the update took the template's Blur1 size (%r)" % (size_of(blur1),))
        check(size_of(blur2) == (CLEAN_ARTIST_SIZE, CLEAN_ARTIST_SIZE), "the update kept the artist's Blur2 size (%r)" % (size_of(blur2),))
        check(source_of(blur3) == "Blur2", "Blur3 is fed by Blur2 (%r)" % source_of(blur3))
        check(source_of(blur2) == "Blur1", "Blur2 is still fed by Blur1 (%r)" % source_of(blur2))
        check(not any(app.isMergeConflictBadgeShown(n) for n in (blur1, blur2, blur3)), "no node carries a conflict badge")
        check(len(rows()) == 0, "the panel lists no conflict")
        after = snapshot(names)

        gt.section("single undo")
        if undo("Update to Latest"):
            yield ("sleep", SETTLE_MS)
            restored = snapshot(names)
            report("NOTE after one undo %r" % (restored,))
            check(restored == before, "one undo restores exactly the pre-merge graph (%r)" % (restored,))
            check(app.getNode("Blur3") is None, "one undo removes Blur3")
            check(size_of(app.getNode("Blur1")) == base_size, "one undo restores Blur1's size (%r)" % (size_of(app.getNode("Blur1")),))
            check(size_of(app.getNode("Blur2")) == (CLEAN_ARTIST_SIZE, CLEAN_ARTIST_SIZE), "one undo keeps the artist's Blur2 size")
            redo = edit_menu_action("redo")
            if redo is not None and redo.isEnabled():
                redo.trigger()
                QApplication.processEvents()
                yield ("sleep", SETTLE_MS)
                check(snapshot(names) == after, "redo brings the updated graph back")

        gt.section("conflicted import")
        require(app.saveProjectAs(clean_after), "save the clean scenario so the reset has no unsaved changes")
        require(app.resetProject(), "reset the project for the conflicted scenario")
        yield ("sleep", SETTLE_MS)
        yield from through_file_dialog("actionImportTemplate", conflict_v1, "File > Import Template... (conflicted scenario)")
        for name in ("Blur1", "Blur2"):
            node = app.getNode(name)
            require(node is not None, "the import created " + name)
            node.getParam(SIZE).set(CONFLICT_ARTIST[name], CONFLICT_ARTIST[name])
        count = 2
        yield from graph_menu("Blur1", "actionGraphUpdateToLatest", "Update to Latest")
        ok = yield ("until", lambda: panel_shown() and len(rows()) == count, POLL_TIMEOUT_S)
        require(ok, "the Merge Conflicts panel lists the conflicts")
        yield ("sleep", SETTLE_MS)
        check_panel(count, "after the update")
        for name in ("Blur1", "Blur2"):
            node = app.getNode(name)
            check(size_of(node) == (CONFLICT_ARTIST[name],) * 2, "the update kept the artist's %s size (%r)" % (name, size_of(node)))
            check(app.isMergeConflictBadgeShown(node), name + " carries the conflict badge")
        gt.shot("m97_template_merge.png")

        gt.section("resolve")
        chosen = select_first()
        require(chosen is not None, "select the first conflict")
        report("NOTE resolving %r" % (chosen,))
        require(click("mergeConflictTheirs"), "resolve the first conflict with Theirs")
        yield ("sleep", SETTLE_MS)
        check_panel(count - 1, "after Theirs")
        resolved = [n for n in ("Blur1", "Blur2") if n in chosen[0]]
        require(len(resolved) == 1, "the resolved conflict is listed under Blur1 or Blur2 (%r)" % (chosen,))
        left = "Blur2" if resolved[0] == "Blur1" else "Blur1"
        check(size_of(app.getNode(resolved[0])) == (CONFLICT_V2[resolved[0]],) * 2, "Theirs gives %s the template's size (%r)" % (resolved[0], size_of(app.getNode(resolved[0]))))
        check(not app.isMergeConflictBadgeShown(app.getNode(resolved[0])), "the badge leaves " + resolved[0])
        check(app.isMergeConflictBadgeShown(app.getNode(left)), left + " keeps its badge")
        check(size_of(app.getNode(left)) == (CONFLICT_ARTIST[left],) * 2, left + " keeps the artist's size")

        gt.section("save with an unresolved conflict")
        require(app.saveProjectAs(shot), "save the project with one conflict unresolved")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        check(len(rows()) == 0, "the panel lists nothing after the reset")
        require(app.loadProject(shot) is not None, "reopen the project")
        check(QApplication.activeModalWidget() is None, "opening a project with a conflict raises no modal dialog")
        ok = yield ("until", lambda: app.getNode(left) is not None and len(rows()) > 0, POLL_TIMEOUT_S)
        require(ok, "the reopened project lists its conflict")
        ok = yield ("until", panel_shown, POLL_TIMEOUT_S)
        check(ok, "opening a project with a conflict shows the Merge Conflicts panel")
        yield ("sleep", SETTLE_MS)
        check_panel(count - 1, "after reopening")
        check(any(left in group for group, _ in rows()), "the persisted conflict is listed under " + left)
        check(app.isMergeConflictBadgeShown(app.getNode(left)), "the reopened %s carries the badge" % left)
        check(not app.isMergeConflictBadgeShown(app.getNode(resolved[0])), "the reopened %s carries no badge" % resolved[0])
        check(size_of(app.getNode(left)) == (CONFLICT_ARTIST[left],) * 2, "the reopened %s keeps the artist's size" % left)
        check(size_of(app.getNode(resolved[0])) == (CONFLICT_V2[resolved[0]],) * 2, "the reopened %s keeps the resolved size" % resolved[0])
        require(click("mergeConflictAllTheirs"), "Resolve All Theirs in the reopened project")
        yield ("sleep", SETTLE_MS)
        check_panel(0, "after resolving the reopened conflict")
        check(not app.isMergeConflictBadgeShown(app.getNode(left)), "resolving clears the badge the load put on " + left)
        check(size_of(app.getNode(left)) == (CONFLICT_V2[left],) * 2, "Resolve All Theirs gives %s the template's size" % left)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
