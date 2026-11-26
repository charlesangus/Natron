"""A GUI test of shift-dropping a pipe on empty space inside a USD Material Builder: the tab menu
opens listing only the shader nodes that can take the pipe, the node chosen lands at the drop point
connected by its first compatible port, with its compatible ports highlighted, and the down key
moves the connection to the next one. One undo removes the node and its connection, and redo
brings back both with the final connection. Escape instead cancels the creation. It is driven by
Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/shift_drop GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_shift_drop.py <AppImage or build/fast/App/Natron>'

Shots: shader_graph_shift_drop_menu.png (the filtered menu), shader_graph_shift_drop_cycle.png
(the new node's compatible ports highlighted, mid-cycle) and shader_graph_shift_drop_end.png.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
DRAG_STEPS = 8
BSDF = "cycles_principled_bsdf"
MIX = "cycles_mix_closure"
ADD = "cycles_add_closure"
MATH = "cycles_math"
BACKDROP = "Backdrop"
# How far from the drop point the new node may land, in scene units.
DROP_REACH = 120.0

try:
    import faulthandler

    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QCursor, QMouseEvent
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg
    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def shift_drag(widget, start, end):
        """Drags from start to end and releases with Shift held, the cursor left at end so the menu
        opens there."""
        start, end = QPointF(start), QPointF(end)
        send_mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * (i / float(DRAG_STEPS)), Qt.NoButton, Qt.LeftButton)
        QCursor.setPos(widget.mapToGlobal(end.toPoint()))
        QApplication.processEvents()
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, Qt.ShiftModifier)

    def port_item(graph, builder, node, side, name):
        full = "%s.%s" % (builder.getScriptName(), node.getScriptName())
        for item in graph.scene().items():
            if item.data(0) == side and item.data(1) == name and item.data(2) == full and item.isVisible():
                return item
        return None

    def port_point(graph, builder, node, side, name):
        item = port_item(graph, builder, node, side, name)
        require(item is not None, "%s has a %s port %s" % (node.getScriptName(), side, name))
        return QPointF(graph.mapFromScene(item.scenePos()))

    def highlight_of(graph, builder, node, side, name):
        item = port_item(graph, builder, node, side, name)
        return (item.data(3) or "") if item is not None else None

    def children_of(builder, plugin_id):
        return [n for n in builder.getChildren() if n.getPluginID() == plugin_id]

    def edit_menu_action(prefix):
        win = sg.main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def trigger_edit(prefix):
        action = edit_menu_action(prefix)
        require(action is not None and action.isEnabled(), "an enabled %s action is in the Edit menu" % prefix)
        report("NOTE %s" % action.text())
        action.trigger()
        QApplication.processEvents()

    def choose(line_edit, list_view, text, plugin_id):
        """Filters the open menu by text and picks plugin_id's row; False when it is not listed."""
        line_edit.setText(text)
        QApplication.processEvents()
        model = list_view.model()
        items = getattr(model, "_items", [])
        row = next((r for r, item in enumerate(items) if item["menuobj"].property("natronPluginID") == plugin_id), -1)
        if not check(row >= 0, "the menu filtered by %r offers %s" % (text, plugin_id)):
            return False
        list_view.setCurrentIndex(model.index(row, 0))
        sg.key_click(line_edit, Qt.Key_Return)
        QApplication.processEvents()
        return True

    def shift_drop_create(graph, start, end, builder):
        """Shift-drops a pipe dragged from start at end and creates a mix closure from the menu; the
        node created, or None."""
        focused = yield from sg.focus_graph(graph)
        require(focused, "the builder's graph has the focus")
        before = len(children_of(builder, MIX))
        shift_drag(graph.viewport(), start, end)
        popup, line_edit, list_view = yield from sg.wait_for_popup()
        require(popup is not None, "a shift-drop on empty space opens the tab menu")
        context = tabtabtab_natron._plugin.context() or {}
        report("NOTE shift-drop menu targets %r" % (tuple(context.get("networkTargets") or ()),))
        if not choose(line_edit, list_view, "mix", MIX):
            sg.close_tab_menu(popup)
            return None
        ok = yield ("until", lambda: len(children_of(builder, MIX)) > before, sg.POLL_TIMEOUT_S)
        require(ok, "choosing %s creates it in the builder" % MIX)
        yield ("sleep", SETTLE_MS)
        return children_of(builder, MIX)[-1]

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        sg.park_viewer(app)
        require(ok, "the main window is up")
        shaders = sg.shader_targets()

        gt.section("build")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        bsdf = sg.create(app, BSDF, 0, 0, builder)
        yield ("sleep", SETTLE_MS)
        graph = sg.graph_of(builder)
        require(graph is not None, "the builder's graph is still up")
        graph.centerOn(260, 120)
        yield ("sleep", SETTLE_MS)
        start = port_point(graph, builder, bsdf, "output", "BSDF")
        end = start + QPointF(300, 60)
        drop_scene = graph.mapToScene(end.toPoint())

        gt.section("shift-drop opens a filtered menu")
        focused = yield from sg.focus_graph(graph)
        require(focused, "the builder's graph has the focus")
        shift_drag(graph.viewport(), start, end)
        popup, line_edit, list_view = yield from sg.wait_for_popup()
        require(popup is not None, "a shift-drop on empty space opens the tab menu")
        ids = sg.listed_plugin_ids(tabtabtab_natron)
        report("NOTE shift-drop menu lists %d entries" % len(ids))
        check(MIX in ids and ADD in ids, "the menu offers %s and %s (got %r)" % (MIX, ADD, ids[:10]))
        check(MATH not in ids, "the menu leaves out %s, which takes no closure" % MATH)
        check(BACKDROP not in ids, "the menu leaves out Backdrop")
        unshared = sorted(pid for pid in ids if pid not in shaders or "cycles" not in shaders[pid])
        check(not unshared, "the menu lists only shaders sharing a target with the BSDF (also %r)" % unshared[:5])
        gt.shot("shader_graph_shift_drop_menu.png")
        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)
        check(not children_of(builder, MIX), "closing the menu creates nothing")

        gt.section("create and cycle")
        mix = yield from shift_drop_create(graph, start, end, builder)
        require(mix is not None, "the shift-drop created a mix closure")
        x, y = mix.getPosition()[0:2]
        w, h = mix.getSize()[0:2]
        near = (x - DROP_REACH <= drop_scene.x() <= x + w + DROP_REACH) and (y - DROP_REACH <= drop_scene.y() <= y + h + DROP_REACH)
        check(near, "the node lands at the drop point %r (node at %r, size %r)" % ((drop_scene.x(), drop_scene.y()), (x, y), (w, h)))
        check(sg.input_source(mix, "closure1") == bsdf.getScriptName(),
              "the first compatible input, closure1, is connected (got %r)" % sg.input_source(mix, "closure1"))
        check(mix.getOutputForInput("closure1") == "BSDF", "closure1 reads the BSDF output")
        lit = dict((p, highlight_of(graph, builder, mix, "input", p)) for p in ("fac", "closure1", "closure2"))
        check(lit["closure1"] == "halo" and lit["closure2"] == "halo", "both closure inputs have a halo (got %r)" % lit)
        check(lit["fac"] == "dimmed", "fac, a float, is dimmed (got %r)" % lit)

        sg.key_click(graph, Qt.Key_Down)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(sg.input_source(mix, "closure2") == bsdf.getScriptName(),
              "Down moves the connection to closure2 (got %r)" % sg.input_source(mix, "closure2"))
        check(sg.input_source(mix, "closure1") is None, "closure1 is no longer connected")
        check(highlight_of(graph, builder, mix, "input", "closure1") == "halo", "the highlight stays mid-cycle")
        gt.shot("shader_graph_shift_drop_cycle.png")

        sg.key_click(graph, Qt.Key_Return)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(highlight_of(graph, builder, mix, "input", "closure2") == "", "Enter clears the highlights")
        check(sg.input_source(mix, "closure2") == bsdf.getScriptName(), "Enter keeps the connection on closure2")
        mix_name = mix.getScriptName()
        mix = None

        gt.section("one undo step")
        trigger_edit("undo")
        ok = yield ("until", lambda: not children_of(builder, MIX), sg.POLL_TIMEOUT_S)
        check(ok, "one undo removes the node and its connection")
        trigger_edit("redo")
        ok = yield ("until", lambda: len(children_of(builder, MIX)) == 1, sg.POLL_TIMEOUT_S)
        check(ok, "redo brings the node back")
        if ok:
            redone = children_of(builder, MIX)[0]
            check(redone.getScriptName() == mix_name, "redo brings back the same node (%r)" % redone.getScriptName())
            check(sg.input_source(redone, "closure2") == bsdf.getScriptName(), "redo restores the final connection to closure2")
            check(sg.input_source(redone, "closure1") is None, "redo leaves closure1 unconnected")
            redone = None
        trigger_edit("undo")
        ok = yield ("until", lambda: not children_of(builder, MIX), sg.POLL_TIMEOUT_S)
        check(ok, "undo removes the node again")

        gt.section("Escape cancels the creation")
        mix = yield from shift_drop_create(graph, start, end, builder)
        require(mix is not None, "the second shift-drop created a mix closure")
        mix = None
        sg.key_click(graph, Qt.Key_Escape)
        QApplication.processEvents()
        ok = yield ("until", lambda: not children_of(builder, MIX), sg.POLL_TIMEOUT_S)
        check(ok, "Escape removes the node")
        redo = edit_menu_action("redo")
        check(redo is None or not redo.isEnabled() or "create" not in redo.text().lower(),
              "no redo brings the cancelled node back (got %r)" % (redo.text() if redo else None))
        gt.shot("shader_graph_shift_drop_end.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
