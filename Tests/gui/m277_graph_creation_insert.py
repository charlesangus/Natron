"""Checks that the mask and stencil shortcuts create like the tab menu, and that dragging a node onto
a pipe skips Viewer pipes, enters a node whose main input is already connected through its first
free input, and undoes and redoes as one step. Also checks that nodes with a run of numbered scene
inputs (USDMerge, USDXformPrim) show the compact Merge-like stub layout and still take Y bulk
connections.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m277-gui/out Tests/gui/run-gui-test.sh Tests/gui/m277_graph_creation_insert.py build/release/App/Natron
Shots: shortcut-<op>-selected.png, shortcut-mask-cursor.png, viewer-pipe.png, insert-preconnected.png,
layout-<plugin>.png (node graph viewport).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
SETTLE_MS = 400
# The drop hint accepts a node whose box comes within 10 scene units of the pipe.
BESIDE_PIPE = 5.0
# Positions match to within a scene unit or two once the cursor's whole pixel is mapped back.
POSITION_TOLERANCE = 3.0
BACKGROUND_TOLERANCE = 40

CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
BLUR_ID = "net.sf.cimg.CImgBlur"
MERGE_ID = "Merge"
VIEWER_ID = "fr.inria.built-in.Viewer"
USD_READ_ID = "USDRead"
VARIADIC_IDS = ("USDMerge", "USDXformPrim")

try:
    import faulthandler
    import math

    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QCursor, QEnterEvent, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

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
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def root_names():
        return set(n.getScriptName() for n in app.getChildren())

    def new_nodes(before):
        return [n for n in app.getChildren() if n.getScriptName() not in before]

    def by_name(name):
        return next((n for n in app.getChildren() if n.getScriptName() == name), None)

    def input_name(node, i):
        src = node.getInput(i)
        return src.getScriptName() if src is not None else None

    def operation_of(node):
        param = node.getParam("operation")
        return param.getOption(param.getValue()) if param is not None else None

    def centre(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x + w / 2.0, y + h / 2.0)

    def select_only(nodes):
        app.clearSelection()
        for n in nodes:
            app.selectNode(n, False)
        QApplication.processEvents()

    def frame(graph, scale, scene_x, scene_y):
        graph.resetTransform()
        graph.scale(scale, scale)
        graph.centerOn(scene_x, scene_y)
        QApplication.processEvents()

    def put_cursor(graph, viewport_pos):
        vp = graph.viewport()
        QCursor.setPos(vp.mapToGlobal(viewport_pos))
        glob = QPointF(vp.mapToGlobal(viewport_pos))
        QApplication.sendEvent(graph, QEnterEvent(QPointF(viewport_pos), QPointF(viewport_pos), glob))
        QApplication.processEvents()

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def send_preset_key(graph, key):
        # The physical key's X11 scan code (47 for ;, 48 for ') is what the shortcut matches.
        text = ";" if key == Qt.Key_Semicolon else "'"
        scan = 47 if key == Qt.Key_Semicolon else 48
        graph.setFocus()
        for kind in (QEvent.KeyPress, QEvent.KeyRelease):
            QApplication.sendEvent(graph, QKeyEvent(kind, key, Qt.NoModifier, scan, 0, 0, text))
        QApplication.processEvents()

    def graph_key(graph, key, mods):
        graph.setFocus()
        QTest.keyClick(graph, key, mods)
        QApplication.processEvents()

    def undo(graph):
        graph_key(graph, Qt.Key_Z, Qt.ControlModifier)

    def redo(graph):
        graph_key(graph, Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)

    def viewport_image(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())

    def snap(graph, name):
        pix = viewport_image(graph)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)
        return pix.toImage()

    def create_by_key(graph, key, viewport_pos):
        before = root_names()
        put_cursor(graph, viewport_pos)
        send_preset_key(graph, key)
        ok = yield ("until", lambda: len(new_nodes(before)) > 0, 5.0)
        created = new_nodes(before)
        return created[0] if ok and len(created) == 1 else None

    def create_by_tab_menu(graph, entry_name, viewport_pos):
        before = root_names()
        put_cursor(graph, viewport_pos)
        graph.setFocus()
        QTest.keyClick(graph, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        if not ok:
            return None
        popup = next(d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible())
        line_edit = popup.findChildren(QLineEdit)[0]
        view = popup.findChildren(QListView)[0]
        line_edit.setText(entry_name)
        QApplication.processEvents()
        model = view.model()
        entries = [str(model.index(r, 0).data()) for r in range(model.rowCount())]
        row = next((r for r, e in enumerate(entries)
                    if e.split("[")[0].strip() == entry_name and "[Merge/Merge Presets]" in e), -1)
        if row < 0:
            report("NOTE tab menu entries for %r: %r" % (entry_name, entries))
            QTest.keyClick(popup, Qt.Key_Escape)
            return None
        view.setCurrentIndex(model.index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)
        ok = yield ("until", lambda: len(new_nodes(before)) > 0, 5.0)
        created = new_nodes(before)
        return created[0] if ok and len(created) == 1 else None

    def undo_creation(graph, name):
        undo(graph)
        ok = yield ("until", lambda: name not in root_names(), 5.0)
        return ok

    def shortcuts_section(graph):
        gt.section("shortcuts")
        src = create(CONSTANT_ID, 0, 0)
        gt.close_all_panels()
        sc = centre(src)
        frame(graph, 1.0, sc[0], sc[1] + 100)
        vp = graph.viewport()
        aside = QPoint(vp.width() // 5, vp.height() // 5)
        # The "Merge node connect to A input" preference picks the input a new Merge takes the
        # selected node on, for the shortcut as for the tab menu.
        to_a = NatronEngine.natron.getSettings().getParam("mergeConnectToA").getValue()
        fed, fed_label = (1, "A") if to_a else (0, "B")
        for key, operation, entry in ((Qt.Key_Semicolon, "mask", "Mask"), (Qt.Key_Apostrophe, "stencil", "Stencil")):
            select_only([src])
            node = yield from create_by_key(graph, key, aside)
            if not check(node is not None, "%s with a selection creates one node" % entry):
                continue
            yield ("sleep", SETTLE_MS)
            name = node.getScriptName()
            got = (node.getPluginID(), operation_of(node), input_name(node, fed))
            check(got == (MERGE_ID, operation, src.getScriptName()),
                  "%s: a %s Merge fed through %s by the selected node (got %r)" % (entry, operation, fed_label, got))
            key_pos = node.getPosition()
            check(key_pos[1] >= src.getPosition()[1] + src.getSize()[1]
                  and abs(centre(node)[0] - sc[0]) < src.getSize()[0],
                  "%s: placed beneath the selected node, not at the cursor (node %r, source %r)"
                  % (entry, key_pos, src.getPosition()))
            gt.close_all_panels()
            snap(graph, "shortcut-%s-selected" % operation)
            ok = yield from undo_creation(graph, name)
            check(ok, "%s: one undo removes the shortcut's node" % entry)

            select_only([src])
            tab = yield from create_by_tab_menu(graph, entry, aside)
            if not check(tab is not None, "%s from the tab menu creates one node" % entry):
                continue
            yield ("sleep", SETTLE_MS)
            tab_pos = tab.getPosition()
            check(abs(tab_pos[0] - key_pos[0]) <= POSITION_TOLERANCE
                  and abs(tab_pos[1] - key_pos[1]) <= POSITION_TOLERANCE
                  and input_name(tab, fed) == src.getScriptName() and operation_of(tab) == operation,
                  "%s: the shortcut matches the tab menu's position and connection (key %r, tab %r, tab %s %r)"
                  % (entry, key_pos, tab_pos, fed_label, input_name(tab, fed)))
            gt.close_all_panels()
            ok = yield from undo_creation(graph, tab.getScriptName())
            check(ok, "%s: one undo removes the tab menu's node" % entry)

        # Zoomed in, with nothing selected, the node goes where the cursor is.
        app.clearSelection()
        frame(graph, 2.0, sc[0] + 600, sc[1])
        spot = QPoint(vp.width() * 3 // 10, vp.height() * 3 // 10)
        want = graph.mapToScene(spot)
        node = yield from create_by_key(graph, Qt.Key_Semicolon, spot)
        if check(node is not None, "Mask without a selection creates one node"):
            yield ("sleep", SETTLE_MS)
            key_pos = node.getPosition()
            check(abs(key_pos[0] - want.x()) <= POSITION_TOLERANCE and abs(key_pos[1] - want.y()) <= POSITION_TOLERANCE
                  and input_name(node, 0) is None and input_name(node, 1) is None,
                  "Mask without a selection lands unconnected at the cursor at 2x zoom (node %r, cursor %r)"
                  % (key_pos, (want.x(), want.y())))
            gt.close_all_panels()
            snap(graph, "shortcut-mask-cursor")
            name = node.getScriptName()
            ok = yield from undo_creation(graph, name)
            check(ok, "Mask without a selection: one undo removes the node")
            app.clearSelection()
            tab = yield from create_by_tab_menu(graph, "Mask", spot)
            if check(tab is not None, "Mask from the tab menu without a selection creates one node"):
                tab_pos = tab.getPosition()
                check(abs(tab_pos[0] - key_pos[0]) <= POSITION_TOLERANCE and abs(tab_pos[1] - key_pos[1]) <= POSITION_TOLERANCE,
                      "Mask without a selection matches the tab menu's position (key %r, tab %r)" % (key_pos, tab_pos))
                gt.close_all_panels()
                yield from undo_creation(graph, tab.getScriptName())

    def drag_beside_pipe(graph, src, dst, mover):
        """Drags mover by its centre until its box sits just right of the src to dst pipe, and
        returns the release point, still pressed."""
        sc, dc, mc = centre(src), centre(dst), centre(mover)
        pipe_x = (sc[0] + dc[0]) / 2.0
        target = (pipe_x + mover.getSize()[0] / 2.0 + BESIDE_PIPE, (sc[1] + dc[1]) / 2.0)
        vp = graph.viewport()
        send_mouse(vp, QEvent.MouseButtonPress, graph.mapFromScene(QPointF(mc[0], mc[1])), Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 5):
            f = i / 4.0
            p = graph.mapFromScene(QPointF(mc[0] + (target[0] - mc[0]) * f, mc[1] + (target[1] - mc[1]) * f))
            send_mouse(vp, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        return graph.mapFromScene(QPointF(target[0], target[1]))

    def release(graph, pos):
        send_mouse(graph.viewport(), QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)

    def frame_nodes(graph, nodes, scale):
        xs = [n.getPosition()[0] for n in nodes] + [n.getPosition()[0] + n.getSize()[0] for n in nodes]
        ys = [n.getPosition()[1] for n in nodes] + [n.getPosition()[1] + n.getSize()[1] for n in nodes]
        frame(graph, scale, (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)

    def viewer_section(graph):
        gt.section("viewer pipe")
        x0, y0 = 2000, 0
        src = create(CONSTANT_ID, x0, y0)
        viewer = create(VIEWER_ID, x0, y0 + 300)
        mover = create(BLUR_ID, x0 + 400, y0 + 150)
        gt.close_all_panels()
        viewer.connectInput(0, src)
        app.clearSelection()
        frame_nodes(graph, (src, viewer, mover), 1.5)
        yield ("sleep", SETTLE_MS)
        end = drag_beside_pipe(graph, src, viewer, mover)
        yield ("sleep", SETTLE_MS)
        snap(graph, "viewer-pipe")
        release(graph, end)
        yield ("sleep", SETTLE_MS)
        check(input_name(viewer, 0) == src.getScriptName() and input_name(mover, 0) is None,
              "dropping beside a Viewer pipe inserts nothing (Viewer input %r, dragged node input %r)"
              % (input_name(viewer, 0), input_name(mover, 0)))

        x0 = 2600
        src = create(CONSTANT_ID, x0, y0)
        dst = create(BLUR_ID, x0, y0 + 300)
        mover = create(BLUR_ID, x0 + 400, y0 + 150)
        gt.close_all_panels()
        dst.connectInput(0, src)
        app.clearSelection()
        frame_nodes(graph, (src, dst, mover), 1.5)
        yield ("sleep", SETTLE_MS)
        end = drag_beside_pipe(graph, src, dst, mover)
        yield ("sleep", SETTLE_MS)
        release(graph, end)
        yield ("sleep", SETTLE_MS)
        check(input_name(dst, 0) == mover.getScriptName() and input_name(mover, 0) == src.getScriptName(),
              "dropping beside an ordinary pipe still inserts (Blur input %r, dragged node input %r)"
              % (input_name(dst, 0), input_name(mover, 0)))

    def preconnected_section(graph):
        gt.section("preconnected insert")
        x0, y0 = 3400, 0
        src = create(CONSTANT_ID, x0, y0)
        dst = create(BLUR_ID, x0, y0 + 300)
        feed = create(CONSTANT_ID, x0 + 600, y0)
        merge = create(MERGE_ID, x0 + 400, y0 + 150)
        gt.close_all_panels()
        dst.connectInput(0, src)
        merge.connectInput(0, feed)
        app.clearSelection()
        frame_nodes(graph, (src, dst, feed, merge), 1.5)
        yield ("sleep", SETTLE_MS)
        names = (src.getScriptName(), dst.getScriptName(), feed.getScriptName(), merge.getScriptName())
        before_pos = merge.getPosition()
        end = drag_beside_pipe(graph, src, dst, merge)
        yield ("sleep", SETTLE_MS)
        snap(graph, "insert-preconnected-hint")
        release(graph, end)
        yield ("sleep", SETTLE_MS)
        snap(graph, "insert-preconnected")

        def state():
            return (input_name(dst, 0), input_name(merge, 0), input_name(merge, 1))

        inserted = (names[3], names[2], names[0])
        original = (names[0], names[2], None)
        check(state() == inserted,
              "a Merge whose B is connected inserts through A and keeps B (Blur, B, A inputs: %r)" % (state(),))
        undo(graph)
        ok = yield ("until", lambda: state() == original, 5.0)
        moved_back = merge.getPosition()
        check(ok and abs(moved_back[0] - before_pos[0]) <= POSITION_TOLERANCE
              and abs(moved_back[1] - before_pos[1]) <= POSITION_TOLERANCE,
              "one undo restores the pipes and the dragged node's position (got %r at %r, was at %r)"
              % (state(), moved_back, before_pos))
        redo(graph)
        ok = yield ("until", lambda: state() == inserted, 5.0)
        check(ok, "redo inserts it again (got %r)" % (state(),))

    def stub_hits(img, graph, node, angle):
        """Counts non-background pixels along the line from node's box edge out to most of the
        disconnected-arrow length, in the direction angle (radians, counter-clockwise from +x)."""
        x, y = node.getPosition()
        w, h = node.getSize()
        c = graph.mapFromScene(QPointF(x + w / 2.0, y + h / 2.0))
        tl = graph.mapFromScene(QPointF(x, y))
        br = graph.mapFromScene(QPointF(x + w, y + h))
        hw, hh = (br.x() - tl.x()) / 2.0, (br.y() - tl.y()) / 2.0
        dx, dy = math.cos(angle), -math.sin(angle)
        to_edge = min(hw / abs(dx) if abs(dx) > 1e-6 else 1e9, hh / abs(dy) if abs(dy) > 1e-6 else 1e9)
        arrow = NatronEngine.natron.getSettings().getParam("disconnectedArrowLength").getValue()
        length = arrow * graph.transform().m11()
        bg = img.pixelColor(2, 2)
        hits = 0
        steps = int(length * 0.7)
        for s in range(4, max(5, steps)):
            px, py = c.x() + dx * (to_edge + s), c.y() + dy * (to_edge + s)
            for off in (-1, 0, 1):
                qx, qy = int(round(px - dy * off)), int(round(py + dx * off))
                if 0 <= qx < img.width() and 0 <= qy < img.height():
                    p = img.pixelColor(qx, qy)
                    if (abs(p.red() - bg.red()) > BACKGROUND_TOLERANCE or abs(p.green() - bg.green()) > BACKGROUND_TOLERANCE
                            or abs(p.blue() - bg.blue()) > BACKGROUND_TOLERANCE):
                        hits += 1
                        break
        return hits

    def layout_section(graph):
        gt.section("variadic layout")
        x0, y0 = 4400, 0
        for plugin_id in VARIADIC_IDS:
            if plugin_id not in NatronGui.natron.getPluginIDs():
                check(False, "%s is registered (%s)" % (plugin_id, gt.plugin_problem(plugin_id)))
                continue
            node = create(plugin_id, x0, y0)
            gt.close_all_panels()
            app.clearSelection()
            c = centre(node)
            frame(graph, 2.0, c[0], c[1])
            yield ("sleep", SETTLE_MS)
            img = snap(graph, "layout-%s" % plugin_id)
            shown = dict((label, stub_hits(img, graph, node, a)) for label, a in
                         (("upper right", math.pi / 3), ("upper left", 2 * math.pi / 3), ("left", math.pi)))
            empty = dict((label, stub_hits(img, graph, node, a)) for label, a in
                         (("straight up", math.pi / 2), ("shallow right", math.pi / 6), ("shallow left", 5 * math.pi / 6)))
            check(all(v >= 4 for v in shown.values()),
                  "%s: stubs for the first two inputs above and one aside on the left (pixels %r)" % (plugin_id, shown))
            check(all(v <= 2 for v in empty.values()),
                  "%s: no other input stubs are fanned out above (pixels %r)" % (plugin_id, empty))

            reads = [create(USD_READ_ID, x0 - 300 + 250 * i, y0 - 300) for i in range(3)]
            gt.close_all_panels()
            select_only([node] + reads)
            put_cursor(graph, QPoint(graph.viewport().width() // 2, graph.viewport().height() // 2))
            graph_key(graph, Qt.Key_Y, Qt.NoModifier)
            want = [r.getScriptName() for r in reads]
            ok = yield ("until", lambda: [input_name(node, i) for i in range(3)] == want, 5.0)
            check(ok, "%s: Y connects the three later-selected nodes into inputs 0-2 (got %r)"
                  % (plugin_id, [input_name(node, i) for i in range(3)]))
            app.clearSelection()
            x0 += 1200

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        require(MERGE_ID in NatronGui.natron.getPluginIDs(), "the Merge node is registered", gt.plugin_problem(MERGE_ID))
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active",
                "QGraphicsItem::isActive() is false in an inactive window, so the press misses the node")
        gt.close_all_panels()
        graph = root_graph()
        require(graph is not None, "found the node graph view")

        yield from shortcuts_section(graph)
        yield from viewer_section(graph)
        yield from preconnected_section(graph)
        yield from layout_section(graph)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
