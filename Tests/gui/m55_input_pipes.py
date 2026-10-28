"""A GUI test that fails when a connected input has no drawn pipe, or an unconnected one hides
when its node is deselected, from synthetic cases. It is driven by Tests/gui/run-gui-test.sh, not
ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for why the GL symlink dance is needed):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m55-pipes/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m55_input_pipes.py build/fast/App/Natron'

Two families of bug are probed. One hides an input that should show: an unconnected input the
plugin marks optional (mask or not), or any input of a Reader, while the node is neither hovered
nor selected. The other leaves a *connected* input with no drawn pipe, or one that only reappears
after the node is touched (hovered, moved, selected): edge refreshes that bail out on an input
count mismatch, a source nulled because the input's NodeGui is not built yet, a project load that
only refreshes the root graph's visible nodes, and a shown node whose edge visibility is never
recomputed.

The probe samples pixels rather than the scene graph. Edge does not override type(), so it would
show up as a plain QGraphicsLineItem at zValue() == 15, but in this environment NodeGraph.scene()
raises "Internal C++ object (QGraphicsScene) already deleted" on every call, for every NodeGraph
-- a PySide/shiboken wrapper-identity problem that reproduces on a bare two-node graph. So a
connected pipe is tested by zooming/centring the view on the two nodes, grabbing the viewport,
and sampling points along the straight line between the two node boxes' centres (the portion
inside either box is skipped); enough of that line is non-background colour (differs from a
corner pixel by more than a small tolerance) only when a pipe is actually drawn. A node's
*unconnected* stub (case (e)'s Mask input, which has no second node to anchor a two-point line
search on) is instead tested by counting non-background pixels in a thin strip just past one side
of the node's box. Diffing two captures (e.g. hovered vs. unhovered) does not work: under the
software-rendered Xvfb, two captures of an *unchanged* scene already differ by dozens of sampled
pixels, swamping the few pixels an unconnected input's short stub occupies; an absolute content
count in one capture does not have that problem.

Hovering is not a lever for case (e) either: an edge's hidden/shown state is only recomputed at
specific trigger points (selecting/deselecting the node, (dis)connecting an input, undo/redo,
initializeInputs...), never from a hover event. Selecting the node always reveals the stub, so
case (e) checks the deselected state directly.

A group's subgraph NodeGraph is built with CreateNodeArgsPropSettingsOpened nodes exactly like the
root graph, but Gui::createGroupGui() leaves it unparented from any tab whenever there is no
already-focused graph pane to append it to (true throughout this script, since nothing here
drives real OS focus) -- so before probing one, this script makes it momentarily visible by
calling resize()/move()/show()/raise_() on the NodeGraph widget directly rather than by trying to
reproduce the real Shift+double-click UI gesture, which needs Gui::getLastSelectedGraph() to
already be non-null and is not reliable to drive synthetically.

Each pipe check runs three times -- now, after moving the mouse away from every node (clears any
hover state left over from creating/connecting nodes), and after nudging the destination node by a
few pixels and back -- so that a pipe which only "heals on touch" is caught rather than masked by
incidental hovering or a later move. A shot is kept for every case (not only failing ones) so the
evidence is uniform.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 180
SETTLE_MS = 400
MARGIN = 70.0
STUB_MARGIN = 150.0
MAX_ZOOM = 2.0
DIFF_TOLERANCE = 24
HIT_RATIO_FOR_PRESENT = 0.5

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _made = []

    # ---- node / graph plumbing -------------------------------------------------------------

    def create(plugin_id, x, y, group=None):
        """Every node is created the way the toolbar and Tab menu do (settings panel opened), so
        this script exercises the same creation path the rest of the GUI relies on."""
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        _made.append(node)
        return node

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def input_index(node, label):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == label:
                return i
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def group_graph(group_node):
        """The NodeGraph for group_node's subgraph, found by the objectName createGroupGui()
        sets to the group's label at creation time (Gui/Gui05.cpp). Unlike root_graph() this
        deliberately does not filter by isVisible(): the subgraph starts out unparented from any
        tab (see the module docstring), and show_group_graph() below is what makes it paintable."""
        label = group_node.getLabel()
        candidates = [g for g in gt.widgets_of("NodeGraph") if g.objectName() == label]
        return candidates[-1] if candidates else None

    def show_group_graph(group_node, x=30, y=30, w=760, h=620):
        """Temporarily shows a group's subgraph as a plain overlay widget so its viewport can be
        screenshotted, without relying on the real (focus-dependent) Shift+double-click gesture.
        Pair with hide_group_graph() once done, so later root-graph shots are not covered by it."""
        sub = group_graph(group_node)
        if sub is None:
            return None
        sub.resize(w, h)
        sub.move(x, y)
        sub.show()
        sub.raise_()
        QApplication.processEvents()
        return sub

    def hide_group_graph(sub):
        if sub is None:
            return
        try:
            sub.hide()
        except RuntimeError:
            pass
        QApplication.processEvents()

    def box(effect):
        x, y = effect.getPosition()
        w, h = effect.getSize()
        return x, y, w, h

    def point_in_box(px, py, bx, by, bw, bh, pad=3.0):
        return (bx - pad) <= px <= (bx + bw + pad) and (by - pad) <= py <= (by + bh + pad)

    def frame(graph, nodes, margin=MARGIN, max_zoom=MAX_ZOOM):
        if graph is None or not nodes:
            return
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y = n.getPosition()
            w, h = n.getSize()
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        bw = max(xe) - min(xs) + 2 * margin
        bh = max(ye) - min(ys) + 2 * margin
        vp = graph.viewport()
        want = min(max_zoom, vp.width() / bw, vp.height() / bh)
        graph.resetTransform()
        graph.scale(want, want)
        graph.centerOn((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0)
        QApplication.processEvents()

    def viewport_image(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        return pix, pix.toImage()

    def graph_shot(graph, name):
        if graph is None or not graph.isVisible():
            report("NOTE %s: no visible graph to crop, skipping the shot" % name)
            return
        pix, _img = viewport_image(graph)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def hover_away(graph):
        """Moves the (synthetic) mouse to a blank corner of the viewport, clearing any hover
        state a prior create()/connect() call might have left on a node."""
        if graph is None:
            return
        vp = graph.viewport()
        ev = QMouseEvent(QEvent.MouseMove, QPointF(2.0, 2.0), Qt.NoButton, Qt.NoButton, Qt.NoModifier)
        QApplication.sendEvent(vp, ev)
        QApplication.processEvents()

    def nudge(node):
        x, y = node.getPosition()
        node.setPosition(x + 3, y + 3)
        QApplication.processEvents()
        node.setPosition(x, y)
        QApplication.processEvents()

    def send_key(widget, key, modifiers=Qt.NoModifier):
        if not check(widget is not None, "a widget exists to send the key event to"):
            return
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def edit_menu_actions():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow):
                continue
            mb = win.menuBar()
            if mb is None:
                continue
            for act in mb.actions():
                menu = act.menu()
                if menu is not None and act.text().lower().startswith("edit"):
                    return menu.actions()
        return []

    def trigger_undo():
        """Undo/Redo are plain QActions on the Edit menu (QUndoStack::createUndoAction), not
        slots, and their Ctrl+Z shortcut is matched by Qt's shortcut map before an event reaches
        a widget's own event handler -- a step QApplication.sendEvent() to a specific widget
        skips, unlike the Backspace/Ctrl+C/Ctrl+V keys above, which NodeGraph's own event
        handlers read directly off the QKeyEvent (confirmed empirically against this binary:
        sendEvent()-ing Ctrl+Z never undid anything, while triggering the action directly did).
        So undo is driven by finding and triggering that action instead."""
        for a in edit_menu_actions():
            if a.text().lower().startswith("undo") and a.isEnabled():
                a.trigger()
                QApplication.processEvents()
                return True
        check(False, "found an enabled Undo action in the Edit menu")
        return False

    def add_project_view(name):
        views_param = app.getProjectParam("viewsList")
        if not check(views_param is not None, "found the project's viewsList param"):
            return
        table = views_param.getTable()
        table.append([name])
        views_param.setTable(table)
        QApplication.processEvents()

    # ---- pixel probes -----------------------------------------------------------------------

    def pixel_differs(img, px, py, bg, radius=1):
        """Whether any pixel within radius of (px, py) differs from bg. Edge::initLine anchors a
        connected line at the destination box's boundary but at the *source* box's centre (see
        the module docstring), so the straight line between the two boxes' centres is only an
        approximation of the real drawn segment; sampling a small neighbourhood rather than the
        exact point absorbs that drift (and ordinary anti-aliasing) without weakening the probe
        against a genuinely missing pipe -- a disconnected control still reads 0 hits at radius 1
        (checked empirically against this binary)."""
        for ddx in range(-radius, radius + 1):
            for ddy in range(-radius, radius + 1):
                x, y = px + ddx, py + ddy
                if 0 <= x < img.width() and 0 <= y < img.height():
                    c = img.pixelColor(x, y)
                    diff = abs(c.red() - bg.red()) + abs(c.green() - bg.green()) + abs(c.blue() - bg.blue())
                    if diff > DIFF_TOLERANCE:
                        return True
        return False

    def pipe_state(graph, source, dest):
        """"present" when enough points sampled along the straight line between source's and
        dest's box centres (skipping the segments inside either box) are not background-coloured;
        "missing" otherwise. See the module docstring for why this samples pixels at all."""
        if graph is None:
            return "no graph"
        frame(graph, [source, dest])
        _pix, img = viewport_image(graph)
        bg = img.pixelColor(2, 2)
        sx, sy, sw, sh = box(source)
        dx, dy, dw, dh = box(dest)
        scx, scy = sx + sw / 2.0, sy + sh / 2.0
        dcx, dcy = dx + dw / 2.0, dy + dh / 2.0
        hits = 0
        total = 0
        steps_n = 60
        for i in range(1, steps_n):
            t = i / float(steps_n)
            wx = scx + (dcx - scx) * t
            wy = scy + (dcy - scy) * t
            if point_in_box(wx, wy, sx, sy, sw, sh) or point_in_box(wx, wy, dx, dy, dw, dh):
                continue
            total += 1
            p = graph.mapFromScene(QPointF(wx, wy))
            if pixel_differs(img, p.x(), p.y(), bg):
                hits += 1
        if total == 0:
            return "no segment"
        return "present" if (hits / float(total)) >= HIT_RATIO_FOR_PRESENT else "missing"

    def check_pipe_persists(graph, source, dest, label, want="present"):
        """A pipe must match want now, after hovering away, and after nudging the destination --
        so one that only heals on touch is recorded as a failure, not a pass."""
        ok = True
        state = pipe_state(graph, source, dest)
        ok = check(state == want, "%s: pipe is %s (got %s)" % (label, want, state)) and ok
        hover_away(graph)
        state = pipe_state(graph, source, dest)
        ok = check(state == want, "%s: pipe is %s after hovering away (got %s)" % (label, want, state)) and ok
        nudge(dest)
        state = pipe_state(graph, source, dest)
        ok = check(state == want,
                   "%s: pipe is %s after nudging the destination (got %s)" % (label, want, state)) and ok
        return ok

    def strip_content(graph, node, side, near, far, tol=DIFF_TOLERANCE):
        """Absolute (single-capture, not diff) count of non-background pixels in a thin strip
        just outside one side of node's box -- used for an unconnected input's stub, which (with
        nothing connected to it) has no second node to anchor a two-point pipe search on. Diffing
        two captures (e.g. hovered vs. unhovered, or selected vs. deselected) was tried first and
        discarded: under this environment's software-rendered Xvfb, repeated captures of an
        unchanged scene already differ by dozens of sampled pixels (confirmed empirically with an
        all-inputs-connected control graph), swamping the handful of pixels an unconnected
        input's short stub actually occupies. An absolute content count in a single capture does
        not have that problem: an empty strip reads ~0 regardless of capture-to-capture noise."""
        frame(graph, [node], margin=STUB_MARGIN)
        _pix, img = viewport_image(graph)
        bg = img.pixelColor(2, 2)
        bx, by, bw, bh = box(node)
        if side == "right":
            sx0, sy0, sx1, sy1 = bx + bw + near, by - 10, bx + bw + far, by + bh + 10
        elif side == "left":
            sx0, sy0, sx1, sy1 = bx - far, by - 10, bx - near, by + bh + 10
        elif side == "top":
            sx0, sy0, sx1, sy1 = bx - 10, by - far, bx + bw + 10, by - near
        else:
            sx0, sy0, sx1, sy1 = bx - 10, by + bh + near, bx + bw + 10, by + bh + far
        tl = graph.mapFromScene(QPointF(sx0, sy0))
        br = graph.mapFromScene(QPointF(sx1, sy1))
        x0, x1 = min(tl.x(), br.x()), max(tl.x(), br.x())
        y0, y1 = min(tl.y(), br.y()), max(tl.y(), br.y())
        hits = 0
        total = 0
        for py in range(max(0, y0), min(img.height(), y1)):
            for px in range(max(0, x0), min(img.width(), x1)):
                total += 1
                c = img.pixelColor(px, py)
                diff = abs(c.red() - bg.red()) + abs(c.green() - bg.green()) + abs(c.blue() - bg.blue())
                if diff > tol:
                    hits += 1
        return hits, total

    def check_stub_persists(graph, node, side, label, min_hits=20):
        """The stub on the given side of node must be drawn now, after hovering away, and after
        nudging the node -- so one that only heals on touch is recorded as a failure, not a
        pass."""
        ok = True

        def once(phase):
            hits, total = strip_content(graph, node, side, 10, 80)
            return check(hits >= min_hits,
                         "%s: the stub is drawn %s (%d/%d non-background px in its strip)"
                         % (label, phase, hits, total))

        ok = once("now") and ok
        hover_away(graph)
        ok = once("after hovering away") and ok
        nudge(node)
        ok = once("after nudging the node") and ok
        return ok

    # ---- steps ------------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- (a1) Group gains a second Input; the first pipe (outside the group) must survive.
        # A plain Group comes with one Input/Output pair already built in, so group_a1 already
        # exposes 1 input before anything else is created; connecting to it and then adding one
        # more Input node gives the "one Input, then a second Input" shape under test.
        gt.section("a1-group-second-input")
        group_a1 = create("Group", 0, 0)
        const_a1 = create("Constant", -300, 0)
        check(group_a1.getMaxInputCount() == 1, "a1: the Group starts with its built-in 1 input")
        connect(group_a1, 0, const_a1)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        check_pipe_persists(root_graph(), const_a1, group_a1, "a1: Constant -> Group before the 2nd Input")
        create("Input", 0, 250, group=group_a1)
        yield ("sleep", SETTLE_MS)
        check(group_a1.getMaxInputCount() >= 2, "a1: the Group now exposes >= 2 inputs")
        check_pipe_persists(root_graph(), const_a1, group_a1, "a1: Constant -> Group after the 2nd Input")
        graph_shot(root_graph(), "a1-group-second-input")

        # ---- (a2) JoinViews gains a project view; its two existing input pipes must survive.
        gt.section("a2-joinviews-new-view")
        join = create("JoinViews", 900, 0)
        add_project_view("M55View2")
        yield ("sleep", SETTLE_MS)
        check(join.getMaxInputCount() >= 2, "a2: JoinViews exposes >= 2 inputs with 2 project views")
        src_a2_0 = create("Constant", 600, -150)
        src_a2_1 = create("Constant", 600, 150)
        connect(join, 0, src_a2_0)
        connect(join, 1, src_a2_1)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        check_pipe_persists(root_graph(), src_a2_0, join, "a2: view-input 0 before the 3rd view")
        check_pipe_persists(root_graph(), src_a2_1, join, "a2: view-input 1 before the 3rd view")
        add_project_view("M55View3")
        yield ("sleep", SETTLE_MS)
        check(join.getMaxInputCount() >= 3, "a2: JoinViews exposes >= 3 inputs with 3 project views")
        check_pipe_persists(root_graph(), src_a2_0, join, "a2: view-input 0 after the 3rd view")
        check_pipe_persists(root_graph(), src_a2_1, join, "a2: view-input 1 after the 3rd view")
        graph_shot(root_graph(), "a2-joinviews-new-view")

        # ---- (b2) Copy a connected chain and paste it into a group graph.
        gt.section("b2-copy-paste-into-group")
        group_b2 = create("Group", 1500, 0)
        const_b2 = create("Constant", 1500, 400)
        blur_b2 = create("Blur", 1500, 600)
        connect(blur_b2, 0, const_b2)
        app.setSelection([const_b2, blur_b2])
        QApplication.processEvents()
        before_b2 = set(n.getScriptName() for n in group_b2.getChildren())
        app.copySelectedNodes()
        app.pasteNodes(group_b2)
        yield ("sleep", SETTLE_MS)
        pasted_b2 = [n for n in group_b2.getChildren() if n.getScriptName() not in before_b2]
        if check(len(pasted_b2) == 2, "b2: pasting the chain created 2 nodes inside the group"):
            pasted_const = next((n for n in pasted_b2 if n.getPluginID() == "Constant"), None)
            pasted_blur = next((n for n in pasted_b2 if n.getPluginID() == "Blur"), None)
            if check(pasted_const is not None and pasted_blur is not None,
                     "b2: found the pasted Constant and Blur"):
                sub_b2 = show_group_graph(group_b2)
                yield ("sleep", SETTLE_MS)
                if check(sub_b2 is not None, "b2: showed the group's NodeGraph"):
                    check_pipe_persists(sub_b2, pasted_const, pasted_blur,
                                         "b2: pasted Constant -> Blur inside the group")
                    graph_shot(sub_b2, "b2-copy-paste-into-group")
                    hide_group_graph(sub_b2)

        # ---- (c1) Delete a node with both an input and an output connected, then undo.
        gt.section("c1-delete-undo")
        src_c1 = create("Constant", 2100, 0)
        mid_c1 = create("Blur", 2100, 250)
        dst_c1 = create("Blur", 2100, 500)
        connect(mid_c1, 0, src_c1)
        connect(dst_c1, 0, mid_c1)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        mid_c1_name = mid_c1.getScriptName()
        root_c1 = root_graph()
        app.setSelection([mid_c1])
        QApplication.processEvents()
        send_key(root_c1, Qt.Key_Backspace)
        check(app.getNode(mid_c1_name) is None, "c1: Backspace removed the middle node")
        trigger_undo()
        yield ("sleep", SETTLE_MS)
        restored_c1 = app.getNode(mid_c1_name)
        if check(restored_c1 is not None, "c1: Ctrl+Z restored the middle node"):
            app.clearSelection()
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
            check_pipe_persists(root_graph(), src_c1, restored_c1, "c1: Constant -> middle after undo")
            check_pipe_persists(root_graph(), restored_c1, dst_c1, "c1: middle -> Blur after undo")
            graph_shot(root_graph(), "c1-delete-undo")

        # ---- (c2) Same as (c1), but the chain lives inside a group.
        gt.section("c2-delete-undo-in-group")
        group_c2 = create("Group", 2700, 0)
        src_c2 = create("Constant", 0, 0, group=group_c2)
        mid_c2 = create("Blur", 0, 250, group=group_c2)
        dst_c2 = create("Blur", 0, 500, group=group_c2)
        connect(mid_c2, 0, src_c2)
        connect(dst_c2, 0, mid_c2)
        sub_c2 = show_group_graph(group_c2)
        check(sub_c2 is not None, "c2: showed the group's NodeGraph")
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        mid_c2_name = mid_c2.getScriptName()
        app.setSelection([mid_c2])
        QApplication.processEvents()
        send_key(sub_c2, Qt.Key_Backspace)
        check(group_c2.getNode(mid_c2_name) is None, "c2: Backspace removed the middle node")
        trigger_undo()
        yield ("sleep", SETTLE_MS)
        restored_c2 = group_c2.getNode(mid_c2_name)
        if check(restored_c2 is not None, "c2: Ctrl+Z restored the middle node"):
            app.clearSelection(group_c2)
            yield ("sleep", SETTLE_MS)
            check_pipe_persists(sub_c2, src_c2, restored_c2, "c2: Constant -> middle after undo (in group)")
            check_pipe_persists(sub_c2, restored_c2, dst_c2, "c2: middle -> Blur after undo (in group)")
            graph_shot(sub_c2, "c2-delete-undo-in-group")
        hide_group_graph(sub_c2)

        # ---- (d) Overlapping nodes, then separated again: the pipe must come back with its arrow.
        gt.section("d-overlap-separate")
        src_d = create("Constant", 3300, 0)
        dst_d = create("Blur", 3300, 300)
        connect(dst_d, 0, src_d)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        check_pipe_persists(root_graph(), src_d, dst_d, "d: pipe before overlapping the nodes")
        dst_x, dst_y = dst_d.getPosition()
        src_d.setPosition(dst_x, dst_y)
        yield ("sleep", SETTLE_MS)
        graph_shot(root_graph(), "d-overlap")
        src_d.setPosition(3300, 0)
        yield ("sleep", SETTLE_MS)
        check_pipe_persists(root_graph(), src_d, dst_d, "d: pipe after separating the overlapping nodes")
        graph_shot(root_graph(), "d-separated")

        # ---- (e) Unconnected optional/mask input, and a Reader: must stay visible when
        # deselected and unhovered.
        gt.section("e-unconnected-optional-and-reader")
        merge_e = create("Merge", 3900, 0)
        a_idx = input_index(merge_e, "A")
        b_idx = input_index(merge_e, "B")
        mask_idx = input_index(merge_e, "Mask")
        check(mask_idx is not None, "e: found Merge's Mask input")
        src_a_e = create("Constant", 3600, -150)
        src_b_e = create("Constant", 3600, 150)
        if a_idx is not None:
            connect(merge_e, a_idx, src_a_e)
        if b_idx is not None:
            connect(merge_e, b_idx, src_b_e)
        # Mask (mask_idx) is deliberately left unconnected: it is the stub this case targets. With
        # A connected from upper-left and B from lower-left, Natron lays the one remaining (Mask)
        # input's stub out to the right of the node.
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        check_stub_persists(root_graph(), merge_e, "right", "e: Merge's unconnected Mask input")
        graph_shot(root_graph(), "e-unconnected-optional")

        fixture = os.path.join(gt.FIXTURES, "flat-rgba-only.exr")
        if os.path.isfile(fixture):
            reader_e = app.createReader(fixture)
            if check(reader_e is not None, "e: created a Reader for the control"):
                reader_e.setPosition(4300, 0)
                _made.append(reader_e)
                n_inputs = reader_e.getMaxInputCount()
                report("NOTE e: the native Read has %d input(s); the old auto-hide "
                       "preference's Reader clause has no input edge to hide on this build, so "
                       "no pipe/stub check applies to it here" % n_inputs)
        else:
            report("NOTE e: fixture %s not found, skipping the Reader control" % fixture)

        # ---- controls: "Hide inputs" must keep hiding a *connected* input while deselected.
        # setInputVisible()-style Python hiding is not exposed on Effect (Engine/PyNode.h has no
        # such method), so it is not exercised here.
        gt.section("controls-hide-inputs-knob")
        src_ctrl = create("Constant", 4700, 0)
        dst_ctrl = create("Blur", 4700, 300)
        connect(dst_ctrl, 0, src_ctrl)
        hide_param = dst_ctrl.getParam("hideInputs")
        if check(hide_param is not None, "controls: found the per-node 'Hide inputs' knob"):
            hide_param.set(True)
            app.clearSelection()
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
            check_pipe_persists(root_graph(), src_ctrl, dst_ctrl,
                                 "controls: 'Hide inputs' on, deselected", want="missing")
            app.selectNode(dst_ctrl, True)
            yield ("sleep", SETTLE_MS)
            state_selected = pipe_state(root_graph(), src_ctrl, dst_ctrl)
            report("NOTE controls: with 'Hide inputs' on, selecting the node makes the pipe %s "
                   "(kept, pre-existing behaviour)" % state_selected)
            app.clearSelection()
            hide_param.set(False)
            yield ("sleep", SETTLE_MS)
            graph_shot(root_graph(), "controls-hide-inputs-knob")
        report("NOTE controls: setInputVisible()-style hiding through the Python API was not "
               "exercised -- Engine/PyNode.h's Effect class does not expose isInputVisible/"
               "setInputVisible to Python")

        # ---- (b1) Group holding Constant->Blur->Output, saved, closed and reloaded. Run last:
        # resetProject() wipes every node made above, so nothing after this case can rely on them.
        # Connects into the Group's own built-in Output (see a1's comment above) rather than
        # creating a second one, so the reloaded lookup by exact name below is unambiguous.
        gt.section("b1-save-close-reload-group")
        group_b1 = create("Group", 0, 900)
        const_b1 = create("Constant", 0, 900, group=group_b1)
        blur_b1 = create("Blur", 0, 1100, group=group_b1)
        output_b1 = next((n for n in group_b1.getChildren()
                           if n.getPluginID() == "Output"), None)
        check(output_b1 is not None, "b1: found the Group's built-in Output")
        connect(blur_b1, 0, const_b1)
        # The built-in Output starts wired straight to the built-in Input as a pass-through;
        # drop that before connecting Blur1 to it.
        output_b1.disconnectInput(0)
        connect(output_b1, 0, blur_b1)
        group_b1_name = group_b1.getScriptName()
        const_b1_name = const_b1.getScriptName()
        blur_b1_name = blur_b1.getScriptName()
        output_b1_name = output_b1.getScriptName()
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        rt_path = os.path.join(gt.OUT, "b1.ntpz")
        if check(app.saveProjectAs(rt_path), "b1: save the project to " + rt_path):
            if check(app.resetProject(), "b1: reset the project (close, keep the window)"):
                reloaded = app.loadProject(rt_path)
                yield ("sleep", SETTLE_MS)
                if check(reloaded is not None, "b1: reload " + rt_path):
                    rt_group = reloaded.getNode(group_b1_name)
                    if check(rt_group is not None, "b1: found the reloaded Group"):
                        rt_const = reloaded.getNode(group_b1_name + "." + const_b1_name)
                        rt_blur = reloaded.getNode(group_b1_name + "." + blur_b1_name)
                        rt_output = reloaded.getNode(group_b1_name + "." + output_b1_name)
                        if check(rt_const is not None and rt_blur is not None and rt_output is not None,
                                 "b1: found the reloaded Constant, Blur and Output inside the Group"):
                            sub_b1 = show_group_graph(rt_group)
                            yield ("sleep", SETTLE_MS)
                            if check(sub_b1 is not None, "b1: showed the reloaded subgraph NodeGraph"):
                                check_pipe_persists(sub_b1, rt_const, rt_blur,
                                                     "b1: Constant -> Blur after reload")
                                check_pipe_persists(sub_b1, rt_blur, rt_output,
                                                     "b1: Blur -> Output after reload")
                                graph_shot(sub_b1, "b1-save-close-reload-group")
                                hide_group_graph(sub_b1)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
