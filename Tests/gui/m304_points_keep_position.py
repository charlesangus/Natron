"""Roto points keep their positions when a shape is keyed while the viewer renders.

A Roto node holds an eight-point shape keyed at frames 1, 11 and 21, with a wide feather and
motion blur so each viewer render takes a while. Each step makes its edit just after a render
starts, so the edit lands while the node renders:
  1. Insert: at the unkeyed frame 6, a Ctrl+Alt click on the first segment inserts a point. The
     new point sits under the click, never at 0,0, and every other point keeps its position at
     frames 1, 6, 11 and 21.
  2. Add Key: at frame 16, a script moves point 3 at frame 21, then Add Key keys frame 16. The
     key holds the pose shown before it was pressed, and point 3 keeps its move at frame 21.
  3. Dope sheet: a script moves point 4 at frame 11, then the frame-16 key is dragged to frame
     18 on the shape's row. Frame 18 holds the pose frame 16 had, and point 4 keeps its move.
Saves a screenshot after each step.

Render timing decides whether an edit overlaps a render, so a pass here can miss the bug; the
engine tests in RotoKeyDuringRender_Test are the proof.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m304-shots/out -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m304_points_keep_position.py build/fast/App/Natron
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 2500
# Long enough for the render a seek or an edit starts to be running, short enough to land in it.
RENDER_START_MS = 40
CENTRE_X, CENTRE_Y, RADIUS = 960.0, 540.0, 300.0
POINTS = 8
KEYS = (1.0, 11.0, 21.0)
KEY_SHIFT_X = 20.0
FEATHER = 250.0
MOTION_BLUR_SAMPLES = 8
EDIT = (5.0, -4.0)
TOL = 1e-6

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QWidget

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _keep = {}

    def display_alpha():
        """Roto writes only the alpha channel by default, which the RGB display shows black."""
        for w in gt.widgets_of("ComboBox") + gt.widgets_of("ChannelsComboBox"):
            try:
                if not (w.isVisible() and "Display Channels:" in (w.toolTip() or "")):
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
            except RuntimeError:
                continue
            if "Alpha" in items:
                QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index("Alpha")))
                QApplication.processEvents()
                return True
        return False

    def seek(frame):
        _keep["pyviewer"].seek(int(frame))
        QApplication.processEvents()

    def octagon(t):
        shift = KEY_SHIFT_X * KEYS.index(t)
        return [(CENTRE_X + shift + RADIUS * math.cos(2.0 * math.pi * i / POINTS),
                 CENTRE_Y + RADIUS * math.sin(2.0 * math.pi * i / POINTS)) for i in range(POINTS)]

    def make_shape(ctx):
        first = octagon(KEYS[0])
        bez = ctx.createBezier(first[0][0], first[0][1], KEYS[0])
        for x, y in first[1:]:
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        for t in KEYS[1:]:
            for i, (x, y) in enumerate(octagon(t)):
                bez.setPointAtIndex(i, t, x, y, x, y, x, y)
        bez.setFeatherDistance(FEATHER, KEYS[0])
        blur = bez.getParam("motionBlur")
        if blur is not None:
            blur.setValue(MOTION_BLUR_SAMPLES)
        else:
            report("NOTE the shape has no motionBlur param; renders are slowed by the feather only")
        return bez

    def pose(bez, t):
        return [tuple(rg.point(bez, i, t)) for i in range(bez.getNumControlPoints())]

    def far_from_origin(p):
        return math.hypot(p[0], p[1]) > 1.0

    def same(a, b):
        return rg.close(a, b, TOL)

    # ---- dope sheet -------------------------------------------------------------------------

    def dope_view():
        views = [v for v in gt.widgets_of("DopeSheetView") if v.isVisible()]
        return views[0] if views else None

    def hierarchy():
        trees = gt.widgets_of("HierarchyView")
        return trees[0] if trees else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def show_dope_sheet():
        views = gt.widgets_of("DopeSheetView")
        if not views:
            return False
        _keep["dope"] = views[0]
        pane = ancestor_of_class(views[0], "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if "dope" in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def shape_row_y(shape_label):
        """Centre y of the "Shape" row under the shape's item row, scrolled into view."""
        tree = hierarchy()
        if tree is None:
            return None
        _keep["tree"] = tree
        model = tree.model()

        def find(parent):
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                if idx.data() == shape_label:
                    for c in range(model.rowCount(idx)):
                        kid = model.index(c, 0, idx)
                        if kid.data() == "Shape":
                            return kid
                found = find(idx)
                if found is not None:
                    return found
            return None
        idx = find(tree.rootIndex())
        if idx is None:
            return None
        tree.scrollTo(idx)
        QApplication.processEvents()
        rect = tree.visualRect(idx)
        return rect.center().y() if rect.isValid() and rect.height() > 0 else None

    def mouse(view, kind, x, y, button, buttons):
        QApplication.sendEvent(view, QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def key_xs(y):
        """Centres of the runs of view pixels on row y where the cursor shows a key."""
        view = dope_view()
        xs, run = [], []
        for x in range(0, view.width()):
            mouse(view, QEvent.MouseMove, x, y, Qt.NoButton, Qt.NoButton)
            if view.cursor().shape() == Qt.CrossCursor:
                run.append(x)
            elif run:
                xs.append((run[0] + run[-1]) / 2.0)
                run = []
        if run:
            xs.append((run[0] + run[-1]) / 2.0)
        mouse(view, QEvent.MouseMove, 0, 0, Qt.NoButton, Qt.NoButton)
        return xs

    def drag_key(x0, y, dx):
        view = dope_view()
        view.window().activateWindow()
        mouse(view, QEvent.MouseMove, x0, y, Qt.NoButton, Qt.NoButton)
        mouse(view, QEvent.MouseButtonPress, x0, y, Qt.LeftButton, Qt.LeftButton)
        n = 8
        for i in range(1, n + 1):
            mouse(view, QEvent.MouseMove, x0 + dx * i / n, y, Qt.NoButton, Qt.LeftButton)
        mouse(view, QEvent.MouseButtonRelease, x0 + dx, y, Qt.LeftButton, Qt.NoButton)

    # ---- steps ------------------------------------------------------------------------------

    def insert_step(node, bez, gl, vmap):
        gt.section("insert a point at an unkeyed frame while rendering")
        frames = (1.0, 6.0, 11.0, 21.0)
        before = dict((t, pose(bez, t)) for t in frames)
        p0, p1 = before[6.0][0], before[6.0][1]
        target = ((p0[0] + p1[0]) / 2.0, (p0[1] + p1[1]) / 2.0)
        rg.select_tool(gl)
        seek(6)
        yield ("sleep", RENDER_START_MS)
        pos = vmap.to_widget(target[0], target[1])
        rg.with_modifiers(gl, Qt.ControlModifier | Qt.AltModifier, lambda m: rg.click(gl, pos, m))
        yield ("sleep", RENDER_MS)

        require(bez.getNumControlPoints() == POINTS + 1, "the Ctrl+Alt click inserted one point (%d points)" % bez.getNumControlPoints())
        tol = 4.0 * vmap.pixel_scale()
        inserted = rg.point(bez, 1, 6.0)
        check(far_from_origin(inserted), "the inserted point is not at 0,0 at frame 6: %r" % (inserted,))
        check(math.hypot(inserted[0] - target[0], inserted[1] - target[1]) <= tol,
              "the inserted point is under the click: %r, clicked %r" % (inserted[:2], target))
        check(6.0 in bez.getKeyframes(), "the insert auto-keyed frame 6: %r" % (bez.getKeyframes(),))
        for t in frames:
            after = pose(bez, t)
            check(far_from_origin(after[1]), "the inserted point is not at 0,0 at frame %g: %r" % (t, after[1]))
            moved = [i for i in range(POINTS) if not same(after[0 if i == 0 else i + 1], before[t][i])]
            check(not moved, "the other points keep their positions at frame %g (moved: %r)" % (t, moved))
        report("NOTE caption m304-insert.png: frame 6 after inserting a point on the first segment while rendering")
        gt.shot("m304-insert.png")

    def add_key_step(node, bez):
        gt.section("Add Key while rendering")
        seek(16)
        yield ("sleep", RENDER_MS)
        select_all = node.getParam("selectAllAction")
        require(select_all is not None, "the Roto node has the select-all action")
        select_all.trigger()
        add_key = node.getParam("addKeyframeButton")
        require(add_key is not None, "the Roto node has the Add Key button")
        edited_before = rg.point(bez, 3, 21.0)
        bez.movePointByIndex(3, 21.0, EDIT[0], EDIT[1])
        expected_21 = tuple(rg.point(bez, 3, 21.0))
        expected_16 = pose(bez, 16.0)
        check(not same(expected_21, edited_before), "the script moved point 3 at frame 21")
        yield ("sleep", RENDER_START_MS)
        add_key.trigger()
        yield ("sleep", RENDER_MS)

        check(16.0 in bez.getKeyframes(), "Add Key keyed frame 16: %r" % (bez.getKeyframes(),))
        after = pose(bez, 16.0)
        moved = [i for i in range(len(after)) if not same(after[i], expected_16[i])]
        check(not moved, "the frame-16 key holds the pose shown before Add Key (moved: %r)" % (moved,))
        check(all(far_from_origin(p) for p in after), "no point is at 0,0 at frame 16")
        check(same(rg.point(bez, 3, 21.0), expected_21),
              "point 3 keeps its move at frame 21: %r, expected %r" % (rg.point(bez, 3, 21.0), expected_21))
        report("NOTE caption m304-add-key.png: frame 16 after Add Key pressed while rendering")
        gt.shot("m304-add-key.png")

    def dope_sheet_step(node, bez):
        gt.section("move a key in the dope sheet while rendering")
        require(show_dope_sheet(), "the dope sheet tab is current")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: shape_row_y(bez.getLabel()) is not None, 10.0)
        require(ok, "the dope sheet shows the shape's Shape row")
        y = shape_row_y(bez.getLabel())
        keys = sorted(bez.getKeyframes())
        xs = key_xs(y)
        report("NOTE shape keys %r drawn at x %r" % (keys, xs))
        require(len(xs) == len(keys) and len(xs) >= 2, "the dope sheet draws every shape key")
        ppf = (xs[-1] - xs[0]) / (keys[-1] - keys[0])
        require(ppf > 2.0, "the dope sheet shows frames %g to %g wide enough to drag a key (%.2f px per frame)" % (keys[0], keys[-1], ppf))
        x16 = xs[keys.index(16.0)]

        seek(18)
        yield ("sleep", RENDER_MS)
        bez.movePointByIndex(4, 11.0, EDIT[0], EDIT[1])
        expected_11 = tuple(rg.point(bez, 4, 11.0))
        expected_18 = pose(bez, 16.0)
        yield ("sleep", RENDER_START_MS)
        drag_key(x16, y, 2.0 * ppf)
        yield ("sleep", RENDER_MS)

        keys = bez.getKeyframes()
        check(18.0 in keys and 16.0 not in keys, "the key moved from frame 16 to 18: %r" % (keys,))
        after = pose(bez, 18.0)
        moved = [i for i in range(len(after)) if not same(after[i], expected_18[i])]
        check(not moved, "frame 18 holds the pose frame 16 had (moved: %r)" % (moved,))
        check(all(far_from_origin(p) for p in after), "no point is at 0,0 at frame 18")
        check(same(rg.point(bez, 4, 11.0), expected_11),
              "point 4 keeps its move at frame 11: %r, expected %r" % (rg.point(bez, 4, 11.0), expected_11))
        report("NOTE caption m304-dope-sheet.png: the shape's keys after dragging frame 16 to 18 while rendering")
        gt.shot("m304-dope-sheet.png")

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        _keep["pyviewer"] = app.getViewer(viewer.getScriptName())
        require(_keep["pyviewer"] is not None, "the viewer has a Python handle")

        gt.close_all_panels()
        rg.activate()
        node, ctx = rg.open_roto(app, "Roto")
        require(node is not None, "create Roto", gt.plugin_problem("Roto"))
        require(ctx is not None, "Roto has a roto context")
        _keep["node"] = node
        seek(1)
        # Built unviewed: a shape keyed during a render is the bug under test, not a baseline.
        viewer.disconnectInput(0)
        yield ("sleep", RENDER_MS)
        bez = make_shape(ctx)
        require(bez is not None, "created the keyed shape")
        _keep["bez"] = bez
        viewer.connectInput(0, node)
        check(all(t in bez.getKeyframes() for t in KEYS), "the shape is keyed at %r: %r" % (KEYS, bez.getKeyframes()))
        yield ("sleep", RENDER_MS)

        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        _keep["gl"] = gl
        rg.activate()
        rg.untone_view()
        require(display_alpha(), "the viewer displays the alpha channel")
        yield ("sleep", RENDER_MS)
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")

        for cmd in insert_step(node, bez, gl, vmap):
            yield cmd
        for cmd in add_key_step(node, bez):
            yield cmd
        for cmd in dope_sheet_step(node, bez):
            yield cmd
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
