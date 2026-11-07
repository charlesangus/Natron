"""Real mouse events on the root graph after resetProject(), twice in a row: a click selects the
node under it, a drag moves it without disconnecting any input, and select-all selects every node.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m118-shots/out GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m118_click_after_reset.py build/fast/App/Natron'

The root graph is the NodeGraph without an object name; a Group's graph is named after the group.
The main window must be active: QGraphicsItem::isActive() is false in an inactive window and
presses are dropped.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
DRAG_STEPS = 4
DRAG_DX = 160.0
ROUNDS = 2
CHAIN = ("Constant", "Grade", "Blur", "Grade")
MARGIN = 80.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(240, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if not w.objectName() and w.isVisible()]
        return graphs[0] if graphs else None

    def activate():
        win = main_window()
        win.activateWindow()
        win.raise_()
        return win

    def build_chain():
        nodes = []
        for i, plugin in enumerate(CHAIN):
            node = app.createNode(plugin, -1, None)
            gt.require(node is not None, "create " + plugin, gt.plugin_problem(plugin))
            node.setPosition(0, i * 200)
            if nodes:
                gt.require(node.connectInput(0, nodes[-1]), "connect %s to %s" % (node.getScriptName(),
                                                                                  nodes[-1].getScriptName()))
            nodes.append(node)
        return nodes

    def frame(graph, nodes):
        xs = [n.getPosition()[0] for n in nodes]
        ys = [n.getPosition()[1] for n in nodes]
        bw = max(xs) - min(xs) + 2 * MARGIN + DRAG_DX + max(n.getSize()[0] for n in nodes)
        bh = max(ys) - min(ys) + 2 * MARGIN + max(n.getSize()[1] for n in nodes)
        vp = graph.viewport()
        want = min(1.5, vp.width() / bw, vp.height() / bh)
        graph.resetTransform()
        graph.scale(want, want)
        graph.centerOn(min(xs) + bw / 2.0 - MARGIN, (max(ys) + min(ys)) / 2.0)
        QApplication.processEvents()

    def grab_point(node):
        """The node's left edge: the pipes between stacked nodes run through its middle."""
        x, y = node.getPosition()
        _w, h = node.getSize()
        return x + 8.0, y + h / 2.0

    def send(graph, etype, scene_pos, button, buttons):
        pos = graph.mapFromScene(QPointF(scene_pos[0], scene_pos[1]))
        QApplication.sendEvent(graph.viewport(), QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def click(graph, scene_pos):
        send(graph, QEvent.MouseButtonPress, scene_pos, Qt.LeftButton, Qt.LeftButton)
        send(graph, QEvent.MouseButtonRelease, scene_pos, Qt.LeftButton, Qt.NoButton)

    def drag(graph, start, end):
        send(graph, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            at = (start[0] + (end[0] - start[0]) * f, start[1] + (end[1] - start[1]) * f)
            send(graph, QEvent.MouseMove, at, Qt.NoButton, Qt.LeftButton)
        send(graph, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def selected_names():
        return sorted(n.getScriptName() for n in app.getSelectedNodes())

    def wiring(nodes):
        """For each node after the first, the script name of whatever feeds input 0."""
        out = []
        for node in nodes[1:]:
            src = node.getInput(0)
            out.append(src.getScriptName() if src is not None else None)
        return out

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the root node graph is up")
        win = activate()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active")
        gt.close_all_panels()

        for round_no in range(1, ROUNDS + 1):
            label = "round%d" % round_no
            gt.section(label)
            gt.require(app.resetProject(), "%s: reset the project" % label)
            ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
            gt.require(ok, "%s: the root graph is up after the reset" % label)
            win = activate()
            ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
            gt.require(ok, "%s: the main window is active after the reset" % label)

            nodes = build_chain()
            names = [n.getScriptName() for n in nodes]
            wired = wiring(nodes)
            gt.close_all_panels()
            app.clearSelection()
            graph = root_graph()
            frame(graph, nodes)
            yield ("sleep", SETTLE_MS)

            target = nodes[1]
            click(graph, grab_point(target))
            check(selected_names() == [target.getScriptName()],
                  "%s: a click selects only the node under it (got %r)" % (label, selected_names()))

            before = target.getPosition()
            start = grab_point(target)
            drag(graph, start, (start[0] + DRAG_DX, start[1]))
            after = target.getPosition()
            check(after[0] - before[0] > DRAG_DX / 2.0,
                  "%s: the drag moved the node (x %.1f -> %.1f)" % (label, before[0], after[0]))
            check(wiring(nodes) == wired,
                  "%s: every input is still connected after the drag (%r, was %r)" % (label, wiring(nodes), wired))

            app.clearSelection()
            app.selectAllNodes()
            QApplication.processEvents()
            check(selected_names() == sorted(names),
                  "%s: select all selects every node (got %r of %r)" % (label, selected_names(), sorted(names)))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
