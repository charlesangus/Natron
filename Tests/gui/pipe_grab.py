"""Checks the node graph pipe preferences: Pipe Width sets the pen of every pipe, connected or not,
Disconnected Arrow Length and Output Stub Length set where a dangling input and an output stub end,
and the grab radii grow with the width (12 / 24 viewport pixels for a connected / dangling pipe at
the default width 3) but never shrink below 10 / 18, nor below half the arrowhead's width. Arrowhead
Width and Arrowhead Length size the heads in pipe widths (5 and 8 by default). Output Style draws an
output that feeds nothing as an arrow stub, or any output as a nub, a half circle under the node (the
default), which is grabbed to drag a connection like the stub; on a node that already feeds another,
the nub adds a connection and leaves the existing one. In the Nub style every pipe a node feeds
leaves from the nub's centre. A Dot has no nub: hovering it shows a ring around its disk, a drag
from the ring pulls a new pipe and a drag from the disk moves the Dot, whose pipes leave from its
centre. With no button down, the pipe a click would pick up is highlighted.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/pipe-grab/out Tests/gui/run-gui-test.sh Tests/gui/pipe_grab.py build/fast/App/Natron
Shots (the node graph viewport): pipe-grab-nub-geometry.png, pipe-grab-nub.png and pipe-grab-arrow.png
(the two output styles), pipe-grab-defaults.png and pipe-grab-defaults-zoom2.png (every pipe
preference at its default: a Blur feeding two others through its nub, their unconnected outputs'
nubs and the pipes' arrow heads), pipe-grab-nub-hover.png (a connected output's nub highlighted
under the cursor), pipe-grab-hover.png (a dangling input highlighted under the cursor),
pipe-grab-dot.png and pipe-grab-dot-ring.png (a Dot without and with the pointer on it).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 400
LINE_ITEM_TYPE = 6
# Pipe ends are laid out from the node's outline, which can sit a pixel or two off getSize().
POSITION_TOLERANCE = 3.0
DEFAULTS = {"pipeWidth": 3, "disconnectedArrowLength": 40, "outputStubLength": 20,
            "arrowheadWidth": 5, "arrowheadLength": 8}
LABELS = {"pipeWidth": "Pipe Width", "disconnectedArrowLength": "Disconnected Arrow Length",
          "outputStubLength": "Output Stub Length", "outputStyle": "Output Style",
          "arrowheadWidth": "Arrowhead Width", "arrowheadLength": "Arrowhead Length"}
OUTPUT_STYLE_ARROW = 0
OUTPUT_STYLE_NUB = 1
OUTPUT_STYLE_IDS = ["arrow", "nub"]
# Edge.cpp: the nub's radius is 0.6 of the Output Stub Length.
NUB_RADIUS = 0.6 * DEFAULTS["outputStubLength"]
GRAB_RADIUS_DEFAULT = 12.0
DANGLING_RADIUS_DEFAULT = 24.0
# Edge.cpp: an arrow head is Arrowhead Width pipe widths across its base and Arrowhead Length pipe
# widths long; its bounds are padded a pixel each side.
ARROW_HEAD_WIDTH_DEFAULT = DEFAULTS["arrowheadWidth"] * DEFAULTS["pipeWidth"]
# Edge.cpp ARROW_OFFSET: the head's tip stops this far short of the node it points at.
ARROW_OFFSET = 3.0
# DotGui.cpp DOT_GUI_DIAMETER; a Dot cannot be resized, so getSize() can report 0 x 0.
DOT_DIAMETER = 15.0
# Edge.cpp DOT_RING_GAP: between the disk and the inner edge of the ring.
DOT_RING_GAP = 3.0
GRAB_RADIUS_MIN = 10.0
DANGLING_RADIUS_MIN = 18.0
# Edge::kHoverHighlightDataKey
HOVER_DATA_KEY = 0
DRAG_OFFSET = (-120, 0)

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    settings = NatronEngine.natron.getSettings()

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    # Looked up afresh after every event pass: a child wrapper does not survive processEvents().
    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def centre(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x + w / 2.0, y + h / 2.0)

    def set_pref(name, value):
        p = settings.getParam(name)
        require(p is not None, "Preferences have a parameter named " + name)
        p.setValue(value)
        settings.saveSettings()
        QApplication.processEvents()

    def pipes():
        """(p1, p2, pen width) in scene coordinates for every visible pipe in the node graph view."""
        graph = root_graph()
        if graph is None:
            return []
        out = []
        try:
            items = list(graph.items())
        except RuntimeError:
            return out
        for it in items:
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                out.append(((p1.x(), p1.y()), (p2.x(), p2.y()), it.pen().widthF()))
            except RuntimeError:
                continue
        return out

    def near(a, b, tol=POSITION_TOLERANCE):
        return abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol

    def dangling_input(node):
        """The upward input arrow of node, as (node end, free end), or None."""
        c = centre(node)
        for p1, p2, _ in pipes():
            if near(p1, c) and abs(p2[0] - p1[0]) < 1.0 and p2[1] < p1[1]:
                return (p1, p2)
        return None

    def bottom_centre(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x + w / 2.0, y + h)

    def output_stub(node):
        """The output stub of node, as (free end, node end), or None. The node end is the node's
        centre in the Arrow style and the nub's centre, the middle of the bottom edge, in the Nub
        style."""
        for p1, p2, _ in pipes():
            if (near(p2, centre(node)) or near(p2, bottom_centre(node))) and abs(p2[0] - p1[0]) < 1.0 and p1[1] > p2[1]:
                return (p1, p2)
        return None

    def dangling_extent(node):
        """The boundingRect height of the upward dangling input of node (line pad plus arrow head), or None."""
        graph = root_graph()
        c = centre(node)
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p1.x(), p1.y()), c) and abs(p2.x() - p1.x()) < 1.0 and p2.y() < p1.y():
                    return it.boundingRect().height()
            except RuntimeError:
                continue
        return None

    def dangling_input_hovered(node):
        """Whether the upward dangling input of node is hover-highlighted, or None if it is not shown."""
        graph = root_graph()
        c = centre(node)
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p1.x(), p1.y()), c) and abs(p2.x() - p1.x()) < 1.0 and p2.y() < p1.y():
                    return bool(it.data(HOVER_DATA_KEY))
            except RuntimeError:
                continue
        return None

    def nub_hovered(node):
        """Whether the output nub of node is hover-highlighted, or None if no nub is shown. Told apart
        from a pipe the node feeds straight down by its free end, NUB_RADIUS below the node."""
        graph = root_graph()
        c = bottom_centre(node)
        free = (c[0], c[1] + NUB_RADIUS)
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p2.x(), p2.y()), c) and near((p1.x(), p1.y()), free):
                    return bool(it.data(HOVER_DATA_KEY))
            except RuntimeError:
                continue
        return None

    def pipe_bounds_width(src, dst, tol):
        """The boundingRect width of the pipe item from src into dst, or None."""
        graph = root_graph()
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p2.x(), p2.y()), centre(src), tol) and near((p1.x(), p1.y()), centre(dst), tol):
                    return it.boundingRect().width()
            except RuntimeError:
                continue
        return None

    def pipe_item(src_end, dst, tol):
        """The pipe item into dst whose source end is within tol of the scene point src_end, or None.
        The pipe's destination end lies on dst's outline, so it is tested against the outline's reach
        from dst's centre (half the larger side) rather than a fixed height."""
        graph = root_graph()
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p2.x(), p2.y()), src_end, tol) and near((p1.x(), p1.y()), centre(dst), max(dst.getSize()) / 2.0 + tol):
                    return it
            except RuntimeError:
                continue
        return None

    def head_contains(item, along, across):
        """Whether the item's shape (pipe and arrow head) holds the scene point along px back from the
        head's tip and across px to the side of the pipe's axis."""
        line = item.line()
        p1 = item.mapToScene(line.p1())
        p2 = item.mapToScene(line.p2())
        dx, dy = p2.x() - p1.x(), p2.y() - p1.y()
        n = (dx * dx + dy * dy) ** 0.5
        ux, uy = dx / n, dy / n
        t = ARROW_OFFSET + along
        pt = QPointF(p1.x() + ux * t - uy * across, p1.y() + uy * t + ux * across)
        return item.shape().contains(item.mapFromScene(pt))

    def dot_centre(dot):
        x, y = dot.getPosition()
        w, h = dot.getSize()
        if w <= 0 or h <= 0:
            w, h = DOT_DIAMETER, DOT_DIAMETER
        return (x + w / 2.0, y + h / 2.0)

    def dot_ring(dot):
        """The output item of a Dot in the Nub style, a ring whose line is a point at the Dot's
        centre, or None."""
        graph = root_graph()
        c = dot_centre(dot)
        for it in (list(graph.items()) if graph is not None else []):
            try:
                if it.type() != LINE_ITEM_TYPE or not it.isVisible():
                    continue
                line = it.line()
                p1 = it.mapToScene(line.p1())
                p2 = it.mapToScene(line.p2())
                if near((p1.x(), p1.y()), c, 1.0) and near((p2.x(), p2.y()), c, 1.0):
                    return it
            except RuntimeError:
                continue
        return None

    def ring_hovered(dot):
        ring = dot_ring(dot)
        return None if ring is None else bool(ring.data(HOVER_DATA_KEY))

    def connected_pipe(src, dst, tol):
        """The pipe from src into dst, as (dst end, src end), or None."""
        for p1, p2, _ in pipes():
            if near(p2, centre(src), tol) and near(p1, centre(dst), tol):
                return (p1, p2)
        return None

    def dist_to_segment(p, a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        l2 = dx * dx + dy * dy
        t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
        qx, qy = a[0] + t * dx, a[1] + t * dy
        return ((p[0] - qx) ** 2 + (p[1] - qy) ** 2) ** 0.5

    def send_mouse(etype, pos, button, buttons):
        graph = root_graph()
        require(graph is not None, "a visible node graph")
        QApplication.sendEvent(graph.viewport(), QMouseEvent(etype, QPointF(pos[0], pos[1]), button, buttons,
                                                             Qt.NoModifier))
        QApplication.processEvents()

    def to_view(scene_pt):
        graph = root_graph()
        p = graph.mapFromScene(QPointF(scene_pt[0], scene_pt[1]))
        return (p.x(), p.y())

    def to_scene(view_pt):
        graph = root_graph()
        p = graph.mapToScene(int(view_pt[0]), int(view_pt[1]))
        return (p.x(), p.y())

    def drag_between(press_view, target):
        """Presses at press_view, drags to target and returns the pipe ends seen at the drag's end,
        then releases there."""
        send_mouse(QEvent.MouseButtonPress, press_view, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 5):
            f = i / 4.0
            p = (press_view[0] + (target[0] - press_view[0]) * f, press_view[1] + (target[1] - press_view[1]) * f)
            send_mouse(QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        target_scene = to_scene(target)
        followed = [(p1, p2) for p1, p2, _ in pipes() if near(p1, target_scene) or near(p2, target_scene)]
        send_mouse(QEvent.MouseButtonRelease, target, Qt.LeftButton, Qt.NoButton)
        return followed

    def drag_from(press_view):
        """drag_between press_view and DRAG_OFFSET from it, over empty graph."""
        return drag_between(press_view, (press_view[0] + DRAG_OFFSET[0], press_view[1] + DRAG_OFFSET[1]))

    def beside_at(seg, frac, offset):
        """The viewport point offset pixels left of the point frac along a near-vertical scene segment."""
        pt = (seg[0][0] + (seg[1][0] - seg[0][0]) * frac, seg[0][1] + (seg[1][1] - seg[0][1]) * frac)
        v = to_view(pt)
        return (v[0] - offset, v[1])

    def shoot(name):
        graph = root_graph()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def beside(seg, offset):
        """The viewport point offset pixels left of the midpoint of a vertical scene segment."""
        mid = ((seg[0][0] + seg[1][0]) / 2.0, (seg[0][1] + seg[1][1]) / 2.0)
        v = to_view(mid)
        return (v[0] - offset, v[1])

    def beside_free_end(stub_seg, offset=15, up=5):
        """The viewport point offset pixels left of a stub's free end, up pixels back along the stub.
        The midpoint of a stub lies against the node's bottom edge, where a press selects the node
        instead of grabbing the pipe."""
        v = to_view((stub_seg[0][0], stub_seg[0][1] - up))
        return (v[0] - offset, v[1])

    def nearest_pipe_px(press_view):
        p = to_scene(press_view)
        # A Dot's ring is a point, not a pipe.
        ds = [dist_to_segment(p, a, b) for a, b, _ in pipes() if not near(a, b, 0.5)]
        return min(ds) if ds else float("inf")

    def steps():
        gt.section("pipe-grab")
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: main_window() is not None and main_window().isActiveWindow(), 10.0)
        require(ok, "the main window is active",
                "QGraphicsItem::isActive() is false in an inactive window, so presses miss the graph")

        for name, value in sorted(DEFAULTS.items()):
            p = settings.getParam(name)
            require(p is not None, "Preferences have a parameter named " + name)
            check(p.getDefaultValue() == value, "%s defaults to %g (got %g)" % (name, value, p.getDefaultValue()))
            check(p.getLabel() == LABELS[name], "%s is labelled %r (got %r)" % (name, LABELS[name], p.getLabel()))
            set_pref(name, value)
        style = settings.getParam("outputStyle")
        require(style is not None, "Preferences have a parameter named outputStyle")
        check(style.getDefaultValue() == OUTPUT_STYLE_NUB, "outputStyle defaults to Nub (got %r)" % style.getDefaultValue())
        check(style.getLabel() == LABELS["outputStyle"], "outputStyle is labelled %r (got %r)"
              % (LABELS["outputStyle"], style.getLabel()))
        options = [str(o) for o in style.getOptions()]
        check(options == OUTPUT_STYLE_IDS, "the output styles are %r (got %r)" % (OUTPUT_STYLE_IDS, options))
        set_pref("outputStyle", OUTPUT_STYLE_NUB)

        lone = create("Blur", 0, 0)
        src = create("Constant", 300, -200)
        dst = create("Blur", 300, 50)
        dst.connectInput(0, src)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        require(graph is not None, "a visible node graph")
        graph.resetTransform()
        lc = centre(lone)
        graph.centerOn(lc[0] + 150, lc[1])
        yield ("sleep", SETTLE_MS)
        check(abs(root_graph().transform().m11() - 1.0) < 1e-9, "the graph is at zoom 1")

        lx, ly = lone.getPosition()
        lw, lh = lone.getSize()
        inp = dangling_input(lone)
        stub = output_stub(lone)
        require(inp is not None, "the lone Blur shows an upward dangling input arrow")
        require(stub is not None, "the lone Blur shows an output stub")
        check(abs((ly - inp[1][1]) - DEFAULTS["disconnectedArrowLength"]) <= POSITION_TOLERANCE,
              "the dangling input reaches %d px above the node (free end at y %g, node top %g)"
              % (DEFAULTS["disconnectedArrowLength"], inp[1][1], ly))
        check(abs((stub[0][1] - (ly + lh)) - NUB_RADIUS) <= POSITION_TOLERANCE,
              "the output nub reaches %g px below the node (free end at y %g, node bottom %g)"
              % (NUB_RADIUS, stub[0][1], ly + lh))
        connected = [w for p1, p2, w in pipes() if near(p2, centre(src), lh) and near(p1, centre(dst), lh)]
        require(connected, "the Constant to Blur pipe is in view")
        shoot("pipe-grab-nub-geometry.png")

        set_pref("outputStyle", OUTPUT_STYLE_ARROW)
        yield ("sleep", SETTLE_MS)
        stub = output_stub(lone)
        require(stub is not None, "the lone Blur shows an output stub in the Arrow style")
        check(abs((stub[0][1] - (ly + lh)) - DEFAULTS["outputStubLength"]) <= POSITION_TOLERANCE,
              "the output stub reaches %d px below the node (free end at y %g, node bottom %g)"
              % (DEFAULTS["outputStubLength"], stub[0][1], ly + lh))

        gt.section("pipe-width")
        for width in (1, 6, DEFAULTS["pipeWidth"]):
            set_pref("pipeWidth", width)
            yield ("sleep", SETTLE_MS)
            widths = sorted(set(w for _, _, w in pipes()))
            check(widths == [float(width)], "Pipe Width %d: every pipe is drawn %d px wide (widths %r)"
                  % (width, width, widths))

        gt.section("pipe-lengths")
        before_inp, before_stub = dangling_input(lone), output_stub(lone)
        set_pref("disconnectedArrowLength", 60)
        set_pref("outputStubLength", 35)
        yield ("sleep", SETTLE_MS)
        after_inp, after_stub = dangling_input(lone), output_stub(lone)
        require(after_inp is not None and after_stub is not None, "the dangling input and output stub are still shown")
        check(abs((before_inp[1][1] - after_inp[1][1]) - 20.0) < 0.5,
              "Disconnected Arrow Length 40 -> 60 moves the free end 20 px up (%g -> %g)"
              % (before_inp[1][1], after_inp[1][1]))
        check(abs((after_stub[0][1] - before_stub[0][1]) - 15.0) < 0.5,
              "Output Stub Length 20 -> 35 moves the free end 15 px down (%g -> %g)"
              % (before_stub[0][1], after_stub[0][1]))
        set_pref("disconnectedArrowLength", DEFAULTS["disconnectedArrowLength"])
        set_pref("outputStubLength", DEFAULTS["outputStubLength"])
        yield ("sleep", SETTLE_MS)

        gt.section("grab-default-width")
        inp, stub = dangling_input(lone), output_stub(lone)
        require(inp is not None and stub is not None, "the dangling input and output stub are back")
        press = beside(inp, 15)
        report("NOTE dangling input press at %r, nearest pipe %.1f px" % (press, nearest_pipe_px(press)))
        followed = drag_from(press)
        check(any(near(a, centre(lone)) for a, _ in followed),
              "a press 15 px beside the dangling input drags it (%r)" % followed)
        yield ("sleep", SETTLE_MS)
        check(not lone.getInput(0), "dropping the dragged input over empty graph leaves it unconnected")

        stub = output_stub(lone)
        require(stub is not None, "the output stub is back after the input drag")
        press = beside_free_end(stub)
        report("NOTE output stub press at %r, nearest pipe %.1f px" % (press, nearest_pipe_px(press)))
        followed = drag_from(press)
        check(any(near(b, centre(lone)) for _, b in followed),
              "a press 15 px beside the output stub drags it (%r)" % followed)
        yield ("sleep", SETTLE_MS)

        gt.section("grab-default-radii")
        for offset, grabs in ((DANGLING_RADIUS_DEFAULT - 3, True), (DANGLING_RADIUS_DEFAULT + 4, False)):
            inp = dangling_input(lone)
            require(inp is not None, "the dangling input is shown")
            press = beside(inp, offset)
            nearest = nearest_pipe_px(press)
            report("NOTE dangling input press %g px beside, nearest pipe %.1f px" % (offset, nearest))
            if not grabs:
                require(nearest > DANGLING_RADIUS_DEFAULT, "no pipe lies within %g px of the press (nearest %.1f px)"
                        % (DANGLING_RADIUS_DEFAULT, nearest))
            followed = drag_from(press)
            dragged = any(near(a, centre(lone)) for a, _ in followed)
            check(dragged == grabs, "at width %d a press %g px beside the dangling input %s (%r)"
                  % (DEFAULTS["pipeWidth"], offset, "drags it" if grabs else "grabs nothing", followed))
            yield ("sleep", SETTLE_MS)
        for width, radius in ((DEFAULTS["pipeWidth"], GRAB_RADIUS_DEFAULT), (1, GRAB_RADIUS_MIN)):
            set_pref("pipeWidth", width)
            yield ("sleep", SETTLE_MS)
            for offset, grabs in ((radius - 3, True), (radius + 3, False)):
                pipe = connected_pipe(src, dst, lh)
                require(pipe is not None, "the Constant to Blur pipe is shown")
                press = beside_at(pipe, 0.35, offset)
                nearest = nearest_pipe_px(press)
                report("NOTE connected pipe press %g px beside, nearest pipe %.1f px" % (offset, nearest))
                if not grabs:
                    require(nearest > radius, "no pipe lies within %g px of the press (nearest %.1f px)"
                            % (radius, nearest))
                followed = drag_from(press)
                dragged = any(near(a, centre(dst), lh) for a, _ in followed)
                check(dragged == grabs, "at width %d a press %g px beside the connected pipe %s (%r)"
                      % (width, offset, "drags it" if grabs else "grabs nothing", followed))
                yield ("sleep", SETTLE_MS)
                if not dst.getInput(0):
                    dst.connectInput(0, src)
                    yield ("sleep", SETTLE_MS)
        set_pref("pipeWidth", DEFAULTS["pipeWidth"])
        yield ("sleep", SETTLE_MS)

        gt.section("grab-width-1")
        set_pref("pipeWidth", 1)
        yield ("sleep", SETTLE_MS)
        inp = dangling_input(lone)
        require(inp is not None, "the dangling input is shown at width 1")
        followed = drag_from(beside(inp, 15))
        check(any(near(a, centre(lone)) for a, _ in followed),
              "at width 1 a press 15 px beside the dangling input still drags it (%r)" % followed)
        yield ("sleep", SETTLE_MS)
        inp = dangling_input(lone)
        require(inp is not None, "the dangling input is back at width 1")
        press = beside(inp, 25)
        nearest = nearest_pipe_px(press)
        require(nearest > DANGLING_RADIUS_MIN, "no pipe lies within %g px of the press (nearest %.1f px)"
                % (DANGLING_RADIUS_MIN, nearest))
        pos_before = lone.getPosition()
        followed = drag_from(press)
        check(not followed, "at width 1 a press 25 px beside the dangling input grabs nothing (%r)" % followed)
        check(lone.getPosition() == pos_before, "the press moved no node")
        yield ("sleep", SETTLE_MS)

        gt.section("grab-width-6")
        set_pref("pipeWidth", 6)
        yield ("sleep", SETTLE_MS)
        inp = dangling_input(lone)
        require(inp is not None, "the dangling input is shown at width 6")
        followed = drag_from(beside(inp, 25))
        check(any(near(a, centre(lone)) for a, _ in followed),
              "at width 6 a press 25 px beside the dangling input drags it (%r)" % followed)
        yield ("sleep", SETTLE_MS)

        gt.section("arrow-head-scales")
        extents = {}
        for width in (2, 6):
            set_pref("pipeWidth", width)
            yield ("sleep", SETTLE_MS)
            extents[width] = dangling_extent(lone)
            require(extents[width] is not None, "the dangling input is shown at width %d" % width)
        check(extents[6] - extents[2] >= 2.0,
              "the dangling input's bounds (line pad and arrow head) grow from width 2 to 6 (%g -> %g)"
              % (extents[2], extents[6]))

        gt.section("grab-zoom-half")
        set_pref("pipeWidth", DEFAULTS["pipeWidth"])
        graph = root_graph()
        graph.scale(0.5, 0.5)
        lc = centre(lone)
        graph.centerOn(lc[0] + 150, lc[1])
        yield ("sleep", SETTLE_MS)
        check(abs(root_graph().transform().m11() - 0.5) < 1e-9, "the graph is at zoom 0.5")
        inp = dangling_input(lone)
        require(inp is not None, "the dangling input is shown at zoom 0.5")
        followed = drag_from(beside(inp, 15))
        check(any(near(a, centre(lone)) for a, _ in followed),
              "at zoom 0.5 a press 15 viewport px beside the dangling input drags it (%r)" % followed)
        yield ("sleep", SETTLE_MS)
        root_graph().resetTransform()

        gt.section("output-nub")
        set_pref("pipeWidth", DEFAULTS["pipeWidth"])
        set_pref("outputStyle", OUTPUT_STYLE_NUB)
        feeder = create("Blur", 600, 0)
        taker = create("Blur", 600, 250)
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        fc, tc = centre(feeder), centre(taker)
        root_graph().centerOn((fc[0] + lc[0]) / 2.0, (fc[1] + tc[1]) / 2.0)
        yield ("sleep", SETTLE_MS)
        fx, fy = feeder.getPosition()
        fw, fh = feeder.getSize()
        nub = output_stub(feeder)
        require(nub is not None, "the feeder shows an output nub")
        check(abs((nub[0][1] - (fy + fh)) - NUB_RADIUS) <= POSITION_TOLERANCE,
              "the nub reaches %g px below the node (free end at y %g, node bottom %g)"
              % (NUB_RADIUS, nub[0][1], fy + fh))
        shoot("pipe-grab-nub.png")
        set_pref("outputStyle", OUTPUT_STYLE_ARROW)
        yield ("sleep", SETTLE_MS)
        shoot("pipe-grab-arrow.png")
        set_pref("outputStyle", OUTPUT_STYLE_NUB)
        yield ("sleep", SETTLE_MS)
        press = to_view((fc[0], fy + fh + NUB_RADIUS / 2.0))
        drag_between(press, to_view(tc))
        yield ("sleep", SETTLE_MS)
        fed = taker.getInput(0)
        check(fed is not None and fed.getScriptName() == feeder.getScriptName(),
              "a drag from the nub onto another Blur connects it (input %r)" % (fed.getScriptName() if fed else None))

        gt.section("connected-nub")
        check(nub_hovered(feeder) is not None, "the feeder still shows its nub now that it feeds the taker")
        second = create("Blur", 800, 250)
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        sc = centre(second)
        root_graph().centerOn((fc[0] + sc[0]) / 2.0, (fc[1] + sc[1]) / 2.0)
        yield ("sleep", SETTLE_MS)
        nub_press = to_view((fc[0], fy + fh + NUB_RADIUS / 2.0))
        send_mouse(QEvent.MouseMove, nub_press, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(nub_hovered(feeder) is True,
              "the connected feeder's nub is highlighted with the pointer on it, over the pipe it feeds")
        shoot("pipe-grab-nub-hover.png")
        drag_between(nub_press, to_view(sc))
        yield ("sleep", SETTLE_MS)
        fed = second.getInput(0)
        check(fed is not None and fed.getScriptName() == feeder.getScriptName(),
              "a drag from the connected feeder's nub onto a third Blur connects it (input %r)"
              % (fed.getScriptName() if fed else None))
        fed = taker.getInput(0)
        check(fed is not None and fed.getScriptName() == feeder.getScriptName(),
              "the feeder still feeds the first taker after the second connection (input %r)"
              % (fed.getScriptName() if fed else None))
        check(nub_hovered(feeder) is not None, "the feeder shows its nub while feeding two Blurs")
        check(nub_hovered(taker) is not None and nub_hovered(second) is not None,
              "the two takers, which feed nothing, show their nubs")

        gt.section("pipes-leave-the-nub")
        fb = bottom_centre(feeder)
        for target in (taker, second):
            check(pipe_item(fb, target, POSITION_TOLERANCE) is not None,
                  "in the Nub style the pipe into %s leaves from the feeder's nub centre %r" % (target.getScriptName(), fb))
        set_pref("outputStyle", OUTPUT_STYLE_ARROW)
        yield ("sleep", SETTLE_MS)
        check(pipe_item(fc, second, POSITION_TOLERANCE) is not None,
              "in the Arrow style the pipe into %s leaves from the feeder's centre %r" % (second.getScriptName(), fc))
        set_pref("outputStyle", OUTPUT_STYLE_NUB)
        yield ("sleep", SETTLE_MS)

        gt.section("defaults-shots")
        for name, value in sorted(DEFAULTS.items()):
            check(settings.getParam(name).getValue() == value, "%s is at its default %g" % (name, value))
        check(settings.getParam("outputStyle").getValue() == OUTPUT_STYLE_NUB, "outputStyle is at its default Nub")
        tw = pipe_bounds_width(feeder, taker, fh)
        check(tw is not None and abs(tw - (ARROW_HEAD_WIDTH_DEFAULT + 2.0)) <= 1.0,
              "the straight-down feeder-to-taker pipe's bounds are its %g px arrow head plus a pixel each side (got %r)"
              % (ARROW_HEAD_WIDTH_DEFAULT, tw))
        away = to_view((fc[0] + 100.0, fy - 150.0))
        require(nearest_pipe_px(away) > DANGLING_RADIUS_DEFAULT, "no pipe lies near the parked pointer")
        send_mouse(QEvent.MouseMove, away, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        shoot("pipe-grab-defaults.png")
        root_graph().scale(2.0, 2.0)
        root_graph().centerOn(fc[0], (fc[1] + tc[1]) / 2.0)
        yield ("sleep", SETTLE_MS)
        shoot("pipe-grab-defaults-zoom2.png")
        root_graph().resetTransform()
        yield ("sleep", SETTLE_MS)

        gt.section("arrowhead-prefs")
        root_graph().centerOn(fc[0], (fc[1] + tc[1]) / 2.0)
        yield ("sleep", SETTLE_MS)
        pw = DEFAULTS["pipeWidth"]
        for head_width, width in ((DEFAULTS["arrowheadWidth"], pw), (8, pw), (8, 2)):
            set_pref("arrowheadWidth", head_width)
            set_pref("pipeWidth", width)
            yield ("sleep", SETTLE_MS)
            tw = pipe_bounds_width(feeder, taker, fh)
            check(tw is not None and abs(tw - (head_width * width + 2.0)) <= 1.0,
                  "Arrowhead Width %g at Pipe Width %d: the straight-down pipe's bounds are a %g px head plus a "
                  "pixel each side (got %r)" % (head_width, width, head_width * width, tw))
        set_pref("arrowheadWidth", DEFAULTS["arrowheadWidth"])
        set_pref("pipeWidth", pw)
        yield ("sleep", SETTLE_MS)
        # A quarter head width off the axis is outside the pipe's own stroke, and inside the head
        # from half way back from the tip to its base.
        across = DEFAULTS["arrowheadWidth"] * pw / 4.0
        default_length = DEFAULTS["arrowheadLength"] * pw
        item = pipe_item(bottom_centre(feeder), taker, POSITION_TOLERANCE)
        require(item is not None, "the feeder-to-taker pipe is shown")
        check(head_contains(item, 0.75 * default_length, across),
              "at the default Arrowhead Length the head holds a point %g px back from its tip" % (0.75 * default_length))
        check(not head_contains(item, 1.2 * default_length, across),
              "at the default Arrowhead Length the head ends %g px back from its tip" % default_length)
        set_pref("arrowheadLength", 4)
        yield ("sleep", SETTLE_MS)
        item = pipe_item(bottom_centre(feeder), taker, POSITION_TOLERANCE)
        require(item is not None, "the feeder-to-taker pipe is shown at Arrowhead Length 4")
        check(not head_contains(item, 0.75 * default_length, across),
              "at Arrowhead Length 4 the head no longer reaches %g px back from its tip" % (0.75 * default_length))
        check(head_contains(item, 0.75 * 4 * pw, across),
              "at Arrowhead Length 4 the head holds a point %g px back from its tip" % (0.75 * 4 * pw))
        set_pref("arrowheadLength", DEFAULTS["arrowheadLength"])
        # A head wider than twice the grab radius widens the radius to half the head.
        set_pref("arrowheadWidth", 12)
        yield ("sleep", SETTLE_MS)
        pipe = connected_pipe(feeder, taker, fh)
        require(pipe is not None, "the feeder-to-taker pipe is shown at Arrowhead Width 12")
        offset = GRAB_RADIUS_DEFAULT + 3
        press = beside_at(pipe, 0.35, offset)
        report("NOTE wide-head press %g px beside, nearest pipe %.1f px" % (offset, nearest_pipe_px(press)))
        followed = drag_from(press)
        check(any(near(a, centre(taker), fh) for a, _ in followed),
              "with a %g px head a press %g px beside the connected pipe drags it (%r)" % (12 * pw, offset, followed))
        yield ("sleep", SETTLE_MS)
        if not taker.getInput(0):
            taker.connectInput(0, feeder)
        set_pref("arrowheadWidth", DEFAULTS["arrowheadWidth"])
        yield ("sleep", SETTLE_MS)

        gt.section("hover-highlight")
        lc = centre(lone)
        root_graph().centerOn(lc[0] + 150, lc[1])
        yield ("sleep", SETTLE_MS)
        inp = dangling_input(lone)
        require(inp is not None, "the dangling input is shown")
        check(dangling_input_hovered(lone) is False, "the dangling input starts without the hover highlight")
        send_mouse(QEvent.MouseMove, beside(inp, 15), Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(dangling_input_hovered(lone) is True, "the dangling input is highlighted with the pointer 15 px beside it")
        shoot("pipe-grab-hover.png")
        away = beside(inp, 120)
        require(nearest_pipe_px(away) > DANGLING_RADIUS_DEFAULT, "no pipe lies near the point away from the input")
        send_mouse(QEvent.MouseMove, away, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(dangling_input_hovered(lone) is False, "the highlight clears with the pointer away from every pipe")

        gt.section("dot-ring")
        set_pref("outputStyle", OUTPUT_STYLE_NUB)
        dot = create("Dot", 1400, 0)
        dc = dot_centre(dot)
        # The Dot is fed from straight above and feeds a Blur straight to its right, so the pipes
        # leave its left side free.
        upper = create("Blur", 0, 0)
        uw, uh = upper.getSize()
        upper.setPosition(dc[0] - uw / 2.0, dc[1] - 150.0 - uh)
        right = create("Blur", 0, 0)
        rw, rh = right.getSize()
        right.setPosition(dc[0] + 200.0, dc[1] - rh / 2.0)
        lower = create("Blur", dc[0] - 250.0, dc[1] + 150.0)
        dot.connectInput(0, upper)
        right.connectInput(0, dot)
        app.clearSelection()
        require(lower.getInput(0) is None, "the Blur under the Dot starts unconnected")
        yield ("sleep", SETTLE_MS)
        root_graph().centerOn(dc[0], dc[1])
        yield ("sleep", SETTLE_MS)
        dc = dot_centre(dot)
        ring = dot_ring(dot)
        require(ring is not None, "the Dot's output is laid out as a ring around its disk")
        disk_bottom = (dc[0], dc[1] + DOT_DIAMETER / 2.0)
        nubs = [(p1, p2) for p1, p2, _ in pipes() if near(p2, disk_bottom) and near(p1, (disk_bottom[0], disk_bottom[1] + NUB_RADIUS))]
        check(not nubs, "no nub is drawn under the Dot in the Nub style (%r)" % nubs)
        check(pipe_item(dc, right, POSITION_TOLERANCE) is not None,
              "the Dot's pipe into the Blur leaves from the disk's centre %r" % (dc,))
        away = to_view((dc[0] - 100.0, dc[1] + 100.0))
        require(nearest_pipe_px(away) > DANGLING_RADIUS_DEFAULT, "no pipe lies near the parked pointer")
        send_mouse(QEvent.MouseMove, away, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(ring_hovered(dot) is False, "the ring is hidden with the pointer away from the Dot")
        shoot("pipe-grab-dot.png")
        send_mouse(QEvent.MouseMove, to_view(dc), Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(ring_hovered(dot) is True, "the ring shows with the pointer on the Dot's disk")
        shoot("pipe-grab-dot-ring.png")
        ring_radius = DOT_DIAMETER / 2.0 + DOT_RING_GAP + DEFAULTS["pipeWidth"] / 2.0
        on_ring = to_view((dc[0] - ring_radius - 2.0, dc[1]))
        report("NOTE ring press at %r, nearest pipe %.1f px" % (on_ring, nearest_pipe_px(on_ring)))
        require(nearest_pipe_px(on_ring) > GRAB_RADIUS_DEFAULT, "no pipe is within grab reach of the ring press")
        send_mouse(QEvent.MouseMove, on_ring, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(ring_hovered(dot) is True, "the ring shows with the pointer on it")
        send_mouse(QEvent.MouseMove, away, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(ring_hovered(dot) is False, "the ring hides again with the pointer away")
        drag_between(on_ring, to_view(centre(lower)))
        yield ("sleep", SETTLE_MS)
        fed = lower.getInput(0)
        check(fed is not None and fed.getScriptName() == dot.getScriptName(),
              "a drag from the ring onto a Blur connects the Dot to it (input %r)" % (fed.getScriptName() if fed else None))
        fed = right.getInput(0)
        check(fed is not None and fed.getScriptName() == dot.getScriptName(),
              "the Dot still feeds its first Blur (input %r)" % (fed.getScriptName() if fed else None))
        fed = dot.getInput(0)
        check(fed is not None and fed.getScriptName() == upper.getScriptName(),
              "the ring drag left the Dot's own input alone (input %r)" % (fed.getScriptName() if fed else None))
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        before = dot.getPosition()
        disk = to_view(dot_centre(dot))
        drag_between(disk, (disk[0] - 60, disk[1] + 40))
        yield ("sleep", SETTLE_MS)
        after = dot.getPosition()
        check(abs(after[0] - before[0]) > 20.0 or abs(after[1] - before[1]) > 20.0,
              "a drag from the disk moves the Dot (%r -> %r)" % (before, after))
        for taker_node in (right, lower):
            fed = taker_node.getInput(0)
            check(fed is not None and fed.getScriptName() == dot.getScriptName(),
                  "moving the Dot keeps it feeding %s (input %r)"
                  % (taker_node.getScriptName(), fed.getScriptName() if fed else None))

        for name, value in sorted(DEFAULTS.items()):
            set_pref(name, value)
        set_pref("outputStyle", OUTPUT_STYLE_NUB)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
