"""Roto and RotoPaint keyframes show in the dope sheet and the curve editor.

Creates a Roto and a RotoPaint node, each with two Beziers keyed at frames 1, 10 and 20, one
keyed item opacity and one keyed node knob (Mix). In the dope sheet it checks the node, item,
Shape and knob rows, then selects every key, deletes them and undoes, reading the keys back from
Python each time. In the curve editor it checks the node-knob rows are there and no shape
"Animation" track is, then puts the selected control points into the curve editor with the
viewer menu's "Show in Curve Editor" action, checks each point gets x and y curves, and removes
them again. Saves screenshots of both editors.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m261-shots/out -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m261_roto_keys_editors.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
SHAPE_KEYS = (1.0, 10.0, 20.0)
ITEM_KEY = 10.0
NODE_KEY = 20.0

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QPoint, Q_ARG, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeWidget, QWidget

    import guitest as gt
    from guitest import check, report

    import faulthandler
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

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def make_tab_current(editor, word):
        pane = ancestor_of_class(editor, "TabWidget")
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

    def trigger_edit(name):
        """Undo is a QAction on the Edit menu, matched by Qt's shortcut map before a key event
        sent to a widget would reach it, so the action itself is triggered."""
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = act.menu()
                if menu is None or not act.text().lower().startswith("edit"):
                    continue
                for a in menu.actions():
                    if a.text().lower().startswith(name) and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        return check(False, "found an enabled %s action in the Edit menu" % name)

    _keepalive = {}

    def editor_widgets(editor_class):
        """DopeSheetEditor has no Q_OBJECT, so it is stood in for by its view."""
        return gt.widgets_of("DopeSheetView" if editor_class == "DopeSheetEditor" else editor_class)

    def editor_tree(editor_class):
        """The editor's tree, found again on every call: a wrapper held across a yield is marked
        deleted. The editor's wrapper is kept alive with it, since a child wrapper dies with its
        parent's."""
        if editor_class == "DopeSheetEditor":
            trees = gt.widgets_of("HierarchyView")
            if trees:
                _keepalive[editor_class] = trees[0]
            return trees[0] if trees else None
        editors = gt.widgets_of(editor_class)
        if not editors:
            return None
        trees = editors[0].findChildren(QTreeWidget)
        if trees:
            _keepalive[editor_class] = editors[0]
        return trees[0] if trees else None

    def snapshot(editor_class):
        """The editor's tree as plain data, read through the model: [{'text', 'hidden', 'kids'}]."""
        tree = editor_tree(editor_class)
        if tree is None:
            return []
        model = tree.model()

        def walk(parent):
            out = []
            for r in range(model.rowCount(parent)):
                idx = model.index(r, 0, parent)
                out.append({"text": str(idx.data()), "hidden": tree.isRowHidden(r, parent),
                            "kids": walk(idx)})
            return out
        return walk(tree.rootIndex())

    def rows_under(item, depth=0):
        """(depth, text, hidden) of the item and every descendant."""
        rows = [(depth, item["text"], item["hidden"])]
        for kid in item["kids"]:
            rows.extend(rows_under(kid, depth + 1))
        return rows

    def top_item(editor_class, label):
        for item in snapshot(editor_class):
            if item["text"] == label:
                return item
        return None

    def node_rows(editor_class, label):
        item = top_item(editor_class, label)
        return rows_under(item) if item is not None else None

    def child_with_text(item, text):
        for kid in item["kids"]:
            if kid["text"] == text:
                return kid
        return None

    def visible_text_at(rows, depth, text):
        return any(d == depth and t == text and not h for d, t, h in rows)

    def curve_widget_ink():
        """Pixels of the visible curve widget that differ from its dominant background colour."""
        cws = [w for w in gt.widgets_of("CurveWidget") if w.isVisible()]
        if not cws:
            return None
        cw = cws[0]
        QApplication.processEvents()
        origin = cw.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(
            0, origin.x(), origin.y(), cw.width(), cw.height()).toImage()
        counts = {}
        for yd in range(0, img.height(), 3):
            for xd in range(0, img.width(), 3):
                rgb = img.pixel(xd, yd)
                counts[rgb] = counts.get(rgb, 0) + 1
        background = max(counts, key=counts.get)
        return sum(n for rgb, n in counts.items() if rgb != background)

    def build_host(plugin_id, x0, y0, node_y):
        node = app.createNode(plugin_id)
        gt.require(node is not None, "created %s" % plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(0, node_y)
        ctx = node.getRotoContext()
        gt.require(ctx is not None, "%s has a roto context" % node.getLabel())
        shapes = []
        for s in range(2):
            ox = x0 + 300.0 * s
            pts = [(ox, y0), (ox + 100.0, y0), (ox + 100.0, y0 + 100.0), (ox, y0 + 100.0)]
            bez = ctx.createBezier(pts[0][0], pts[0][1], SHAPE_KEYS[0])
            gt.require(bez is not None, "created a Bezier in %s" % node.getLabel())
            for p in pts[1:]:
                bez.addControlPoint(p[0], p[1])
            bez.setCurveFinished(True)
            for k, t in enumerate(SHAPE_KEYS):
                for i, (x, y) in enumerate(pts):
                    x += 10.0 * k
                    bez.setPointAtIndex(i, t, x, y, x - 20.0, y, x + 20.0, y)
            bez.setOpacity(0.5, ITEM_KEY)
            shapes.append(bez)
        mix = node.getParam("hostMix")
        gt.require(mix is not None, "%s has a Mix knob" % node.getLabel())
        mix.setValueAtTime(0.5, NODE_KEY)
        return node, shapes

    def key_state(hosts):
        """Per host: the node knob's key count and, per shape, its key times and opacity key count."""
        state = []
        for node, shapes in hosts:
            state.append((node.getParam("hostMix").getNumKeys(),
                          [(tuple(b.getKeyframes()), b.getOpacityParam().getNumKeys()) for b in shapes]))
        return state

    def write_sequence(prefix, first, last):
        """A tiny PPM sequence prefix.0001.ppm ..., returns the '####' pattern path."""
        os.makedirs(os.path.dirname(prefix), exist_ok=True)
        for f in range(first, last + 1):
            with open("%s.%04d.ppm" % (prefix, f), "wb") as fh:
                fh.write(b"P6\n8 8\n255\n" + bytes([200, 60, 60]) * 64)
        return prefix + ".####.ppm"

    def bar_run_lengths(widget):
        """Lengths of the long same-colour horizontal runs in the widget, the Reader bars among them."""
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(
            0, origin.x(), origin.y(), widget.width(), widget.height()).toImage()
        runs = []
        for yd in range(0, img.height(), 2):
            best, run, prev = 0, 0, None
            for xd in range(img.width()):
                rgb = img.pixel(xd, yd)
                run = run + 1 if rgb == prev else 1
                prev = rgb
                best = max(best, run)
            runs.append(best)
        return runs, img.width()

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        gt.require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 2400)

        gt.section("setup")
        hosts = [build_host("fr.inria.built-in.Roto", 100.0, 100.0, 0),
                 build_host("fr.inria.built-in.RotoPaint", 100.0, 400.0, 400)]
        expected = [(1, [(SHAPE_KEYS, 1), (SHAPE_KEYS, 1)]) for _h in hosts]
        initial = key_state(hosts)
        report("NOTE keys %r" % (initial,))
        check(initial == expected, "each host has a Mix key and two shapes keyed at 1/10/20 with one opacity key")
        seq_dir = os.path.join(gt.OUT, "m261-clips")
        clips = []
        for name, last in (("long", 40), ("short", 10)):
            clip = app.createReader(write_sequence(os.path.join(seq_dir, name), 1, last))
            gt.require(clip is not None, "created a Read of %d frames" % last)
            clip.setPosition(600 if name == "long" else 400, 1200)
            clips.append(clip)
        for node in [h[0] for h in hosts] + clips:
            app.selectNode(node, True)
            QMetaObject.invokeMethod(root_graph(), "showSelectedNodeSettingsPanel")
            yield ("sleep", SETTLE_MS)
        ok = yield ("until", gt.any_panel_open, 10.0)
        check(ok, "the hosts' settings panels opened")
        yield ("sleep", SETTLE_MS)

        gt.section("dope-sheet")
        editors = editor_widgets("DopeSheetEditor")
        gt.require(editors, "found the dope sheet")
        gt.require(make_tab_current(editors[0], "dope"), "brought the dope sheet's tab to the front")
        yield ("sleep", SETTLE_MS)
        for node, shapes in hosts:
            label = node.getLabel()
            rows = node_rows("DopeSheetEditor", label)
            report("NOTE dope sheet rows of %s: %r" % (label, rows))
            check(rows is not None, "%s has a node row in the dope sheet" % label)
            if rows is None:
                continue
            check(visible_text_at(rows, 1, "Mix"), "%s's Mix knob has a row" % label)
            for bez in shapes:
                item = child_with_text(top_item("DopeSheetEditor", label), bez.getLabel())
                check(item is not None and not item["hidden"], "%s has an item row for %s" % (label, bez.getLabel()))
                if item is None:
                    continue
                sub = rows_under(item)
                check(visible_text_at(sub, 1, "Shape"), "%s has a visible Shape row" % bez.getLabel())
                check(visible_text_at(sub, 1, "Opacity"), "%s has a visible Opacity row" % bez.getLabel())
        gt.shot("m261-dope-sheet.png")

        views = gt.widgets_of("DopeSheetView")
        gt.require(views, "found the dope sheet view")
        report("NOTE selecting all keys")
        QMetaObject.invokeMethod(views[0], "onSelectedAllTriggered")
        QApplication.processEvents()
        report("NOTE selected all keys")
        yield ("sleep", SETTLE_MS)
        gt.shot("m261-dope-sheet-all-selected.png")
        report("NOTE deleting selected keys")
        QMetaObject.invokeMethod(views[0], "deleteSelectedKeyframes")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        deleted = key_state(hosts)
        report("NOTE keys after delete %r" % (deleted,))
        check(deleted == [(0, [((), 0), ((), 0)]) for _h in hosts],
              "select all and delete in the dope sheet removes every shape, item and node key")
        trigger_edit("undo")
        yield ("sleep", SETTLE_MS)
        restored = key_state(hosts)
        report("NOTE keys after undo %r" % (restored,))
        check(restored == expected, "one undo restores every key")
        gt.shot("m261-dope-sheet-after-undo.png")

        gt.section("curve-editor")
        editors = gt.widgets_of("CurveEditor")
        gt.require(editors, "found the curve editor")
        gt.require(make_tab_current(editors[0], "curve"), "brought the curve editor's tab to the front")
        yield ("sleep", SETTLE_MS)
        for node, shapes in hosts:
            label = node.getLabel()
            rows = node_rows("CurveEditor", label)
            report("NOTE curve editor rows of %s: %r" % (label, rows))
            check(rows is not None, "%s has a row in the curve editor" % label)
            if rows is None:
                continue
            check(any(d == 1 and t == "Mix" for d, t, _h in rows), "%s's Mix knob has a curve editor row" % label)
            check(not any(t == "Animation" for _d, t, _h in rows), "%s has no shape Animation track" % label)
            for bez in shapes:
                check(any(d == 1 and t == bez.getLabel() for d, t, _h in rows),
                      "%s has a curve editor row for %s" % (label, bez.getLabel()))

        paint, paint_shapes = hosts[1]
        paint_label = paint.getLabel()
        tree = editor_tree("CurveEditor")
        if tree is not None:
            tree.clearSelection()
        yield ("sleep", SETTLE_MS)
        ink_none = curve_widget_ink()
        # The first trigger selects every shape when none is selected, the next one their points.
        select_all = paint.getParam("selectAllAction")
        show = paint.getParam("showInCurveEditorAction")
        gt.require(select_all is not None and show is not None, "the RotoPaint node has the select-all and show actions")
        select_all.trigger()
        select_all.trigger()
        show.trigger()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        with_points = 0
        for bez in paint_shapes:
            shape_item = child_with_text(top_item("CurveEditor", paint_label), bez.getLabel())
            check(shape_item is not None, "%s still has its curve editor row" % bez.getLabel())
            if shape_item is None:
                continue
            points = [k for k in shape_item["kids"] if k["text"].startswith("Point ")]
            if not points:
                report("NOTE %s has no point rows (it is not among the selected shapes)" % bez.getLabel())
                continue
            with_points += 1
            check(len(points) == bez.getNumControlPoints(), "%s shows a row per control point (%d)" % (
                bez.getLabel(), len(points)))
            for point in points:
                coords = [k["text"] for k in point["kids"]]
                check(coords == ["x", "y"] and not point["hidden"],
                      "%s %s has x and y curves (%r)" % (bez.getLabel(), point["text"], coords))
        check(with_points >= 1, "at least one selected shape shows its control points")
        for cw in gt.widgets_of("CurveWidget"):
            if cw.isVisible():
                QMetaObject.invokeMethod(cw, "frameAll")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        ink_points = curve_widget_ink()
        report("NOTE curve widget ink: nothing selected %r, points shown %r" % (ink_none, ink_points))
        check(ink_none is not None and ink_points is not None and ink_points > ink_none,
              "the shown points' x/y curves draw in the curve widget")
        gt.shot("m261-curve-editor-points.png")

        paint.getParam("removeFromCurveEditorAction").trigger()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        rows = node_rows("CurveEditor", paint_label) or []
        check(not any(t.startswith("Point ") for _d, t, _h in rows), "removing the points drops their rows")
        gt.shot("m261-curve-editor.png")

        gt.section("framing")
        # Needs the build with the dope sheet framing over every clip range (not the one before it).
        editors = editor_widgets("DopeSheetEditor")
        make_tab_current(editors[0], "dope")
        yield ("sleep", SETTLE_MS)
        views = gt.widgets_of("DopeSheetView")
        # A click on empty space clears any key selection, so F frames the whole project.
        QTest.mouseClick(views[0], Qt.LeftButton, Qt.NoModifier, QPoint(views[0].width() - 6, views[0].height() - 6))
        QApplication.processEvents()
        QMetaObject.invokeMethod(views[0], "centerOnSelection")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        report("NOTE dope sheet top-level rows: %r" % ([(i["text"], i["hidden"]) for i in snapshot("DopeSheetEditor")],))
        runs, width = bar_run_lengths(gt.widgets_of("DopeSheetView")[0])
        # Both bars whole: the 40-frame bar spans the view, the 10-frame bar about a quarter of it.
        quarter = [r for r in runs if 0.15 * width <= r <= 0.4 * width]
        full = [r for r in runs if r >= 0.85 * width]
        report("NOTE framing: view width %d, %d rows with a bar run of 15-40%%, %d rows with 85%%+" % (
            width, len(quarter), len(full)))
        check(len(quarter) >= 3 and len(full) >= 3,
              "framing the dope sheet shows a 10-frame and a 40-frame Read bar whole (needs the P2.T3 build)")
        gt.shot("m261-dope-sheet-framing.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
