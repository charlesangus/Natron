"""A GUI test of Linked Nodes in the node graph: Create Linked Node (Alt+K) and Unlink Node
(Alt+Shift+K) as undoable steps, the solid line and the chain-link badge between Linked Nodes, the
expression arrows, the link and expression line preferences, and the Show Link Lines switch
(Shift+E). It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m92-linked/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m92_linked_nodes.py <AppImage or build/fast/App/Natron>'

The lines are found among the graph scene's items by their tool tips: "... are Linked Nodes" for
the solid lines between Linked Nodes, "<------->" for the dashed lines of knob links and "(master)"
for expression arrows; the badge by its "is a Linked Node" tool tip. Each set of Linked Nodes, and each
knob link group, draws a line from each member to its nearest other member, joined into one tree of
lines, so four Linked Nodes in a row draw three lines. An expression is a directed edge, so every
reader draws one arrow straight to each node it reads, wherever the nodes are.
Dash patterns are read from the line items' pens; line colours and the badge's drawing are only
checked in the screenshots; m92_badge_1x.png is a crop of the badge at 1:1 graph zoom and
m92_badge_1x_x4.png the same pixels enlarged.

Knob links are checked in node panels: a linked knob's name label (dynamic property "linked", which
the stylesheet draws in the link colour) and its tool tip, a knob linked in one dimension only, the
Unlink / Re-link items of the knob's right-click menu (Re-link through the confirmation dialog), and
Copy Link / Paste Link, each step undone and redone.
"""
import re
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

    def badge_crop(name):
        """Saves the Linked Node badge of the node at 1:1 graph zoom, cropped, and enlarged four times
        without smoothing."""
        graph = root_graph()
        node_pos = app.getNode(name).getPosition()
        badges = []
        for item in graph.items():
            if item.isVisible() and "is a Linked Node" in item.toolTip().replace("&nbsp;", " "):
                badges.append(item)
        if not check(len(badges) > 0, "found a Linked Node badge to crop"):
            return
        # The badge nearest the node's top-left corner sits on that node's bottom-left corner.
        badge = min(badges, key=lambda it: abs(it.sceneBoundingRect().center().x() - node_pos[0])
                    + abs(it.sceneBoundingRect().center().y() - node_pos[1]))
        graph.resetTransform()
        graph.centerOn(badge.sceneBoundingRect().center())
        QApplication.processEvents()
        check(abs(graph.transform().m11() - 1.0) < 1e-9, "the graph is at 1:1 zoom for the badge crop")
        r = graph.mapFromScene(badge.sceneBoundingRect()).boundingRect().adjusted(-40, -30, 40, 30)
        pix = graph.viewport().grab(r)
        path = os.path.join(gt.OUT, "m92_badge_1x.png")
        if pix.save(path):
            report("SHOT " + path)
        big = pix.scaled(pix.width() * 4, pix.height() * 4, Qt.IgnoreAspectRatio, Qt.FastTransformation)
        path = os.path.join(gt.OUT, "m92_badge_1x_x4.png")
        if big.save(path):
            report("SHOT " + path)

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

    def line_items(kind):
        """The visible line items of the root graph of one kind of lines(): "nodes", "knob" or "expression"."""
        marks = {"nodes": "are Linked Nodes", "knob": "&lt;-------&gt;", "expression": "(master)"}
        found = []
        for item in root_graph().items():
            if item.isVisible() and marks[kind] in item.toolTip().replace("&nbsp;", " "):
                found.append(item)
        return found

    def pen_styles(kind):
        """The pen styles of the visible lines of one kind, as a set of names."""
        styles = set()
        for item in line_items(kind):
            styles.add(item.pen().style().name if hasattr(item, "pen") else "no pen")
        return styles

    def linked_node_pairs():
        """The pairs of labels joined by a line between Linked Nodes, each pair as a sorted tuple."""
        pairs = set()
        for item in line_items("nodes"):
            m = re.search(r"(\w+) and (\w+) are Linked Nodes", item.toolTip().replace("&nbsp;", " "))
            if m:
                pairs.add(tuple(sorted((m.group(1), m.group(2)))))
        return pairs

    def expression_arrows_to(master_label):
        """For each expression arrow to master_label, the sorted labels of the readers its tool tip names."""
        arrows = []
        for item in line_items("expression"):
            readers = set()
            for m in re.finditer(r"%s\.\w+ \(master\) -+(?:&gt;|>)(\w+)\." % master_label, item.toolTip().replace("&nbsp;", " ")):
                readers.add(m.group(1))
            if readers:
                arrows.append(",".join(sorted(readers)))
        return sorted(arrows)

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

    def name_label(name, param=PARAM):
        """The name label of the node's parameter in its open panel, or None."""
        p = panel_of(name)
        if p is None:
            return None
        text = app.getNode(name).getParam(param).getLabel()
        for w in p.findChildren(QWidget):
            if gt.class_name(w) == "ClickableLabel" and w.isVisible() \
                    and re.sub("<[^>]*>", "", w.text()).strip() == text:
                keep_alive.append(w)
                return w
        return None

    def link_state(name, param=PARAM):
        """'linked' when the parameter's name label is in the link colour, 'plain' otherwise, None
        without a label."""
        w = name_label(name, param)
        if w is None:
            return None
        return "linked" if w.property("linked") else "plain"

    def linked_labels_in_panel(name):
        p = panel_of(name)
        if p is None:
            return 0
        return len([w for w in p.findChildren(QWidget)
                    if gt.class_name(w) == "ClickableLabel" and w.isVisible() and w.property("linked")])

    def label_tip(name, param=PARAM):
        w = name_label(name, param)
        return w.toolTip().replace("&nbsp;", " ") if w is not None else ""

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
        check(c["nodes"] == 1, "one solid line joins the two Linked Nodes (%r)" % c)
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
        badge_crop(a)
        yield ("sleep", SETTLE_MS)

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
        check(c["nodes"] == 2, "three Linked Nodes are joined by two lines, not one per pair (%r)" % c)
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
        check(c["nodes"] == 1, "the Linked Nodes line is still drawn (%r)" % c)
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
        yield from nearest_member_steps()
        yield from expression_set_steps()

        faulthandler.cancel_dump_traceback_later()

    def knob_link_steps():
        gt.section("linked knob names on a Linked Node pair")
        gt.close_all_panels()
        p = create(BLUR, 0, 600, panel=True).getScriptName()
        app.getNode(p).setLabel("PanelOriginal")
        yield ("sleep", SETTLE_MS)
        gt.require(panel_of(p) is not None, "the panel of %s is open" % p)
        check(link_state(p) == "plain" and linked_labels_in_panel(p) == 0,
              "an ordinary node's panel shows no linked name (%r)" % link_state(p))
        before = script_names()
        select_only(p)
        graph_key(Qt.Key_K, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        added = sorted(script_names() - before)
        gt.require(len(added) == 1, "Alt+K created a Linked Node of %s (added %r)" % (p, added))
        q = added[0]
        app.getNode(q).setPosition(300, 600)
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "linked", "the linked knob's name is in the link colour (%r)" % link_state(p))
        check(linked_labels_in_panel(p) > 1, "every linked knob of the Linked Node shows a linked name (%d)"
              % linked_labels_in_panel(p))
        box = first_spin_box(p)
        check(box is not None and box.isEnabled(), "the linked knob is not greyed out")
        check(linked(p, q, 4.0) and linked(q, p, 6.0), "the pair edits both ways")
        texts = menu_texts(p)
        check("Unlink" in texts and "Re-link (discards local values)" not in texts,
              "a linked knob's menu offers Unlink only (%r)" % [t for t in texts if "link" in t.lower()])
        tip = label_tip(p)
        check("Linked to" in tip and "%s." % q in tip, "the name's tool tip says it is linked to %s (%r)" % (q, tip))

        gt.section("partially linked knob")
        u = create(BLUR, 0, 750, panel=True).getScriptName()
        app.getNode(u).setLabel("PartSource")
        v = create(BLUR, 300, 750, panel=True).getScriptName()
        app.getNode(v).setLabel("PartTarget")
        yield ("sleep", SETTLE_MS)
        gt.require(app.getNode(v).getParam(PARAM).linkTo(app.getNode(u).getParam(PARAM), 0, 0),
                   "link only the first dimension of %s.%s to %s" % (v, PARAM, u))
        yield ("sleep", SETTLE_MS)
        check(link_state(v) == "linked" and link_state(u) == "linked",
              "a knob linked in one dimension shows a linked name on both nodes (%r, %r)" % (link_state(v), link_state(u)))
        tip = label_tip(v)
        check("Partially linked" in tip and "is linked to" in tip and "is not linked" in tip,
              "its tool tip says which dimension is linked (%r)" % tip)
        app.getNode(u).getParam(PARAM).setValue(21.0, 0)
        app.getNode(u).getParam(PARAM).setValue(22.0, 1)
        app.getNode(v).getParam(PARAM).setValue(5.0, 1)
        QApplication.processEvents()
        pv = app.getNode(v).getParam(PARAM)
        check(pv.getValue(0) == 21.0 and pv.getValue(1) == 5.0, "only the linked dimension follows (%r, %r)"
              % (pv.getValue(0), pv.getValue(1)))
        app.getNode(v).getParam(PARAM).setExpression("frame", False, 1)
        yield ("sleep", SETTLE_MS)
        check(link_state(v) == "linked", "an expression on the unlinked dimension leaves the name linked (%r)" % link_state(v))
        gt.shot("m92_knob_linked_panel.png")
        app.getNode(v).getParam(PARAM).setExpression("", False, 1)

        gt.section("unlink from the menu")
        knob_menu(p, "Unlink")
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "plain", "unlinking clears the link colour (%r)" % link_state(p))
        tip = label_tip(p)
        check("Unlinked from" in tip, "the tool tip says what it was unlinked from (%r)" % tip)
        set_value(q, 2.0)
        check(not linked(p, q, 9.0) and value(q) == 2.0, "the unlinked knob edits on its own")
        check(not linked(q, p, 1.0), "and the other knob no longer reaches it")
        texts = menu_texts(p)
        check("Re-link (discards local values)" in texts and "Unlink" not in texts,
              "an unlinked knob's menu offers Re-link only (%r)" % [t for t in texts if "link" in t.lower()])
        gt.shot("m92_knob_unlinked_panel.png")
        undo()
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "linked", "undoing the unlink brings the link colour back (%r)" % link_state(p))
        redo()
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "plain", "redoing it clears it (%r)" % link_state(p))

        gt.section("re-link from the menu after the warning")
        set_value(p, 5.0)
        set_value(q, 8.0)
        answer_dialog_later("Cancel")
        knob_menu(p, "Re-link (discards local values)")
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "plain" and value(p) == 5.0, "cancelling the warning leaves the knob unlinked")
        answer_dialog_later("OK")
        knob_menu(p, "Re-link (discards local values)")
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "linked" and value(p) == 8.0, "accepting it re-links and takes the linked value (%r)" % value(p))
        undo()
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "plain" and value(p) == 5.0, "undoing the re-link restores the local value (%r)" % value(p))
        redo()
        yield ("sleep", SETTLE_MS)
        check(link_state(p) == "linked" and value(p) == 8.0, "redoing it re-links (%r)" % value(p))

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
        lines_before = lines()
        nodes_before = lines_before["nodes"]
        knob_menu(s, "Copy Link")
        yield ("sleep", SETTLE_MS)
        knob_menu(t, "Paste Link")
        yield ("sleep", SETTLE_MS)
        check(value(t) == 3.0, "the pasted knob takes the copied knob's value (%r)" % value(t))
        check(linked(s, t, 12.0) and linked(t, s, 13.0), "Paste Link links both ways")
        check(link_state(t) == "linked" and link_state(s) == "linked",
              "both knobs show a linked name (%r, %r)" % (link_state(t), link_state(s)))
        c = lines()
        check(c["knob"] == lines_before["knob"] + 1 and c["nodes"] == nodes_before, "the paste draws a knob link line, not a Linked Nodes line (%r)" % c)
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_paste_link.png")
        knob_menu(t, "Unlink")
        yield ("sleep", SETTLE_MS)
        check(link_state(t) == "plain" and not linked(s, t, 14.0), "Unlink unlinks the pasted knob")
        undo()
        yield ("sleep", SETTLE_MS)
        check(link_state(t) == "linked", "undo re-links it (%r)" % link_state(t))
        undo()
        yield ("sleep", SETTLE_MS)
        check(value(t) == 1.0, "undoing the paste restores the target's value (%r)" % value(t))
        check(link_state(t) == "plain" and link_state(s) == "plain",
              "and the link and its colour are gone (%r, %r)" % (link_state(t), link_state(s)))
        check(not linked(s, t, 15.0), "the knobs edit on their own again")
        c = lines()
        check(c["knob"] == lines_before["knob"], "the knob link line is gone (%r, before %r)" % (c, lines_before))
        redo()
        yield ("sleep", SETTLE_MS)
        check(link_state(t) == "linked" and linked(s, t, 16.0), "redoing the paste links them again")

    def nearest_member_steps():
        gt.section("nearest-member lines and line styles")
        before = lines()
        r1 = create(BLUR, 0, 1500).getScriptName()
        app.getNode(r1).setLabel("Row1")
        rows = [r1]
        for i in range(2, 5):
            n = app.getNode(r1).createLinkedNode()
            gt.require(n is not None, "createLinkedNode made Row%d" % i)
            n.setLabel("Row%d" % i)
            n.setPosition(300 * (i - 1), 1500)
            rows.append(n.getScriptName())
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] - before["nodes"] == 3, "four Linked Nodes in a row draw three lines, not six (%r, before %r)" % (c, before))
        pairs = linked_node_pairs()
        check({("Row1", "Row2"), ("Row2", "Row3"), ("Row3", "Row4")} <= pairs,
              "each Linked Node in the row joins its neighbour (%r)" % sorted(pairs))
        check(pen_styles("nodes") == {"SolidLine"}, "Linked Node lines are solid (%r)" % pen_styles("nodes"))
        check(pen_styles("knob") == {"DashLine"}, "knob link lines are dashed (%r)" % pen_styles("knob"))
        check(pen_styles("expression") == {"SolidLine"}, "expression arrows stay solid (%r)" % pen_styles("expression"))
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_nearest_member_row.png")

        app.getNode(rows[3]).setPosition(-300, 1500)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["nodes"] - before["nodes"] == 3, "moving a node keeps three lines (%r, before %r)" % (c, before))
        pairs = linked_node_pairs()
        check(("Row1", "Row4") in pairs and ("Row3", "Row4") not in pairs,
              "the moved node now joins its new nearest member (%r)" % sorted(pairs))
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_nearest_member_moved.png")

    def expression_set_steps():
        gt.section("direct expression arrows")
        before = lines()
        ctrl = create(BLUR, 0, 2500).getScriptName()
        app.getNode(ctrl).setLabel("Ctrl")
        readers = []
        for i in range(1, 5):
            n = create(BLUR, 300 * i, 2500).getScriptName()
            app.getNode(n).setLabel("Reader%d" % i)
            gt.require(app.getNode(n).getParam(PARAM).setExpression("%s.%s.getValue(0)" % (ctrl, PARAM), False, 0),
                       "set an expression on Reader%d reading Ctrl" % i)
            readers.append(n)
        yield ("sleep", SETTLE_MS)
        all_readers = ["Reader1", "Reader2", "Reader3", "Reader4"]
        c = lines()
        check(c["expression"] - before["expression"] == 4,
              "Ctrl read by four readers in a row draws four arrows (%r, before %r)" % (c, before))
        check(expression_arrows_to("Ctrl") == all_readers,
              "each arrow goes from one reader straight to Ctrl (%r)" % expression_arrows_to("Ctrl"))
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_expression_arrows_row.png")

        app.getNode(ctrl).setPosition(1500, 2500)
        app.getNode(readers[0]).setPosition(1800, 2800)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] - before["expression"] == 4,
              "moving Ctrl and a reader keeps four arrows (%r, before %r)" % (c, before))
        check(expression_arrows_to("Ctrl") == all_readers,
              "and the same reader-to-Ctrl pairs (%r)" % expression_arrows_to("Ctrl"))

        app.getNode(readers[3]).getParam(PARAM).setExpression("", False, 0)
        yield ("sleep", SETTLE_MS)
        c = lines()
        check(c["expression"] - before["expression"] == 3,
              "a reader dropping its expression removes its arrow (%r, before %r)" % (c, before))
        check(expression_arrows_to("Ctrl") == all_readers[:3],
              "and the other readers keep theirs (%r)" % expression_arrows_to("Ctrl"))
        frame_all()
        yield ("sleep", SETTLE_MS)
        gt.shot("m92_expression_arrows_moved.png")

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
