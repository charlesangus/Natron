"""A GUI test of Linked Nodes in the node graph: Create Linked Node (Alt+K) and Unlink Node
(Alt+Shift+K) as undoable steps, the dashed line and the "L" glyph between Linked Nodes, the
expression arrows, the link and expression line preferences, and the Show Link Lines switch
(Shift+E). It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m92-linked/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m92_linked_nodes.py <AppImage or build/fast/App/Natron>'

The lines are found among the graph scene's items by their tool tips: "... are Linked Nodes" for
the dashed lines, "<------->" for the lines of knob links and "(master)" for expression arrows; the
glyph by its "is a Linked Node" tool tip. Line colours and dash patterns are only checked in the
screenshots.

Phase 92.3's knob links are checked in node panels: the link circle at the end of a linked knob's
row (object name "knobLinkButton", dynamic property "linked"), its click to unlink and to re-link
through the confirmation dialog, the Unlink / Re-link items, and Copy Link / Paste Link through
the knob's right-click menu, each step undone and redone.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 400
BLUR = "net.sf.cimg.CImgBlur"
PARAM = "size"

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QPoint, Qt, QTimer
    from PySide6.QtGui import QCursor
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QWidget

    import guitest as gt
    from guitest import check, report

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- plumbing --------------------------------------------------------------------------

    def create(plugin_id, x, y, panel=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(panel)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def script_names():
        return set(n.getScriptName() for n in app.getChildren())

    def value(name):
        return app.getNode(name).getParam(PARAM).getValue(0)

    def set_value(name, v):
        app.getNode(name).getParam(PARAM).setValue(v, 0)

    def graph_key(key, mods):
        """Sends key to the root node graph with the mouse over its centre, once the main window
        is active: an inactive window drops the graph's shortcuts."""
        graph = root_graph()
        win = graph.window()
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)
        QCursor.setPos(graph.mapToGlobal(QPoint(graph.width() // 2, graph.height() // 2)))
        QApplication.processEvents()
        QTest.keyClick(graph, key, mods)
        QApplication.processEvents()

    def frame_all():
        """Frames every node of the root graph (the Frame All Nodes action with nothing selected)."""
        app.clearSelection()
        QApplication.processEvents()
        QMetaObject.invokeMethod(root_graph(), "centerOnAllNodes")
        QApplication.processEvents()

    def select_only(name):
        app.selectNode(app.getNode(name), True)
        QApplication.processEvents()

    def lines():
        """The visible link lines of the root graph, as a dict kind -> count."""
        counts = {"nodes": 0, "knob": 0, "expression": 0, "glyph": 0}
        graph = root_graph()
        for item in graph.items():
            if not item.isVisible():
                continue
            tip = item.toolTip().replace("&nbsp;", " ")
            if "are Linked Nodes" in tip:
                counts["nodes"] += 1
            elif "&lt;-------&gt;" in tip:
                counts["knob"] += 1
            elif "(master)" in tip:
                counts["expression"] += 1
            elif "is a Linked Node" in tip:
                counts["glyph"] += 1
        return counts

    def linked(a, b, v):
        """Whether a value set on a reaches b."""
        set_value(a, v)
        QApplication.processEvents()
        return abs(value(b) - v) < 1e-9

    settings = NatronEngine.natron.getSettings()

    def set_pref(name, v):
        p = settings.getParam(name)
        gt.require(p is not None, "Preferences have a parameter named " + name)
        p.setValue(v)
        settings.saveSettings()
        QApplication.processEvents()

    def set_colour_pref(name, r, g, b):
        p = settings.getParam(name)
        gt.require(p is not None, "Preferences have a parameter named " + name)
        p.setValue(r, 0)
        p.setValue(g, 1)
        p.setValue(b, 2)
        settings.saveSettings()
        QApplication.processEvents()

    # ---- knob links in panels --------------------------------------------------------------

    keep_alive = []

    def panel_of(name):
        """The settings panel of the node with that script name, found by the label in its
        header."""
        label = app.getNode(name).getLabel()
        for p in gt.widgets_of("NodeSettingsPanel"):
            for le in p.findChildren(QLineEdit):
                if gt.class_name(le) == "LineEdit" and le.text() == label:
                    del keep_alive[:]
                    keep_alive.append(p)
                    return p
        return None

    def first_spin_box(name):
        """The first dimension's spin box of the node's first parameter (a Blur's size)."""
        p = panel_of(name)
        if p is None:
            return None
        boxes = [w for w in p.findChildren(QLineEdit)
                 if gt.class_name(w) == "SpinBox" and w.isVisible() and w.objectName() == "dim-0"]
        return boxes[0] if boxes else None

    def link_button(name):
        """The link circle on the row of the node's first parameter, or None."""
        w = first_spin_box(name)
        while w is not None:
            keep_alive.append(w)
            found = w.findChildren(QAbstractButton, "knobLinkButton", Qt.FindDirectChildrenOnly)
            if found:
                return found[0]
            w = w.parentWidget()
        return None

    def circle(name):
        """'filled', 'empty' or None, for the link circle of the node's first parameter."""
        b = link_button(name)
        if b is None or not b.isVisible():
            return None
        return "filled" if b.property("linked") else "empty"

    def circles_in_panel(name):
        p = panel_of(name)
        if p is None:
            return 0
        return len([b for b in p.findChildren(QAbstractButton, "knobLinkButton") if b.isVisible()])

    def answer_dialog_later(text):
        """Clicks the button whose text is text in the next modal dialog, from inside its event loop."""
        def answer():
            w = QApplication.activeModalWidget()
            if w is None:
                QTimer.singleShot(50, answer)
                return
            for btn in w.findChildren(QAbstractButton):
                if btn.text().replace("&", "") == text:
                    btn.click()
                    return
            w.reject()
        QTimer.singleShot(100, answer)

    def menu_action_later(text):
        """Triggers the action named text in the next popup menu (submenus included), then closes it."""
        def find(menu):
            for act in menu.actions():
                if act.text().replace("&", "") == text:
                    return act
                sub = gt.menu_of(act)
                if sub is not None:
                    found = find(sub)
                    if found is not None:
                        return found
            return None

        def pick():
            menu = QApplication.activePopupWidget()
            if menu is None:
                QTimer.singleShot(50, pick)
                return
            act = find(menu)
            menu.close()
            if act is not None:
                act.trigger()
            else:
                report("NOTE no menu action named " + text)
        QTimer.singleShot(100, pick)

    def knob_menu(name, text):
        """Opens the right-click menu of the node's first parameter and triggers text in it."""
        box = first_spin_box(name)
        if not check(box is not None, "found the first spin box of %s's panel" % name):
            return
        menu_action_later(text)
        box.customContextMenuRequested.emit(QPoint(2, 2))
        QApplication.processEvents()

    def menu_texts(name):
        """The texts of the right-click menu of the node's first parameter, top level only."""
        texts = []
        box = first_spin_box(name)
        if box is None:
            return texts

        def grab():
            menu = QApplication.activePopupWidget()
            if menu is None:
                QTimer.singleShot(50, grab)
                return
            texts.extend(a.text().replace("&", "") for a in menu.actions())
            menu.close()
        QTimer.singleShot(100, grab)
        box.customContextMenuRequested.emit(QPoint(2, 2))
        QApplication.processEvents()
        return texts

    def undo():
        graph_key(Qt.Key_Z, Qt.ControlModifier)

    def redo():
        graph_key(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.require(QTest is not None, "PySide6.QtTest is available for the graph shortcuts")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("create linked node")
        a = create(BLUR, 0, 0).getScriptName()
        app.getNode(a).setLabel("Original")
        yield ("sleep", SETTLE_MS)
        before = script_names()
        select_only(a)
        graph_key(Qt.Key_K, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        added = sorted(script_names() - before)
        gt.require(len(added) == 1, "Alt+K created one node (added %r)" % added)
        b = added[0]
        check(app.getNode(b).getPluginID() == app.getNode(a).getPluginID(), "the Linked Node is a " + BLUR)
        c = lines()
        check(c["nodes"] == 1, "one dashed line joins the two Linked Nodes (%r)" % c)
        check(c["glyph"] == 2, "both Linked Nodes show the link glyph (%r)" % c)

        gt.section("undo create")
        # Before any other edit, so that Create Linked Node is the last step of the history.
        graph_key(Qt.Key_Z, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 0 and c["glyph"] == 0, "undoing Create Linked Node removes the line and the glyphs (%r)" % c)
        graph_key(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 1 and c["glyph"] == 2, "redoing it brings back the line and the glyphs (%r)" % c)

        gt.section("linked edits")
        app.getNode(b).setPosition(300, 150)
        yield ("sleep", SETTLE_MS)
        check(linked(a, b, 7.0), "an edit of %s reaches its Linked Node %s" % (a, b))
        check(linked(b, a, 3.0), "an edit of %s reaches %s: the link has no direction" % (b, a))
        app.getNode(a).setColor(0.9, 0.2, 0.2)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 1 and c["glyph"] == 2, "a user colour leaves the line and the glyphs (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_linked_pair.png")

        gt.section("unlink node")
        select_only(b)
        graph_key(Qt.Key_K, Qt.AltModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 0 and c["glyph"] == 0, "Unlink Node removes the line and the glyphs (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_unlinked.png")
        graph_key(Qt.Key_Z, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 1 and c["glyph"] == 2, "undoing Unlink Node relinks the pair (%r)" % c)
        graph_key(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 0 and c["glyph"] == 0, "redoing Unlink Node unlinks it again (%r)" % c)
        set_value(b, 2.0)
        check(not linked(a, b, 11.0), "after Unlink Node an edit of %s no longer reaches %s" % (a, b))

        gt.section("three linked nodes")
        select_only(a)
        before = script_names()
        graph_key(Qt.Key_K, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        added = sorted(script_names() - before)
        gt.require(len(added) == 1, "Alt+K created a second Linked Node (added %r)" % added)
        l2 = added[0]
        app.getNode(l2).setPosition(-300, 150)
        select_only(a)
        before = script_names()
        graph_key(Qt.Key_K, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        added = sorted(script_names() - before)
        gt.require(len(added) == 1, "Alt+K created a third Linked Node (added %r)" % added)
        l3 = added[0]
        app.getNode(l3).setPosition(0, 300)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 3, "three Linked Nodes are joined by three dashed lines (%r)" % c)
        check(linked(l3, l2, 9.0), "an edit of %s reaches %s, both linked from %s" % (l3, l2, a))
        app.getNode(l3).destroy()
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] == 1 and c["glyph"] == 2, "deleting one of three Linked Nodes leaves one line between the other two (%r)" % c)

        gt.section("expression line")
        e = create(BLUR, 300, -200).getScriptName()
        gt.require(app.getNode(e).getParam(PARAM).setExpression("%s.%s.getValue(0)" % (a, PARAM), False, 0),
                   "set an expression on %s.%s reading %s" % (e, PARAM, a))
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 1, "the expression draws one arrow (%r)" % c)
        check(c["nodes"] == 1, "the dashed line is still drawn (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_link_and_expression.png")

        gt.section("line preferences")
        set_pref("showExpressionLines", False)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 0 and c["nodes"] == 1, "Show expression lines off hides only the expression arrow (%r)" % c)
        set_pref("showExpressionLines", True)
        set_pref("showLinkLines", False)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 1 and c["nodes"] == 0, "Show link lines off hides only the link lines (%r)" % c)
        check(c["glyph"] == 2, "the glyph is shown whatever the line preferences (%r)" % c)
        set_pref("showLinkLines", True)
        set_colour_pref("linkLineColor", 1.0, 0.0, 1.0)
        set_colour_pref("expressionLineColor", 1.0, 1.0, 0.0)
        yield ("sleep", SETTLE_MS)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_recoloured_lines.png")
        set_colour_pref("linkLineColor", 70. / 255., 140. / 255., 1.0)
        set_colour_pref("expressionLineColor", 143. / 255., 201. / 255., 103. / 255.)

        gt.section("show link lines switch")
        graph_key(Qt.Key_E, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 0 and c["nodes"] == 0, "Shift+E hides every link and expression line (%r)" % c)
        check(c["glyph"] == 2, "Shift+E leaves the glyph (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_lines_hidden.png")
        graph_key(Qt.Key_E, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 1 and c["nodes"] == 1, "Shift+E again shows them (%r)" % c)
        set_pref("showExpressionLines", False)
        graph_key(Qt.Key_E, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 0 and c["nodes"] == 0, "with expression lines off, Shift+E hides the link lines (%r)" % c)
        graph_key(Qt.Key_E, Qt.ShiftModifier)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] == 0 and c["nodes"] == 1, "and Shift+E again shows only the link lines (%r)" % c)
        set_pref("showExpressionLines", True)

        yield from knob_link_steps()

        faulthandler.cancel_dump_traceback_later()

    def knob_link_steps():
        gt.section("link circles on a Linked Node pair")
        gt.close_all_panels()
        p = create(BLUR, 0, 600, panel=True).getScriptName()
        app.getNode(p).setLabel("PanelOriginal")
        yield ("sleep", SETTLE_MS)
        gt.require(panel_of(p) is not None, "the panel of %s is open" % p)
        check(circle(p) is None and circles_in_panel(p) == 0, "an ordinary node's panel shows no link circle")
        before = script_names()
        select_only(p)
        graph_key(Qt.Key_K, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        added = sorted(script_names() - before)
        gt.require(len(added) == 1, "Alt+K created a Linked Node of %s (added %r)" % (p, added))
        q = added[0]
        app.getNode(q).setPosition(300, 600)
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "filled", "the linked knob shows a filled circle (%r)" % circle(p))
        box = first_spin_box(p)
        check(box is not None and box.isEnabled(), "the linked knob is not greyed out")
        check(linked(p, q, 4.0) and linked(q, p, 6.0), "the pair edits both ways")
        texts = menu_texts(p)
        check("Unlink" in texts and "Re-link (discards local values)" not in texts,
              "a linked knob's menu offers Unlink only (%r)" % [t for t in texts if "link" in t.lower()])
        b = link_button(p)
        check(b is not None and "%s." % q in b.toolTip(), "the circle's tooltip names %s" % q)
        gt.shot("m92_knob_linked_panel.png")

        gt.section("circle unlinks")
        link_button(p).click()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "empty", "clicking the circle empties it (%r)" % circle(p))
        set_value(q, 2.0)
        check(not linked(p, q, 9.0) and value(q) == 2.0, "the unlinked knob edits on its own")
        check(not linked(q, p, 1.0), "and the other knob no longer reaches it")
        texts = menu_texts(p)
        check("Re-link (discards local values)" in texts and "Unlink" not in texts,
              "an unlinked knob's menu offers Re-link only (%r)" % [t for t in texts if "link" in t.lower()])
        gt.shot("m92_knob_unlinked_panel.png")
        undo()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "filled", "undoing the unlink fills the circle again (%r)" % circle(p))
        redo()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "empty", "redoing it empties it (%r)" % circle(p))

        gt.section("circle re-links after the warning")
        set_value(p, 5.0)
        set_value(q, 8.0)
        answer_dialog_later("Cancel")
        link_button(p).click()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "empty" and value(p) == 5.0, "cancelling the warning leaves the knob unlinked")
        answer_dialog_later("OK")
        link_button(p).click()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "filled" and value(p) == 8.0, "accepting it re-links and takes the linked value (%r)" % value(p))
        undo()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "empty" and value(p) == 5.0, "undoing the re-link restores the local value (%r)" % value(p))
        redo()
        yield ("sleep", SETTLE_MS)
        check(circle(p) == "filled" and value(p) == 8.0, "redoing it re-links (%r)" % value(p))

        gt.section("paste link")
        gt.close_all_panels()
        s = create(BLUR, 0, 900, panel=True).getScriptName()
        app.getNode(s).setLabel("LinkSource")
        t = create(BLUR, 300, 900, panel=True).getScriptName()
        app.getNode(t).setLabel("LinkTarget")
        yield ("sleep", SETTLE_MS)
        gt.require(panel_of(s) is not None and panel_of(t) is not None, "the panels of %s and %s are open" % (s, t))
        set_value(s, 3.0)
        set_value(t, 1.0)
        nodes_before = lines()["nodes"]
        knob_menu(s, "Copy Link")
        yield ("sleep", SETTLE_MS)
        knob_menu(t, "Paste Link")
        yield ("sleep", SETTLE_MS)
        check(value(t) == 3.0, "the pasted knob takes the copied knob's value (%r)" % value(t))
        check(linked(s, t, 12.0) and linked(t, s, 13.0), "Paste Link links both ways")
        check(circle(t) == "filled" and circle(s) == "filled", "both knobs show a filled circle (%r, %r)" % (circle(t), circle(s)))
        c = lines()
        check(c["knob"] >= 1 and c["nodes"] == nodes_before, "the paste draws a knob link line, not a Linked Nodes line (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_paste_link.png")
        link_button(t).click()
        yield ("sleep", SETTLE_MS)
        check(circle(t) == "empty" and not linked(s, t, 14.0), "the pasted knob's circle unlinks it")
        undo()
        yield ("sleep", SETTLE_MS)
        check(circle(t) == "filled", "undo re-links it (%r)" % circle(t))
        undo()
        yield ("sleep", SETTLE_MS)
        check(value(t) == 1.0, "undoing the paste restores the target's value (%r)" % value(t))
        check(circle(t) is None and circle(s) is None, "and the link and its circles are gone (%r, %r)" % (circle(t), circle(s)))
        check(not linked(s, t, 15.0), "the knobs edit on their own again")
        c = lines()
        check(c["knob"] == 0, "the knob link line is gone (%r)" % c)
        redo()
        yield ("sleep", SETTLE_MS)
        check(circle(t) == "filled" and linked(s, t, 16.0), "redoing the paste links them again")

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
