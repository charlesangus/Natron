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
screenshots. A knob-level Paste Link line needs Phase 92.3's GUI, so it is not drawn here.
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
    from PySide6.QtCore import QMetaObject, QPoint, Qt
    from PySide6.QtGui import QCursor
    from PySide6.QtWidgets import QApplication

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

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
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

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
