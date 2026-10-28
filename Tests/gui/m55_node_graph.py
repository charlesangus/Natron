"""Screenshot and interaction checks for node-graph splicing and the render dialog: splice-into-
pipe (always on, unconnected nodes and chains only, highlighted target pipe, one undo step) and
the render dialog (writers, frame-range preset, background checkbox, modal foreground progress
with Cancel).

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m55-shots/out GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m55_node_graph.py build/fast/App/Natron'

Conventions shared with Tests/gui/m55_input_pipes.py and build/m24-shots/m24_border.py:
  - nodes are created the way the toolbar and Tab menu do (CreateNodeArgsPropSettingsOpened=True),
    which is also what builds a node's KnobGui widgets, e.g. the Write node's Render button;
  - NodeGraph.scene() raises "Internal C++ object (QGraphicsScene) already deleted" in this
    binary, so connections are verified through the Effect API (getInput()) and on-screen state
    through viewport pixel grabs, never through the scene;
  - drags are synthetic QMouseEvents (press, several moves, release) sent to the NodeGraph's
    viewport, at scene positions mapped through the view's mapFromScene().

The main window must be active before any drag: QGraphicsItem::isActive() is false in an inactive
window, NodeGraph::hasItemNearbyMouse() then skips every node, and a press lands on a nearby pipe.

Natron widget classes without Q_OBJECT (Button, DialogButtonBox) report their Qt base class name
(QPushButton, QDialogButtonBox) and are looked up under both. SpinBox exposes QLineEdit's API but
not its own setValue(), so a value is typed with setText() plus Return, which SpinBox parses in
interpretReturn(). ComboBox is a QFrame, so its setCurrentIndex slot is invoked by name.

RenderDialog.exec() runs a nested event loop, so each interaction is scheduled with
QTimer.singleShot() before whatever opens the dialog, and ends by clicking Ok or Cancel; a dialog
left open is rejected so the run cannot hang. The foreground QProgressDialog is shown with show(),
not exec(), so the script polls for it instead.
"""
import glob
import os
import re
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 240
SETTLE_MS = 400
MARGIN = 80.0
MAX_ZOOM = 1.8
GREEN_R_MAX = 110
GREEN_B_MAX = 110
GREEN_G_MIN = 170
DRAG_STEPS = 4

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Q_ARG, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QDialogButtonBox, QMainWindow, QPushButton, QWidget

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _made = []

    # ---- node / graph plumbing, shared shape with m55_input_pipes.py ------------------------

    def create(plugin_id, x, y, group=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        _made.append(node)
        return node

    def create_writer(filename, x, y):
        gt.close_all_panels()
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createWriter(filename, None, props)
        gt.require(node is not None, "create writer for " + filename)
        node.setPosition(x, y)
        _made.append(node)
        return node

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def input_connected_from(node, source):
        """The input index of node connected from source, comparing by script name since Effect
        wrapper objects returned by getInput() are not the same Python object as source."""
        if node is None or source is None:
            return None
        for i in range(node.getMaxInputCount()):
            inp = node.getInput(i)
            if inp is not None and inp.getScriptName() == source.getScriptName():
                return i
        return None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def box(effect):
        x, y = effect.getPosition()
        w, h = effect.getSize()
        return x, y, w, h

    def node_center_scene(effect):
        x, y, w, h = box(effect)
        return (x + w / 2.0, y + h / 2.0)

    def point_in_box(px, py, bx, by, bw, bh, pad=3.0):
        return (bx - pad) <= px <= (bx + bw + pad) and (by - pad) <= py <= (by + bh + pad)

    def frame(graph, nodes, margin=MARGIN, max_zoom=MAX_ZOOM):
        if graph is None or not nodes:
            return
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y, w, h = box(n)
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

    # ---- green-pixel probe for the highlighted pipe / hint edges ---------------------------------
    # Edge::paint draws a highlighted pipe, and the splice hint edges, in plain Qt::green. None of
    # this script's own node bodies, selection halos (white) or category tints are pure green, so
    # "is there a green pixel in roughly this area" is a safe, cheap stand-in for "is the hint/
    # highlight drawn here" without needing scene() (broken in this binary, see the module
    # docstring) or any new Python binding.

    def is_green(c):
        return (c.green() >= GREEN_G_MIN) and (c.red() <= GREEN_R_MAX) and (c.blue() <= GREEN_B_MAX)

    def count_green_in_scene_rect(graph, img, rect_scene, pad=12.0):
        x0, y0, x1, y1 = rect_scene
        x0 -= pad
        y0 -= pad
        x1 += pad
        y1 += pad
        tl = graph.mapFromScene(QPointF(x0, y0))
        br = graph.mapFromScene(QPointF(x1, y1))
        px0, px1 = sorted((tl.x(), br.x()))
        py0, py1 = sorted((tl.y(), br.y()))
        px0 = max(0, px0)
        py0 = max(0, py0)
        px1 = min(img.width(), px1)
        py1 = min(img.height(), py1)
        hits = 0
        for y in range(py0, py1):
            for x in range(px0, px1):
                if is_green(img.pixelColor(x, y)):
                    hits += 1
        return hits

    def check_green_present(graph, rect_scene, label, want_present=True):
        _pix, img = viewport_image(graph)
        hits = count_green_in_scene_rect(graph, img, rect_scene)
        report("NOTE %s: %d green px sampled" % (label, hits))
        if want_present:
            return check(hits > 0, "%s: a green hint/highlight pixel is drawn" % label)
        else:
            return check(hits == 0, "%s: no green hint/highlight pixel is drawn (got %d)" % (label, hits))

    # ---- synthetic drag, press/move*3+/release on the NodeGraph viewport ----------------------

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        ev = QMouseEvent(etype, QPointF(pos), button, buttons, modifiers)
        QApplication.sendEvent(widget, ev)
        QApplication.processEvents()

    def begin_drag(graph, node):
        start = node_center_scene(node)
        p = graph.mapFromScene(QPointF(start[0], start[1]))
        send_mouse(graph.viewport(), QEvent.MouseButtonPress, p, Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
        return start

    def drag_step(graph, start_scene, end_scene, frac, modifiers):
        sx = start_scene[0] + (end_scene[0] - start_scene[0]) * frac
        sy = start_scene[1] + (end_scene[1] - start_scene[1]) * frac
        p = graph.mapFromScene(QPointF(sx, sy))
        send_mouse(graph.viewport(), QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton, modifiers)
        return (sx, sy)

    def end_drag(graph, pos_scene):
        p = graph.mapFromScene(QPointF(pos_scene[0], pos_scene[1]))
        send_mouse(graph.viewport(), QEvent.MouseButtonRelease, p, Qt.LeftButton, Qt.NoButton, Qt.NoModifier)

    def drag_node_to(graph, node, target_scene, modifiers=Qt.NoModifier, steps=DRAG_STEPS,
                     at_each_step=None, release=True):
        """Presses on node's centre, moves it towards target_scene in >= steps increments (the
        task asks for at least three moves), optionally calling at_each_step(pos) after every
        move, then releases (unless release=False, to inspect a held drag before dropping it)."""
        start = begin_drag(graph, node)
        pos = start
        for i in range(1, steps + 1):
            pos = drag_step(graph, start, target_scene, i / float(steps), modifiers)
            if at_each_step:
                at_each_step(pos)
        if release:
            end_drag(graph, pos)
        return pos

    def send_key(widget, key, modifiers=Qt.NoModifier):
        if not check(widget is not None, "a widget exists to send the key event to"):
            return
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def set_line_edit_value(widget, text):
        """For a SpinBox (": public LineEdit : public QLineEdit"); see the module docstring."""
        widget.setText(str(text))
        send_key(widget, Qt.Key_Return)

    def set_combo_index(combo, index):
        """For a ComboBox (": public QFrame"); see the module docstring."""
        try:
            QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
            QApplication.processEvents()
            return True
        except Exception:
            report("NOTE set_combo_index(%d) raised: %s" % (index, traceback.format_exc()))
            return False

    # ---- menu / action lookup, generalised from m55_input_pipes.py's edit_menu_actions() -------

    def menu_actions(title_substr):
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow):
                continue
            mb = win.menuBar()
            if mb is None:
                continue
            for act in mb.actions():
                menu = act.menu()
                if menu is not None and title_substr.lower() in act.text().lower():
                    return menu.actions()
        return []

    def find_action(menu_title_substr, text_substr):
        for a in menu_actions(menu_title_substr):
            if text_substr.lower() in a.text().lower():
                return a
        return None

    def trigger_action_prefix(prefix):
        """Undo/Redo are plain QActions on the Edit menu, matched and triggered the same way
        m55_input_pipes.py's trigger_undo() does (Ctrl+Z/Ctrl+Shift+Z are caught by Qt's shortcut
        map before a sendEvent()-delivered key event reaches NodeGraph, so triggering the action
        object directly is the reliable path)."""
        for a in menu_actions("edit"):
            if a.text().lower().startswith(prefix) and a.isEnabled():
                a.trigger()
                QApplication.processEvents()
                return True
        check(False, "found an enabled '%s' action in the Edit menu" % prefix)
        return False

    # ---- render dialog plumbing ------------------------------------------------------------------

    def render_dialog_parts(dialog):
        by_class = {}
        for w in dialog.findChildren(QWidget):
            by_class.setdefault(gt.class_name(w), []).append(w)
        error_labels = [w for w in by_class.get("Label", [])
                        if w.isVisible() and w.styleSheet().replace(" ", "") == "color:red;"]
        spins = sorted(by_class.get("SpinBox", []), key=lambda w: w.pos().x())
        checks = [w for w in by_class.get("QCheckBox", []) if w.isVisible()]
        boxes = by_class.get("DialogButtonBox", []) + by_class.get("QDialogButtonBox", [])
        trees = by_class.get("QTreeWidget", [])
        combos = by_class.get("ComboBox", [])
        return {
            "combo": combos[0] if combos else None,
            "first": spins[0] if len(spins) > 0 else None,
            "last": spins[1] if len(spins) > 1 else None,
            "step": spins[2] if len(spins) > 2 else None,
            "background": checks[0] if checks else None,
            "buttons": boxes[0] if boxes else None,
            "tree": trees[0] if trees else None,
            "error": error_labels[0] if error_labels else None,
        }

    def find_render_dialog():
        dlgs = [w for w in gt.widgets_of("RenderDialog") if w.isVisible()]
        return dlgs[-1] if dlgs else None

    def open_render_dialog_and(trigger, configure, label, delay_ms=300):
        """Drives a RenderDialog opened synchronously (QDialog.exec(), a real nested event loop)
        by trigger(); configure() is scheduled to fire from inside that nested loop and must end
        by accepting or rejecting the dialog (clicking Ok/Cancel), so that trigger() has returned
        by the time this function does. See the module docstring."""
        state = {"ran": False}

        def wrapped():
            state["ran"] = True
            try:
                configure()
            except Exception:
                report("NOTE %s: configure() raised: %s" % (label, traceback.format_exc()))
            leftover = find_render_dialog()
            if leftover is not None:
                check(False, "%s: configure() left the RenderDialog open; rejecting it" % label)
                leftover.reject()

        QTimer.singleShot(delay_ms, wrapped)
        try:
            trigger()
        except Exception:
            report("NOTE %s: trigger() raised: %s" % (label, traceback.format_exc()))
        QApplication.processEvents()
        check(state["ran"], "%s: the dialog's configure callback ran" % label)
        return state["ran"]

    def find_progress_dialog():
        for w in QApplication.topLevelWidgets():
            if gt.class_name(w) == "QProgressDialog" and w.isVisible():
                return w
        return None

    def find_write_render_button(writer, quiet=False):
        candidates = [w for w in gt.widgets_of("Button") + gt.widgets_of("QPushButton")
                      if w.isVisible() and w.text() == "Render"]
        if not quiet:
            report("NOTE %s: %d visible 'Render' Button widget(s) found" % (writer.getLabel(), len(candidates)))
        return candidates[-1] if candidates else None

    def wait_for_render_button(writer):
        """A new node's settings panel is built on a later event-loop pass than createNode()."""
        return ("until", lambda: find_write_render_button(writer, quiet=True) is not None, 10.0)

    def click_write_render_button(writer):
        button = find_write_render_button(writer)
        if button is not None:
            button.click()

    def count_output_files(pattern):
        return sorted(glob.glob(pattern))

    def frame_numbers_from(paths, regex):
        nums = set()
        for p in paths:
            m = re.search(regex, os.path.basename(p))
            if m:
                nums.add(int(m.group(1)))
        return nums

    def progress_panel_status_texts():
        """Every cell of the Progress panel's task table (a TableView, i.e. a QTreeView over a
        TableModel); ProgressTaskInfo sets the status cell to Paused/Canceled/Finished."""
        texts = []
        try:
            for panel in gt.widgets_of("ProgressPanel"):
                for view in panel.findChildren(QWidget):
                    if gt.class_name(view) != "TableView":
                        continue
                    model = view.model()
                    for row in range(model.rowCount()):
                        for col in range(model.columnCount()):
                            value = model.data(model.index(row, col))
                            if value is not None:
                                texts.append(str(value))
        except Exception:
            report("NOTE progress_panel_status_texts raised: %s" % traceback.format_exc())
        return texts

    def progress_panel_is_current_tab():
        """TabWidget::makeCurrentTab() hides every tab but the one it makes current (it calls
        setVisible(false) on the old one and setVisible(true) on the new one), so a ProgressPanel
        that is visible is the current tab of whatever pane holds it."""
        return any(w.isVisible() for w in gt.widgets_of("ProgressPanel"))

    PANEL_TIMER_WAIT_MS = 3600

    def check_panel_stays_hidden_after_timer(label):
        """ProgressPanel raises itself NATRON_DISPLAY_PROGRESS_PANEL_AFTER_MS (3 s) after a task
        starts, and again when the engine reports the task stopped; both arrive after the modal
        dialog has closed, so the tab is checked again once they have had time to fire."""
        yield ("sleep", PANEL_TIMER_WAIT_MS)
        QApplication.processEvents()
        check(not progress_panel_is_current_tab(),
             "%s: the Progress panel is still not the current tab %.1f s later"
             % (label, PANEL_TIMER_WAIT_MS / 1000.))

    def ensure_progress_panel_not_current(label):
        """A background render raises the Progress panel by design (render-background-custom
        runs earlier and does exactly that), so a foreground section cannot tell its own render
        apart from that leftover state unless it first puts another tab back in front of the
        Progress panel's pane itself. TabBar.setCurrentIndex is a slot wired to
        TabWidget::makeCurrentTab, so driving it switches the pane for real, not just the tab
        bar's own highlight."""
        panels = [w for w in gt.widgets_of("ProgressPanel") if w.parentWidget() is not None]
        if panels:
            pane = panels[0].parentWidget()
            bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
            if len(bars) == 1:
                bar = bars[0]
                count = bar.property("count")
                current = bar.property("currentIndex")
                if count is not None and current is not None and count > 1:
                    QMetaObject.invokeMethod(bar, "setCurrentIndex", Q_ARG(int, (current + 1) % count))
                    QApplication.processEvents()
        check(not progress_panel_is_current_tab(),
             "%s: the Progress panel is not the current tab before starting the render" % label)

    # ---- steps ------------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active",
                   "QGraphicsItem::isActive() is false in an inactive window, so presses miss every node")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # =====================================================================================
        # Splice into pipe: always on, unconnected nodes/chains only, Ctrl has no special
        # meaning during the drag (it no longer suppresses splicing in favour of a hint).
        # =====================================================================================

        # ---- splice-vertical: hover-with-highlight, drop, and a single-node one-step undo/redo.
        gt.section("splice-vertical")
        src_v = create("Constant", 0, 0)
        dst_v = create("Blur", 0, 300)
        connect(dst_v, 0, src_v)
        mover_v = create("Blur", 400, 150)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph_v = root_graph()
        frame(graph_v, [src_v, dst_v, mover_v])
        yield ("sleep", SETTLE_MS)

        pipe_rect_v = (0.0, 0.0, 400.0, 450.0)
        mover_pre_pos = mover_v.getPosition()

        def mid_check_v(_pos):
            graph_shot(graph_v, "splice-01-hover-vertical")
            check_green_present(graph_v, pipe_rect_v, "splice-vertical: hover shows the highlight")

        # The highlighted pipe sits below node bodies, so the node is dropped just beside the pipe
        # (still inside the splice tolerance) to leave the highlight in view.
        beside_v = box(mover_v)[2] / 2.0 + 9.0
        target_v = (node_center_scene(src_v)[0] + beside_v,
                    (node_center_scene(src_v)[1] + node_center_scene(dst_v)[1]) / 2.0)
        drag_node_to(graph_v, mover_v, target_v, modifiers=Qt.NoModifier, at_each_step=None,
                    release=False)
        mid_check_v(target_v)
        end_drag(graph_v, target_v)
        yield ("sleep", SETTLE_MS)
        graph_shot(graph_v, "splice-02-after-drop-vertical")

        entry_input_v = input_connected_from(mover_v, src_v)
        check(entry_input_v is not None, "splice-vertical: mover's preferred input now connects from the old source")
        check(input_connected_from(dst_v, mover_v) is not None,
             "splice-vertical: destination's pipe input now connects from the dropped node")
        check(input_connected_from(dst_v, src_v) is None,
             "splice-vertical: destination is no longer directly connected to the old source")

        trigger_action_prefix("undo")
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(dst_v, src_v) is not None,
             "splice-vertical: one Ctrl+Z restores the original connection")
        check(input_connected_from(mover_v, src_v) is None,
             "splice-vertical: one Ctrl+Z disconnects the moved node's new input")
        undone_pos = mover_v.getPosition()
        check(abs(undone_pos[0] - mover_pre_pos[0]) < 1.0 and abs(undone_pos[1] - mover_pre_pos[1]) < 1.0,
             "splice-vertical: one Ctrl+Z also restores the node's pre-drag position "
             "(got %r, want %r)" % (undone_pos, mover_pre_pos))

        trigger_action_prefix("redo")
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(mover_v, src_v) is not None,
             "splice-vertical: one Ctrl+Shift+Z (redo) re-applies the splice connection")
        check(input_connected_from(dst_v, mover_v) is not None,
             "splice-vertical: redo also re-applies the destination's connection")
        graph_shot(graph_v, "splice-02b-after-redo-vertical")

        # ---- splice-diagonal: the same hover-with-highlight check over a diagonal pipe.
        gt.section("splice-diagonal")
        src_d = create("Constant", 900, 0)
        dst_d = create("Blur", 1250, 350)
        connect(dst_d, 0, src_d)
        mover_d = create("Blur", 900, 500)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph_d = root_graph()
        frame(graph_d, [src_d, dst_d, mover_d])
        yield ("sleep", SETTLE_MS)

        pipe_rect_d = (900.0, 0.0, 1300.0, 400.0)
        # Off the pipe along its normal, by enough that the node's corner still grazes the pipe
        # while the closest point on the pipe (where the highlight is sampled) is clear of the
        # node body.
        sdx = node_center_scene(dst_d)[0] - node_center_scene(src_d)[0]
        sdy = node_center_scene(dst_d)[1] - node_center_scene(src_d)[1]
        slen = (sdx * sdx + sdy * sdy) ** 0.5
        nx, ny = sdy / slen, -sdx / slen
        off_d = (abs(nx) * box(mover_d)[2] + abs(ny) * box(mover_d)[3]) / 2.0 + 8.0
        target_d = ((node_center_scene(src_d)[0] + node_center_scene(dst_d)[0]) / 2.0 + nx * off_d,
                    (node_center_scene(src_d)[1] + node_center_scene(dst_d)[1]) / 2.0 + ny * off_d)
        drag_node_to(graph_d, mover_d, target_d, modifiers=Qt.NoModifier, release=False)
        graph_shot(graph_d, "splice-03-hover-diagonal")
        check_green_present(graph_d, pipe_rect_d, "splice-diagonal: hover shows the highlight")
        end_drag(graph_d, target_d)
        yield ("sleep", SETTLE_MS)
        graph_shot(graph_d, "splice-04-after-drop-diagonal")
        check(input_connected_from(mover_d, src_d) is not None,
             "splice-diagonal: mover's preferred input now connects from the old source")
        check(input_connected_from(dst_d, mover_d) is not None,
             "splice-diagonal: destination's pipe input now connects from the dropped node")

        # ---- splice-chain: a selected 2-node chain splices with the top as entry, bottom as exit.
        gt.section("splice-chain")
        src_c = create("Constant", 1800, 0)
        dst_c = create("Blur", 1800, 500)
        connect(dst_c, 0, src_c)
        chain_entry = create("Blur", 2200, 150)
        chain_exit = create("Blur", 2200, 300)
        connect(chain_exit, 0, chain_entry)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph_c = root_graph()
        frame(graph_c, [src_c, dst_c, chain_entry, chain_exit])
        yield ("sleep", SETTLE_MS)

        app.setSelection([chain_entry, chain_exit])
        QApplication.processEvents()
        chain_entry_pre = chain_entry.getPosition()
        chain_exit_pre = chain_exit.getPosition()
        target_c = (node_center_scene(src_c)[0], (node_center_scene(src_c)[1] + node_center_scene(dst_c)[1]) / 2.0)
        # Press on chain_entry: it is already part of the (shift-less) selection, so
        # NodeGraph::mousePressEvent (Gui/NodeGraph10.cpp) leaves the multi-selection intact and
        # starts a drag of the whole selection rather than narrowing it to just this node.
        drag_node_to(graph_c, chain_entry, target_c, modifiers=Qt.NoModifier, release=False)
        graph_shot(graph_c, "splice-05-chain-hover")
        end_drag(graph_c, target_c)
        yield ("sleep", SETTLE_MS)
        graph_shot(graph_c, "splice-06-chain-after-drop")

        check(input_connected_from(chain_entry, src_c) is not None,
             "splice-chain: the chain's entry (top) now connects from the old source")
        check(input_connected_from(dst_c, chain_exit) is not None,
             "splice-chain: the destination now connects from the chain's exit (bottom)")
        check(input_connected_from(dst_c, src_c) is None,
             "splice-chain: destination no longer connects directly to the old source")
        check(input_connected_from(chain_exit, chain_entry) is not None,
             "splice-chain: the chain's own internal connection is untouched")

        trigger_action_prefix("undo")
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(dst_c, src_c) is not None,
             "splice-chain: one Ctrl+Z restores the original connection")
        check(input_connected_from(chain_entry, src_c) is None,
             "splice-chain: one Ctrl+Z disconnects the chain's entry")
        entry_undone = chain_entry.getPosition()
        exit_undone = chain_exit.getPosition()
        check(abs(entry_undone[0] - chain_entry_pre[0]) < 1.0 and abs(entry_undone[1] - chain_entry_pre[1]) < 1.0,
             "splice-chain: one Ctrl+Z also restores the chain entry's pre-drag position")
        check(abs(exit_undone[0] - chain_exit_pre[0]) < 1.0 and abs(exit_undone[1] - chain_exit_pre[1]) < 1.0,
             "splice-chain: one Ctrl+Z also restores the chain exit's pre-drag position")

        trigger_action_prefix("redo")
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(chain_entry, src_c) is not None,
             "splice-chain: one Ctrl+Shift+Z (redo) re-applies the splice")
        check(input_connected_from(dst_c, chain_exit) is not None,
             "splice-chain: redo also re-applies the destination's connection")
        graph_shot(graph_c, "splice-06b-chain-after-redo")

        # ---- splice-no-splice-connected: an already-connected node dragged across another pipe
        # does not splice (its preferred input is already taken, so planSplice's entry count is 0).
        gt.section("splice-no-splice-connected")
        src_n1 = create("Constant", 2800, 0)
        mid_n = create("Blur", 2800, 200)
        connect(mid_n, 0, src_n1)
        src_n2 = create("Constant", 3100, 400)
        dst_n2 = create("Blur", 3100, 700)
        connect(dst_n2, 0, src_n2)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph_n = root_graph()
        frame(graph_n, [src_n1, mid_n, src_n2, dst_n2])
        yield ("sleep", SETTLE_MS)

        pipe_rect_n2 = (box(src_n2)[0], box(src_n2)[1],
                        box(dst_n2)[0] + box(dst_n2)[2], box(dst_n2)[1] + box(dst_n2)[3])
        target_n = (node_center_scene(src_n2)[0], (node_center_scene(src_n2)[1] + node_center_scene(dst_n2)[1]) / 2.0)
        drag_node_to(graph_n, mid_n, target_n, modifiers=Qt.NoModifier, release=False)
        graph_shot(graph_n, "splice-07-connected-node-no-splice-hover")
        check_green_present(graph_n, pipe_rect_n2, "no-splice: an already-connected node dragged over a pipe "
                            "shows no highlight", want_present=False)
        end_drag(graph_n, target_n)
        yield ("sleep", SETTLE_MS)
        check(input_connected_from(mid_n, src_n1) is not None,
             "no-splice: the dragged node's own connection is unchanged")
        check(input_connected_from(dst_n2, src_n2) is not None,
             "no-splice: the pipe it was dragged across is unchanged")
        check(input_connected_from(dst_n2, mid_n) is None,
             "no-splice: the dragged node was not inserted into the other pipe")

        # ---- splice-ctrl-still-splices: Ctrl has no special meaning during a node drag anymore,
        # so a Ctrl-held drag over a pipe splices exactly like an unmodified one.
        gt.section("splice-ctrl-still-splices")
        src_ctrl = create("Constant", 3800, 0)
        dst_ctrl = create("Blur", 3800, 300)
        connect(dst_ctrl, 0, src_ctrl)
        mover_ctrl = create("Blur", 4200, 150)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph_ctrl = root_graph()
        frame(graph_ctrl, [src_ctrl, dst_ctrl, mover_ctrl])
        yield ("sleep", SETTLE_MS)

        pipe_rect_ctrl = (3800.0, 0.0, 4200.0, 450.0)
        target_ctrl = (node_center_scene(src_ctrl)[0],
                       (node_center_scene(src_ctrl)[1] + node_center_scene(dst_ctrl)[1]) / 2.0)
        drag_node_to(graph_ctrl, mover_ctrl, target_ctrl, modifiers=Qt.ControlModifier, release=False)
        graph_shot(graph_ctrl, "splice-08-ctrl-still-splices")
        check_green_present(graph_ctrl, pipe_rect_ctrl,
                            "ctrl-still-splices: holding Ctrl still shows the splice highlight")
        end_drag(graph_ctrl, target_ctrl)
        yield ("sleep", SETTLE_MS)

        check(input_connected_from(mover_ctrl, src_ctrl) is not None,
             "ctrl-still-splices: mover's preferred input now connects from the old source")
        check(input_connected_from(dst_ctrl, mover_ctrl) is not None,
             "ctrl-still-splices: destination's pipe input now connects from the dropped node")
        check(input_connected_from(dst_ctrl, src_ctrl) is None,
             "ctrl-still-splices: destination is no longer directly connected to the old source")

        # =====================================================================================
        # Render dialog: writers/preset/background checkbox, modal foreground progress.
        # =====================================================================================
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- render-entry-points: the dialog opens from the Write panel's Render button, Render
        # Selected and Render All; Cancel on each leaves no rendered file behind. Render Selected
        # here deliberately selects an actual Write node rather than a plain effect: Gui::
        # renderSelectedNode() auto-creates a writer via Gui::createWriter() for a non-writer
        # selection, which (with no filename argument) opens a native, blocking Save dialog that
        # nothing in this script could answer.
        gt.section("render-entry-points")
        ep2_path = os.path.join(gt.OUT, "render_ep2.####.png")
        writer_ep2 = create_writer(ep2_path, 5600, 300)
        ep1_path = os.path.join(gt.OUT, "render_ep1.####.png")
        writer_ep1 = create_writer(ep1_path, 5600, 0)
        yield wait_for_render_button(writer_ep1)

        def cancel_dialog(label):
            def configure():
                dlg = find_render_dialog()
                if not check(dlg is not None, "%s: the RenderDialog opened" % label):
                    return
                gt.shot("render-dialog-%s.png" % label)
                parts = render_dialog_parts(dlg)
                if check(parts["buttons"] is not None, "%s: found the dialog's button box" % label):
                    cancel_btn = parts["buttons"].button(QDialogButtonBox.StandardButton.Cancel)
                    if check(cancel_btn is not None, "%s: found the Cancel button" % label):
                        cancel_btn.click()
            return configure

        ok1 = open_render_dialog_and(lambda: click_write_render_button(writer_ep1),
                                     cancel_dialog("write-panel"), "render-entry-write-panel")
        if not ok1:
            report("NOTE render-entry-write-panel: could not find/click the Write panel's Render button")
        yield ("sleep", SETTLE_MS)
        check(not glob.glob(os.path.join(gt.OUT, "render_ep1.*.png")),
             "render-entry-write-panel: Cancel left no rendered file")

        app.setSelection([writer_ep2])
        graph_ep = root_graph()
        if graph_ep is not None:
            graph_ep.setFocus()
        QApplication.processEvents()
        render_selected_action = find_action("render", "selected")
        if check(render_selected_action is not None, "found the 'Render Selected' action in the Render menu"):
            open_render_dialog_and(render_selected_action.trigger, cancel_dialog("render-selected"),
                                   "render-entry-render-selected")
            yield ("sleep", SETTLE_MS)
            check(not glob.glob(os.path.join(gt.OUT, "render_ep2.*.png")),
                 "render-entry-render-selected: Cancel left no rendered file")

        render_all_action = find_action("render", "all")
        if check(render_all_action is not None, "found the 'Render All' action in the Render menu"):
            open_render_dialog_and(render_all_action.trigger, cancel_dialog("render-all"),
                                   "render-entry-render-all")
            yield ("sleep", SETTLE_MS)
            check(not glob.glob(os.path.join(gt.OUT, "render_ep1.*.png")) and
                 not glob.glob(os.path.join(gt.OUT, "render_ep2.*.png")),
                 "render-entry-render-all: Cancel left no rendered file")

        # ---- render-background-custom: background checked, Custom 3-5 writes exactly those
        # frames and leaves a Progress panel task (the render runs asynchronously, so files are
        # polled for rather than checked immediately).
        gt.section("render-background-custom")
        bg_path = os.path.join(gt.OUT, "render_bg.####.png")
        writer_bg = create_writer(bg_path, 5600, 600)
        src_bg = create("Constant", 5300, 600)
        connect(writer_bg, 0, src_bg)
        yield wait_for_render_button(writer_bg)

        def configure_bg():
            dlg = find_render_dialog()
            if not check(dlg is not None, "render-background-custom: the RenderDialog opened"):
                return
            parts = render_dialog_parts(dlg)
            if check(parts["combo"] is not None, "render-background-custom: found the preset combo"):
                set_combo_index(parts["combo"], 2)  # eRenderRangePresetCustom
                check(str(parts["combo"].property("natronCurrentText")) == "Custom",
                     "render-background-custom: preset combo now reads Custom")
            if check(parts["first"] is not None and parts["last"] is not None,
                     "render-background-custom: found the first/last spin boxes"):
                set_line_edit_value(parts["first"], 3)
                set_line_edit_value(parts["last"], 5)
                if parts["step"] is not None:
                    set_line_edit_value(parts["step"], 1)
            if check(parts["background"] is not None, "render-background-custom: found the background checkbox"):
                parts["background"].setChecked(True)
            gt.shot("render-dialog-background-custom.png")
            if check(parts["buttons"] is not None, "render-background-custom: found the dialog's button box"):
                ok_btn = parts["buttons"].button(QDialogButtonBox.StandardButton.Ok)
                if check(ok_btn is not None and ok_btn.isEnabled(),
                         "render-background-custom: the Ok button is enabled"):
                    ok_btn.click()

        render_button_bg = find_write_render_button(writer_bg)
        if check(render_button_bg is not None, "render-background-custom: found the Write panel's Render button"):
            open_render_dialog_and(render_button_bg.click, configure_bg, "render-background-custom")

        found = yield ("until",
                       lambda: len(count_output_files(os.path.join(gt.OUT, "render_bg.*.png"))) >= 3,
                       60.0)
        check(found, "render-background-custom: 3 frames were written within the timeout")
        bg_files = count_output_files(os.path.join(gt.OUT, "render_bg.*.png"))
        bg_frames = frame_numbers_from(bg_files, r"render_bg\.(\d+)\.png$")
        check(bg_frames == {3, 4, 5}, "render-background-custom: exactly frames 3, 4 and 5 were written "
             "(got %r)" % sorted(bg_frames))
        graph_shot(root_graph(), "render-background-custom-graph")

        # ---- render-foreground: a modest foreground (background unchecked) render shows the
        # modal progress dialog, blocks other windows, closes itself, and leaves every file.
        gt.section("render-foreground")
        fg_path = os.path.join(gt.OUT, "render_fg.####.png")
        writer_fg = create_writer(fg_path, 5600, 900)
        src_fg = create("Constant", 5300, 900)
        connect(writer_fg, 0, src_fg)
        yield wait_for_render_button(writer_fg)
        FG_FRAMES = 10

        def configure_fg_custom(first, last):
            def configure():
                dlg = find_render_dialog()
                if not check(dlg is not None, "render-foreground: the RenderDialog opened"):
                    return
                parts = render_dialog_parts(dlg)
                if parts["combo"] is not None:
                    set_combo_index(parts["combo"], 2)
                if parts["first"] is not None and parts["last"] is not None:
                    set_line_edit_value(parts["first"], first)
                    set_line_edit_value(parts["last"], last)
                    if parts["step"] is not None:
                        set_line_edit_value(parts["step"], 1)
                if parts["background"] is not None:
                    parts["background"].setChecked(False)
                if check(parts["buttons"] is not None, "render-foreground: found the dialog's button box"):
                    ok_btn = parts["buttons"].button(QDialogButtonBox.StandardButton.Ok)
                    if ok_btn is not None:
                        ok_btn.click()
            return configure

        ensure_progress_panel_not_current("render-foreground")
        render_button_fg = find_write_render_button(writer_fg)
        if check(render_button_fg is not None, "render-foreground: found the Write panel's Render button"):
            open_render_dialog_and(render_button_fg.click, configure_fg_custom(1, FG_FRAMES), "render-foreground")

        yield ("sleep", 150)
        pdlg_fg = find_progress_dialog()
        if check(pdlg_fg is not None, "render-foreground: the modal progress dialog is up"):
            gt.shot("render-foreground-mid.png")
            check(QApplication.activeModalWidget() is pdlg_fg,
                 "render-foreground: the progress dialog is the active modal widget (blocks other windows)")
            check(not progress_panel_is_current_tab(),
                 "render-foreground: the Progress panel is not the current tab while the modal dialog is up")
            closed = yield ("until", lambda: find_progress_dialog() is None, 90.0)
            check(closed, "render-foreground: the progress dialog closed by itself")
        check(not progress_panel_is_current_tab(),
             "render-foreground: the Progress panel did not become the current tab after the render finished")
        yield ("sleep", SETTLE_MS)
        fg_files = count_output_files(os.path.join(gt.OUT, "render_fg.*.png"))
        check(len(fg_files) == FG_FRAMES,
             "render-foreground: all %d frames were written (got %d)" % (FG_FRAMES, len(fg_files)))

        # ---- render-foreground-cancel: a longer render, cancelled almost immediately, stops
        # early and leaves the progress dialog closed with the panel showing Paused/Canceled.
        gt.section("render-foreground-cancel")
        cancel_path = os.path.join(gt.OUT, "render_cancel.####.png")
        writer_cancel = create_writer(cancel_path, 5600, 1200)
        src_cancel = create("Blur", 5300, 1200)
        src_cancel_upstream = create("Constant", 5000, 1200)
        connect(src_cancel, 0, src_cancel_upstream)
        connect(writer_cancel, 0, src_cancel)
        yield wait_for_render_button(writer_cancel)
        size_param = src_cancel.getParam("size")
        if size_param is not None:
            try:
                size_param.set(200, 200)
            except Exception:
                report("NOTE render-foreground-cancel: could not set Blur's size param, "
                      "continuing with the default (slower render is a bonus, not a requirement)")
        else:
            report("NOTE render-foreground-cancel: Blur has no 'size' param on this build; "
                  "cancelling immediately after start should still outrun a 60-frame render")
        CANCEL_FRAMES = 60

        ensure_progress_panel_not_current("render-foreground-cancel")
        render_button_cancel = find_write_render_button(writer_cancel)
        if check(render_button_cancel is not None, "render-foreground-cancel: found the Render button"):
            open_render_dialog_and(render_button_cancel.click, configure_fg_custom(1, CANCEL_FRAMES),
                                   "render-foreground-cancel")

        yield ("sleep", 300)
        pdlg_cancel = find_progress_dialog()
        if check(pdlg_cancel is not None, "render-foreground-cancel: the progress dialog is up"):
            first = yield ("until", lambda: len(count_output_files(os.path.join(gt.OUT, "render_cancel.*.png"))) >= 1,
                           60.0)
            check(first, "render-foreground-cancel: the first frame was written before cancelling")
            gt.shot("render-foreground-cancel-before.png")
            check(not progress_panel_is_current_tab(),
                 "render-foreground-cancel: the Progress panel is not the current tab while the modal dialog is up")
            # QProgressDialog.cancel() only hides the dialog without emitting canceled(); the
            # Cancel button is what a person presses.
            cancel_buttons = [b for b in pdlg_cancel.findChildren(QPushButton) if b.text() == "Cancel"]
            if check(len(cancel_buttons) == 1, "render-foreground-cancel: found the progress dialog's Cancel button"):
                cancel_buttons[0].click()
            QApplication.processEvents()
            closed = yield ("until", lambda: find_progress_dialog() is None, 60.0)
            check(closed, "render-foreground-cancel: the progress dialog closed after Cancel")
        check(not progress_panel_is_current_tab(),
             "render-foreground-cancel: the Progress panel did not become the current tab after cancelling")
        yield from check_panel_stays_hidden_after_timer("render-foreground-cancel")
        yield ("sleep", SETTLE_MS)
        cancel_files = count_output_files(os.path.join(gt.OUT, "render_cancel.*.png"))
        check(len(cancel_files) < CANCEL_FRAMES,
             "render-foreground-cancel: fewer than %d frames were written (got %d)"
             % (CANCEL_FRAMES, len(cancel_files)))
        # The panel's status cell is set from cancelTask(), a direct call from the Cancel click, but
        # a frame-progress report already in flight on the render thread can be delivered right
        # after and can race it; poll briefly rather than reading the cell exactly once.
        wait_start = time.time()
        row_canceled = yield ("until",
                              lambda: any(s in ("Paused", "Canceled") for s in progress_panel_status_texts()),
                              10.0)
        statuses = progress_panel_status_texts()
        report("NOTE render-foreground-cancel: Progress panel status text(s) seen: %r (waited %.2fs)"
              % (statuses, time.time() - wait_start))
        check(row_canceled, "render-foreground-cancel: the Progress panel shows Paused or Canceled")
        gt.shot("render-foreground-cancel-panel.png")

        # ---- render-background-negative-range: a background render runs in a separate process,
        # whose command line cannot express a negative frame, so the dialog refuses a range that
        # starts below 0 while "Render in background" is checked, and only then.
        gt.section("render-background-negative-range")
        neg_path = os.path.join(gt.OUT, "render_neg.####.png")
        writer_neg = create_writer(neg_path, 5600, 1500)
        src_neg = create("Constant", 5300, 1500)
        connect(writer_neg, 0, src_neg)
        yield wait_for_render_button(writer_neg)

        def configure_negative():
            label = "render-background-negative-range"
            dlg = find_render_dialog()
            if not check(dlg is not None, "%s: the RenderDialog opened" % label):
                return
            parts = render_dialog_parts(dlg)

            def ok_enabled():
                QApplication.processEvents()
                if parts["buttons"] is None:
                    return None
                ok_btn = parts["buttons"].button(QDialogButtonBox.StandardButton.Ok)
                return ok_btn is not None and ok_btn.isEnabled()

            if parts["combo"] is not None:
                set_combo_index(parts["combo"], 2)
            if not check(parts["first"] is not None and parts["last"] is not None
                         and parts["background"] is not None and parts["buttons"] is not None,
                         "%s: found the spin boxes, the background checkbox and the button box" % label):
                return
            set_line_edit_value(parts["first"], -5)
            set_line_edit_value(parts["last"], 5)
            if parts["step"] is not None:
                set_line_edit_value(parts["step"], 1)

            parts["background"].setChecked(False)
            check(ok_enabled() is True,
                  "%s: a foreground render may start below frame 0" % label)

            parts["background"].setChecked(True)
            check(ok_enabled() is False,
                  "%s: the Ok button is disabled for a background render starting below 0" % label)
            gt.shot("render-dialog-background-negative.png")
            error = render_dialog_parts(dlg)["error"]
            check(error is not None and "separate process" in error.text(),
                  "%s: an inline error names the separate-process limit (got %r)"
                  % (label, error.text() if error is not None else None))

            set_line_edit_value(parts["first"], 0)
            check(ok_enabled() is True,
                  "%s: the Ok button is enabled again once the range starts at 0" % label)

            cancel_btn = parts["buttons"].button(QDialogButtonBox.StandardButton.Cancel)
            if cancel_btn is not None:
                cancel_btn.click()

        ensure_progress_panel_not_current("render-background-negative-range")
        render_button_neg = find_write_render_button(writer_neg)
        if check(render_button_neg is not None,
                 "render-background-negative-range: found the Render button"):
            open_render_dialog_and(render_button_neg.click, configure_negative,
                                   "render-background-negative-range")
        yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
