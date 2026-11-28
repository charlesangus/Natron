"""Picks from a graph comparison, each one undo step, in the real application.

A base project holds, side by side: A1 -> Mid -> A3; B1 -> P1 -> P2 -> B4; a Group Grp holding
GBlur; C1 -> S -> C3 with a lone Other; a lone Grade1; D1 -> D3; and a lone Mover. After it is
saved, Mid, P1, P2, Grp and S are deleted without reconnecting, C3 is fed from Other, Grade1's
multiply and gamma change, Added is inserted between D1 and D3 and Mover is moved. Comparing with
the base draws the five ghosts and the node graph's merge bar counts the changes.

Restore brings Mid back spliced between A1 and A3, and Edit > Undo puts its ghost back. A rubber band
selects the ghosts of P1 and P2, and Restore Selected on the merge bar brings them back as a chain.
Restoring Grp brings it back with GBlur inside. Restoring S leaves C3 on Other, since the artist
rewired it, and adds a splice choice on C3: Splice In feeds C3 from S, Edit > Undo brings the choice
back and Keep My Wiring keeps Other. Take on Grade1's multiply gives it the base value and leaves
gamma, Revert to Other gives gamma the base value too, Remove deletes Added and bridges D1 to D3,
and Restore Position puts Mover back. The merge bar's counts follow every pick, and Clear Comparison
hides the bar.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m649_compare_picks.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
BLUR = "Blur"
GRADE = "Grade"
GROUP = "Group"
SPACING = 150.0
COLUMN_B = 400.0
BAND_MARGIN = 20.0
DRAG_STEPS = 8
MOVE_BY = (300.0, 200.0)
POSITION_TOLERANCE = 1.0
GHOSTS = ["Grp", "Mid", "P1", "P2", "S"]

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, Q_ARG
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QPushButton, QWidget

    import NatronEngine

    import guitest as gt
    from guitest import check, report, require

    _held = []

    def hold(w):
        """Keeps w's wrapper alive for the rest of the run, so the wrappers of its children stay valid."""
        if w is not None:
            _held.append(w)
        return w

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

    def top_level_graph():
        """The project's own NodeGraph; a Group's graph is a NodeGraph too, named after its label."""
        labels = set(n.getLabel() for n in app.getChildren())
        graphs = [g for g in gt.widgets_of("NodeGraph") if g.objectName() not in labels]
        return hold(graphs[0]) if graphs else None

    def show_top_level_graph():
        """Makes the project's node graph the current tab of its pane and returns it."""
        graph = top_level_graph()
        if graph is None or graph.isVisible():
            return graph
        pane = graph.parentWidget()
        while pane is not None and gt.class_name(pane) != "TabWidget":
            pane = pane.parentWidget()
        if pane is None:
            return graph
        hold(pane)
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        for i in range(bars[0].count() if bars else 0):
            if "node graph" in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                break
        return graph

    def shown_widget(cls, name):
        """The visible widget of class cls called name: every node graph has its own merge bar."""
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, cls) and w.objectName() == name and w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def bar_counts():
        """The merge bar's added, changed, removed and moved counts, or None when it shows none."""
        label = shown_widget(QLabel, "mergeBarCounts")
        if label is None:
            return None
        found = {}
        for part in label.text().split(","):
            words = part.split()
            if len(words) == 2 and words[0].isdigit():
                found[words[1]] = int(words[0])
        return found if len(found) == 4 else None

    def counts_are(want):
        found = bar_counts()
        return found is not None and all(found.get(k) == v for k, v in want.items())

    _last_counts = {}

    def expect_counts(want, label):
        """Waits for the merge bar to show want, a dict of the counts that must match, and returns
        the full counts it shows, or the last ones it showed updated with want when it shows none."""
        ok = yield ("until", lambda: counts_are(want), POLL_TIMEOUT_S)
        check(ok, "%s: the merge bar counts %r (got %r)" % (label, want, bar_counts()))
        found = bar_counts()
        if found is None:
            found = dict(_last_counts)
            found.update(want)
        _last_counts.clear()
        _last_counts.update(found)
        return dict(found)

    def ghost_names():
        return sorted(str(g["name"]) for g in app.getComparisonGhosts())

    def selected_ghosts():
        return sorted(str(g["name"]) for g in app.getComparisonGhosts() if g["selected"])

    def blur(name, x, y, source=None, group=None):
        node = app.createNode(BLUR, -1, group) if group is not None else app.createNode(BLUR)
        require(node is not None, "create " + name, gt.plugin_problem(BLUR))
        node.setScriptName(name)
        node.setPosition(x, y)
        if source is not None:
            require(node.connectInput(0, source), "connect %s to %s" % (source.getScriptName(), name))
        return node

    def source_of(node):
        source = node.getInput(0) if node is not None else None
        return source.getScriptName() if source is not None else None

    def values_of(param):
        return tuple(param.getValue(d) for d in range(param.getNumDimensions()))

    def set_all(param, value):
        for d in range(param.getNumDimensions()):
            param.setValue(value, d)

    def close(a, b):
        return len(a) == len(b) and all(abs(x - y) < 1e-6 for x, y in zip(a, b))

    def splice_choices():
        return [c for c in app.getMergeConflicts() if str(c["kind"]) == "splice-choice" and str(c["node"]) == "C3"]

    def send(graph, etype, scene, button, buttons):
        port = graph.viewport()
        pos = QPointF(graph.mapFromScene(QPointF(scene[0], scene[1])))
        QApplication.sendEvent(port, QMouseEvent(etype, pos, QPointF(port.mapToGlobal(pos)), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def rubber_band(graph, start, end):
        send(graph, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            at = (start[0] + (end[0] - start[0]) * f, start[1] + (end[1] - start[1]) * f)
            send(graph, QEvent.MouseMove, at, Qt.NoButton, Qt.LeftButton)
        send(graph, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def column_b_ghost_rects():
        """The ghost rectangles drawn in B's column, those of P1 and P2."""
        rects = []
        for i in range(app.getDiffGhostCount()):
            r = app.getDiffGhostRect(i)
            if abs((r.x1 + r.x2) / 2.0 - COLUMN_B) < SPACING:
                rects.append(r)
        return rects

    def steps():
        gt.section("base")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        base = os.path.join(gt.OUT, "m649_compare_base.ntpz")
        a1 = blur("A1", 0, 0)
        mid = blur("Mid", 0, SPACING, a1)
        a3 = blur("A3", 0, 2 * SPACING, mid)
        b1 = blur("B1", COLUMN_B, 0)
        p1 = blur("P1", COLUMN_B, SPACING, b1)
        p2 = blur("P2", COLUMN_B, 2 * SPACING, p1)
        b4 = blur("B4", COLUMN_B, 3 * SPACING, p2)
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        grp = app.createNode(GROUP, -1, None, props)
        require(grp is not None, "create Grp", gt.plugin_problem(GROUP))
        grp.setScriptName("Grp")
        grp.setPosition(800, 0)
        blur("GBlur", 0, 0, None, grp)
        c1 = blur("C1", 1100, 0)
        s = blur("S", 1100, SPACING, c1)
        c3 = blur("C3", 1100, 2 * SPACING, s)
        other = blur("Other", 1300, SPACING)
        grade = app.createNode(GRADE)
        require(grade is not None, "create Grade1", gt.plugin_problem(GRADE))
        grade.setScriptName("Grade1")
        grade.setPosition(1600, 0)
        multiply = grade.getParam("multiply")
        gamma = grade.getParam("gamma")
        require(multiply is not None and gamma is not None, "Grade1 has multiply and gamma")
        base_multiply = values_of(multiply)
        base_gamma = values_of(gamma)
        d1 = blur("D1", 1900, 0)
        d3 = blur("D3", 1900, 2 * SPACING, d1)
        mover = blur("Mover", 2200, 0)
        base_mover = tuple(mover.getPosition())
        yield ("sleep", SETTLE_MS)
        require(app.saveProjectAs(base), "save the base")

        gt.section("edits")
        for node in (mid, p1, p2, grp, s):
            node.destroy(False)
        yield ("sleep", SETTLE_MS)
        require(c3.connectInput(0, other), "feed C3 from Other")
        set_all(multiply, 2.0)
        set_all(gamma, 0.5)
        added = blur("Added", 1900, SPACING, d1)
        d3.disconnectInput(0)
        require(d3.connectInput(0, added), "feed D3 from Added")
        mover.setPosition(base_mover[0] + MOVE_BY[0], base_mover[1] + MOVE_BY[1])
        yield ("sleep", SETTLE_MS)

        gt.section("compare")
        require(app.compareWithFile(base), "compare with the base")
        ok = yield ("until", lambda: len(app.getComparisonGhosts()) == len(GHOSTS), POLL_TIMEOUT_S)
        check(ok, "the comparison draws %d ghosts (%r)" % (len(GHOSTS), ghost_names()))
        check(ghost_names() == GHOSTS, "the ghosts are those of the deleted nodes (%r)" % ghost_names())
        check(app.mergeBarMode() == "comparison", "the merge bar is in comparison mode (%r)" % app.mergeBarMode())
        graph = show_top_level_graph()
        require(graph is not None, "the node graph is up")
        counts = yield from expect_counts({"added": 1, "changed": 5, "moved": 1}, "after comparing")
        report("NOTE counts after comparing %r" % counts)
        require(bar_counts() is not None, "the merge bar shows added, changed, removed and moved counts")
        check(counts.get("removed", 0) >= len(GHOSTS), "the merge bar counts at least the %d removed nodes (%r)" % (len(GHOSTS), counts))
        for name, tint in (("Grade1", "changed"), ("Added", "added"), ("Mover", "moved"), ("A3", "changed")):
            check(app.getDiffTint(app.getNode(name)) == tint, "%s is marked %s (%r)" % (name, tint, app.getDiffTint(app.getNode(name))))
        gt.shot("m649_compare_toolbar.png")

        gt.section("restore spliced")
        before = dict(counts)
        require(app.restoreFromComparison(["Mid"]), "Restore Mid")
        yield ("sleep", SETTLE_MS)
        mid = app.getNode("Mid")
        require(mid is not None, "Restore brings Mid back")
        check(source_of(mid) == "A1", "the restored Mid is fed by A1 (%r)" % source_of(mid))
        check(source_of(app.getNode("A3")) == "Mid", "the restored Mid is spliced into A3 (%r)" % source_of(app.getNode("A3")))
        check("Mid" not in ghost_names(), "Mid's ghost is gone (%r)" % ghost_names())
        yield from expect_counts({"added": before["added"], "changed": before["changed"] - 1,
                                  "removed": before["removed"] - 1, "moved": before["moved"]}, "after Restore")
        if undo("Restore"):
            yield ("sleep", SETTLE_MS)
            check(app.getNode("Mid") is None, "undoing Restore removes Mid again")
            ok = yield ("until", lambda: "Mid" in ghost_names(), POLL_TIMEOUT_S)
            check(ok, "undoing Restore puts Mid's ghost back (%r)" % ghost_names())
            check(source_of(app.getNode("A3")) is None, "undoing Restore leaves A3 unfed (%r)" % source_of(app.getNode("A3")))
            counts = yield from expect_counts(before, "after undoing Restore")

        gt.section("restore a chain")
        before = dict(counts)
        win = main_window()
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        report("NOTE the main window is active: %r" % ok)
        graph = show_top_level_graph()
        rects = column_b_ghost_rects()
        check(len(rects) == 2, "two ghosts are drawn in B's column, P1 and P2 (got %d)" % len(rects))
        if rects:
            left = min(r.x1 for r in rects) - BAND_MARGIN
            top = min(r.y1 for r in rects) - BAND_MARGIN
            right = max(r.x2 for r in rects) + BAND_MARGIN
            bottom = max(r.y2 for r in rects) + BAND_MARGIN
            graph.resetTransform()
            graph.centerOn(QPointF((left + right) / 2.0, (top + bottom) / 2.0))
            QApplication.processEvents()
            rubber_band(graph, (left, top), (right, bottom))
            yield ("sleep", SETTLE_MS)
        check(selected_ghosts() == ["P1", "P2"], "the rubber band selects the ghosts of P1 and P2 (%r)" % selected_ghosts())
        check(all(n.getScriptName() not in ("B1", "B4") for n in app.getSelectedNodes()),
              "the rubber band selects neither B1 nor B4")
        gt.shot("m649_selected_ghosts.png")
        if selected_ghosts() != ["P1", "P2"]:
            app.selectComparisonGhosts(["P1", "P2"])
            QApplication.processEvents()
        restore = shown_widget(QPushButton, "mergeBarRestoreSelected")
        require(restore is not None and restore.isEnabled(), "the merge bar's Restore Selected is enabled")
        restore.click()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        require(app.getNode("P1") is not None and app.getNode("P2") is not None, "Restore Selected brings P1 and P2 back")
        check(source_of(app.getNode("P1")) == "B1", "P1 is fed by B1 (%r)" % source_of(app.getNode("P1")))
        check(source_of(app.getNode("P2")) == "P1", "P2 is fed by P1, as a chain (%r)" % source_of(app.getNode("P2")))
        check(source_of(app.getNode("B4")) == "P2", "B4 is fed by P2 (%r)" % source_of(app.getNode("B4")))
        counts = yield from expect_counts({"added": before["added"], "changed": before["changed"] - 1,
                                           "removed": before["removed"] - 2, "moved": before["moved"]}, "after Restore Selected")

        gt.section("restore a group")
        before = dict(counts)
        require(app.restoreFromComparison(["Grp"]), "Restore Grp")
        yield ("sleep", SETTLE_MS)
        check(app.getNode("Grp") is not None, "Restore brings Grp back")
        check(app.getNode("Grp.GBlur") is not None, "the restored Grp holds GBlur")
        check("Grp" not in ghost_names(), "Grp's ghost is gone (%r)" % ghost_names())
        show_top_level_graph()
        ok = yield ("until", lambda: counts_are({"added": before["added"], "changed": before["changed"], "moved": before["moved"]})
                    and bar_counts()["removed"] < before["removed"], POLL_TIMEOUT_S)
        check(ok, "after restoring Grp the merge bar counts fewer removed nodes (%r, was %r)" % (bar_counts(), before))
        counts = bar_counts() or before
        _last_counts.clear()
        _last_counts.update(counts)

        gt.section("pending splice")
        before = dict(counts)
        require(app.restoreFromComparison(["S"]), "Restore S")
        yield ("sleep", SETTLE_MS)
        check(app.getNode("S") is not None, "Restore brings S back")
        check(source_of(app.getNode("S")) == "C1", "the restored S is fed by C1 (%r)" % source_of(app.getNode("S")))
        check(source_of(app.getNode("C3")) == "Other", "Restore leaves the rewired C3 on Other (%r)" % source_of(app.getNode("C3")))
        choices = splice_choices()
        require(len(choices) == 1, "Restore adds one splice choice on C3 (%r)" % (app.getMergeConflicts(),))
        report("NOTE splice choice %r" % (choices[0],))
        check(list(choices[0]["choices"]) == ["mine", "theirs"], "the splice choice offers mine and theirs (%r)" % (choices[0]["choices"],))
        check([str(c) for c in choices[0]["choiceLabels"]] == ["Keep My Wiring", "Splice In"],
              "the splice choice reads Keep My Wiring and Splice In (%r)" % (choices[0]["choiceLabels"],))
        check(app.mergeBarMode() == "comparison", "the comparison keeps the merge bar (%r)" % app.mergeBarMode())
        counts = yield from expect_counts({"added": before["added"], "changed": before["changed"],
                                           "removed": before["removed"] - 1, "moved": before["moved"]}, "after Restore S")
        require(app.resolveMergeConflict(int(choices[0]["index"]), "theirs"), "Splice In")
        yield ("sleep", SETTLE_MS)
        check(source_of(app.getNode("C3")) == "S", "Splice In feeds C3 from S (%r)" % source_of(app.getNode("C3")))
        check(len(splice_choices()) == 0, "Splice In resolves the splice choice")
        if undo("Splice In"):
            yield ("sleep", SETTLE_MS)
            check(source_of(app.getNode("C3")) == "Other", "undoing Splice In feeds C3 from Other again (%r)" % source_of(app.getNode("C3")))
            choices = splice_choices()
            check(len(choices) == 1, "undoing Splice In brings the splice choice back")
        if choices:
            require(app.resolveMergeConflict(int(choices[0]["index"]), "mine"), "Keep My Wiring")
            yield ("sleep", SETTLE_MS)
            check(source_of(app.getNode("C3")) == "Other", "Keep My Wiring keeps C3 on Other (%r)" % source_of(app.getNode("C3")))
            check(len(splice_choices()) == 0, "Keep My Wiring resolves the splice choice")

        gt.section("take")
        grade = app.getNode("Grade1")
        require(app.takeKnobFromComparison(grade, "multiply"), "Take multiply on Grade1")
        yield ("sleep", SETTLE_MS)
        check(close(values_of(grade.getParam("multiply")), base_multiply), "Take gives multiply the base value (%r)" % (values_of(grade.getParam("multiply")),))
        check(all(abs(v - 0.5) < 1e-6 for v in values_of(grade.getParam("gamma"))), "Take leaves gamma alone (%r)" % (values_of(grade.getParam("gamma")),))
        check(app.getDiffTint(grade) == "changed", "Grade1 is still marked changed (%r)" % app.getDiffTint(grade))
        counts = yield from expect_counts(counts, "after Take")

        gt.section("revert")
        before = dict(counts)
        require(app.revertFromComparison([grade]), "Revert to Other on Grade1")
        yield ("sleep", SETTLE_MS)
        check(close(values_of(grade.getParam("gamma")), base_gamma), "Revert to Other gives gamma the base value (%r)" % (values_of(grade.getParam("gamma")),))
        check(app.getDiffTint(grade) == "", "Grade1 is no longer marked (%r)" % app.getDiffTint(grade))
        counts = yield from expect_counts({"added": before["added"], "changed": before["changed"] - 1,
                                           "removed": before["removed"], "moved": before["moved"]}, "after Revert to Other")

        gt.section("remove")
        before = dict(counts)
        require(app.removeAddedFromComparison([app.getNode("Added")]), "Remove Added")
        yield ("sleep", SETTLE_MS)
        check(app.getNode("Added") is None, "Remove deletes Added")
        check(source_of(app.getNode("D3")) == "D1", "Remove bridges D1 to D3 (%r)" % source_of(app.getNode("D3")))
        counts = yield from expect_counts({"added": before["added"] - 1, "changed": before["changed"] - 1,
                                           "removed": before["removed"], "moved": before["moved"]}, "after Remove")

        gt.section("restore position")
        before = dict(counts)
        mover = app.getNode("Mover")
        require(app.restorePositionFromComparison([mover]), "Restore Position on Mover")
        yield ("sleep", SETTLE_MS)
        x, y = mover.getPosition()
        check(abs(x - base_mover[0]) <= POSITION_TOLERANCE and abs(y - base_mover[1]) <= POSITION_TOLERANCE,
              "Restore Position puts Mover back at %r (got %r)" % (base_mover, (x, y)))
        check(app.getDiffTint(mover) == "", "Mover is no longer marked (%r)" % app.getDiffTint(mover))
        counts = yield from expect_counts({"added": before["added"], "changed": before["changed"],
                                           "removed": before["removed"], "moved": before["moved"] - 1}, "after Restore Position")
        report("NOTE counts at the end %r" % counts)

        gt.section("clear")
        show_top_level_graph()
        clear = shown_widget(QPushButton, "mergeBarClearComparison")
        require(clear is not None and clear.isEnabled(), "the merge bar's Clear Comparison is enabled")
        clear.click()
        QApplication.processEvents()
        ok = yield ("until", lambda: app.mergeBarMode() == "none", POLL_TIMEOUT_S)
        check(ok, "Clear Comparison hides the merge bar (%r)" % app.mergeBarMode())
        check(len(app.getComparisonGhosts()) == 0, "Clear Comparison removes the ghosts")

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
