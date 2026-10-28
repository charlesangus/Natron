"""The dope sheet draws and moves clips for the native TimeOffset and FrameRange nodes, and draws
keys upstream of a TimeOffset at the frames the viewer shows them.

A Read feeds TimeOffset, which feeds FrameRange; a second Read feeds Retime. The clip of each row
is found by hovering along it: the view shows the open-hand cursor over a TimeOffset or FrameRange
clip. The TimeOffset clip at offset 0 gives the frames-to-pixels scale, and the clip must then
follow a constant offset, an animated offset (each end sits at the output frame t with
t - offset(t) = that end of the Read's clip) and FrameRange's first and last frames. Dragging a
clip must move a constant offset by a whole number of frames, every key of an animated offset by
the same number of frames in time and value, and FrameRange's first and last frames together.
Dragging either end of the FrameRange clip moves only its first or only its last frame, and undo
puts each back.

A Transform keyed at frame 10 feeds a second TimeOffset of +5. With the viewer on the Transform
the key sits at 10; with the viewer on the TimeOffset it is drawn at 15, and dragging it to 17
moves the key to 12.

Screenshots of the dope sheet go to $NATRON_GUI_TEST_OUT as m124_dope_*.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron/.worktrees/m124 && \
        NATRON_GUI_TEST_OUT=build/m124-dope/out Tests/gui/run-gui-test.sh \
        Tests/gui/m124_dope_sheet_time.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 240
SETTLE_MS = 400
READ_ID = "fr.inria.built-in.Read"
TIMEOFFSET_ID = "TimeOffset"
FRAMERANGE_ID = "FrameRange"
RETIME_ID = "Retime"
TRANSFORM_ID = "net.sf.openfx.TransformPlugin"
SEQUENCE = "flat-seq-layers.####.exr"
READ_LAST_FRAME = 30
CONSTANT_OFFSET = 10
ANIMATED_KEYS = ((1.0, 0.0), (40.0, 20.0))
FRAME_RANGE = (12, 33)
FIT_RANGE = (-5, 60)
MAX_ZOOM_OUT_STEPS = 40
DRAG_FRAMES = 5
MIN_PIXELS_PER_FRAME = 3.0
TRIM_FIRST_FRAMES = 3
TRIM_LAST_FRAMES = 4
KEY_FRAME = 10
KEY_SHIFT = 5
KEY_DRAG_TO = 17

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent, QWheelEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- plumbing --------------------------------------------------------------------------

    def opened_props():
        # A node joins the dope sheet when its settings panel is created.
        return {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}

    def create(plugin_id, x, y):
        node = app.createNode(plugin_id, -1, None, opened_props())
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def make_dope_sheet_current():
        for pane_name in ("pane3", "pane2", "pane1", "pane4"):
            tw = app.getTabWidget(pane_name)
            if tw is None:
                continue
            for i in range(tw.count()):
                if "dope" in tw.getTabLabel(i).lower():
                    tw.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def dope_view():
        views = [v for v in gt.widgets_of("DopeSheetView") if v.isVisible()]
        return views[0] if views else None

    def hierarchy():
        views = [v for v in gt.widgets_of("HierarchyView") if v.isVisible()]
        return views[0] if views else None

    def tree_rows():
        """(label, QModelIndex, hidden) for every row of the dope sheet tree, depth first."""
        tree = hierarchy()
        if tree is None:
            return []
        model = tree.model()
        out = []

        def walk(parent):
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                out.append((idx.data(), idx, tree.isRowHidden(r, parent)))
                walk(idx)

        walk(tree.rootIndex())
        return out

    def row_index(label):
        for text, idx, _hidden in tree_rows():
            if text == label:
                return idx
        return None

    def row_center_y(label):
        tree, idx = hierarchy(), row_index(label)
        if tree is None or idx is None:
            return None
        rect = tree.visualRect(idx)
        return rect.center().y() if rect.isValid() and rect.height() > 0 else None

    def mouse(view, kind, x, y, button, buttons):
        QApplication.sendEvent(view, QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def zoom_out(view):
        pos = QPointF(view.width() / 2.0, view.height() / 2.0)
        QApplication.sendEvent(view, QWheelEvent(pos, QPointF(view.mapToGlobal(pos.toPoint())), QPoint(0, 0),
                                                 QPoint(0, -120), Qt.NoButton, Qt.NoModifier,
                                                 Qt.NoScrollPhase, False))
        QApplication.processEvents()

    def clip_span(label):
        """Pixel extent [left, right] of the open-hand region on label's row, or None."""
        view, y = dope_view(), row_center_y(label)
        if view is None or y is None:
            return None
        xs = []
        for x in range(0, view.width()):
            mouse(view, QEvent.MouseMove, x, y, Qt.NoButton, Qt.NoButton)
            if view.cursor().shape() == Qt.OpenHandCursor:
                xs.append(x)
        mouse(view, QEvent.MouseMove, 0, 0, Qt.NoButton, Qt.NoButton)
        return (min(xs), max(xs)) if xs else None

    def drag_clip(label, dx):
        span = clip_span(label)
        if span is None:
            return False
        view, y = dope_view(), row_center_y(label)
        x0 = (span[0] + span[1]) / 2.0
        win = view.window()
        win.activateWindow()
        mouse(view, QEvent.MouseMove, x0, y, Qt.NoButton, Qt.NoButton)
        mouse(view, QEvent.MouseButtonPress, x0, y, Qt.LeftButton, Qt.LeftButton)
        steps_n = 8
        for i in range(1, steps_n + 1):
            mouse(view, QEvent.MouseMove, x0 + dx * i / steps_n, y, Qt.NoButton, Qt.LeftButton)
        mouse(view, QEvent.MouseButtonRelease, x0 + dx, y, Qt.LeftButton, Qt.NoButton)
        return True

    def drag_from(x0, y, dx):
        view = dope_view()
        if view is None or y is None:
            return False
        win = view.window()
        win.activateWindow()
        mouse(view, QEvent.MouseMove, x0, y, Qt.NoButton, Qt.NoButton)
        mouse(view, QEvent.MouseButtonPress, x0, y, Qt.LeftButton, Qt.LeftButton)
        steps_n = 8
        for i in range(1, steps_n + 1):
            mouse(view, QEvent.MouseMove, x0 + dx * i / steps_n, y, Qt.NoButton, Qt.LeftButton)
        mouse(view, QEvent.MouseButtonRelease, x0 + dx, y, Qt.LeftButton, Qt.NoButton)
        return True

    def child_row_y(parent_label, child_label):
        """Centre y of the row child_label below the row parent_label, scrolled into view."""
        tree, parent = hierarchy(), row_index(parent_label)
        if tree is None or parent is None:
            return None
        model = tree.model()
        for r in range(model.rowCount(parent)):
            idx = model.index(r, 0, parent)
            if idx.data() == child_label:
                tree.scrollTo(idx)
                QApplication.processEvents()
                rect = tree.visualRect(idx)
                return rect.center().y() if rect.isValid() and rect.height() > 0 else None
        return None

    def cursor_at(x, y):
        view = dope_view()
        mouse(view, QEvent.MouseMove, x, y, Qt.NoButton, Qt.NoButton)
        shape = view.cursor().shape()
        mouse(view, QEvent.MouseMove, 0, 0, Qt.NoButton, Qt.NoButton)
        return shape

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def trigger_undo():
        win = main_window()
        if win is None or win.menuBar() is None:
            return False
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith("undo") and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        return False

    def clear_dope_selection():
        # A press on empty space below the rows clears the selection, so a selected clip's box no
        # longer captures the next press or hover.
        view = dope_view()
        x, y = 4, view.height() - 30
        mouse(view, QEvent.MouseButtonPress, x, y, Qt.LeftButton, Qt.LeftButton)
        mouse(view, QEvent.MouseButtonRelease, x, y, Qt.LeftButton, Qt.NoButton)

    def crop_shot(name):
        view = dope_view()
        tree = hierarchy()
        target = view.parentWidget() if view is not None and view.parentWidget() is not None else view
        if target is None or not target.isVisible():
            report("NOTE %s: the dope sheet is not visible" % name)
            return
        QApplication.processEvents()
        tl = target.mapToGlobal(QPoint(0, 0))
        rect = QRect(tl.x(), tl.y(), target.width(), target.height())
        if tree is not None:
            ttl = tree.mapToGlobal(QPoint(0, 0))
            rect = rect.united(QRect(ttl.x(), ttl.y(), tree.width(), tree.height()))
        screen = QGuiApplication.primaryScreen()
        pix = screen.grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height())
        path = os.path.join(gt.OUT, name + ".png")
        if pix.save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def output_time(param, frame):
        # Mirrors the engine: the t with t - offset(t) = frame, by fixed-point iteration.
        t = frame + param.getValueAtTime(frame, 0)
        for _ in range(64):
            nxt = frame + param.getValueAtTime(t, 0)
            if abs(nxt - t) < 1e-9:
                return nxt
            t = nxt
        return t

    def keys_of(param):
        out = []
        for k in range(param.getNumKeys(0)):
            found, time = param.getKeyTime(k, 0)
            if found:
                out.append((time, param.getValueAtTime(time, 0)))
        return out

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        path = os.path.join(gt.FIXTURES, SEQUENCE)
        read = app.createReader(path, None, opened_props())
        require(read is not None, "create a Read for " + SEQUENCE)
        require(read.getPluginID() == READ_ID, "the Read is the native Read (%s)" % read.getPluginID())
        read.setPosition(0, 0)
        read.getParam("lastFrame").set(READ_LAST_FRAME)
        start = read.getParam("startingTime").get()
        first = read.getParam("firstFrame").get()
        last = read.getParam("lastFrame").get()
        length = last - first + 1
        report("NOTE Read: startingTime %d, firstFrame %d, lastFrame %d" % (start, first, last))
        require(length >= 2, "the Read spans at least two frames")

        offset_node = create(TIMEOFFSET_ID, 0, 150)
        require(offset_node.connectInput(0, read), "connect the Read to TimeOffset")
        range_node = create(FRAMERANGE_ID, 0, 300)
        require(range_node.connectInput(0, offset_node), "connect TimeOffset to FrameRange")
        read2 = app.createReader(path, None, opened_props())
        require(read2 is not None, "create a second Read")
        read2.setPosition(300, 0)
        retime_node = create(RETIME_ID, 300, 150)
        require(retime_node.connectInput(0, read2), "connect the second Read to Retime")
        retime_node.getParam("speed").set(2.0)

        offset = offset_node.getParam("timeOffset")
        require(offset is not None and hasattr(offset, "setValueAtTime"), "TimeOffset has an animatable timeOffset")
        range_first = range_node.getParam("firstFrame")
        range_last = range_node.getParam("lastFrame")
        require(range_first is not None and range_last is not None, "FrameRange has firstFrame and lastFrame")

        require(make_dope_sheet_current(), "the dope sheet tab exists")
        ok = yield ("until", lambda: dope_view() is not None and hierarchy() is not None, POLL_TIMEOUT_S)
        require(ok, "the dope sheet is shown")
        labels = [n.getLabel() for n in (read, offset_node, range_node, read2, retime_node)]
        ok = yield ("until", lambda: all(row_index(l) is not None for l in labels), POLL_TIMEOUT_S)
        require(ok, "the dope sheet has a row for each node",
                "missing %r; rows %r" % ([l for l in labels if row_index(l) is None],
                                         [(t, h) for t, _i, h in tree_rows()]))
        hierarchy().expandAll()
        # A wide FrameRange clip stands in for every range measured below: the view is framed and
        # then zoomed out around its centre until that clip lies inside it, so all the measurements
        # share one fixed scale.
        range_defaults = (range_first.get(), range_last.get())
        range_first.set(FIT_RANGE[0])
        range_last.set(FIT_RANGE[1])
        yield ("sleep", SETTLE_MS)
        QMetaObject.invokeMethod(dope_view(), "centerOnSelection")
        yield ("sleep", SETTLE_MS)
        fits = False
        for _ in range(MAX_ZOOM_OUT_STEPS):
            span, width = clip_span(range_node.getLabel()), dope_view().width()
            if span is not None and span[0] > 0 and span[1] < width - 1:
                fits = True
                break
            zoom_out(dope_view())
            yield ("sleep", 50)
        require(fits, "the dope sheet zooms out to hold frames %d to %d" % FIT_RANGE)
        range_first.set(range_defaults[0])
        range_last.set(range_defaults[1])
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: all(row_center_y(l) is not None for l in labels), POLL_TIMEOUT_S)
        require(ok, "every row is laid out")

        # ---- scale from the TimeOffset clip at offset 0.
        gt.section("ranges")
        offset.set(0.0)
        yield ("sleep", SETTLE_MS)
        base = clip_span(offset_node.getLabel())
        require(base is not None, "the TimeOffset row has a clip at offset 0")
        ppf = (base[1] - base[0] + 1) / float(length)
        report("NOTE TimeOffset clip at offset 0 spans pixels %r, %.2f pixels per frame" % (base, ppf))
        require(ppf >= MIN_PIXELS_PER_FRAME, "the dope sheet is zoomed in enough to measure clips",
                "%.2f pixels per frame" % ppf)
        tol = max(2.0, 0.3 * ppf)
        crop_shot("m124_dope_offset0")

        def pixel_of(frame):
            return base[0] + (frame - start) * ppf

        def expect_span(label, frames, what):
            span = clip_span(label)
            if not check(span is not None, "%s: the clip is drawn" % what):
                return
            want = (pixel_of(frames[0]), pixel_of(frames[1]) - 1)
            check(abs(span[0] - want[0]) <= tol and abs(span[1] - want[1]) <= tol,
                  "%s: the clip spans frames %.2f to %.2f (pixels %r, expected %.1f to %.1f)"
                  % (what, frames[0], frames[1], span, want[0], want[1]))

        offset.set(float(CONSTANT_OFFSET))
        yield ("sleep", SETTLE_MS)
        expect_span(offset_node.getLabel(), (start + CONSTANT_OFFSET, start + length + CONSTANT_OFFSET),
                    "constant offset %d" % CONSTANT_OFFSET)
        crop_shot("m124_dope_offset_constant")

        for time, value in ANIMATED_KEYS:
            offset.setValueAtTime(value, time, 0)
        yield ("sleep", SETTLE_MS)
        check(offset.getNumKeys(0) == len(ANIMATED_KEYS), "the offset has %d keys" % len(ANIMATED_KEYS))
        animated = (output_time(offset, start), output_time(offset, start + length))
        report("NOTE animated offset: clip expected from %.3f to %.3f" % animated)
        expect_span(offset_node.getLabel(), animated, "animated offset")
        crop_shot("m124_dope_offset_animated")

        range_first.set(FRAME_RANGE[0])
        range_last.set(FRAME_RANGE[1])
        yield ("sleep", SETTLE_MS)
        expect_span(range_node.getLabel(), (FRAME_RANGE[0], FRAME_RANGE[1] + 1),
                    "FrameRange %d-%d" % FRAME_RANGE)
        crop_shot("m124_dope_framerange")

        # ---- drags.
        gt.section("drags")
        dx = DRAG_FRAMES * ppf

        keys_before = keys_of(offset)
        require(drag_clip(offset_node.getLabel(), dx), "dragged the animated TimeOffset clip")
        yield ("sleep", SETTLE_MS)
        keys_after = keys_of(offset)
        report("NOTE animated offset keys %r -> %r" % (keys_before, keys_after))
        if check(len(keys_after) == len(keys_before), "the drag kept the offset's key count"):
            shifts = [(a[0] - b[0], a[1] - b[1]) for a, b in zip(keys_after, keys_before)]
            d = shifts[0][0]
            check(d != 0 and abs(d - round(d)) < 1e-9 and abs(d - DRAG_FRAMES) <= 1,
                  "the drag moved the keys by a whole number of frames near %d (%r)" % (DRAG_FRAMES, d))
            check(all(abs(s[0] - d) < 1e-9 and abs(s[1] - d) < 1e-9 for s in shifts),
                  "every key moved by the same frames in time and value (%r)" % (shifts,))
        crop_shot("m124_dope_offset_animated_dragged")
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        offset.removeAnimation(0)
        offset.set(float(CONSTANT_OFFSET))
        yield ("sleep", SETTLE_MS)
        require(drag_clip(offset_node.getLabel(), dx), "dragged the constant TimeOffset clip")
        yield ("sleep", SETTLE_MS)
        moved = offset.get() - CONSTANT_OFFSET
        check(offset.getNumKeys(0) == 0, "the drag did not key the constant offset")
        check(moved != 0 and abs(moved - round(moved)) < 1e-9 and abs(moved - DRAG_FRAMES) <= 1,
              "the drag moved the constant offset by a whole number of frames near %d (%r)" % (DRAG_FRAMES, moved))
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        f0, l0 = range_first.get(), range_last.get()
        require(drag_clip(range_node.getLabel(), -dx), "dragged the FrameRange clip")
        yield ("sleep", SETTLE_MS)
        df, dl = range_first.get() - f0, range_last.get() - l0
        check(df == dl and df != 0 and abs(df + DRAG_FRAMES) <= 1,
              "the drag moved firstFrame and lastFrame together by about -%d (%d, %d)" % (DRAG_FRAMES, df, dl))
        crop_shot("m124_dope_framerange_dragged")
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        # ---- FrameRange end handles: just outside each end of the clip.
        gt.section("framerange ends")
        f0, l0 = range_first.get(), range_last.get()
        range_y = row_center_y(range_node.getLabel())
        left_x = pixel_of(f0) - 1
        right_x = pixel_of(l0 + 1) + 1
        check(cursor_at(left_x, range_y) == Qt.SplitHCursor, "the left end of the FrameRange clip shows the resize cursor")
        check(cursor_at(right_x, range_y) == Qt.SplitHCursor, "the right end of the FrameRange clip shows the resize cursor")
        check(cursor_at(pixel_of((f0 + l0 + 1) / 2.0), range_y) == Qt.OpenHandCursor,
              "the middle of the FrameRange clip still shows the move cursor")

        require(drag_from(left_x, range_y, TRIM_FIRST_FRAMES * ppf), "dragged the left end")
        yield ("sleep", SETTLE_MS)
        f1, l1 = range_first.get(), range_last.get()
        check(abs(f1 - f0 - TRIM_FIRST_FRAMES) <= 1 and f1 != f0 and l1 == l0,
              "the left end moved only firstFrame, by about %d (%d -> %d, lastFrame %d -> %d)"
              % (TRIM_FIRST_FRAMES, f0, f1, l0, l1))
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        right_x = pixel_of(l1 + 1) + 1
        require(drag_from(right_x, row_center_y(range_node.getLabel()), TRIM_LAST_FRAMES * ppf), "dragged the right end")
        yield ("sleep", SETTLE_MS)
        f2, l2 = range_first.get(), range_last.get()
        check(abs(l2 - l1 - TRIM_LAST_FRAMES) <= 1 and l2 != l1 and f2 == f1,
              "the right end moved only lastFrame, by about %d (%d -> %d, firstFrame %d -> %d)"
              % (TRIM_LAST_FRAMES, l1, l2, f1, f2))
        crop_shot("m124_dope_framerange_ends")
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        check(trigger_undo(), "undo the right-end drag")
        yield ("sleep", SETTLE_MS)
        check((range_first.get(), range_last.get()) == (f1, l1),
              "undo restores lastFrame in one step (%r)" % ((range_first.get(), range_last.get()),))
        check(trigger_undo(), "undo the left-end drag")
        yield ("sleep", SETTLE_MS)
        check((range_first.get(), range_last.get()) == (f0, l0),
              "undo restores firstFrame in one step (%r)" % ((range_first.get(), range_last.get()),))

        # ---- keys upstream of a TimeOffset follow the viewer.
        gt.section("keys through a TimeOffset")
        transform = create(TRANSFORM_ID, 600, 0)
        shift_node = create(TIMEOFFSET_ID, 600, 150)
        require(shift_node.connectInput(0, transform), "connect the Transform to the second TimeOffset")
        shift_node.getParam("timeOffset").set(float(KEY_SHIFT))
        rotate = transform.getParam("rotate")
        require(rotate is not None, "the Transform has a rotate parameter")
        rotate.setValueAtTime(30.0, KEY_FRAME, 0)
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, transform), "the Viewer shows the Transform")
        ok = yield ("until", lambda: row_index(transform.getLabel()) is not None, POLL_TIMEOUT_S)
        require(ok, "the dope sheet has a row for the Transform")
        hierarchy().expandAll()
        yield ("sleep", SETTLE_MS)
        rotate_label = rotate.getLabel()
        key_y = child_row_y(transform.getLabel(), rotate_label)
        require(key_y is not None, "the Transform's %s row is laid out" % rotate_label,
                "rows %r" % ([(t, h) for t, _i, h in tree_rows()],))
        dope_view().update()
        yield ("sleep", SETTLE_MS)
        check(cursor_at(pixel_of(KEY_FRAME), key_y) == Qt.CrossCursor,
              "viewing the Transform, its key is at frame %d" % KEY_FRAME)

        viewer.disconnectInput(0)
        require(viewer.connectInput(0, shift_node), "the Viewer shows the TimeOffset")
        yield ("sleep", SETTLE_MS)
        key_y = child_row_y(transform.getLabel(), rotate_label)
        check(cursor_at(pixel_of(KEY_FRAME + KEY_SHIFT), key_y) == Qt.CrossCursor,
              "viewing the TimeOffset, the key is drawn at frame %d" % (KEY_FRAME + KEY_SHIFT))
        check(cursor_at(pixel_of(KEY_FRAME), key_y) != Qt.CrossCursor,
              "viewing the TimeOffset, nothing is drawn at frame %d" % KEY_FRAME)
        crop_shot("m124_dope_keys_shifted")

        require(drag_from(pixel_of(KEY_FRAME + KEY_SHIFT), key_y, (KEY_DRAG_TO - KEY_FRAME - KEY_SHIFT) * ppf),
                "dragged the shifted key")
        yield ("sleep", SETTLE_MS)
        keys = keys_of(rotate)
        report("NOTE rotate keys after the drag %r" % (keys,))
        check(len(keys) == 1 and abs(keys[0][0] - (KEY_DRAG_TO - KEY_SHIFT)) < 1e-9,
              "dragging the key to frame %d writes frame %d (%r)" % (KEY_DRAG_TO, KEY_DRAG_TO - KEY_SHIFT, keys))
        crop_shot("m124_dope_keys_dragged")
        clear_dope_selection()

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
