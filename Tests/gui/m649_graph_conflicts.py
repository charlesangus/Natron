"""Merge conflicts resolved on the node graph, in the real application.

A template holds, far apart: Val; Key, whose size has a key at frame 1; and Up -> Rd. It is saved as
version 1, and as version 2 where Val's size is 4, Key's size gets a key of 4 at frame 10 and Rd is
fed from a new Feed. It is imported, the artist sets Val's size to 2, keys Key's size to 2 at frame
10 and feeds Rd from Artist, and the update to version 2 raises a value conflict on Val, a key
conflict at one frame on Key and an input conflict on Rd.

The Merge Conflicts panel does not open, the merge bar is in conflict mode and the graph frames the
first conflicted node. Clicking Val's badge opens the popover with the choices the conflict lists;
Theirs resolves it and closes the popover, Edit > Undo brings it back, and Theirs again resolves
it. Key's popover offers Mine and Theirs and no Keep Both Keys, and All Mine resolves its conflicts
and is undone. Alt+Down and Alt+Up frame the conflicted nodes in turn and open their popovers. On
Rd's popover, hovering Theirs draws its preview wire in full, and clicking the end of that wire
feeds Rd from Feed; Edit > Undo feeds it from Artist again.

A comparison with a saved copy, after Val's size and Up's position change, hides the badges and
puts the merge bar in comparison mode; Alt+Shift+Down and Alt+Shift+Up frame and select the
changed nodes. The Node Diff panel's Take on Val's size gives it the saved value as one undo step,
its Revert Node does the same for the whole node, and Up, which only moved, gets a disabled Revert
Node and no Take. Clearing the comparison brings the badges back. All Theirs on the merge bar
resolves everything and is undone, and the project saved and reopened opens no panel and still
draws Rd's structural preview.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m649_graph_conflicts.py
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
EDITED_SIZE = 7.0
BASE_KEY = (1.0, 1.0)
KEY_FRAME = 10.0
FAR = 1500.0
MOVE_BY = (0.0, -300.0)
# Where NodeGui puts the conflict badge's centre: left of the node's top-right corner by half the
# warning ellipse, half the link badge and a 2 px gap.
BADGE_OFFSET = 10.0 / 2.0 + 18.0 / 2.0 + 2.0
SAMPLE_RADIUS = 3

try:
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QEnterEvent, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QFrame, QLabel, QMainWindow, QPushButton

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
        labels = set(n.getLabel() for n in app.getChildren())
        graphs = [g for g in gt.widgets_of("NodeGraph") if g.objectName() not in labels]
        return hold(graphs[0]) if graphs else None

    def widget(cls, name, shown=False):
        """The widget of class cls called name, the visible one when shown: every node graph has
        its own merge bar and popover."""
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, cls) and w.objectName() == name and (w.isVisible() or not shown):
                    return w
            except RuntimeError:
                continue
        return None

    def panel_shown():
        return any(p.isVisible() for p in gt.widgets_of("MergeConflictPanel"))

    def bar_count_text():
        label = widget(QLabel, "mergeBarConflictCount", True)
        return label.text() if label is not None else None

    def bar_counts(count):
        want = "1 merge conflict" if count == 1 else "%d merge conflicts" % count
        return app.mergeBarMode() == "conflicts" and bar_count_text() == want

    def conflicts():
        return list(app.getMergeConflicts())

    def conflicts_on(name):
        return [c for c in conflicts() if str(c["node"]) == name]

    def conflicted_names():
        names = []
        for c in conflicts():
            if str(c["node"]) not in names:
                names.append(str(c["node"]))
        return names

    def rect_of(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x, y, w, h)

    def centre_of(rect):
        return (rect[0] + rect[2] / 2.0, rect[1] + rect[3] / 2.0)

    def edge_point(rect, outside):
        """Where the segment from rect's centre towards outside leaves rect, as the previews draw it."""
        cx, cy = centre_of(rect)
        dx = outside[0] - cx
        dy = outside[1] - cy
        if dx == 0.0 and dy == 0.0:
            return (cx, cy)
        sx = (rect[2] / 2.0) / abs(dx) if dx != 0.0 else 1e300
        sy = (rect[3] / 2.0) / abs(dy) if dy != 0.0 else 1e300
        s = min(1.0, sx, sy)
        return (cx + dx * s, cy + dy * s)

    def wire(source, reader):
        """The ends of the preview wire from source to reader, in node coordinates."""
        s = rect_of(source)
        r = rect_of(reader)
        return edge_point(s, centre_of(r)), edge_point(r, centre_of(s))

    def view_point(graph, scene):
        return graph.mapFromScene(QPointF(scene[0], scene[1]))

    def framed(node):
        """Whether node's centre lies within a few pixels of the middle of the node graph's view."""
        graph = top_level_graph()
        if graph is None or node is None:
            return False
        at = view_point(graph, centre_of(rect_of(node)))
        port = graph.viewport().rect()
        return (abs(at.x() - port.center().x()) <= port.width() / 4.0
                and abs(at.y() - port.center().y()) <= port.height() / 4.0)

    def framed_name(names):
        found = [n for n in names if framed(app.getNode(n))]
        return found[0] if len(found) == 1 else None

    def look_at(node):
        graph = top_level_graph()
        graph.resetTransform()
        c = centre_of(rect_of(node))
        graph.centerOn(QPointF(c[0], c[1]))
        QApplication.processEvents()

    def click_at(graph, scene):
        port = graph.viewport()
        pos = QPointF(view_point(graph, scene))
        glob = QPointF(port.mapToGlobal(pos))
        QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonPress, pos, glob, Qt.LeftButton, Qt.LeftButton, Qt.NoModifier))
        QApplication.processEvents()
        QApplication.sendEvent(port, QMouseEvent(QEvent.MouseButtonRelease, pos, glob, Qt.LeftButton, Qt.NoButton, Qt.NoModifier))
        QApplication.processEvents()

    def press(graph, key, modifiers):
        QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def popover():
        return widget(QFrame, "MergePopover", True)

    def popover_buttons():
        """The texts of the popover's choice buttons. The popover deletes its buttons on every
        refresh, so only plain values leave this function."""
        p = popover()
        if p is None:
            return []
        return [b.text() for b in p.findChildren(QPushButton)
                if b.objectName().startswith("mergePopoverChoice_") and b.isVisible()]

    def choice_in(p, text):
        for b in p.findChildren(QPushButton):
            if b.objectName().startswith("mergePopoverChoice_") and b.text() == text and b.isVisible():
                return b
        return None

    def named_in(p, name):
        for b in p.findChildren(QPushButton):
            if b.objectName() == name and b.isVisible():
                return b
        return None

    def popover_has(name):
        p = popover()
        return p is not None and named_in(p, name) is not None

    def click_popover_choice(text, label):
        p = popover()
        return click_button(choice_in(p, text) if p is not None else None, label)

    def click_popover_named(name, label):
        p = popover()
        return click_button(named_in(p, name) if p is not None else None, label)

    def hover_popover_choice(text, entering):
        p = popover()
        b = choice_in(p, text) if p is not None else None
        if b is not None:
            hover(b, entering)
        return b is not None

    def click_button(b, label):
        if not check(b is not None and b.isEnabled(), "%s is shown and enabled" % label):
            return False
        b.click()
        QApplication.processEvents()
        return True

    def hover(b, entering):
        if entering:
            centre = QPointF(b.rect().center())
            QApplication.sendEvent(b, QEnterEvent(centre, centre, QPointF(b.mapToGlobal(centre))))
        else:
            QApplication.sendEvent(b, QEvent(QEvent.Leave))
        QApplication.processEvents()

    def preview_wires_shown():
        graph = top_level_graph()
        if graph is None or graph.scene() is None:
            return False
        for item in graph.scene().items():
            obj = item.toGraphicsObject()
            if obj is not None and obj.objectName() == "MergeConflictPreviewWires":
                return obj.isVisible()
        return False

    def pixels_around(graph, scene):
        """The colours of the view's pixels around scene, as plain tuples."""
        image = graph.viewport().grab().toImage()
        at = view_point(graph, scene)
        found = []
        for dx in range(-SAMPLE_RADIUS, SAMPLE_RADIUS + 1):
            for dy in range(-SAMPLE_RADIUS, SAMPLE_RADIUS + 1):
                x, y = at.x() + dx, at.y() + dy
                if 0 <= x < image.width() and 0 <= y < image.height():
                    c = image.pixelColor(x, y)
                    found.append((c.red(), c.green(), c.blue()))
        return found

    def size_of(node):
        param = node.getParam(SIZE)
        return (param.getValue(0), param.getValue(1))

    def size_is(node, value):
        return all(abs(v - value) < 1e-9 for v in size_of(node))

    def key_at(node):
        return node.getParam(SIZE).getValueAtTime(KEY_FRAME, 0)

    def source_of(node):
        source = node.getInput(0)
        return source.getScriptName() if source is not None else None

    def blur(name, x, y, source=None):
        node = app.createNode(BLUR)
        require(node is not None, "create " + name, gt.plugin_problem(BLUR))
        node.setScriptName(name)
        node.setPosition(x, y)
        if source is not None:
            require(node.connectInput(0, source), "connect %s to %s" % (source.getScriptName(), name))
        return node

    def diff_rows():
        return [str(r) for r in app.getNodeDiffRows()]

    def steps():
        gt.section("templates")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        v1 = os.path.join(gt.OUT, "m649_conflicts_template_v1.ntpz")
        v2 = os.path.join(gt.OUT, "m649_conflicts_template_v2.ntpz")
        snapshot = os.path.join(gt.OUT, "m649_conflicts_snapshot.ntpz")
        shot_file = os.path.join(gt.OUT, "m649_conflicts_shot.ntpz")
        val = blur("Val", 0, 0)
        key = blur("Key", FAR, 0)
        up = blur("Up", 2 * FAR, 0)
        rd = blur("Rd", 2 * FAR, 300, up)
        for d in range(2):
            key.getParam(SIZE).setValueAtTime(BASE_KEY[d], 1.0, d)
        require(app.saveProjectAs(v1), "save template version 1")
        val.getParam(SIZE).set(SUPERVISOR_SIZE, SUPERVISOR_SIZE)
        for d in range(2):
            key.getParam(SIZE).setValueAtTime(SUPERVISOR_SIZE, KEY_FRAME, d)
        feed = blur("Feed", 2 * FAR - 150, 150)
        rd.disconnectInput(0)
        require(rd.connectInput(0, feed), "connect Feed to Rd")
        require(app.saveProjectAs(v2), "save template version 2")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)

        gt.section("update")
        artist = blur("Artist", 2 * FAR - 300, 150)
        template = app.importTemplateAsUndoStep(v1)
        require(bool(template), "import template version 1")
        yield ("sleep", SETTLE_MS)
        val, key, up, rd = (app.getNode(n) for n in ("Val", "Key", "Up", "Rd"))
        require(None not in (val, key, up, rd), "the import created Val, Key, Up and Rd")
        # The import may place the template elsewhere; Artist keeps its place beside Rd.
        ax, ay = rd.getPosition()
        artist.setPosition(ax - 300, ay - 150)
        val.getParam(SIZE).set(ARTIST_SIZE, ARTIST_SIZE)
        for d in range(2):
            key.getParam(SIZE).setValueAtTime(ARTIST_SIZE, KEY_FRAME, d)
        rd.disconnectInput(0)
        require(rd.connectInput(0, artist), "connect Artist to Rd")
        check(app.mergeBarMode() == "none", "the merge bar is hidden before the update (%r)" % app.mergeBarMode())

        # The versions are plain scripts, not published versions of the template.
        status = app.updateFromTemplate(template, v2, True)
        require(status == "updated", "the update to version 2 merges (got %r)" % status)
        count = len(conflicts())
        require(count >= 3, "the update raises a conflict on each of Val, Key and Rd (got %d)" % count)
        report("NOTE conflicts %r" % ([(str(c["node"]), str(c["kind"]), str(c["label"])) for c in conflicts()],))
        names = conflicted_names()
        check(sorted(names) == ["Key", "Rd", "Val"], "the conflicts are on Val, Key and Rd (%r)" % names)
        feed = app.getNode("Feed")
        require(feed is not None, "the update added Feed")
        check(source_of(rd) == "Artist", "the update kept the artist's input on Rd (%r)" % source_of(rd))
        ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
        check(ok, "the merge bar is in conflict mode and counts the conflicts (%r, %r)" % (app.mergeBarMode(), bar_count_text()))
        graph = top_level_graph()
        require(graph is not None, "the node graph is up")
        check(framed(app.getNode(names[0])), "the update frames %s, the first conflicted node" % names[0])
        yield ("sleep", SETTLE_MS)
        check(not panel_shown(), "the Merge Conflicts panel does not open by itself after the update")
        check(QApplication.activeModalWidget() is None, "the update raises no modal dialog")
        for name in names:
            check(app.isMergeConflictBadgeShown(app.getNode(name)), "%s carries the conflict badge" % name)
        gt.shot("m649_conflict_toolbar.png")

        gt.section("badge popover")
        win = main_window()
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        report("NOTE the main window is active: %r" % ok)
        look_at(val)
        x, y, w, h = rect_of(val)
        click_at(graph, (x + w - BADGE_OFFSET, y))
        ok = yield ("until", app.isMergePopoverOpen, POLL_TIMEOUT_S)
        check(ok, "clicking Val's badge opens the merge popover")
        val_conflicts = conflicts_on("Val")
        require(len(val_conflicts) == 1, "Val has one conflict (%r)" % (val_conflicts,))
        labels = [str(l) for l in val_conflicts[0]["choiceLabels"]]
        check(popover_buttons() == labels, "the popover offers the conflict's choices %r (got %r)" % (labels, popover_buttons()))
        check(popover_has("mergePopoverAllMine") and popover_has("mergePopoverAllTheirs"),
              "the popover offers All Mine and All Theirs")
        check(QApplication.activeModalWidget() is None, "the popover is not modal")
        gt.shot("m649_merge_popover.png")
        for attempt in ("first", "again"):
            require(click_popover_choice("Theirs", "Theirs on Val's popover"), "resolve Val with Theirs")
            yield ("sleep", SETTLE_MS)
            check(size_is(val, SUPERVISOR_SIZE), "Theirs gives Val the template's size (%r)" % (size_of(val),))
            check(len(conflicts_on("Val")) == 0, "Theirs resolves Val's conflict")
            ok = yield ("until", lambda: not app.isMergePopoverOpen(), POLL_TIMEOUT_S)
            check(ok, "the popover closes with the node's last conflict")
            check(bar_counts(count - 1), "the merge bar counts one conflict fewer (%r)" % bar_count_text())
            check(not app.isMergeConflictBadgeShown(val), "Val loses its badge")
            if attempt == "again" or not undo("Theirs on Val"):
                break
            yield ("sleep", SETTLE_MS)
            check(size_is(val, ARTIST_SIZE), "undoing Theirs gives Val the artist's size again (%r)" % (size_of(val),))
            check(len(conflicts_on("Val")) == 1, "undoing Theirs brings Val's conflict back")
            ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
            check(ok, "undoing Theirs restores the merge bar's count (%r)" % bar_count_text())
            require(app.openMergePopover(val), "reopen Val's popover")
            QApplication.processEvents()
        count = len(conflicts())

        gt.section("key conflict")
        key_conflicts = [c for c in conflicts_on("Key") if str(c["kind"]) == "curve-key"]
        require(len(key_conflicts) > 0, "Key has key conflicts (%r)" % (conflicts_on("Key"),))
        for c in key_conflicts:
            choices = [str(n) for n in c["choices"]]
            report("NOTE key conflict %r offers %r %r" % (str(c["label"]), choices, [str(l) for l in c["choiceLabels"]]))
            check("mine" in choices and "theirs" in choices, "a key conflict at one frame offers Mine and Theirs (%r)" % choices)
            check(all(n in ("mine", "theirs", "base") for n in choices),
                  "a key conflict at one frame offers no Keep Both Keys (%r)" % choices)
        require(app.openMergePopover(key), "open Key's popover")
        QApplication.processEvents()
        texts = popover_buttons()
        check(len(texts) > 0 and not any("both" in t.lower() for t in texts), "Key's popover offers no Keep Both Keys (%r)" % texts)
        key_count = len(conflicts_on("Key"))
        require(click_popover_named("mergePopoverAllMine", "All Mine on Key's popover"), "All Mine on Key")
        yield ("sleep", SETTLE_MS)
        check(len(conflicts_on("Key")) == 0, "All Mine resolves every conflict of Key")
        check(abs(key_at(key) - ARTIST_SIZE) < 1e-9, "All Mine keeps the artist's key at frame %g (%r)" % (KEY_FRAME, key_at(key)))
        ok = yield ("until", lambda: not app.isMergePopoverOpen(), POLL_TIMEOUT_S)
        check(ok, "the popover closes once Key has nothing left to choose")
        if undo("All Mine on Key"):
            yield ("sleep", SETTLE_MS)
            check(len(conflicts_on("Key")) == key_count, "undoing All Mine brings Key's conflicts back")
            ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
            check(ok, "undoing All Mine restores the merge bar's count (%r)" % bar_count_text())

        gt.section("navigation")
        remaining = conflicted_names()
        check(sorted(remaining) == ["Key", "Rd"], "Key and Rd are left with conflicts (%r)" % remaining)
        graph.resetTransform()
        QApplication.processEvents()
        seen = []
        for _ in range(2):
            press(graph, Qt.Key_Down, Qt.AltModifier)
            yield ("sleep", SETTLE_MS)
            seen.append(framed_name(remaining))
            check(app.isMergePopoverOpen(), "Alt+Down opens the popover of the node it frames")
        check(sorted(n for n in seen if n) == sorted(remaining), "Alt+Down frames each conflicted node in turn (%r)" % seen)
        press(graph, Qt.Key_Up, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        check(seen[0] is not None and framed_name(remaining) == seen[0], "Alt+Up frames the previous conflicted node (%r)" % framed_name(remaining))
        check(app.isMergePopoverOpen(), "Alt+Up opens the popover of the node it frames")
        p = popover()
        if p is not None:
            QApplication.sendEvent(p, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
            QApplication.processEvents()
        check(not app.isMergePopoverOpen(), "Escape closes the popover")

        gt.section("structural preview")
        look_at(rd)
        require(app.openMergePopover(rd), "open Rd's popover")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(preview_wires_shown(), "while Rd's popover is open its sides are drawn")
        theirs_start, theirs_end = wire(feed, rd)
        middle = ((theirs_start[0] + theirs_end[0]) / 2.0, (theirs_start[1] + theirs_end[1]) / 2.0)
        faint = pixels_around(graph, middle)
        require(hover_popover_choice("Theirs", True), "Rd's popover offers Theirs (%r)" % popover_buttons())
        yield ("sleep", SETTLE_MS)
        full = pixels_around(graph, middle)
        check(len(faint) > 0 and faint != full, "hovering Theirs draws its wire from Feed in full")
        gt.shot("m649_structural_preview.png")
        hover_popover_choice("Theirs", False)
        yield ("sleep", SETTLE_MS)
        click_at(graph, theirs_end)
        yield ("sleep", SETTLE_MS)
        check(source_of(rd) == "Feed", "clicking the end of Theirs' preview wire feeds Rd from Feed (%r)" % source_of(rd))
        check(len(conflicts_on("Rd")) == 0, "clicking the preview resolves Rd's conflict")
        if undo("the preview pick"):
            yield ("sleep", SETTLE_MS)
            check(source_of(rd) == "Artist", "undoing the preview pick feeds Rd from Artist again (%r)" % source_of(rd))
            check(len(conflicts_on("Rd")) == 1, "undoing the preview pick brings Rd's conflict back")
        ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
        check(ok, "the merge bar counts %d conflicts (%r)" % (count, bar_count_text()))

        gt.section("comparison")
        require(app.saveProjectAs(snapshot), "save a copy to compare with")
        val.getParam(SIZE).set(EDITED_SIZE, EDITED_SIZE)
        ux, uy = up.getPosition()
        up.setPosition(ux + MOVE_BY[0], uy + MOVE_BY[1])
        yield ("sleep", SETTLE_MS)
        require(app.compareWithFile(snapshot), "compare with the saved copy")
        ok = yield ("until", lambda: app.mergeBarMode() == "comparison", POLL_TIMEOUT_S)
        check(ok, "a comparison puts the merge bar in comparison mode (%r)" % app.mergeBarMode())
        check(app.getDiffTint(val) == "changed" and app.getDiffTint(up) == "moved",
              "Val is changed and Up moved (%r, %r)" % (app.getDiffTint(val), app.getDiffTint(up)))
        for name in remaining:
            check(not app.isMergeConflictBadgeShown(app.getNode(name)), "the comparison hides %s's badge" % name)
        graph.resetTransform()
        QApplication.processEvents()
        selected = []
        for mods, label in ((Qt.AltModifier | Qt.ShiftModifier, "Alt+Shift+Down"), (Qt.AltModifier | Qt.ShiftModifier, "Alt+Shift+Down")):
            press(graph, Qt.Key_Down, mods)
            yield ("sleep", SETTLE_MS)
            names_now = [n.getScriptName() for n in app.getSelectedNodes()]
            selected.append(names_now[0] if len(names_now) == 1 else None)
            check(selected[-1] is not None and framed(app.getNode(selected[-1])), "%s frames and selects a changed node (%r)" % (label, names_now))
        check(sorted(n for n in selected if n) == ["Up", "Val"], "Alt+Shift+Down visits Val and Up (%r)" % selected)
        press(graph, Qt.Key_Up, Qt.AltModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        names_now = [n.getScriptName() for n in app.getSelectedNodes()]
        check(names_now == [selected[0]], "Alt+Shift+Up goes back to %r (%r)" % (selected[0], names_now))
        p = popover()
        if p is not None:
            QApplication.sendEvent(p, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
            QApplication.processEvents()

        gt.section("diff panel picks")
        app.selectNode(val, True)
        ok = yield ("until", lambda: "Val" in diff_rows() and widget(QPushButton, "nodeDiffTake_Val_size") is not None, POLL_TIMEOUT_S)
        check(ok, "the Node Diff panel lists Val with a Take on size (%r)" % diff_rows())
        check(widget(QPushButton, "nodeDiffRevertNode_Val") is not None, "the Node Diff panel offers Revert Node on Val")
        take = widget(QPushButton, "nodeDiffTake_Val_size")
        if click_button(take, "Take on Val's size in the Node Diff panel"):
            ok = yield ("until", lambda: size_is(val, SUPERVISOR_SIZE), POLL_TIMEOUT_S)
            check(ok, "Take gives Val the saved size (%r)" % (size_of(val),))
            ok = yield ("until", lambda: app.getDiffTint(val) == "", POLL_TIMEOUT_S)
            check(ok, "after Take Val no longer differs (%r)" % app.getDiffTint(val))
            if undo("Take from the Node Diff panel"):
                yield ("sleep", SETTLE_MS)
                check(size_is(val, EDITED_SIZE), "undoing Take gives Val the edited size again (%r)" % (size_of(val),))
                ok = yield ("until", lambda: app.getDiffTint(val) == "changed", POLL_TIMEOUT_S)
                check(ok, "undoing Take marks Val changed again (%r)" % app.getDiffTint(val))
        app.selectNode(val, True)
        ok = yield ("until", lambda: widget(QPushButton, "nodeDiffRevertNode_Val") is not None, POLL_TIMEOUT_S)
        revert = widget(QPushButton, "nodeDiffRevertNode_Val")
        if click_button(revert, "Revert Node on Val in the Node Diff panel"):
            ok = yield ("until", lambda: size_is(val, SUPERVISOR_SIZE), POLL_TIMEOUT_S)
            check(ok, "Revert Node gives Val the saved size (%r)" % (size_of(val),))
            ok = yield ("until", lambda: app.getDiffTint(val) == "", POLL_TIMEOUT_S)
            check(ok, "after Revert Node Val no longer differs (%r)" % app.getDiffTint(val))
        app.selectNode(up, True)
        ok = yield ("until", lambda: "Up" in diff_rows() and widget(QPushButton, "nodeDiffRevertNode_Up") is not None, POLL_TIMEOUT_S)
        check(ok, "the Node Diff panel lists Up (%r)" % diff_rows())
        revert = widget(QPushButton, "nodeDiffRevertNode_Up")
        check(revert is not None and not revert.isEnabled(), "Revert Node is disabled on Up, which only moved")
        check(not any(w.objectName().startswith("nodeDiffTake_Up_") for w in QApplication.allWidgets() if isinstance(w, QPushButton)),
              "the Node Diff panel offers no Take on Up")

        gt.section("clear comparison")
        clear = widget(QPushButton, "mergeBarClearComparison", True)
        require(click_button(clear, "the merge bar's Clear Comparison"), "clear the comparison")
        ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
        check(ok, "clearing the comparison puts the merge bar back in conflict mode (%r, %r)" % (app.mergeBarMode(), bar_count_text()))
        for name in remaining:
            check(app.isMergeConflictBadgeShown(app.getNode(name)), "clearing the comparison brings %s's badge back" % name)

        gt.section("all theirs")
        all_theirs = widget(QPushButton, "mergeBarAllTheirs", True)
        require(click_button(all_theirs, "the merge bar's All Theirs"), "All Theirs")
        ok = yield ("until", lambda: len(conflicts()) == 0 and app.mergeBarMode() == "none", POLL_TIMEOUT_S)
        check(ok, "All Theirs resolves every conflict and hides the merge bar (%r)" % app.mergeBarMode())
        check(source_of(rd) == "Feed", "All Theirs feeds Rd from Feed (%r)" % source_of(rd))
        check(abs(key_at(key) - SUPERVISOR_SIZE) < 1e-9, "All Theirs gives Key the template's key (%r)" % key_at(key))
        if undo("All Theirs"):
            ok = yield ("until", lambda: bar_counts(count), POLL_TIMEOUT_S)
            check(ok, "undoing All Theirs brings the conflicts back (%r)" % bar_count_text())
            check(source_of(rd) == "Artist", "undoing All Theirs feeds Rd from Artist again (%r)" % source_of(rd))

        gt.section("reopen")
        require(app.saveProjectAs(shot_file), "save the shot with its conflicts")
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        require(app.loadProject(shot_file) is not None, "reopen the shot")
        ok = yield ("until", lambda: app.getNode("Rd") is not None and bar_counts(count), POLL_TIMEOUT_S)
        check(ok, "the reopened shot's merge bar counts its conflicts (%r, %r)" % (app.mergeBarMode(), bar_count_text()))
        yield ("sleep", SETTLE_MS)
        check(not panel_shown(), "the Merge Conflicts panel does not open by itself on load")
        rd = app.getNode("Rd")
        look_at(rd)
        require(app.openMergePopover(rd), "open the reopened Rd's popover")
        QApplication.processEvents()
        ok = yield ("until", preview_wires_shown, POLL_TIMEOUT_S)
        check(ok, "the reopened Rd's input conflict still draws its structural preview")

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
