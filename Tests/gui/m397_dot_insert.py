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
# Turns on the node graph's Dot-insertion diagnostics in natron.log; the graph reads the variable
# on every event, so setting it from this script inside the Natron process is enough.

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
    from PySide6.QtWidgets import QApplication, QGraphicsLineItem

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

    def press_ctrl(graph):
        """Ctrl goes down as X11 delivers it: the event carries the modifier state from before the
        press. QKeyEvent::modifiers() flips the bit of the key's own modifier, so an event built
        with ControlModifier would report no modifier and the graph would not show bend points."""
        graph.setFocus()
        send_key(graph, QEvent.KeyPress, Qt.Key_Control, Qt.NoModifier)

    def release_ctrl(graph):
        send_key(graph, QEvent.KeyRelease, Qt.Key_Control, Qt.ControlModifier)

    def ctrl_drag(graph, start, end, finish=None):
        """Presses at start with Ctrl held, moves to end in steps, runs finish (if any) with the
        button still down, then releases. Ctrl is pressed as a key first, as a user holds it."""
        vp = graph.viewport()
        ctrl = Qt.ControlModifier
        press_ctrl(graph)
        start = QPointF(start)
        end = QPointF(end)
        send_mouse(vp, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, ctrl)
        for i in range(1, DRAG_STEPS + 1):
            send_mouse(vp, QEvent.MouseMove, start + (end - start) * (i / float(DRAG_STEPS)),
                       Qt.NoButton, Qt.LeftButton, ctrl)
        if finish is not None:
            finish()
        send_mouse(vp, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, ctrl)
        release_ctrl(graph)

    def bend_point(graph, src, dst):
        """The scene position of the bend point of the pipe from src to dst, from the Edge item.
        Edge::initLineNow() sets the item's line from dst's boundary to src's centre, or in the
        Nub output style (the default) to the middle of src's bottom edge, where the nub is; the
        bend point (Edge::middlePoint, grabbed within 10 scene units by Edge::isNearbyBendPoint) is
        the midpoint of the part of that line between the two node boundaries."""
        sx, sy, sw, sh = box(src)
        src_c = QPointF(sx + sw / 2.0, sy + sh / 2.0)
        src_bottom = QPointF(sx + sw / 2.0, sy + sh)
        for item in graph.scene().items():
            if not isinstance(item, QGraphicsLineItem):
                continue
            line = item.line()
            p1 = item.mapToScene(line.p1())
            p2 = item.mapToScene(line.p2())
            from_nub = (p2 - src_bottom).manhattanLength() <= 3.0
            if not from_nub and (p2 - src_c).manhattanLength() > 3.0:
                continue
            dx, dy, dw, dh = box(dst)
            if not (dx - 3.0 <= p1.x() <= dx + dw + 3.0 and dy - 3.0 <= p1.y() <= dy + dh + 3.0):
                continue
            # Walk from the source centre towards the destination boundary until leaving the source
            # box; a pipe from the nub already starts on the boundary.
            d = p1 - p2
            tmin = 0.0 if from_nub else 1.0
            if not from_nub and abs(d.x()) > 1e-9:
                tmin = min(tmin, (sw / 2.0) / abs(d.x()))
            if not from_nub and abs(d.y()) > 1e-9:
                tmin = min(tmin, (sh / 2.0) / abs(d.y()))
            leave = p2 + d * tmin
            mid = (leave + p1) / 2.0
            report("NOTE pipe item line %r -> %r, leaves the source at %r" % ((p1.x(), p1.y()), (p2.x(), p2.y()), (leave.x(), leave.y())))
            return (mid.x(), mid.y())
        return None

    def wiring():
        """Every node in the graph with what feeds its first input, for the log."""
        parts = []
        for n in app.getChildren():
            if n.getMaxInputCount() < 1:
                continue
            fed = n.getInput(0)
            parts.append("%s<-%s" % (n.getScriptName(), fed.getScriptName() if fed else None))
        return ", ".join(parts)

    def check_inserted(label, source, dest):
        """Checks that dest is fed by a Dot that the source feeds, and returns that Dot."""
        dot = dest.getInput(0)
        report("NOTE %s: wiring %s" % (label, wiring()))
        check(is_dot(dot), "%s: the Blur's input is a Dot" % label)
        check(is_dot(dot) and same(dot.getInput(0), source), "%s: the Dot is fed by the Constant" % label)
        return dot if is_dot(dot) else None

    def make_pipe(label, x):
        """A Constant feeding a Blur straight below it, so the pipe's bend point is known."""
        source = create("Constant", x, 0)
        dest = create("Blur", x, PIPE_GAP)
        gt.close_all_panels()
        sx, sy, sw, sh = box(source)
        dx, dy, dw, dh = box(dest)
        dest.setPosition(sx + sw / 2.0 - dw / 2.0, dy)
        gt.require(dest.connectInput(0, source), "%s: connect the Blur to the Constant" % label)
        return source, dest

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("a-click")
        source, dest = make_pipe("a-click", 0)
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        bend = bend_point(graph, source, dest)
        gt.require(bend is not None, "a-click: found the Constant -> Blur pipe item in the graph scene")
        report("NOTE source %r, dest %r, bend point %r" % (box(source), box(dest), bend))
        view_at(graph, bend)
        at = to_vp(graph, bend)
        vp = graph.viewport()
        ctrl = Qt.ControlModifier
        press_ctrl(graph)
        send_mouse(vp, QEvent.MouseButtonPress, at, Qt.LeftButton, Qt.LeftButton, ctrl)
        yield ("sleep", SETTLE_MS)
        gt.shot("a_click_pressed.png")
        check_inserted("a-click (button down)", source, dest)
        send_mouse(vp, QEvent.MouseButtonRelease, at, Qt.LeftButton, Qt.NoButton, ctrl)
        release_ctrl(graph)
        # A click that grabbed the pipe instead picks it up; Escape drops it so the next section
        # starts from a clean graph.
        send_key(graph, QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier)
        send_key(graph, QEvent.KeyRelease, Qt.Key_Escape, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        gt.shot("a_click_released.png")
        check_inserted("a-click (released)", source, dest)

        gt.section("a-insert")
        source, dest = make_pipe("a-insert", 600)
        yield ("sleep", SETTLE_MS)
        sx, sy, sw, sh = box(source)
        graph = root_graph()
        bend = bend_point(graph, source, dest)
        gt.require(bend is not None, "a-insert: found the Constant -> Blur pipe item in the graph scene")
        report("NOTE source %r, dest %r, bend point %r" % (box(source), box(dest), bend))
        view_at(graph, bend)
        start = to_vp(graph, bend)
        end = to_vp(graph, (bend[0] + DOT_DRAG[0], bend[1] + DOT_DRAG[1]))
        ctrl_drag(graph, start, end)
        yield ("sleep", SETTLE_MS)
        gt.shot("a_insert_dragged.png")

        dot = check_inserted("a-insert", source, dest)
        gt.require(dot is not None, "a-insert: Ctrl+click on the bend point puts a Dot on the Blur's input")
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
