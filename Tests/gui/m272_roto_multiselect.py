"""Selecting several Roto and RotoPaint items keeps each item's own values, and edits reach each.

A RotoPaint gets two strokes painted at frames 5 and 12 with different opacities, and a Roto gets
two Beziers whose opacity is keyed at different frames. In each node's curve list the script
selects one item, then both, deselects the second and clears the selection, reading every item's
values back each time and checking that the node's knobs and its panel show the item selected
last. With both items selected it types an opacity into the panel, then sets the node's opacity
from Python in an undo step: both items change, and one undo gives each its own values back. A
Python write to one selected item's own opacity changes only that item. Saves screenshots of each
stage.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m272-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m272_roto_multiselect.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
PEN_INTERVAL_MS = 8
BRUSH_PX = 30.0
# Frame -> (opacity, height of the stroke as a fraction of the viewer).
STROKES = {5: (0.3, 0.35), 12: (0.6, 0.65)}
# Opacity keys of each Bezier: frame -> value.
BEZIER_KEYS = ({1.0: 0.2, 10.0: 0.8}, {4.0: 0.5, 15.0: 0.9})
EDIT_FRAME = 7.0
PANEL_VALUE = 0.45
NODE_VALUE = 0.35
ITEM_VALUE = 0.15
DIGITS = 6
# The panel's spin box shows a rounded value.
PANEL_TOL = 1e-2

try:
    import NatronEngine
    from PySide6.QtCore import QPointF, Qt
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTreeWidget

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Python wrappers die with their parent wrapper, so every one used later is held here.
    held = []

    def hold(obj):
        held.append(obj)
        return obj

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def curve_tree(panel):
        for tree in panel.findChildren(QTreeWidget):
            parent = tree.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return tree
                parent = parent.parentWidget()
        return None

    def fresh_tree(node):
        """The node's curve list, found again on every call: a tree wrapper held across a yield can
        be invalidated, and the panel wrapper is held so the tree's wrapper outlives this call."""
        panel = panel_of(node)
        if panel is None:
            return None
        hold(panel)
        return curve_tree(panel)

    def tree_item(tree, item):
        found = tree.findItems(item.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
        return found[0] if found else None

    def select_in_tree(tree, items):
        """Selects items in the curve list one after the other, as ctrl-clicks do."""
        tree.clearSelection()
        rows = []
        for item in items:
            row = tree_item(tree, item)
            if row is not None:
                row.setSelected(True)
                rows.append(row)
        QApplication.processEvents()
        return rows

    def opacity_boxes(node):
        """The spin boxes of the node's opacity knob, found by its script name in their tooltip,
        with the panel wrapper held so theirs outlive this call."""
        panel = panel_of(node)
        if panel is None:
            return []
        hold(panel)
        tag = "<b>opacity</b>"
        boxes = []
        for w in panel.findChildren(QLineEdit):
            try:
                if gt.class_name(w) == "SpinBox" and tag in w.toolTip():
                    boxes.append(w)
            except RuntimeError:
                continue
        return boxes

    def state(param_of):
        """An item's or the node's values: its opacity keys, its opacity at the edit frame and its
        lifetime frame, rounded so that equal values compare equal."""
        opacity = param_of("opacity")
        keys = []
        for i in range(opacity.getNumKeys(0)):
            found, t = opacity.getKeyTime(i, 0)
            if found:
                keys.append((round(t, DIGITS), round(opacity.getValueAtTime(t, 0), DIGITS)))
        return {"keys": tuple(keys),
                "opacity": round(opacity.getValueAtTime(EDIT_FRAME, 0), DIGITS),
                "frame": param_of("lifeTimeFrame").getValue(0)}

    def item_state(item):
        return state(item.getParam)

    def node_state(node):
        return state(node.getParam)

    def states(items):
        return [item_state(i) for i in items]

    def check_items(items, expected, what):
        now = states(items)
        if not check(now == expected, what):
            report("NOTE expected %r, got %r" % (expected, now))

    def check_shows(node, item, what):
        """The node's knobs and its panel show item's values."""
        shown = node_state(node)
        wanted = item_state(item)
        if not check(shown == wanted, "%s: the node's knobs show %s" % (what, item.getScriptName())):
            report("NOTE node %r, item %r" % (shown, wanted))
        boxes = opacity_boxes(node)
        if not check(len(boxes) > 0, "%s: the panel has an opacity box" % what):
            return
        try:
            typed = float(boxes[0].text())
        except ValueError:
            typed = None
        current = round(item.getParam("opacity").getValue(0), DIGITS)
        check(typed is not None and abs(typed - current) < PANEL_TOL,
              "%s: the panel shows %s's opacity %g (box %r)" % (what, item.getScriptName(), current,
                                                               boxes[0].text()))

    def set_opacity(param, value):
        """Keys the opacity at the edit frame when it is animated, else sets it."""
        if param.getIsAnimated(0):
            param.set(value, EDIT_FRAME)
        else:
            param.set(value)

    def edited(before, value):
        """before with its opacity at the edit frame set to value, keyed there when animated."""
        after = dict(before)
        after["opacity"] = round(value, DIGITS)
        if before["keys"]:
            keys = dict(before["keys"])
            keys[round(EDIT_FRAME, DIGITS)] = round(value, DIGITS)
            after["keys"] = tuple(sorted(keys.items()))
        return after

    def undo(items, expected, what):
        rg.trigger("undo")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_items(items, expected, "%s: one undo gives each item its own values back" % what)

    def selection_phase(node, items, tag):
        gt.section("%s-selection" % tag)
        first, second = items
        base = states(items)
        report("NOTE %s items %r" % (tag, base))
        require(fresh_tree(node) is not None, "%s: the panel holds the curve list" % tag)

        rows = select_in_tree(fresh_tree(node), [first])
        require(len(rows) == 1, "%s: selected %s in the curve list" % (tag, first.getScriptName()))
        yield ("sleep", SETTLE_MS)
        check_items(items, base, "%s: selecting one item keeps every item's values" % tag)
        check_shows(node, first, "%s, one selected" % tag)
        gt.shot("m272-%s-one-selected.png" % tag)

        rows = select_in_tree(fresh_tree(node), items)
        require(len(rows) == 2, "%s: selected both items in the curve list" % tag)
        yield ("sleep", SETTLE_MS)
        check_items(items, base, "%s: selecting both items keeps every item's values" % tag)
        check_shows(node, second, "%s, both selected" % tag)
        gt.shot("m272-%s-both-selected.png" % tag)

        tree = fresh_tree(node)
        row = tree_item(tree, second)
        require(row is not None, "%s: %s is in the curve list" % (tag, second.getScriptName()))
        row.setSelected(False)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_items(items, base, "%s: deselecting the second item keeps every item's values" % tag)
        check_shows(node, first, "%s, second deselected" % tag)

        fresh_tree(node).clearSelection()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_items(items, base, "%s: clearing the selection keeps every item's values" % tag)
        gt.shot("m272-%s-cleared.png" % tag)

    def edit_phase(node, items, tag):
        gt.section("%s-edits" % tag)
        first, second = items
        rows = select_in_tree(fresh_tree(node), items)
        require(len(rows) == 2, "%s: selected both items in the curve list" % tag)
        yield ("sleep", SETTLE_MS)
        base = states(items)

        boxes = opacity_boxes(node)
        require(len(boxes) > 0, "%s: the panel has an opacity box" % tag)
        box = boxes[0]
        box.setFocus()
        box.setText(repr(PANEL_VALUE))
        rg.send_key(box, Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        check_items(items, [edited(s, PANEL_VALUE) for s in base],
                    "%s: an opacity typed in the panel reaches both items and keeps their other values" % tag)
        gt.shot("m272-%s-panel-edit.png" % tag)
        yield from undo(items, base, "%s panel edit" % tag)
        check_shows(node, second, "%s, after undoing the panel edit" % tag)

        with app.newUndoStep("Node Opacity"):
            set_opacity(node.getParam("opacity"), NODE_VALUE)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_items(items, [edited(s, NODE_VALUE) for s in base],
                    "%s: setting the node's opacity from Python reaches both items" % tag)
        gt.shot("m272-%s-node-write.png" % tag)
        yield from undo(items, base, "%s node write" % tag)

        with app.newUndoStep("Item Opacity"):
            set_opacity(first.getParam("opacity"), ITEM_VALUE)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_items(items, [edited(base[0], ITEM_VALUE), base[1]],
                    "%s: a Python write to %s's opacity changes only it" % (tag, first.getScriptName()))
        check_shows(node, second, "%s, after a write to the first item" % tag)
        gt.shot("m272-%s-item-write.png" % tag)
        yield from undo(items, base, "%s item write" % tag)

        fresh_tree(node).clearSelection()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def strokes_of(ctx):
        return [hold(it) for it in ctx.getBaseLayer().getChildren()
                if not isinstance(it, (NatronEngine.Layer, NatronEngine.BezierCurve))]

    def paint_line(gl, y_frac):
        w, h = float(gl.width()), float(gl.height())
        pts = [QPointF(w * (0.3 + 0.4 * i / 9.0), h * y_frac) for i in range(10)]
        rg.press(gl, pts[0])
        for p in pts[1:]:
            yield ("sleep", PEN_INTERVAL_MS)
            rg.move(gl, p, Qt.LeftButton)
        rg.release(gl, pts[-1])

    def make_bezier(ctx, x, keys):
        bez = hold(ctx.createBezier(x, 300.0, 1.0))
        require(bez is not None, "created a Bezier")
        for px, py in ((x + 200.0, 300.0), (x + 200.0, 500.0), (x, 500.0)):
            bez.addControlPoint(px, py)
        bez.setCurveFinished(True)
        opacity = bez.getParam("opacity")
        for t, v in sorted(keys.items()):
            opacity.set(v, t)
        return bez

    def steps():
        ok = yield ("until", lambda: rg.main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: rg.viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        rg.activate()
        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        require(py_viewer is not None, "the viewer has a Python handle")

        gt.section("paint-strokes")
        paint, pctx = rg.open_roto(app, "RotoPaint")
        require(paint is not None, "created a RotoPaint", gt.plugin_problem("RotoPaint"))
        require(pctx is not None, "the RotoPaint has a roto context")
        hold(paint)
        hold(pctx)
        ok = yield ("until", lambda: panel_of(paint) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the RotoPaint's settings panel")
        yield ("sleep", RENDER_MS)
        gl = hold(rg.viewer_gl())
        require(gl is not None, "the viewer is visible")
        rg.activate()
        require(rg.pick_tool("PaintBrushToolButton", "PaintSolidTool"), "picked the solid brush")
        paint.getParam("multiStrokeEnabledButton").setValue(False)
        paint.getParam("sizeSpinbox").setValue(BRUSH_PX)
        QApplication.processEvents()
        for count, frame in enumerate(sorted(STROKES), 1):
            py_viewer.seek(frame)
            yield ("sleep", SETTLE_MS)
            yield from paint_line(gl, STROKES[frame][1])
            ok = yield ("until", lambda: len(strokes_of(pctx)) == count, POLL_TIMEOUT_S)
            require(ok, "painted a stroke at frame %d" % frame)
            yield ("sleep", SETTLE_MS)
        rg.select_tool(gl)
        fresh_tree(paint).clearSelection()
        QApplication.processEvents()
        strokes = sorted(strokes_of(pctx), key=lambda s: s.getParam("lifeTimeFrame").getValue(0))
        frames = [s.getParam("lifeTimeFrame").getValue(0) for s in strokes]
        require(frames == sorted(STROKES), "the strokes live at frames %r (got %r)" % (sorted(STROKES), frames))
        for stroke, frame in zip(strokes, sorted(STROKES)):
            stroke.getParam("opacity").set(STROKES[frame][0])
        py_viewer.seek(int(EDIT_FRAME))
        yield ("sleep", RENDER_MS)
        gt.shot("m272-rotopaint-strokes.png")

        yield from selection_phase(paint, strokes, "rotopaint")
        yield from edit_phase(paint, strokes, "rotopaint")

        gt.section("roto-shapes")
        gt.close_all_panels()
        roto, rctx = rg.open_roto(app, "Roto")
        require(roto is not None, "created a Roto", gt.plugin_problem("Roto"))
        require(rctx is not None, "the Roto has a roto context")
        hold(roto)
        hold(rctx)
        roto.setPosition(200, 0)
        ok = yield ("until", lambda: panel_of(roto) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the Roto's settings panel")
        shapes = [make_bezier(rctx, 200.0 + 400.0 * i, keys) for i, keys in enumerate(BEZIER_KEYS)]
        yield ("sleep", SETTLE_MS)
        require(fresh_tree(roto) is not None, "the Roto's panel holds the curve list")
        fresh_tree(roto).clearSelection()
        QApplication.processEvents()
        py_viewer.seek(int(EDIT_FRAME))
        yield ("sleep", RENDER_MS)
        gt.shot("m272-roto-shapes.png")

        yield from selection_phase(roto, shapes, "roto")
        yield from edit_phase(roto, shapes, "roto")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
