"""M146: a RotoPaint node's curve animation shows in the curve editor.

Creates a RotoPaint node with a Bezier whose opacity is keyed at two frames, opens the curve
editor, and checks that the node's row holds the shape's animation as a tree item, that the item
can be selected, and that selecting it draws a curve in the curve widget. Saves a screenshot of
the curve editor.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m146-shots/out -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m146_curve_editor_roto.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
KEY_FRAMES = ((1.0, 1.0), (20.0, 0.25))

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QPoint, Q_ARG
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeWidget, QTreeWidgetItem, QWidget

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

    def make_curve_editor_current(editor):
        pane = ancestor_of_class(editor, "TabWidget")
        if pane is None:
            return False
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not bars:
            return False
        for i in range(bars[0].count()):
            if "curve" in bars[0].tabText(i).lower():
                QMetaObject.invokeMethod(pane, "makeCurrentTab", Q_ARG(int, i))
                QApplication.processEvents()
                return True
        return False

    def editor_tree():
        """The curve editor's tree, found again on every call: a wrapper held across a yield is
        marked deleted."""
        editors = gt.widgets_of("CurveEditor")
        if not editors:
            return None
        trees = editors[0].findChildren(QTreeWidget)
        return trees[0] if trees else None

    def rows_under(item, depth=0):
        """(depth, text) of the item and every descendant."""
        rows = [(depth, item.text(0))]
        for i in range(item.childCount()):
            rows.extend(rows_under(item.child(i), depth + 1))
        return rows

    def node_rows(label):
        tree = editor_tree()
        if tree is None:
            return None
        for i in range(tree.topLevelItemCount()):
            item = tree.topLevelItem(i)
            if item.text(0) == label:
                return rows_under(item)
        return None

    def expand_and_select(label, child_text):
        """Expands the node's row and selects the descendant whose text contains child_text;
        returns whether it exists and ended up selected."""
        tree = editor_tree()
        if tree is None:
            return (False, False)

        def walk(item):
            item.setExpanded(True)
            for i in range(item.childCount()):
                yield item.child(i)
                for sub in walk(item.child(i)):
                    yield sub

        for i in range(tree.topLevelItemCount()):
            top = tree.topLevelItem(i)
            if top.text(0) != label:
                continue
            for item in walk(top):
                if child_text in item.text(0):
                    tree.clearSelection()
                    item.setSelected(True)
                    tree.setCurrentItem(item)
                    QApplication.processEvents()
                    return (True, item.isSelected())
        return (False, False)

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

        gt.section("rotopaint-curves")
        roto = app.createNode("fr.inria.built-in.RotoPaint")
        gt.require(roto is not None, "created a RotoPaint node", gt.plugin_problem("fr.inria.built-in.RotoPaint"))
        roto.setPosition(0, 0)
        ctx = roto.getRotoContext()
        gt.require(ctx is not None, "the RotoPaint node has a roto context")
        bez = ctx.createBezier(100.0, 100.0, 1.0)
        gt.require(bez is not None, "created a Bezier")
        for x, y in ((200.0, 100.0), (200.0, 200.0), (100.0, 200.0)):
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        for t, v in KEY_FRAMES:
            bez.setOpacity(v, t)
        shape_name = bez.getScriptName()
        roto_label = roto.getLabel()
        check(bez.getNumControlPoints() == 4, "the Bezier has four control points")
        yield ("sleep", SETTLE_MS)

        editors = gt.widgets_of("CurveEditor")
        gt.require(editors, "found the curve editor")
        gt.require(make_curve_editor_current(editors[0]), "brought the curve editor's tab to the front")
        yield ("sleep", SETTLE_MS)

        rows = node_rows(roto_label)
        check(rows is not None, "the RotoPaint node has a row in the curve editor")
        report("NOTE rows %r" % (rows,))
        check(rows is not None and len(rows) > 1, "the node's row has child items (the roto items)")

        found, selected = expand_and_select(roto_label, shape_name)
        check(found, "the Bezier %s has a tree item under the RotoPaint row" % shape_name)
        check(selected, "the Bezier's item can be selected")
        yield ("sleep", SETTLE_MS)
        ink_selected = curve_widget_ink()
        report("NOTE curve widget ink with the shape selected: %r" % (ink_selected,))

        tree = editor_tree()
        if tree is not None:
            tree.clearSelection()
        yield ("sleep", SETTLE_MS)
        ink_none = curve_widget_ink()
        report("NOTE curve widget ink with nothing selected: %r" % (ink_none,))

        found, _sel = expand_and_select(roto_label, shape_name)
        yield ("sleep", SETTLE_MS)
        ink_again = curve_widget_ink()
        check(ink_selected is not None and ink_none is not None and ink_again is not None
              and ink_again > ink_none,
              "selecting the shape draws a curve in the curve widget (%r > %r)" % (ink_again, ink_none))
        gt.shot("m146-curve-editor-rotopaint.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
