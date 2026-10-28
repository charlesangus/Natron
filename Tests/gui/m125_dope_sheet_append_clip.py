"""The dope sheet draws the clip of a native AppendClip, with its cross-dissolves, and draws keys
upstream of each AppendClip input at the frames the viewer shows them.

A Read of 20 frames feeds AppendClip input 1; a second Read of 30 frames feeds a Transform, which
feeds input 2. AppendClip starts at frame 5 with a 4-frame cross-dissolve, so input 1 plays at
frames 5 to 24 and input 2 at 21 to 50. A FrameRange on the first Read, set to frames -5 to 60,
gives the frames-to-pixels scale (its clip shows the open-hand cursor). The AppendClip row is
then read from a screen grab: the clip fills frames 5 to 50 and nothing outside, and the
cross-dissolve at frames 21 to 24 is drawn in its own colour. Changing the cross-dissolve and
disconnecting input 2 must redraw the clip.

The Transform is keyed at frame 10. With the viewer on the Transform the key sits at 10; with the
viewer on AppendClip it is drawn at 30, where input 2 plays its frame 10.

Screenshots of the dope sheet go to $NATRON_GUI_TEST_OUT as m125_dope_*.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron/.worktrees/m125 && \
        NATRON_GUI_TEST_OUT=build/m125-dope/out Tests/gui/run-gui-test.sh \
        Tests/gui/m125_dope_sheet_append_clip.py <AppImage or build/fast/App/Natron>'
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
APPENDCLIP_ID = "AppendClip"
FRAMERANGE_ID = "FrameRange"
TRANSFORM_ID = "net.sf.openfx.TransformPlugin"
SEQUENCE = "flat-seq-layers.####.exr"
READ1_LAST_FRAME = 20
READ2_LAST_FRAME = 30
APPEND_FIRST_FRAME = 5
DISSOLVE = 4
FIT_RANGE = (-5, 60)
MAX_ZOOM_OUT_STEPS = 40
MIN_PIXELS_PER_FRAME = 6.0
COLOUR_TOL = 10
KEY_FRAME = 10

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

    def grab_row(label):
        """A function frame-pixel -> QColor sampling label's row at its centre, from one screen grab."""
        view, y = dope_view(), row_center_y(label)
        if view is None or y is None:
            return None
        dope_view().update()
        QApplication.processEvents()
        origin = view.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), view.width(), view.height()).toImage()
        s = img.width() / float(max(1, view.width()))

        def colour(x):
            xd, yd = int(round(x * s)), int(round(y * s))
            if 0 <= xd < img.width() and 0 <= yd < img.height():
                c = img.pixelColor(xd, yd)
                return (c.red(), c.green(), c.blue())
            return None

        return colour

    def same(a, b):
        return a is not None and b is not None and all(abs(p - q) <= COLOUR_TOL for p, q in zip(a, b))

    def read_range(read):
        start = read.getParam("startingTime").get()
        length = read.getParam("lastFrame").get() - read.getParam("firstFrame").get() + 1
        return start, length

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        path = os.path.join(gt.FIXTURES, SEQUENCE)
        read1 = app.createReader(path, None, opened_props())
        require(read1 is not None, "create a Read for " + SEQUENCE)
        require(read1.getPluginID() == READ_ID, "the Read is the native Read (%s)" % read1.getPluginID())
        read1.setPosition(0, 0)
        read1.getParam("lastFrame").set(READ1_LAST_FRAME)
        read2 = app.createReader(path, None, opened_props())
        require(read2 is not None, "create a second Read")
        read2.setPosition(300, 0)
        read2.getParam("lastFrame").set(READ2_LAST_FRAME)
        start1, length1 = read_range(read1)
        start2, length2 = read_range(read2)
        report("NOTE Reads: input 1 starts at %d for %d frames, input 2 at %d for %d frames"
               % (start1, length1, start2, length2))

        transform = create(TRANSFORM_ID, 300, 150)
        require(transform.connectInput(0, read2), "connect the second Read to the Transform")
        rotate = transform.getParam("rotate")
        require(rotate is not None, "the Transform has a rotate parameter")
        rotate.setValueAtTime(30.0, KEY_FRAME, 0)

        append = create(APPENDCLIP_ID, 150, 300)
        require(append.connectInput(0, read1), "connect the first Read to AppendClip input 1")
        require(append.connectInput(1, transform), "connect the Transform to AppendClip input 2")
        first_frame = append.getParam("firstFrame")
        dissolve = append.getParam("crossDissolve")
        require(first_frame is not None and dissolve is not None, "AppendClip has firstFrame and crossDissolve")
        first_frame.set(APPEND_FIRST_FRAME)
        dissolve.set(DISSOLVE)

        range_node = create(FRAMERANGE_ID, 0, 150)
        require(range_node.connectInput(0, read1), "connect the first Read to FrameRange")
        range_node.getParam("firstFrame").set(FIT_RANGE[0])
        range_node.getParam("lastFrame").set(FIT_RANGE[1])

        require(make_dope_sheet_current(), "the dope sheet tab exists")
        ok = yield ("until", lambda: dope_view() is not None and hierarchy() is not None, POLL_TIMEOUT_S)
        require(ok, "the dope sheet is shown")
        labels = [n.getLabel() for n in (read1, read2, transform, append, range_node)]
        ok = yield ("until", lambda: all(row_index(l) is not None for l in labels), POLL_TIMEOUT_S)
        require(ok, "the dope sheet has a row for each node",
                "missing %r; rows %r" % ([l for l in labels if row_index(l) is None],
                                         [(t, h) for t, _i, h in tree_rows()]))
        hierarchy().expandAll()
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
        ok = yield ("until", lambda: all(row_center_y(l) is not None for l in labels), POLL_TIMEOUT_S)
        require(ok, "every row is laid out")
        clear_dope_selection()
        yield ("sleep", SETTLE_MS)

        base = clip_span(range_node.getLabel())
        require(base is not None, "the FrameRange row has a clip")
        ppf = (base[1] - base[0] + 1) / float(FIT_RANGE[1] + 1 - FIT_RANGE[0])
        report("NOTE FrameRange clip %d-%d spans pixels %r, %.2f pixels per frame" % (FIT_RANGE + (base, ppf)))
        require(ppf >= MIN_PIXELS_PER_FRAME, "the dope sheet is zoomed in enough to sample half frames",
                "%.2f pixels per frame" % ppf)

        def pixel_of(frame):
            return base[0] + (frame - FIT_RANGE[0]) * ppf

        # ---- the clip and its cross-dissolve. Samples sit on half frames, away from the lines the
        # view draws at whole frames (the current frame, the project bounds, each input's ends).
        append_label = append.getLabel()

        def expect_clip(first, last, overlap, what):
            colour = grab_row(append_label)
            if not check(colour is not None, "%s: the AppendClip row is laid out" % what):
                return
            fill = colour(pixel_of(first + 2.5))
            outside = colour(pixel_of(first - 0.5))
            report("NOTE %s: fill %r, outside %r" % (what, fill, outside))
            check(not same(fill, outside), "%s: the clip is drawn" % what)
            check(same(colour(pixel_of(first + 0.5)), fill), "%s: the clip starts at frame %d" % (what, first))
            check(same(colour(pixel_of(last + 0.5)), fill), "%s: the clip reaches frame %d" % (what, last))
            check(not same(colour(pixel_of(last + 1.5)), fill), "%s: the clip ends after frame %d" % (what, last))
            if overlap is not None:
                mid = colour(pixel_of(overlap[0] + 1.5))
                check(not same(mid, fill) and not same(mid, outside),
                      "%s: the cross-dissolve at frames %d to %d has its own colour (%r)" % (what, overlap[0], overlap[1], mid))
                check(same(colour(pixel_of(overlap[0] - 1.5)), fill) and same(colour(pixel_of(overlap[1] + 1.5)), fill),
                      "%s: the clip is plain on either side of the cross-dissolve" % what)

        gt.section("clip")
        c1_last = APPEND_FIRST_FRAME + length1 - 1
        c2_first = c1_last + 1 - DISSOLVE
        c2_last = c2_first + length2 - 1
        expect_clip(APPEND_FIRST_FRAME, c2_last, (c2_first, c1_last), "a %d-frame cross-dissolve" % DISSOLVE)
        crop_shot("m125_dope_append_clip")

        # ---- keys above input 2 follow where AppendClip plays it.
        gt.section("keys through AppendClip")
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, transform), "the Viewer shows the Transform")
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
        require(viewer.connectInput(0, append), "the Viewer shows AppendClip")
        yield ("sleep", SETTLE_MS)
        dope_view().update()
        yield ("sleep", SETTLE_MS)
        shifted = KEY_FRAME + c2_first - start2
        key_y = child_row_y(transform.getLabel(), rotate_label)
        check(cursor_at(pixel_of(shifted), key_y) == Qt.CrossCursor,
              "viewing AppendClip, the key is drawn at frame %d" % shifted)
        check(cursor_at(pixel_of(KEY_FRAME), key_y) != Qt.CrossCursor,
              "viewing AppendClip, nothing is drawn at frame %d" % KEY_FRAME)
        crop_shot("m125_dope_append_keys")

        # ---- the clip follows the cross-dissolve and the inputs.
        gt.section("redraw")
        dissolve.set(0)
        yield ("sleep", SETTLE_MS)
        expect_clip(APPEND_FIRST_FRAME, c1_last + length2, None, "no cross-dissolve")
        colour = grab_row(append_label)
        check(colour is not None and same(colour(pixel_of(c1_last + 1.5)), colour(pixel_of(c1_last - 1.5))),
              "no cross-dissolve: nothing is drawn in a cross-dissolve colour")
        crop_shot("m125_dope_append_no_dissolve")

        append.disconnectInput(1)
        yield ("sleep", SETTLE_MS)
        expect_clip(APPEND_FIRST_FRAME, c1_last, None, "input 2 disconnected")
        crop_shot("m125_dope_append_one_input")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
