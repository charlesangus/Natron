"""Template import, artist edit, template update and its merge conflicts, end to end in the real application.

A template (Blur1 -> Blur2) is saved as version 1. Version 2 of the clean scenario sets Blur1's size
to 4 and adds Blur3 fed by Blur2. The template is imported, the artist sets Blur2's size to 2, the
comparison with the template's base tints Blur2 as changed, and the template file is replaced by
version 2. Update from Template raises no conflict, takes Blur1's size and Blur3 from the template and
keeps the artist's Blur2 size. Edit > Undo once restores exactly the graph before the update, so the
update is one undo step.

The conflicted scenario has a version 2 that sets both sizes (Blur1 to 4, Blur2 to 6) while the artist
set them to 2 and 3. The update keeps the artist's values and raises two conflicts, listed in the Merge
Conflicts panel. Theirs resolves the first from the panel. The project is saved with the other still
unresolved, reset and reopened: the panel lists the one conflict again, with the badge on its node and
the artist's value kept, and resolving it from the reopened panel clears the badge.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m97_template_merge.py
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
BLUR = "Blur"
SIZE = "size"
CLEAN_V2_SIZE = 4.0
CLEAN_ARTIST_SIZE = 2.0
CONFLICT_V2 = {"Blur1": 4.0, "Blur2": 6.0}
CONFLICT_ARTIST = {"Blur1": 2.0, "Blur2": 3.0}

try:
    from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton, QTabBar, QTreeWidget

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

    def snapshot(names):
        """name -> (size, input's script name) for each node, None for a node that is not there."""
        state = {}
        for name in names:
            node = app.getNode(name)
            state[name] = None if node is None else (size_of(node), source_of(node))
        return state

    def make_v1(path):
        first = blur("Blur1", 0, 0)
        blur("Blur2", 0, 200, first)
        require(app.saveProjectAs(path), "save template version 1")
        return first

    def steps():
        gt.section("templates")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        names = ["Blur1", "Blur2", "Blur3"]
        live = os.path.join(gt.OUT, "m97_template_live.ntpz")
        clean_v1 = os.path.join(gt.OUT, "m97_clean_v1.ntpz")
        clean_v2 = os.path.join(gt.OUT, "m97_clean_v2.ntpz")
        conflict_v1 = os.path.join(gt.OUT, "m97_conflict_v1.ntpz")
        conflict_v2 = os.path.join(gt.OUT, "m97_conflict_v2.ntpz")
        shot = os.path.join(gt.OUT, "m97_shot.ntpz")

        first = make_v1(clean_v1)
        first.getParam(SIZE).set(CLEAN_V2_SIZE, CLEAN_V2_SIZE)
        blur("Blur3", 0, 400, app.getNode("Blur2"))
        require(app.saveProjectAs(clean_v2), "save template version 2 of the clean scenario")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)

        first = make_v1(conflict_v1)
        first.getParam(SIZE).set(CONFLICT_V2["Blur1"], CONFLICT_V2["Blur1"])
        app.getNode("Blur2").getParam(SIZE).set(CONFLICT_V2["Blur2"], CONFLICT_V2["Blur2"])
        require(app.saveProjectAs(conflict_v2), "save template version 2 of the conflicted scenario")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)

        gt.section("import and edit")
        shutil.copyfile(clean_v1, live)
        key = app.importTemplate(live)
        require(bool(key), "import template version 1")
        yield ("sleep", SETTLE_MS)
        blur1 = app.getNode("Blur1")
        blur2 = app.getNode("Blur2")
        require(blur1 is not None and blur2 is not None, "the import created Blur1 and Blur2")
        check(app.getNode("Blur3") is None, "version 1 has no Blur3")
        base_size = size_of(blur1)
        blur2.getParam(SIZE).set(CLEAN_ARTIST_SIZE, CLEAN_ARTIST_SIZE)
        yield ("sleep", SETTLE_MS)

        require(app.compareWithTemplateBase(key), "compare with the template's base")
        ok = yield ("until", lambda: app.getDiffTint(app.getNode("Blur2")) != "", POLL_TIMEOUT_S)
        check(ok, "the comparison with the template's base tints the edited Blur2")
        check(app.getDiffTint(app.getNode("Blur2")) == "changed", "Blur2 is tinted as changed (%r)" % app.getDiffTint(app.getNode("Blur2")))
        check(app.getDiffTint(app.getNode("Blur1")) == "", "the untouched Blur1 is not tinted (%r)" % app.getDiffTint(app.getNode("Blur1")))
        app.clearGraphComparison()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(app.getDiffGhostCount() == 0, "no ghost is drawn after clearing the comparison")

        gt.section("clean update")
        shutil.copyfile(clean_v2, live)
        before = snapshot(names)
        report("NOTE before the update %r" % (before,))
        check(before["Blur3"] is None, "Blur3 is absent before the update")
        count = app.updateFromTemplate(key)
        require(count == 0, "the update to version 2 raises no conflict (got %d)" % count)
        yield ("sleep", SETTLE_MS)
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
        if undo("Update from Template"):
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
        require(app.resetProject(), "reset the project for the conflicted scenario")
        yield ("sleep", SETTLE_MS)
        shutil.copyfile(conflict_v1, live)
        key = app.importTemplate(live)
        require(bool(key), "import the conflicted scenario's template version 1")
        yield ("sleep", SETTLE_MS)
        for name in ("Blur1", "Blur2"):
            node = app.getNode(name)
            require(node is not None, "the import created " + name)
            node.getParam(SIZE).set(CONFLICT_ARTIST[name], CONFLICT_ARTIST[name])
        shutil.copyfile(conflict_v2, live)
        count = app.updateFromTemplate(key)
        require(count == 2, "the update raises one conflict per edited size (got %d)" % count)
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
