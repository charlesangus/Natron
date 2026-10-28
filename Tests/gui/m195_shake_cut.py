"""Shake and cut disconnect. Shaking a dragged node (three fast reversals) disconnects all its pipes
at once while the drag goes on, and the drag, move included, is one undo step; a slow back-and-forth
does nothing; a shaken multi-selection keeps the pipes inside it; a Dot shakes like any node.
Ctrl-dragging from empty canvas draws a cut line that highlights the pipes it crosses in the
Preferences Selection colour and cuts them on release in one undo step; letting go of Ctrl mid-cut
keeps cutting; Escape cancels; a cut that crosses nothing changes nothing; Ctrl+Alt and a plain drag
do not cut, and a plain drag still box-selects.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m195-shots/out Tests/gui/run-gui-test.sh Tests/gui/m195_shake_cut.py build/fast/App/Natron
Output: shake-*.png and cut-*.png (the node graph viewport).

Drags are synthetic QMouseEvents sent to the NodeGraph's viewport and keys are QKeyEvents sent to the
NodeGraph. A shake is timed by the graph from the press, so its moves are sent back to back without
processing events in between, and the slow drag sleeps between swings. Connections are read through
the Effect API. The main window must be active before any drag: QGraphicsItem::isActive() is false
in an inactive window, so a press would miss the node.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

SETTLE_MS = 300
ZOOM = 1.0
TOLERANCE = 40
DRAG_STEPS = 4
# Each swing of a shake, in scene units: three times the detector's minimum amplitude.
SWING = 120.0
# Between the turning points of the slow back-and-forth: past the detector's 500 ms window for three.
SLOW_SWING_MS = 300
SEL_RGB_F = (0.1, 0.9, 0.2)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
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

    def place_under(src, node, gap):
        """Puts node gap under src, centred on it, so the pipe between them is vertical."""
        x, y, w, h = box(src)
        nw = node.getSize()[0]
        node.setPosition(x + w / 2.0 - nw / 2.0, y + h + gap)

    def same(a, b):
        return a is not None and b is not None and a.getScriptName() == b.getScriptName()

    def fed_by(node, source, input_nb=0):
        return same(node.getInput(input_nb), source)

    def near(p, q, tol=1.0):
        return abs(p[0] - q[0]) < tol and abs(p[1] - q[1]) < tol

    def show(graph, nodes, extra=0.0):
        xs = [box(n)[0] for n in nodes] + [box(n)[0] + box(n)[2] for n in nodes]
        ys = [box(n)[1] for n in nodes] + [box(n)[1] + box(n)[3] for n in nodes]
        graph.resetTransform()
        graph.scale(ZOOM, ZOOM)
        graph.centerOn((min(xs) + max(xs)) / 2.0 + extra, (min(ys) + max(ys)) / 2.0)
        QApplication.processEvents()

    def fresh(graph, nodes, extra=0.0):
        app.clearSelection()
        gt.close_all_panels()
        show(graph, nodes, extra)

    def send_mouse(graph, etype, scene_pos, button, buttons, modifiers=Qt.NoModifier, process=True):
        p = graph.mapFromScene(QPointF(scene_pos[0], scene_pos[1]))
        QApplication.sendEvent(graph.viewport(), QMouseEvent(etype, QPointF(p), button, buttons, modifiers))
        if process:
            QApplication.processEvents()

    def press_at(graph, pos, modifiers=Qt.NoModifier):
        send_mouse(graph, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        return pos

    def move(graph, start, end, modifiers=Qt.NoModifier, steps=DRAG_STEPS):
        for i in range(1, steps + 1):
            f = i / float(steps)
            pos = (start[0] + (end[0] - start[0]) * f, start[1] + (end[1] - start[1]) * f)
            send_mouse(graph, QEvent.MouseMove, pos, Qt.NoButton, Qt.LeftButton, modifiers)
        return end

    def release(graph, pos, modifiers=Qt.NoModifier):
        send_mouse(graph, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)

    def key(graph, etype, k, modifiers):
        QApplication.sendEvent(graph, QKeyEvent(etype, k, modifiers))
        QApplication.processEvents()

    def shake(graph, pos, swings=5, modifiers=Qt.NoModifier):
        """Fast horizontal swings of SWING about pos, two moves per swing, sent back to back."""
        x, y = pos
        for i in range(swings):
            to = x + SWING if i % 2 == 0 else x
            frm = x if i % 2 == 0 else x + SWING
            send_mouse(graph, QEvent.MouseMove, ((frm + to) / 2.0, y), Qt.NoButton, Qt.LeftButton, modifiers, False)
            send_mouse(graph, QEvent.MouseMove, (to, y), Qt.NoButton, Qt.LeftButton, modifiers, False)
        QApplication.processEvents()
        return (x + SWING if swings % 2 == 1 else x, y)

    def selection_rgb():
        p = NatronEngine.natron.getSettings().getParam("selection")
        return tuple(int(max(0.0, min(1.0, p.getValue(d))) * 255 + 0.5) for d in range(3))

    def set_selection(r, g, b):
        NatronEngine.natron.getSettings().getParam("selection").set(r, g, b, 1.0)
        QApplication.processEvents()

    def grab(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        vp.repaint()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())

    def count_selection_colour(graph, scene_rect):
        """Pixels near the Selection colour inside the scene rectangle (x0, y0, x1, y1)."""
        rgb = selection_rgb()
        img = grab(graph).toImage()
        tl = graph.mapFromScene(QPointF(scene_rect[0], scene_rect[1]))
        br = graph.mapFromScene(QPointF(scene_rect[2], scene_rect[3]))
        n = 0
        for y in range(max(0, tl.y()), min(img.height(), br.y() + 1)):
            for x in range(max(0, tl.x()), min(img.width(), br.x() + 1)):
                c = img.pixelColor(x, y)
                if (abs(c.red() - rgb[0]) <= TOLERANCE and abs(c.green() - rgb[1]) <= TOLERANCE
                        and abs(c.blue() - rgb[2]) <= TOLERANCE):
                    n += 1
        return n

    def shot(graph, name):
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if grab(graph).save(path) else "NOTE could not save ") + path)

    def edit_actions():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = act.menu()
                if menu is not None and "edit" in act.text().lower():
                    return list(menu.actions())
        return []

    def undo_state():
        """The Undo action's text and whether it is enabled, to tell whether a command was pushed."""
        for a in edit_actions():
            if a.text().lower().startswith("undo"):
                return (a.text(), a.isEnabled())
        return None

    def undo():
        for a in edit_actions():
            if a.text().lower().startswith("undo") and a.isEnabled():
                a.trigger()
                QApplication.processEvents()
                return True
        check(False, "found an enabled Undo action in the Edit menu")
        return False

    def pipe_strip(src, dest, cut_y):
        """The upper part of the vertical pipe from src down to dest, clear of a cut line at cut_y."""
        sx, sy, sw, sh = box(src)
        cx = sx + sw / 2.0
        return (cx - 4.0, sy + sh + 4.0, cx + 4.0, cut_y - 12.0)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, 60.0)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active",
                   "QGraphicsItem::isActive() is false in an inactive window, so the press misses the node")
        graph = root_graph()
        gt.require(graph is not None, "a visible node graph")
        default_sel = NatronEngine.natron.getSettings().getParam("selection")
        default_sel = tuple(default_sel.getValue(d) for d in range(3))
        set_selection(*SEL_RGB_F)

        # ---- a fast shake disconnects the node's input and output during the drag; one undo.
        gt.section("shake")
        src = create("net.sf.openfx.ConstantPlugin", 0, 0)
        mover = create("net.sf.cimg.CImgBlur", 0, 0)
        down = create("net.sf.cimg.CImgBlur", 0, 0)
        place_under(src, mover, 120.0)
        place_under(mover, down, 120.0)
        check(mover.connectInput(0, src), "shake: connect the middle node to the source")
        check(down.connectInput(0, mover), "shake: connect the bottom node to the middle one")
        fresh(graph, [src, mover, down], 100.0)
        yield ("sleep", SETTLE_MS)
        pre = mover.getPosition()
        start = press_at(graph, centre(mover))
        pos = move(graph, start, (start[0] + 200.0, start[1]))
        check(fed_by(mover, src) and fed_by(down, mover), "shake: a plain drag keeps the pipes")
        pos = shake(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "shake-01-disconnected-mid-drag")
        check(mover.getInput(0) is None, "shake: the shaken node's input is disconnected during the drag")
        check(down.getInput(0) is None, "shake: the node it fed is disconnected during the drag")
        pos = move(graph, pos, (pos[0] + 60.0, pos[1] + 40.0))
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(mover.getInput(0) is None and down.getInput(0) is None, "shake: the pipes stay cut after the drop")
        check(not near(mover.getPosition(), pre), "shake: the node stays where it was dropped")
        undo()
        yield ("sleep", SETTLE_MS)
        check(fed_by(mover, src) and fed_by(down, mover), "shake: one undo restores both pipes")
        check(near(mover.getPosition(), pre), "shake: the same undo restores the pickup position (%r vs %r)"
              % (mover.getPosition(), pre))

        # ---- a slow back-and-forth with the same swings does not shake.
        gt.section("slow-drag")
        start = press_at(graph, centre(mover))
        pos = move(graph, start, (start[0] + 200.0, start[1]))
        for i in range(6):
            to = (pos[0] + SWING, pos[1]) if i % 2 == 0 else (pos[0] - SWING, pos[1])
            pos = move(graph, pos, to, steps=2)
            yield ("sleep", SLOW_SWING_MS)
        check(fed_by(mover, src) and fed_by(down, mover), "slow-drag: slow swings keep the pipes")
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(fed_by(mover, src) and fed_by(down, mover), "slow-drag: the pipes are still there after the drop")
        undo()
        yield ("sleep", SETTLE_MS)

        # ---- a shaken selection keeps the pipe between its nodes and loses the ones leaving it.
        gt.section("multi-selection")
        a = create("net.sf.openfx.ConstantPlugin", 2000, 0)
        b = create("net.sf.cimg.CImgBlur", 0, 0)
        c = create("net.sf.cimg.CImgBlur", 0, 0)
        d = create("net.sf.cimg.CImgBlur", 0, 0)
        place_under(a, b, 100.0)
        place_under(b, c, 100.0)
        place_under(c, d, 100.0)
        check(b.connectInput(0, a) and c.connectInput(0, b) and d.connectInput(0, c), "multi-selection: chain a-b-c-d")
        fresh(graph, [a, b, c, d], 100.0)
        app.selectNode(b, False)
        app.selectNode(c, False)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        start = press_at(graph, centre(b))
        pos = move(graph, start, (start[0] + 200.0, start[1]))
        pos = shake(graph, pos)
        pos = move(graph, pos, (pos[0] + 40.0, pos[1]))
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "shake-02-multi-selection")
        check(b.getInput(0) is None, "multi-selection: the pipe into the selection is cut")
        check(d.getInput(0) is None, "multi-selection: the pipe out of the selection is cut")
        check(fed_by(c, b), "multi-selection: the pipe inside the selection stays")
        undo()
        yield ("sleep", SETTLE_MS)
        check(fed_by(b, a) and fed_by(c, b) and fed_by(d, c), "multi-selection: one undo restores the chain")

        # ---- a Dot shakes loose like any node.
        gt.section("dot")
        src = create("net.sf.openfx.ConstantPlugin", 4000, 0)
        dot = create("fr.inria.built-in.Dot", 0, 0)
        out = create("net.sf.cimg.CImgBlur", 0, 0)
        place_under(src, dot, 100.0)
        place_under(dot, out, 100.0)
        check(dot.connectInput(0, src) and out.connectInput(0, dot), "dot: chain source-dot-blur")
        fresh(graph, [src, dot, out], 100.0)
        yield ("sleep", SETTLE_MS)
        start = press_at(graph, centre(dot))
        pos = move(graph, start, (start[0] + 200.0, start[1]))
        pos = shake(graph, pos)
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(dot.getInput(0) is None and out.getInput(0) is None, "dot: shaking the Dot cuts both its pipes")

        # ---- cut: Ctrl-drag across two pipes highlights them, then cuts both in one undo step.
        gt.section("cut")
        s1 = create("net.sf.openfx.ConstantPlugin", 6000, 0)
        s2 = create("net.sf.openfx.ConstantPlugin", 6400, 0)
        s3 = create("net.sf.openfx.ConstantPlugin", 6800, 0)
        m1 = create("net.sf.cimg.CImgBlur", 0, 0)
        m2 = create("net.sf.cimg.CImgBlur", 0, 0)
        m3 = create("net.sf.cimg.CImgBlur", 0, 0)
        for s, m in ((s1, m1), (s2, m2), (s3, m3)):
            place_under(s, m, 200.0)
            check(m.connectInput(0, s), "cut: connect " + m.getScriptName())
        fresh(graph, [s1, s2, s3, m1, m2, m3])
        yield ("sleep", SETTLE_MS)
        cut_y = box(s1)[1] + box(s1)[3] + 120.0
        left = (box(s1)[0] - 150.0, cut_y)
        between = ((centre(s2)[0] + centre(s3)[0]) / 2.0, cut_y)
        strips = [pipe_strip(s, m, cut_y) for s, m in ((s1, m1), (s2, m2), (s3, m3))]
        before = [count_selection_colour(graph, r) for r in strips]
        check(before == [0, 0, 0], "cut: no pipe is highlighted before the cut (%r)" % before)
        pos = press_at(graph, left, Qt.ControlModifier)
        mid = ((left[0] + between[0]) / 2.0, cut_y + 10.0)
        pos = move(graph, pos, mid, Qt.ControlModifier)
        # Ctrl let go mid-cut: the cut goes on to the release.
        pos = move(graph, pos, between, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        shot(graph, "cut-01-highlight")
        hits = [count_selection_colour(graph, r) for r in strips]
        check(hits[0] > 0 and hits[1] > 0, "cut: both crossed pipes are drawn in the Selection colour (%r)" % hits)
        check(hits[2] == 0, "cut: the pipe not crossed is not highlighted (%r)" % hits)
        check(fed_by(m1, s1) and fed_by(m2, s2), "cut: nothing is cut before the release")
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "cut-02-cut")
        check(m1.getInput(0) is None and m2.getInput(0) is None, "cut: the release cuts both crossed pipes")
        check(fed_by(m3, s3), "cut: the pipe not crossed stays")
        undo()
        yield ("sleep", SETTLE_MS)
        check(fed_by(m1, s1) and fed_by(m2, s2), "cut: one undo restores both pipes")
        hits = [count_selection_colour(graph, r) for r in strips]
        check(hits == [0, 0, 0], "cut: no highlight is left after the cut (%r)" % hits)

        # ---- Escape cancels a cut and clears its highlight.
        gt.section("escape")
        pos = press_at(graph, left, Qt.ControlModifier)
        pos = move(graph, pos, between, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        hits = [count_selection_colour(graph, r) for r in strips[:2]]
        check(hits[0] > 0 and hits[1] > 0, "escape: the pipes are highlighted before Escape (%r)" % hits)
        key(graph, QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier)
        key(graph, QEvent.KeyRelease, Qt.Key_Escape, Qt.NoModifier)
        yield ("sleep", SETTLE_MS)
        hits = [count_selection_colour(graph, r) for r in strips[:2]]
        check(hits == [0, 0], "escape: Escape clears the highlight (%r)" % hits)
        release(graph, pos, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        check(fed_by(m1, s1) and fed_by(m2, s2), "escape: a cancelled cut cuts nothing")

        # ---- a cut that crosses no pipe pushes nothing.
        gt.section("empty-cut")
        undo_before = undo_state()
        below = box(m1)[1] + box(m1)[3] + 80.0
        pos = press_at(graph, (left[0], below), Qt.ControlModifier)
        pos = move(graph, pos, (between[0], below), Qt.ControlModifier)
        release(graph, pos, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        check(fed_by(m1, s1) and fed_by(m2, s2) and fed_by(m3, s3), "empty-cut: every pipe stays")
        check(undo_state() == undo_before, "empty-cut: no undo step is added (%r vs %r)" % (undo_state(), undo_before))

        # ---- a plain drag still box-selects and cuts nothing.
        gt.section("box-select")
        app.clearSelection()
        x0, y0 = box(s1)[0] - 60.0, box(s1)[1] - 60.0
        x1, y1 = box(m1)[0] + box(m1)[2] + 60.0, box(m1)[1] + box(m1)[3] + 60.0
        pos = press_at(graph, (x0, y0))
        pos = move(graph, pos, (x1, y1))
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        names = sorted(n.getScriptName() for n in app.getSelectedNodes())
        check(names == sorted([s1.getScriptName(), m1.getScriptName()]),
              "box-select: the plain drag selects the boxed nodes (%r)" % names)
        check(fed_by(m1, s1) and fed_by(m2, s2), "box-select: the plain drag cuts nothing")

        # ---- Ctrl+Alt is not a cut. Last, because it may pan the graph, which moves every node in
        # scene coordinates.
        gt.section("ctrl-alt")
        pos = press_at(graph, left, Qt.ControlModifier | Qt.AltModifier)
        pos = move(graph, pos, between, Qt.ControlModifier | Qt.AltModifier)
        release(graph, pos, Qt.ControlModifier | Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        check(fed_by(m1, s1) and fed_by(m2, s2), "ctrl-alt: a Ctrl+Alt drag cuts nothing")

        set_selection(*default_sel)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
