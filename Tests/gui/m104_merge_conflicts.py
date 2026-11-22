"""The Merge Conflicts panel, its resolution buttons and the conflict badge, in the real application.

A template (Blur1 -> Blur2) is saved as version 1 and as version 2, where Blur1's size is 4. It is
imported, the artist sets Blur1's size to 2, and the update to version 2 raises conflicts on that
knob. The panel then lists them under Blur1 with the count in its tab, and Blur1 carries the badge.
One conflict is resolved with Theirs, Base, Mine and Accept in turn, each undone with Edit > Undo,
which brings the conflict back. Resolve All Theirs clears the list and the badge; undoing it brings
them back, and the project saved like that and reopened shows the panel with the same count.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m104_merge_conflicts.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
BLUR = "Blur"
SIZE = "size"
ARTIST_SIZE = 2.0
SUPERVISOR_SIZE = 4.0

try:
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QPushButton, QTabBar, QTreeWidget

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

    def panel():
        for w in gt.widgets_of("MergeConflictPanel"):
            return w
        return None

    def panel_shown():
        p = panel()
        return p is not None and p.isVisible()

    def tree():
        return widget(QTreeWidget, "mergeConflictTree")

    def button(name):
        return widget(QPushButton, name)

    def rows():
        """The conflict rows of the panel, as (node row text, conflict row item)."""
        t = tree()
        found = []
        if t is None:
            return found
        for g in range(t.topLevelItemCount()):
            group = t.topLevelItem(g)
            for i in range(group.childCount()):
                found.append((group.text(0), group.child(i)))
        return found

    def tab_label():
        """The text of the tab the panel is docked in, or "" when it is in none."""
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

    def size_of(node):
        param = node.getParam(SIZE)
        return (param.getValue(0), param.getValue(1))

    def click(name):
        b = button(name)
        if not check(b is not None and b.isEnabled(), "the %s button is enabled" % name):
            return False
        b.click()
        QApplication.processEvents()
        return True

    def select_first():
        t = tree()
        found = rows()
        if not check(t is not None and len(found) > 0, "the panel lists a conflict to select"):
            return None
        t.clearSelection()
        item = found[0][1]
        t.setCurrentItem(item)
        item.setSelected(True)
        QApplication.processEvents()
        return item.text(0)

    def dimension_of(text):
        """The dimension a value conflict's row names, or None."""
        marker = "dimension "
        if marker not in text:
            return None
        try:
            return int(text.split(marker, 1)[1].split()[0])
        except ValueError:
            return None

    def check_panel(count, label):
        found = rows()
        check(len(found) == count, "%s: the panel lists %d conflicts (got %d)" % (label, count, len(found)))
        tab = tab_label()
        if count:
            check(("(%d)" % count) in tab, "%s: the panel's tab shows the count (%r)" % (label, tab))
        else:
            check("(" not in tab, "%s: the panel's tab shows no count (%r)" % (label, tab))
        summary = widget(QLabel, "mergeConflictSummary")
        report("NOTE %s: summary %r" % (label, summary.text() if summary is not None else None))

    def steps():
        gt.section("templates")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        v1 = os.path.join(gt.OUT, "m104_template_v1.ntpz")
        v2 = os.path.join(gt.OUT, "m104_template_v2.ntpz")
        shot = os.path.join(gt.OUT, "m104_shot.ntpz")
        first = app.createNode(BLUR)
        require(first is not None, "create Blur1", gt.plugin_problem(BLUR))
        first.setScriptName("Blur1")
        second = app.createNode(BLUR)
        require(second is not None, "create Blur2", gt.plugin_problem(BLUR))
        second.setScriptName("Blur2")
        second.setPosition(0, 200)
        require(second.connectInput(0, first), "connect Blur1 to Blur2")
        base_size = size_of(first)
        require(app.saveProjectAs(v1), "save template version 1")
        first.getParam(SIZE).set(SUPERVISOR_SIZE, SUPERVISOR_SIZE)
        require(app.saveProjectAs(v2), "save template version 2")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)

        gt.section("update")
        key = app.importTemplate(v1)
        require(bool(key), "import template version 1")
        yield ("sleep", SETTLE_MS)
        blur1 = app.getNode("Blur1")
        blur2 = app.getNode("Blur2")
        require(blur1 is not None and blur2 is not None, "the import created Blur1 and Blur2")
        blur1.getParam(SIZE).set(ARTIST_SIZE, ARTIST_SIZE)
        check(not app.isMergeConflictBadgeShown(blur1), "Blur1 has no conflict badge before the update")

        count = app.updateFromTemplate(key, v2)
        require(count > 0, "the update to version 2 raises conflicts (got %d)" % count)
        ok = yield ("until", panel_shown, POLL_TIMEOUT_S)
        require(ok, "the Merge Conflicts panel is shown after the update")
        yield ("sleep", SETTLE_MS)
        check_panel(count, "after the update")
        check(all("Blur1" in group for group, _ in rows()), "every conflict is listed under Blur1")
        check(size_of(blur1) == (ARTIST_SIZE, ARTIST_SIZE), "the update kept the artist's size (%r)" % (size_of(blur1),))
        check(app.isMergeConflictBadgeShown(blur1), "Blur1 carries the conflict badge")
        check(not app.isMergeConflictBadgeShown(blur2), "Blur2 carries no conflict badge")
        gt.shot("m104_merge_conflicts.png")

        for side, expected in (("Theirs", SUPERVISOR_SIZE), ("Base", None), ("Mine", ARTIST_SIZE), ("Accept", ARTIST_SIZE)):
            gt.section("resolve " + side)
            text = select_first()
            require(text is not None, "select a conflict")
            dim = dimension_of(text)
            report("NOTE %s: resolving %r" % (side, text))
            require(click("mergeConflict" + side), "resolve with " + side)
            yield ("sleep", SETTLE_MS)
            check_panel(count - 1, "after " + side)
            value = size_of(blur1)
            if dim is not None:
                want = base_size[dim] if expected is None else expected
                check(abs(value[dim] - want) < 1e-9, "%s gives dimension %d %r (got %r)" % (side, dim, want, value))
                check(abs(value[1 - dim] - ARTIST_SIZE) < 1e-9, "%s leaves the other dimension alone (%r)" % (side, value))
            if undo("the %s resolution" % side):
                yield ("sleep", SETTLE_MS)
                check_panel(count, "after undoing " + side)
                check(size_of(blur1) == (ARTIST_SIZE, ARTIST_SIZE), "undoing %s restores the size (%r)" % (side, size_of(blur1)))
                check(app.isMergeConflictBadgeShown(blur1), "undoing %s keeps the badge" % side)

        gt.section("resolve all")
        require(click("mergeConflictAllTheirs"), "Resolve All Theirs")
        yield ("sleep", SETTLE_MS)
        check_panel(0, "after Resolve All Theirs")
        check(size_of(blur1) == (SUPERVISOR_SIZE, SUPERVISOR_SIZE), "Resolve All Theirs gives the template's size (%r)" % (size_of(blur1),))
        check(not app.isMergeConflictBadgeShown(blur1), "the badge goes away once every conflict is resolved")
        if undo("Resolve All Theirs"):
            yield ("sleep", SETTLE_MS)
            check_panel(count, "after undoing Resolve All Theirs")
            check(app.isMergeConflictBadgeShown(blur1), "undoing Resolve All Theirs brings the badge back")

        gt.section("reopen")
        require(app.saveProjectAs(shot), "save the shot with its conflicts")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        check(len(rows()) == 0, "the panel lists nothing after the reset")
        require(app.loadProject(shot) is not None, "reopen the shot")
        ok = yield ("until", lambda: app.getNode("Blur1") is not None and len(rows()) > 0, POLL_TIMEOUT_S)
        require(ok, "the reopened shot lists its conflicts")
        ok = yield ("until", panel_shown, POLL_TIMEOUT_S)
        check(ok, "opening a project with conflicts shows the Merge Conflicts panel")
        yield ("sleep", SETTLE_MS)
        check_panel(count, "after reopening")
        check(app.isMergeConflictBadgeShown(app.getNode("Blur1")), "the reopened Blur1 carries the badge")

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
