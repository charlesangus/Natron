"""Kiss connections: while a node is dragged, holding Ctrl previews the connection its nearest free
input arrow tip or its output would make with a node in reach, drawn in the Preferences Selection
colour, and releasing Ctrl makes it while the drag goes on. Covers an input kiss, an output kiss, two
kisses in one drag, only unconnected inputs kissing, no kiss without Ctrl or out of reach, Alt at
release snapping the node back with its new pipes, one undo step per drag, and Escape undoing the
drag's kisses.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m194-shots/out Tests/gui/run-gui-test.sh Tests/gui/m194_kiss.py build/fast/App/Natron
Output: kiss-*.png (the node graph viewport).

Drags are synthetic QMouseEvents sent to the NodeGraph's viewport, and Ctrl is a QKeyEvent sent to
the NodeGraph itself, which has the click focus once a node is pressed. Connections are read through
the Effect API. The main window must be active before any drag: QGraphicsItem::isActive() is false
in an inactive window, so a press would miss the node.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

SETTLE_MS = 300
ZOOM = 1.5
TOLERANCE = 40
DRAG_STEPS = 4
# The dragged node stops this far below the target: its input arrow tips, about 25 scene units
# above its top, then sit inside the target.
INPUT_GAP = 8.0
# The dragged node's bottom stops this far above the target, inside the kiss reach (18 viewport px).
OUTPUT_GAP = 5.0
KISS_RGB_F = (0.1, 0.9, 0.2)

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

    def below(target, mover, gap):
        """Where mover's centre goes for its top to sit gap under target, centred on it."""
        x, y, w, h = box(target)
        return (x + w / 2.0, y + h + gap + mover.getSize()[1] / 2.0)

    def above(target, mover, gap):
        """Where mover's centre goes for its bottom to sit gap over target, centred on it."""
        x, y, w, _h = box(target)
        return (x + w / 2.0, y - gap - mover.getSize()[1] / 2.0)

    def same(a, b):
        return a is not None and b is not None and a.getScriptName() == b.getScriptName()

    def inputs_from(node, source):
        return [i for i in range(node.getMaxInputCount()) if same(node.getInput(i), source)]

    def near(p, q, tol=1.0):
        return abs(p[0] - q[0]) < tol and abs(p[1] - q[1]) < tol

    def show(graph, nodes):
        xs = [box(n)[0] for n in nodes] + [box(n)[0] + box(n)[2] for n in nodes]
        ys = [box(n)[1] for n in nodes] + [box(n)[1] + box(n)[3] for n in nodes]
        graph.resetTransform()
        graph.scale(ZOOM, ZOOM)
        graph.centerOn((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        QApplication.processEvents()

    def fresh(graph, nodes):
        app.clearSelection()
        gt.close_all_panels()
        show(graph, nodes)

    def send_mouse(graph, etype, scene_pos, button, buttons, modifiers=Qt.NoModifier):
        p = graph.mapFromScene(QPointF(scene_pos[0], scene_pos[1]))
        QApplication.sendEvent(graph.viewport(), QMouseEvent(etype, QPointF(p), button, buttons, modifiers))
        QApplication.processEvents()

    def press(graph, node):
        pos = centre(node)
        send_mouse(graph, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
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

    def ctrl_down(graph, pos):
        """Presses Ctrl and nudges the mouse so the held drag sees it on a move as well."""
        key(graph, QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier)
        send_mouse(graph, QEvent.MouseMove, (pos[0] + 0.5, pos[1]), Qt.NoButton, Qt.LeftButton, Qt.ControlModifier)
        send_mouse(graph, QEvent.MouseMove, pos, Qt.NoButton, Qt.LeftButton, Qt.ControlModifier)

    def ctrl_up(graph):
        key(graph, QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier)

    def kiss_at(graph, pos):
        ctrl_down(graph, pos)
        ctrl_up(graph)

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

    def undo():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = act.menu()
                if menu is None or "edit" not in act.text().lower():
                    continue
                for a in menu.actions():
                    if a.text().lower().startswith("undo") and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        check(False, "found an enabled Undo action in the Edit menu")
        return False

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
        set_selection(*KISS_RGB_F)

        # ---- input kiss, with the preview, a kiss that the rest of the drag keeps, and one undo step.
        gt.section("input-kiss")
        src = create("net.sf.openfx.ConstantPlugin", 0, 0)
        mover = create("net.sf.cimg.CImgBlur", 300, 300)
        fresh(graph, [src, mover])
        yield ("sleep", SETTLE_MS)
        pre = mover.getPosition()
        start = press(graph, mover)
        dest = below(src, mover, INPUT_GAP)
        pos = move(graph, start, dest)
        sx, sy, sw, sh = box(src)
        strip = (sx + sw / 2.0 - 20.0, sy + sh + 2.0, sx + sw / 2.0 + 20.0, sy + sh + INPUT_GAP - 2.0)
        yield ("sleep", SETTLE_MS)
        hits = count_selection_colour(graph, strip)
        check(hits == 0, "input-kiss: without Ctrl no preview is drawn (%d px)" % hits)
        ctrl_down(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "kiss-01-input-preview")
        hits = count_selection_colour(graph, strip)
        check(hits > 0, "input-kiss: with Ctrl the preview pipe is drawn in the Selection colour (%d px)" % hits)
        check(not inputs_from(mover, src), "input-kiss: the preview alone connects nothing")
        ctrl_up(graph)
        yield ("sleep", SETTLE_MS)
        check(inputs_from(mover, src) == [0], "input-kiss: releasing Ctrl connects input 0 to the target")
        hits = count_selection_colour(graph, strip)
        check(hits == 0, "input-kiss: the preview is gone once the kiss is made (%d px)" % hits)
        drop = (dest[0] + 150.0, dest[1] + 100.0)
        pos = move(graph, pos, drop)
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "kiss-02-input-dropped")
        check(inputs_from(mover, src) == [0], "input-kiss: the kiss survives the rest of the drag")
        check(not near(mover.getPosition(), pre), "input-kiss: the node stays where it was dropped")
        undo()
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(mover, src), "input-kiss: one undo removes the kiss")
        check(near(mover.getPosition(), pre), "input-kiss: the same undo restores the pickup position")

        # ---- output kiss: the dragged node's output feeds the target's first free input.
        gt.section("output-kiss")
        target = create("net.sf.cimg.CImgBlur", 1000, 300)
        mover = create("net.sf.openfx.ConstantPlugin", 1300, 0)
        fresh(graph, [target, mover])
        yield ("sleep", SETTLE_MS)
        start = press(graph, mover)
        pos = move(graph, start, above(target, mover, OUTPUT_GAP))
        ctrl_down(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "kiss-03-output-preview")
        ctrl_up(graph)
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(inputs_from(target, mover) == [0], "output-kiss: the target's input 0 now takes the dragged node")

        # ---- two kisses in one drag: the second goes to the input the first left free.
        gt.section("two-kisses")
        c1 = create("net.sf.openfx.ConstantPlugin", 2000, 0)
        c2 = create("net.sf.openfx.ConstantPlugin", 2400, 0)
        merge = create("Merge", 2200, 400)
        fresh(graph, [c1, c2, merge])
        yield ("sleep", SETTLE_MS)
        pre = merge.getPosition()
        start = press(graph, merge)
        pos = move(graph, start, below(c1, merge, INPUT_GAP))
        kiss_at(graph, pos)
        first = inputs_from(merge, c1)
        check(len(first) == 1, "two-kisses: the first kiss connects one input to the first node (%r)" % first)
        pos = move(graph, pos, below(c2, merge, INPUT_GAP))
        kiss_at(graph, pos)
        second = inputs_from(merge, c2)
        check(len(second) == 1 and second != first,
              "two-kisses: the second kiss connects another input to the second node (%r, %r)" % (first, second))
        check(inputs_from(merge, c1) == first, "two-kisses: the second kiss leaves the first in place")
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        shot(graph, "kiss-04-two-kisses")
        undo()
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(merge, c1) and not inputs_from(merge, c2),
              "two-kisses: one undo removes both kisses")
        check(near(merge.getPosition(), pre), "two-kisses: the same undo restores the pickup position")

        # ---- only unconnected inputs kiss: a connected input is never rewired.
        gt.section("only-unconnected")
        x = create("net.sf.openfx.ConstantPlugin", 3000, 0)
        y = create("net.sf.openfx.ConstantPlugin", 3400, 0)
        mover = create("net.sf.cimg.CImgBlur", 3000, 300)
        check(mover.connectInput(0, x), "only-unconnected: connect the Blur's input 0")
        fresh(graph, [x, y, mover])
        yield ("sleep", SETTLE_MS)
        start = press(graph, mover)
        pos = move(graph, start, below(y, mover, INPUT_GAP))
        kiss_at(graph, pos)
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(inputs_from(mover, x) == [0], "only-unconnected: the connected input keeps its source")
        check(not inputs_from(mover, y), "only-unconnected: the node in reach is not connected")

        # ---- no Ctrl, no kiss; and with Ctrl, nothing out of reach.
        gt.section("no-ctrl-or-out-of-reach")
        src = create("net.sf.openfx.ConstantPlugin", 4000, 0)
        mover = create("net.sf.cimg.CImgBlur", 4300, 300)
        fresh(graph, [src, mover])
        yield ("sleep", SETTLE_MS)
        start = press(graph, mover)
        pos = move(graph, start, below(src, mover, INPUT_GAP))
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(mover, src), "no-ctrl: a drag without Ctrl connects nothing")
        start = press(graph, mover)
        pos = move(graph, start, below(src, mover, 200.0))
        kiss_at(graph, pos)
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(mover, src), "out-of-reach: Ctrl with nothing in reach connects nothing")

        # ---- Alt at release: back to the pickup position, with the new pipe.
        gt.section("alt-snap-back")
        src = create("net.sf.openfx.ConstantPlugin", 5000, 0)
        mover = create("net.sf.cimg.CImgBlur", 5300, 300)
        fresh(graph, [src, mover])
        yield ("sleep", SETTLE_MS)
        pre = mover.getPosition()
        start = press(graph, mover)
        pos = move(graph, start, below(src, mover, INPUT_GAP))
        kiss_at(graph, pos)
        release(graph, pos, Qt.AltModifier)
        yield ("sleep", SETTLE_MS)
        shot(graph, "kiss-05-alt-snap-back")
        check(near(mover.getPosition(), pre), "alt-snap-back: the node is back where it was picked up (%r vs %r)"
              % (mover.getPosition(), pre))
        check(inputs_from(mover, src) == [0], "alt-snap-back: the kissed pipe stays")
        undo()
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(mover, src), "alt-snap-back: one undo removes the kiss")
        check(near(mover.getPosition(), pre), "alt-snap-back: the node is still at its pickup position after undo")

        # ---- Escape undoes the kisses made earlier in the drag.
        gt.section("escape")
        src = create("net.sf.openfx.ConstantPlugin", 6000, 0)
        mover = create("net.sf.cimg.CImgBlur", 6300, 300)
        fresh(graph, [src, mover])
        yield ("sleep", SETTLE_MS)
        start = press(graph, mover)
        pos = move(graph, start, below(src, mover, INPUT_GAP))
        kiss_at(graph, pos)
        check(inputs_from(mover, src) == [0], "escape: the kiss is made before Escape")
        key(graph, QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier)
        key(graph, QEvent.KeyRelease, Qt.Key_Escape, Qt.NoModifier)
        check(not inputs_from(mover, src), "escape: Escape undoes the kiss at once")
        kiss_at(graph, pos)
        check(not inputs_from(mover, src), "escape: no more kisses for the rest of the drag")
        release(graph, pos)
        yield ("sleep", SETTLE_MS)
        check(not inputs_from(mover, src), "escape: the kiss stays undone after the drop")

        set_selection(*default_sel)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
