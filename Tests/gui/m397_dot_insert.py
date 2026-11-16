"""Ctrl+click on a pipe's bend point inserts a Dot, and the Dot can be Ctrl-dragged away at once.

A Dot has no rectangular body item, so every node-graph path that reads a node's body rectangle
(Ctrl-drag connection aiming, placing a node created with a Dot selected) has to take the Dot's
disk instead. Each step checks that Natron is still running and the graph is wired as expected.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m397-shots/out GUI_TEST_TIMEOUT=120 \
        Tests/gui/run-gui-test.sh Tests/gui/m397_dot_insert.py build/fast/App/Natron'
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 120
SETTLE_MS = 400
DRAG_STEPS = 8
PIPE_GAP = 300.0
DOT_DRAG = (40.0, 20.0)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def box(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def centre(node):
        x, y, w, h = box(node)
        return (x + w / 2.0, y + h / 2.0)

    def same(a, b):
        return a is not None and b is not None and a.getScriptName() == b.getScriptName()

    def is_dot(node):
        return node is not None and node.getPluginID().split(".")[-1] == "Dot"

    def view_at(graph, scene_centre):
        graph.resetTransform()
        for _ in range(3):
            QApplication.processEvents()
            graph.centerOn(scene_centre[0], scene_centre[1])
        QApplication.processEvents()

    def to_vp(graph, scene_pt):
        return QPointF(graph.mapFromScene(QPointF(scene_pt[0], scene_pt[1])))

    def send_mouse(widget, etype, pos, button, buttons, modifiers):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def send_key(widget, etype, key, modifiers):
        QApplication.sendEvent(widget, QKeyEvent(etype, key, modifiers))
        QApplication.processEvents()

    def ctrl_drag(graph, start, end, finish=None):
        """Presses at start with Ctrl held, moves to end in steps, runs finish (if any) with the
        button still down, then releases. Ctrl is pressed as a key first, as a user holds it."""
        vp = graph.viewport()
        ctrl = Qt.ControlModifier
        send_key(graph, QEvent.KeyPress, Qt.Key_Control, ctrl)
        start = QPointF(start)
        end = QPointF(end)
        send_mouse(vp, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, ctrl)
        for i in range(1, DRAG_STEPS + 1):
            send_mouse(vp, QEvent.MouseMove, start + (end - start) * (i / float(DRAG_STEPS)),
                       Qt.NoButton, Qt.LeftButton, ctrl)
        if finish is not None:
            finish()
        send_mouse(vp, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, ctrl)
        send_key(graph, QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("a-insert")
        source = create("Constant", 0, 0)
        dest = create("Blur", 0, PIPE_GAP)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        sx, sy, sw, sh = box(source)
        dx, dy, dw, dh = box(dest)
        # Centred in one column, the pipe is vertical, so its midpoint (the bend point) is known.
        dest.setPosition(sx + sw / 2.0 - dw / 2.0, dy)
        gt.require(dest.connectInput(0, source), "connect the Blur to the Constant")
        yield ("sleep", SETTLE_MS)
        sx, sy, sw, sh = box(source)
        dx, dy, dw, dh = box(dest)
        bend = (sx + sw / 2.0, (sy + sh + dy) / 2.0)
        report("NOTE source %r, dest %r, bend point %r" % (box(source), box(dest), bend))

        graph = root_graph()
        view_at(graph, bend)
        start = to_vp(graph, bend)
        end = to_vp(graph, (bend[0] + DOT_DRAG[0], bend[1] + DOT_DRAG[1]))
        ctrl_drag(graph, start, end)
        yield ("sleep", SETTLE_MS)

        dot = dest.getInput(0)
        report("NOTE the Blur's input after the Ctrl+click: %r" % (dot.getScriptName() if dot else None,))
        gt.require(is_dot(dot), "a-insert: Ctrl+click on the bend point puts a Dot on the Blur's input")
        check(same(dot.getInput(0), source), "a-insert: the Dot is fed by the Constant")
        dot_x, dot_y = dot.getPosition()
        check(abs(dot_x - bend[0]) > 1.0 or abs(dot_y - bend[1]) > 1.0,
              "a-insert: the Ctrl-drag that followed moved the Dot (at %r, bend point %r)" % ((dot_x, dot_y), bend))
        check(app.getNode(dot.getScriptName()) is not None, "a-insert: Natron is alive and the Dot is in the graph")

        gt.section("b-aim-at-dot")
        aimer = create("Grade", sx + 400.0, sy)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        dot_centre = centre(dot)
        aim_start = centre(aimer)
        graph = root_graph()
        view_at(graph, dot_centre)
        # The Grade's free input tip sweeps over the Dot, which makes the Dot a connection target.
        aim_end = (dot_centre[0], dot_centre[1] + 40.0)
        sweep_start = to_vp(graph, aim_start)
        sweep_end = to_vp(graph, aim_end)
        ctrl_drag(graph, sweep_start, sweep_end,
                  finish=lambda: send_key(graph, QEvent.KeyPress, Qt.Key_Escape, Qt.ControlModifier))
        yield ("sleep", SETTLE_MS)
        check(app.getNode(aimer.getScriptName()) is not None and app.getNode(dot.getScriptName()) is not None,
              "b-aim-at-dot: Natron is alive after a Ctrl-drag aimed at the Dot")
        check(same(dest.getInput(0), dot) and same(dot.getInput(0), source),
              "b-aim-at-dot: Escape leaves the Constant -> Dot -> Blur wiring alone")

        gt.section("c-create-with-dot-selected")
        app.clearSelection()
        app.selectNode(dot, True)
        yield ("sleep", SETTLE_MS)
        placed = app.createNode("Grade", -1, None,
                                {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)})
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        check(placed is not None and app.getNode(placed.getScriptName()) is not None,
              "c-create-with-dot-selected: a node created with only the Dot selected is placed beside it")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
