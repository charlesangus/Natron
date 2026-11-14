"""GUI check that RotoPaint strokes show their painted frame as a timeline marker and a dope sheet key.

Scenario, run on a RotoPaint at the top level and again on one inside a Group: a Bezier keyed at
frames 1 and 20, then strokes painted with the mouse on frames 5, 5 and 12 (Multi-Stroke off, so each
pen stroke is its own item). After each step the script compares the timeline's markers, read through
TimeLineGui::keyframeMarkerTimes(), with the expected frames:
  - current: the last painted stroke's frame shows with nothing selected;
  - selection: one stroke, then several (two on frame 5 give one marker), the Bezier alone and with
    strokes; the Bezier's markers (1, 20) stay whenever it is selected;
  - lifetime: the Lifetime "Frame" knob on the item and on the node's proxy moves the marker, and every
    Lifetime mode keeps showing lifeTimeFrame;
  - delete: a selected stroke and the current stroke deleted from the panel, then undone;
  - item-knob: an item opacity keyed from Python with two strokes and a Bezier selected, and with a
    stroke selected through its layer, adds that frame, and removing the key drops it;
  - stroke-undo: a fourth stroke (frame 15) painted, its creation undone and redone;
  - panel: closing the panel leaves no stroke or Bezier marker, reopening restores the selected items'
    markers (the current stroke is not adopted again, so it does not come back);
  - dope-sheet (top level only): each stroke has a "Painted Frame" row with one key at its frame, a
    Bezier has none, and dragging a key leaves lifeTimeFrame and the key where they were.

Markers update one event-loop turn after a signal, so every comparison waits with "until".

Run through Tests/gui/run-gui-test.sh with a GL-capable setup, like m224_brush_strokes.py. Screenshots of
the timeline and the dope sheet, and results.txt, go to $NATRON_GUI_TEST_OUT (default
build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
POLL_TIMEOUT_S = 15.0
WATCHDOG_S = 600
SETTLE_MS = 300
RENDER_MS = 1500
PEN_INTERVAL_MS = 8
SAMPLES = 10

LIFETIME_ALL = 0
LIFETIME_SINGLE = 1
LIFETIME_FROM_START = 2
LIFETIME_TO_END = 3
LIFETIME_CUSTOM = 4

BEZIER_KEYS = (1, 20)
STROKE_FRAMES = (5, 5, 12)
EXTRA_STROKE_FRAME = 15
ITEM_KEY_FRAME = 9
MULTI_KEY_FRAMES = (7, 10)
PROXY_FRAME = 20

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMetaObject, QPoint, QPointF, QRect, Q_ARG, Q_RETURN_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLineEdit, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Child wrappers die with the parent wrapper, so every widget used across a yield is held here.
    _held = []

    def hold(obj):
        _held.append(obj)
        return obj

    class State(object):
        """One RotoPaint under test."""

        def __init__(self, node, grouped):
            self.node = node
            self.ctx = node.getRotoContext()
            self.grouped = grouped
            self.current = None      # script name of the stroke this panel treats as current
            self.bezier = None       # script name of the Bezier
            self.panel = None        # the settings panel, held across a close

    # ---- timeline ----------------------------------------------------------------------------

    def timeline():
        visible = [w for w in gt.widgets_of("TimeLineGui") if w.isVisible()]
        return visible[0] if visible else None

    def markers():
        w = timeline()
        if w is None:
            return None
        return [int(t) for t in QMetaObject.invokeMethod(w, "keyframeMarkerTimes", Qt.DirectConnection,
                                                         Q_RETURN_ARG("QVariantList"))]

    def timeline_shot(name):
        w = timeline()
        if w is None:
            report("NOTE no timeline for screenshot " + name)
            return
        QApplication.processEvents()
        origin = w.mapToGlobal(w.rect().topLeft())
        path = os.path.join(gt.OUT, name)
        if QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), w.width(), w.height()).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def trigger_edit(prefix):
        win = rg.main_window()
        if win is None or win.menuBar() is None:
            return False
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is None or not act.text().lower().startswith("edit"):
                continue
            for a in menu.actions():
                if a.text().lower().startswith(prefix) and a.isEnabled():
                    a.trigger()
                    QApplication.processEvents()
                    return True
        return False

    # ---- panels and the curve list -------------------------------------------------------------

    def panel_of_node(node):
        """The node's visible settings panel, found by the label in its header's name field."""
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for le in panel.findChildren(QLineEdit):
                    if gt.class_name(le) == "LineEdit" and le.text() == label:
                        return hold(panel)
            except RuntimeError:
                continue
        return None

    def curve_tree(st):
        """(the RotoPanel's tree, the RotoPanel) of the open panel, or (None, None)."""
        panel = panel_of_node(st.node)
        if panel is None:
            return None, None
        for tree in panel.findChildren(QTreeWidget):
            parent = tree.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return hold(tree), hold(parent)
                parent = parent.parentWidget()
        return None, None

    def find_index(tree, texts):
        """The column-0 index of the first row whose text is in texts. The tree is rebuilt on
        structural changes, so indices are never kept across a yield."""
        model = tree.model()

        def walk(parent):
            for r in range(model.rowCount(parent)):
                index = model.index(r, 0, parent)
                if str(index.data()) in texts:
                    return index
                found = walk(index)
                if found is not None:
                    return found
            return None
        return walk(tree.rootIndex())

    def selected_labels(st):
        tree, _rp = curve_tree(st)
        if tree is None:
            return set()
        return set(str(i.data()) for i in tree.selectionModel().selectedIndexes() if i.column() == 0)

    def select_rows(st, items, extend=False):
        """Selects the tree rows of the given items (ItemBase wrappers), as clicks in the list do."""
        tree, _rp = curve_tree(st)
        require(tree is not None, "the RotoPanel of %s is open" % st.node.getLabel())
        tree.expandAll()
        selection = tree.selectionModel()
        if not extend:
            selection.clear()
        for item in items:
            index = find_index(tree, (item.getLabel(), item.getScriptName()))
            require(index is not None, "the curve list has a row for " + item.getLabel())
            selection.select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
        QApplication.processEvents()

    def clear_selection(st):
        tree, _rp = curve_tree(st)
        require(tree is not None, "the RotoPanel of %s is open" % st.node.getLabel())
        tree.selectionModel().clear()
        QApplication.processEvents()

    def close_panel(st):
        panel = panel_of_node(st.node)
        require(panel is not None, "the settings panel of %s is open" % st.node.getLabel())
        st.panel = panel
        QMetaObject.invokeMethod(panel, "closePanel")
        ok = yield ("until", lambda: panel_of_node(st.node) is None, POLL_TIMEOUT_S)
        require(ok, "closed the settings panel of " + st.node.getLabel())

    def reopen_panel(st):
        if st.grouped:
            require(st.panel is not None, "the closed panel is held")
            QMetaObject.invokeMethod(st.panel, "setClosed", Q_ARG(bool, False))
        else:
            graphs = gt.widgets_of("NodeGraph")
            require(graphs, "found the node graph")
            app.setSelection([st.node])
            QMetaObject.invokeMethod(graphs[0], "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of_node(st.node) is not None, POLL_TIMEOUT_S)
        require(ok, "reopened the settings panel of " + st.node.getLabel())
        if not st.grouped:
            app.clearSelection()

    # ---- items ---------------------------------------------------------------------------------

    def walk_items(st):
        """(item wrapper, resolved object) for every item below the base layer, layers included."""
        out = []

        def walk(layer):
            for it in layer.getChildren():
                item = st.ctx.getItemByName(it.getScriptName()) or it
                out.append((it, item))
                if isinstance(item, NatronEngine.Layer):
                    walk(item)
        walk(st.ctx.getBaseLayer())
        return out

    def stroke_names(st):
        return [it.getScriptName() for it, item in walk_items(st)
                if not isinstance(item, (NatronEngine.BezierCurve, NatronEngine.Layer))]

    def item_named(st, name):
        for it, item in walk_items(st):
            if it.getScriptName() == name:
                return it
        return None

    def param_of(st, name, knob):
        item = item_named(st, name)
        require(item is not None, "item %s exists" % name)
        param = item.getParam(knob)
        require(param is not None, "item %s has the %s knob" % (name, knob))
        return param

    def frame_of(st, name):
        """The stroke's lifeTimeFrame as Python reads it. A selected item's knob reads through the
        node's proxy, which holds one selected item's state, so this is the item's own value only
        while it is unselected or the only selected stroke."""
        return int(param_of(st, name, "lifeTimeFrame").getValue())

    def item_times(st, it, item):
        if isinstance(item, NatronEngine.BezierCurve):
            return set(int(k) for k in item.getKeyframes())
        return set([frame_of(st, it.getScriptName())])

    def derived(st):
        """The markers the panel should show, from the live selection and the items' values. Only
        used where at most one stroke is selected, since frame_of follows the proxy."""
        sel = selected_labels(st)
        times = set()

        def walk(layer, inherited):
            for it in layer.getChildren():
                item = st.ctx.getItemByName(it.getScriptName()) or it
                here = inherited or it.getLabel() in sel or it.getScriptName() in sel
                if isinstance(item, NatronEngine.Layer):
                    walk(item, here)
                elif here:
                    times.update(item_times(st, it, item))
        walk(st.ctx.getBaseLayer(), False)
        if st.current is not None and st.current in stroke_names(st):
            times.add(frame_of(st, st.current))
        return sorted(times)

    def expect(label, frames=None, st=None):
        """Markers equal the literal frames, or the derived set when frames is None."""
        def want():
            return derived(st) if frames is None else frames
        ok = yield ("until", lambda: markers() == want(), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        got, wanted = markers(), want()
        check(ok and got == wanted, "%s: markers %r match %r" % (label, got, wanted))

    # ---- painting ------------------------------------------------------------------------------

    def line(gl, y_frac, x0_frac=0.3, x1_frac=0.7):
        w, h = float(gl.width()), float(gl.height())
        return [QPointF(w * (x0_frac + (x1_frac - x0_frac) * i / float(SAMPLES - 1)), h * y_frac)
                for i in range(SAMPLES)]

    def paint_stroke(st, frame, y_frac):
        """Paints one stroke on the given frame and returns its script name."""
        before = set(stroke_names(st))
        py_viewer.seek(frame)
        yield ("sleep", SETTLE_MS)
        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        rg.activate()
        pts = line(gl, y_frac)
        rg.press(gl, pts[0])
        for p in pts[1:]:
            yield ("sleep", PEN_INTERVAL_MS)
            rg.move(gl, p, Qt.LeftButton)
        rg.release(gl, pts[-1])
        ok = yield ("until", lambda: len(set(stroke_names(st)) - before) == 1, POLL_TIMEOUT_S)
        require(ok, "painting on frame %d made one new stroke in %s" % (frame, st.node.getLabel()))
        name = (set(stroke_names(st)) - before).pop()
        yield ("sleep", RENDER_MS)
        check(frame_of(st, name) == frame, "the stroke %s was painted on frame %d (lifeTimeFrame %d)" % (
            name, frame, frame_of(st, name)))
        st.current = name
        return name

    def create_bezier(st):
        ctx = st.ctx
        pts = [(100.0, 100.0), (300.0, 100.0), (300.0, 300.0), (100.0, 300.0)]
        bez = ctx.createBezier(pts[0][0], pts[0][1], BEZIER_KEYS[0])
        require(bez is not None, "created a Bezier in " + st.node.getLabel())
        for x, y in pts[1:]:
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        for t in BEZIER_KEYS:
            for i, (x, y) in enumerate(pts):
                bez.setPointAtIndex(i, t, x + t, y, x + t - 20.0, y, x + t + 20.0, y)
        st.bezier = bez.getScriptName()
        keys = sorted(int(k) for k in bez.getKeyframes())
        require(keys == list(BEZIER_KEYS), "the Bezier is keyed at %r (%r)" % (BEZIER_KEYS, keys))

    # ---- the scenario ----------------------------------------------------------------------------

    def core(st, tag):
        label = st.node.getLabel()
        gt.section(tag + "-setup")
        ok = yield ("until", lambda: panel_of_node(st.node) is not None, POLL_TIMEOUT_S)
        require(ok, "the settings panel of %s is open" % label)
        st.node.getParam("multiStrokeEnabledButton").setValue(False)
        st.node.getParam("sizeSpinbox").setValue(30.0)
        yield ("sleep", SETTLE_MS)
        create_bezier(st)
        yield ("sleep", SETTLE_MS)
        clear_selection(st)
        yield from expect(tag + ": Bezier made, nothing selected, nothing painted", [], st)
        require(rg.pick_tool("PaintBrushToolButton", "PaintSolidTool"), "picked the solid brush")
        names = []
        for k, frame in enumerate(STROKE_FRAMES):
            name = yield from paint_stroke(st, frame, 0.3 + 0.15 * k)
            names.append(name)
        a, b, c = names
        bez = item_named(st, st.bezier)

        def it(name):
            return item_named(st, name)

        gt.section(tag + "-current")
        clear_selection(st)
        yield from expect(tag + ": current stroke, nothing selected", [12], st)
        timeline_shot("m421-%s-timeline-current.png" % tag)

        gt.section(tag + "-selection")
        select_rows(st, [it(a)])
        yield from expect(tag + ": stroke A selected", [5, 12], st)
        select_rows(st, [it(c)])
        yield from expect(tag + ": the current stroke selected", [12], st)
        select_rows(st, [it(a), it(b)])
        yield from expect(tag + ": two strokes on frame 5 give one marker", [5, 12], st)
        timeline_shot("m421-%s-timeline-two-strokes.png" % tag)
        select_rows(st, [bez])
        yield from expect(tag + ": the Bezier alone", [1, 12, 20], st)
        select_rows(st, [bez, it(a), it(b)])
        yield from expect(tag + ": the Bezier and two strokes", [1, 5, 12, 20], st)
        timeline_shot("m421-%s-timeline-bezier-and-strokes.png" % tag)
        clear_selection(st)
        yield from expect(tag + ": selection cleared", [12], st)

        gt.section(tag + "-lifetime")
        frame_knob = param_of(st, b, "lifeTimeFrame")
        mode_knob = param_of(st, b, "lifeTime")
        mode0 = int(mode_knob.getValue())
        select_rows(st, [bez, it(b)])
        yield from expect(tag + ": Bezier and B selected", [1, 5, 12, 20], st)
        param_of(st, b, "lifeTimeFrame").setValue(8)
        yield from expect(tag + ": B's Frame knob set to 8, the Bezier's markers stay", [1, 8, 12, 20], st)
        for mode, mode_name in ((LIFETIME_ALL, "All"), (LIFETIME_CUSTOM, "Custom"),
                                (LIFETIME_TO_END, "To end"), (LIFETIME_FROM_START, "From start")):
            param_of(st, b, "lifeTime").setValue(mode)
            yield from expect(tag + ": B's Lifetime mode %s still shows lifeTimeFrame" % mode_name,
                              [1, 8, 12, 20], st)
        param_of(st, b, "lifeTime").setValue(mode0)
        select_rows(st, [it(b)])
        yield from expect(tag + ": B alone at frame 8", [8, 12], st)
        proxy_frame = st.node.getParam("lifeTimeFrame")
        proxy_mode = st.node.getParam("lifeTime")
        if proxy_frame is None or proxy_mode is None:
            report("NOTE the node has no lifeTimeFrame/lifeTime proxy knobs; proxy edits not checked")
        else:
            proxy_frame.setValue(9)
            ok = yield ("until", lambda: frame_of(st, b) == 9, POLL_TIMEOUT_S)
            check(ok, "the node's Frame proxy moves the selected stroke's lifeTimeFrame to 9 (%d)" % frame_of(st, b))
            yield from expect(tag + ": proxy Frame knob moved B to 9", [9, 12], st)
            proxy_mode.setValue(LIFETIME_FROM_START)
            ok = yield ("until", lambda: int(param_of(st, b, "lifeTime").getValue()) == LIFETIME_FROM_START,
                        POLL_TIMEOUT_S)
            check(ok, "the node's Lifetime proxy sets the selected stroke's mode")
            yield from expect(tag + ": proxy Lifetime mode keeps the marker", [9, 12], st)
            proxy_mode.setValue(mode0)
        param_of(st, b, "lifeTimeFrame").setValue(5)
        yield from expect(tag + ": B restored to frame 5", [5, 12], st)
        ok = yield ("until", lambda: frame_of(st, b) == 5 and int(param_of(st, b, "lifeTime").getValue()) == mode0,
                    POLL_TIMEOUT_S)
        check(ok, "B's Lifetime knobs are back to frame 5, mode %d" % mode0)
        clear_selection(st)

        gt.section(tag + "-delete")
        select_rows(st, [it(b)])
        yield from expect(tag + ": B selected before its delete", [5, 12], st)
        QMetaObject.invokeMethod(curve_tree(st)[1], "onRemoveItemButtonClicked")
        ok = yield ("until", lambda: b not in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "deleting the selected stroke B removed it")
        # The panel selects the previous row after a delete, so the set is read from the live selection.
        yield from expect(tag + ": B deleted", None, st)
        check(len(stroke_names(st)) == 2, "two strokes are left")
        require(trigger_edit("undo"), "undo the delete of B")
        ok = yield ("until", lambda: b in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "undo brought B back")
        yield from expect(tag + ": B restored", None, st)
        clear_selection(st)
        yield from expect(tag + ": B restored, nothing selected", [12], st)
        select_rows(st, [it(c)])
        yield from expect(tag + ": the current stroke selected before its delete", [12], st)
        QMetaObject.invokeMethod(curve_tree(st)[1], "onRemoveItemButtonClicked")
        ok = yield ("until", lambda: c not in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "deleting the current stroke removed it")
        yield from expect(tag + ": the current stroke deleted", None, st)
        check(12 not in (markers() or []), "frame 12 has no marker once its only stroke is gone (%r)" % markers())
        timeline_shot("m421-%s-timeline-current-deleted.png" % tag)
        require(trigger_edit("undo"), "undo the delete of the current stroke")
        ok = yield ("until", lambda: c in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "undo brought the current stroke back")
        clear_selection(st)
        yield from expect(tag + ": the current stroke is back and current again", [12], st)

        gt.section(tag + "-item-knob")
        select_rows(st, [bez, it(a), it(b)])
        yield from expect(tag + ": two strokes and the Bezier selected", [1, 5, 12, 20], st)
        for name in (a, b):
            param_of(st, name, "opacity").setValueAtTime(0.5, ITEM_KEY_FRAME, 0)
        param_of(st, st.bezier, "opacity").setValueAtTime(0.5, ITEM_KEY_FRAME, 0)
        yield from expect(tag + ": item opacity keyed at %d on both strokes and the Bezier" % ITEM_KEY_FRAME,
                          [1, 5, ITEM_KEY_FRAME, 12, 20], st)
        timeline_shot("m421-%s-timeline-item-knob-key.png" % tag)
        for name in (a, b):
            param_of(st, name, "opacity").removeAnimation(0)
        param_of(st, st.bezier, "opacity").removeAnimation(0)
        yield from expect(tag + ": item opacity keys removed", [1, 5, 12, 20], st)
        clear_selection(st)
        layer = st.ctx.createLayer()
        require(layer is not None, "created a layer")
        layer.addItem(it(a))
        ok = yield ("until", lambda: find_index(curve_tree(st)[0], (layer.getLabel(), layer.getScriptName())) is not None,
                    POLL_TIMEOUT_S)
        require(ok, "the curve list shows the new layer")
        select_rows(st, [layer])
        yield from expect(tag + ": stroke A selected through its layer", [5, 12], st)
        param_of(st, a, "opacity").setValueAtTime(0.5, ITEM_KEY_FRAME, 0)
        yield from expect(tag + ": opacity keyed on the stroke selected through its layer",
                          [5, ITEM_KEY_FRAME, 12], st)
        param_of(st, a, "opacity").removeAnimation(0)
        yield from expect(tag + ": that key removed", [5, 12], st)
        clear_selection(st)

        gt.section(tag + "-stroke-undo")
        d = yield from paint_stroke(st, EXTRA_STROKE_FRAME, 0.3 + 0.15 * 3)
        clear_selection(st)
        yield from expect(tag + ": a fourth stroke is current", [EXTRA_STROKE_FRAME], st)
        timeline_shot("m421-%s-timeline-new-stroke.png" % tag)
        require(trigger_edit("undo"), "undo the stroke")
        ok = yield ("until", lambda: d not in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "undo removed the stroke")
        ok = yield ("until", lambda: EXTRA_STROKE_FRAME not in (markers() or []), POLL_TIMEOUT_S)
        check(ok, "the undone stroke's marker is gone (%r)" % markers())
        yield from expect(tag + ": stroke undone", None, st)
        timeline_shot("m421-%s-timeline-stroke-undone.png" % tag)
        require(trigger_edit("redo"), "redo the stroke")
        ok = yield ("until", lambda: d in stroke_names(st), POLL_TIMEOUT_S)
        require(ok, "redo brought the stroke back")
        yield from expect(tag + ": stroke redone", None, st)
        ok = yield ("until", lambda: EXTRA_STROKE_FRAME in (markers() or []), POLL_TIMEOUT_S)
        check(ok, "the redone stroke's marker is back (%r)" % markers())

        gt.section(tag + "-multi-frame")
        # D is current; A and C sit on different frames, so a read through the proxy shows one of them.
        clear_selection(st)
        yield from expect(tag + ": D is current before the multi-frame selection", [EXTRA_STROKE_FRAME], st)
        select_rows(st, [it(a), it(c)])
        yield from expect(tag + ": strokes on frames 5 and 12 selected, D current",
                          [5, 12, EXTRA_STROKE_FRAME], st)
        timeline_shot("m421-%s-timeline-multi-frame.png" % tag)
        select_rows(st, [it(c), it(a)])
        yield from expect(tag + ": the same strokes selected in the other order",
                          [5, 12, EXTRA_STROKE_FRAME], st)
        clear_selection(st)
        yield ("sleep", SETTLE_MS)
        check(frame_of(st, a) == 5 and frame_of(st, c) == 12,
              "selecting A and C together left their own frames at 5 and 12 (%d, %d)" % (frame_of(st, a), frame_of(st, c)))
        # Each item is keyed alone, so the proxy only ever carries one item's curve.
        select_rows(st, [it(a)])
        param_of(st, a, "opacity").setValueAtTime(0.5, MULTI_KEY_FRAMES[0], 0)
        select_rows(st, [it(c)])
        param_of(st, c, "opacity").setValueAtTime(0.5, MULTI_KEY_FRAMES[1], 0)
        select_rows(st, [it(a), it(c)])
        yield from expect(tag + ": opacity keyed at %d on A and %d on C, both selected" % MULTI_KEY_FRAMES,
                          sorted([5, 12, EXTRA_STROKE_FRAME] + list(MULTI_KEY_FRAMES)), st)
        timeline_shot("m421-%s-timeline-multi-frame-keys.png" % tag)
        for name in (a, c):
            select_rows(st, [it(name)])
            param_of(st, name, "opacity").removeAnimation(0)
        select_rows(st, [it(a), it(c)])
        yield from expect(tag + ": those keys removed", [5, 12, EXTRA_STROKE_FRAME], st)
        clear_selection(st)

        gt.section(tag + "-panel")
        select_rows(st, [bez, it(a)])
        yield from expect(tag + ": Bezier and A selected", None, st)
        before = markers()
        yield from close_panel(st)
        st.current = None
        yield from expect(tag + ": panel closed, no stroke or Bezier markers", [], st)
        timeline_shot("m421-%s-timeline-panel-closed.png" % tag)
        yield from reopen_panel(st)
        yield from expect(tag + ": panel reopened, the selected items' markers are back", [1, 5, 20], st)
        report("NOTE markers before the close %r; after the reopen %r" % (before, markers()))
        timeline_shot("m421-%s-timeline-panel-reopened.png" % tag)
        clear_selection(st)
        yield from expect(tag + ": reopened, nothing selected", [], st)
        return dict(a=a, b=b, c=c, d=d)

    # ---- the dope sheet --------------------------------------------------------------------------

    def dope_view():
        views = [v for v in gt.widgets_of("DopeSheetView") if v.isVisible()]
        return views[0] if views else None

    def hierarchy():
        views = [v for v in gt.widgets_of("HierarchyView") if v.isVisible()]
        return views[0] if views else None

    def child_index(tree, parent, text):
        model = tree.model()
        for r in range(model.rowCount(parent)):
            idx = model.index(r, 0, parent)
            if str(idx.data()) == text:
                return idx
        return None

    def row_path(tree, texts):
        """The index at the end of a path of row texts, from the top level."""
        parent = tree.rootIndex()
        idx = None
        for text in texts:
            idx = child_index(tree, parent, text)
            if idx is None:
                return None
            parent = idx
        return idx

    def children_texts(tree, idx):
        model = tree.model()
        return [str(model.index(r, 0, idx).data()) for r in range(model.rowCount(idx))]

    def row_center_y(tree, idx):
        tree.scrollTo(idx)
        QApplication.processEvents()
        rect = tree.visualRect(idx)
        return rect.center().y() if rect.isValid() and rect.height() > 0 else None

    def dope_mouse(view, kind, x, y, button, buttons):
        QApplication.sendEvent(view, QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def key_clusters(view, y):
        """Contiguous x runs of the row where the cursor says a key is under the pointer."""
        xs = []
        for x in range(0, view.width()):
            dope_mouse(view, QEvent.MouseMove, x, y, Qt.NoButton, Qt.NoButton)
            if view.cursor().shape() in (Qt.CrossCursor, Qt.OpenHandCursor):
                xs.append(x)
        dope_mouse(view, QEvent.MouseMove, 0, 0, Qt.NoButton, Qt.NoButton)
        runs = []
        for x in xs:
            if runs and x - runs[-1][1] <= 1:
                runs[-1][1] = x
            else:
                runs.append([x, x])
        return runs

    def dope_drag(view, x0, y, dx):
        view.window().activateWindow()
        dope_mouse(view, QEvent.MouseMove, x0, y, Qt.NoButton, Qt.NoButton)
        dope_mouse(view, QEvent.MouseButtonPress, x0, y, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 9):
            dope_mouse(view, QEvent.MouseMove, x0 + dx * i / 8.0, y, Qt.NoButton, Qt.LeftButton)
        dope_mouse(view, QEvent.MouseButtonRelease, x0 + dx, y, Qt.LeftButton, Qt.NoButton)

    def dope_shot(name):
        view, tree = dope_view(), hierarchy()
        if view is None or not view.isVisible():
            report("NOTE %s: the dope sheet is not visible" % name)
            return
        QApplication.processEvents()
        tl = view.mapToGlobal(QPoint(0, 0))
        rect = QRect(tl.x(), tl.y(), view.width(), view.height())
        if tree is not None:
            ttl = tree.mapToGlobal(QPoint(0, 0))
            rect = rect.united(QRect(ttl.x(), ttl.y(), tree.width(), tree.height()))
        path = os.path.join(gt.OUT, name)
        if QGuiApplication.primaryScreen().grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height()).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def make_tab_current(editor, word):
        pane = rg.ancestor_of_class(editor, "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if word in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def dope_sheet_section(st, tag, names):
        label = st.node.getLabel()
        gt.section(tag + "-dope-sheet")
        require(dope_view() is not None or gt.widgets_of("DopeSheetView"), "found the dope sheet view")
        views = gt.widgets_of("DopeSheetView")
        require(make_tab_current(views[0], "dope"), "brought the dope sheet's tab to the front")
        ok = yield ("until", lambda: dope_view() is not None and hierarchy() is not None, POLL_TIMEOUT_S)
        require(ok, "the dope sheet is visible")
        yield ("sleep", SETTLE_MS)
        tree = hold(hierarchy())
        tree.expandAll()
        QApplication.processEvents()
        node_idx = row_path(tree, [label])
        require(node_idx is not None, "the dope sheet has a row for " + label)
        report("NOTE dope sheet rows under %s: %r" % (label, children_texts(tree, node_idx)))
        painted = {}
        for key, frame in zip(("a", "b", "c", "d"), (5, 5, 12, EXTRA_STROKE_FRAME)):
            item = item_named(st, names[key])
            idx = row_path(tree, [label, item.getLabel()])
            require(idx is not None, "the dope sheet has an item row for stroke " + item.getLabel())
            kids = children_texts(tree, idx)
            check(kids.count("Painted Frame") == 1, "stroke %s has one Painted Frame row (%r)" % (item.getLabel(), kids))
            painted[key] = row_path(tree, [label, item.getLabel(), "Painted Frame"])
        bez = item_named(st, st.bezier)
        bez_kids = children_texts(tree, row_path(tree, [label, bez.getLabel()]) or node_idx)
        check("Painted Frame" not in bez_kids and "Shape" in bez_kids,
              "the Bezier has a Shape row and no Painted Frame row (%r)" % (bez_kids,))
        dope_shot("m421-%s-dope-sheet.png" % tag)

        # Strokes on different frames selected together read through one proxy if the row follows it.
        select_rows(st, [item_named(st, names["a"]), item_named(st, names["c"])])
        yield ("sleep", SETTLE_MS)
        view = dope_view()
        xs = {}
        for key in ("a", "b", "c", "d"):
            y = row_center_y(tree, painted[key])
            require(y is not None, "the Painted Frame row of stroke %s is on screen" % key)
            runs = key_clusters(view, y)
            check(len(runs) == 1 and runs[0][1] - runs[0][0] < 30,
                  "stroke %s's Painted Frame row shows exactly one key (%r)" % (key, runs))
            if runs:
                xs[key] = (runs[0][0] + runs[0][1]) / 2.0
        report("NOTE painted frame key x positions %r" % (xs,))
        if len(xs) == 4:
            check(abs(xs["a"] - xs["b"]) <= 1.5, "the two strokes on frame 5 have their keys at one x")
            per_frame = (xs["c"] - xs["a"]) / 7.0
            check(per_frame > 0 and abs((xs["d"] - xs["c"]) / 3.0 - per_frame) <= 1.0,
                  "the keys sit at frames 5, 12 and 15 in proportion (%.2f px per frame)" % per_frame)
            clear_selection(st)
            yield ("sleep", SETTLE_MS)
            before = markers()
            ya = row_center_y(tree, painted["a"])
            dope_drag(view, xs["a"], ya, 80)
            yield ("sleep", SETTLE_MS)
            check(frame_of(st, names["a"]) == 5, "dragging the Painted Frame key leaves lifeTimeFrame at 5 (%d)" %
                  frame_of(st, names["a"]))
            runs = key_clusters(view, row_center_y(tree, painted["a"]))
            check(len(runs) == 1 and abs((runs[0][0] + runs[0][1]) / 2.0 - xs["a"]) <= 1.5,
                  "the key is where it was after the drag (%r)" % (runs,))
            check(markers() == before, "the drag left the timeline markers alone (%r, %r)" % (before, markers()))
            dope_shot("m421-%s-dope-sheet-after-drag.png" % tag)

            def painted_x(key):
                runs = key_clusters(view, row_center_y(tree, painted[key]))
                return (runs[0][0] + runs[0][1]) / 2.0 if len(runs) == 1 else None

            def painted_rows_unchanged(what):
                for key in ("a", "b", "c", "d"):
                    x = painted_x(key)
                    check(x is not None and abs(x - xs[key]) <= 1.5,
                          "%s: stroke %s's Painted Frame key stays at x %.1f (%r)" % (what, key, xs[key], x))
                ok = yield ("until", lambda: markers() == before, POLL_TIMEOUT_S)
                check(ok, "%s: the timeline markers are unchanged (%r, %r)" % (what, before, markers()))

            gt.section(tag + "-dope-sheet-edits")
            bez_keys = sorted(int(k) for k in bez.getKeyframes())

            def select_painted_key():
                y = row_center_y(tree, painted["a"])
                dope_mouse(view, QEvent.MouseButtonPress, xs["a"], y, Qt.LeftButton, Qt.LeftButton)
                dope_mouse(view, QEvent.MouseButtonRelease, xs["a"], y, Qt.LeftButton, Qt.NoButton)

            select_painted_key()
            QMetaObject.invokeMethod(view, "deleteSelectedKeyframes")
            yield ("sleep", SETTLE_MS)
            yield from painted_rows_unchanged("Delete on a selected Painted Frame key")
            select_painted_key()
            QMetaObject.invokeMethod(view, "copySelectedKeyframes")
            py_viewer.seek(30)
            yield ("sleep", SETTLE_MS)
            QMetaObject.invokeMethod(view, "pasteKeyframesAbsolute")
            yield ("sleep", SETTLE_MS)
            yield from painted_rows_unchanged("pasting a copied Painted Frame key")
            QMetaObject.invokeMethod(view, "onSelectedAllTriggered")
            yield ("sleep", SETTLE_MS)
            yield from painted_rows_unchanged("select all")
            QMetaObject.invokeMethod(view, "copySelectedKeyframes")
            tree.selectionModel().select(painted["a"], QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
            QApplication.processEvents()
            for slot in ("pasteKeyframesAbsolute", "pasteKeyframesRelative"):
                QMetaObject.invokeMethod(view, slot)
                yield ("sleep", SETTLE_MS)
            yield from painted_rows_unchanged("pasting every key onto a Painted Frame row")
            got = sorted(int(k) for k in bez.getKeyframes())
            check(got == bez_keys, "pasting onto a Painted Frame row left the Bezier's shape keys alone (%r, %r)" % (
                bez_keys, got))
            for key, frame in zip(("a", "b", "c", "d"), (5, 5, 12, EXTRA_STROKE_FRAME)):
                check(frame_of(st, names[key]) == frame,
                      "the edits left stroke %s's lifeTimeFrame at %d (%d)" % (key, frame, frame_of(st, names[key])))
            tree.selectionModel().clearSelection()
            dope_mouse(view, QEvent.MouseButtonPress, 2, ya, Qt.LeftButton, Qt.LeftButton)
            dope_mouse(view, QEvent.MouseButtonRelease, 2, ya, Qt.LeftButton, Qt.NoButton)
            yield ("sleep", SETTLE_MS)

            gt.section(tag + "-dope-sheet-proxy")
            proxy_frame = st.node.getParam("lifeTimeFrame")
            if proxy_frame is None:
                report("NOTE the node has no lifeTimeFrame proxy; the proxy edit of the row is not checked")
            else:
                select_rows(st, [item_named(st, names["c"])])
                yield ("sleep", SETTLE_MS)
                proxy_frame.setValue(PROXY_FRAME)
                want = xs["c"] + per_frame * (PROXY_FRAME - 12)
                ok = yield ("until", lambda: painted_x("c") is not None and abs(painted_x("c") - want) <= 1.5,
                            POLL_TIMEOUT_S)
                check(ok, "the proxy Frame set to %d moves C's Painted Frame key to x %.1f (%r)" % (
                    PROXY_FRAME, want, painted_x("c")))
                dope_shot("m421-%s-dope-sheet-proxy-frame.png" % tag)
                proxy_frame.setValue(12)
                ok = yield ("until", lambda: painted_x("c") is not None and abs(painted_x("c") - xs["c"]) <= 1.5,
                            POLL_TIMEOUT_S)
                check(ok, "the proxy Frame set back to 12 moves the key back (%r)" % (painted_x("c"),))
            clear_selection(st)
            yield ("sleep", SETTLE_MS)
            check(frame_of(st, names["a"]) == 5 and frame_of(st, names["c"]) == 12,
                  "A and C are back at frames 5 and 12 (%d, %d)" % (frame_of(st, names["a"]), frame_of(st, names["c"])))

    # ---- steps -----------------------------------------------------------------------------------

    def show_in_viewer(node, viewer, group):
        if viewer.connectInput(0, node):
            return True
        inner = app.createNode("Viewer", -1, group)
        return inner is not None and inner.connectInput(0, node)

    def steps():
        global py_viewer
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None and timeline() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer and its timeline are up")
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        app.getProjectParam("frameRange").set(1, 100)
        gt.close_all_panels()
        rg.activate()
        yield ("sleep", SETTLE_MS)

        rp, _ctx = rg.open_roto(app, "RotoPaint")
        require(rp is not None, "create RotoPaint", gt.plugin_problem("RotoPaint"))
        require(rp.getRotoContext() is not None, "the RotoPaint node has a roto context")
        st = State(rp, False)
        yield ("sleep", RENDER_MS)
        yield from expect("start: no markers before anything is painted", [], st)
        names = yield from core(st, "top")
        # The panel is closed and reopened by core(); the dope sheet needs it open.
        require(panel_of_node(rp) is not None, "the top-level RotoPaint's panel is open")
        yield from dope_sheet_section(st, "top", names)
        graphs = gt.widgets_of("NodeGraph")
        if graphs:
            make_tab_current(graphs[0], "graph")
        yield from close_panel(st)
        yield from expect("top level closed: no markers", [], st)

        gt.section("group-setup")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        group = app.createNode("Group", -1, None, props)
        require(group is not None, "create a Group", gt.plugin_problem("Group"))
        inner_props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        inner = app.createNode("RotoPaint", -1, group, inner_props)
        require(inner is not None, "create a RotoPaint inside the Group")
        require(inner.getRotoContext() is not None, "the grouped RotoPaint has a roto context")
        require(show_in_viewer(inner, viewer, group), "showed the grouped RotoPaint in a viewer")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer is visible")
        yield ("sleep", RENDER_MS)
        gst = State(inner, True)
        gnames = yield from core(gst, "group")

        gt.section("two-panels")
        require(panel_of_node(inner) is not None, "the grouped RotoPaint's panel is open")
        yield from reopen_panel(st)
        require(panel_of_node(rp) is not None, "the top-level RotoPaint's panel is open again")
        select_rows(st, [item_named(st, names["a"])])
        select_rows(gst, [item_named(gst, gnames["c"])])
        yield ("sleep", SETTLE_MS)
        top_only, group_only = derived(st), derived(gst)
        check(top_only and group_only and not set(top_only) & set(group_only),
              "the two panels' strokes sit on different frames (%r, %r)" % (top_only, group_only))
        yield from expect("two RotoPaint panels open: both owners' markers merged",
                          sorted(set(top_only) | set(group_only)), st)
        timeline_shot("m421-two-panels-timeline.png")
        yield from close_panel(st)
        yield from expect("the top-level panel closed: only the grouped RotoPaint's markers", group_only, gst)
        clear_selection(gst)
        yield from expect("the grouped selection cleared", [], gst)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
