"""A GUI test of the window's single undo history: Ctrl+Z and Ctrl+Shift+Z walk every edit in the
order it was made whichever pane the mouse is over, and a node panel's undo/redo buttons step only
that node's own edits. It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m114-undo/out GUI_TEST_TIMEOUT=240 \
        Tests/gui/run-gui-test.sh Tests/gui/m114_global_undo.py <AppImage or build/fast/App/Natron>'

Python edits (Param.set, setPosition, connectInput) never reach the history, so each edit is made
the way a user makes it: typing into a spin box of the node's panel, dragging the node in the
graph, deleting keys with Ctrl+A/Backspace in the curve editor and in the dope sheet, and
connecting the viewer with the "1" key. Ctrl+Z/Ctrl+Shift+Z go through QTest, which sends the
shortcut-override step that QApplication.sendEvent() skips (see m55_input_pipes.py's
trigger_undo()), so the application-wide shortcut is what is being tested. Roto shapes are not
drawn: that needs synthetic pen strokes in the GL viewer.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 400
BLUR = "net.sf.cimg.CImgBlur"
KEYED_PARAM = "size"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QCursor, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLineEdit, QMainWindow, QTabWidget, QTreeWidget

    import guitest as gt
    from guitest import check, report

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # ---- plumbing --------------------------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def undo_enabled():
        a = edit_menu_action("undo")
        return a is not None and a.isEnabled()

    def redo_enabled():
        a = edit_menu_action("redo")
        return a is not None and a.isEnabled()

    def trigger(prefix):
        a = edit_menu_action(prefix)
        if not check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
            return False
        a.trigger()
        QApplication.processEvents()
        return True

    def centre(widget):
        return widget.mapToGlobal(widget.rect().center())

    def shortcut(widget, redo):
        """Ctrl+Z (or Ctrl+Shift+Z) with the mouse over widget, sent to widget."""
        QCursor.setPos(centre(widget))
        QApplication.processEvents()
        mods = Qt.ControlModifier | (Qt.ShiftModifier if redo else Qt.NoModifier)
        if QTest is None:
            report("NOTE PySide6.QtTest is unavailable: triggering the Edit menu action instead of the shortcut")
            trigger("redo" if redo else "undo")
            return
        QTest.keyClick(widget, Qt.Key_Z, mods)
        QApplication.processEvents()

    # ---- panels ----------------------------------------------------------------------------

    def panel_of(name):
        """The settings panel of the node with that script name, found by the label in its
        header's name field."""
        node = app.getNode(name)
        if node is None:
            return None
        label = node.getLabel()
        for p in gt.widgets_of("NodeSettingsPanel"):
            for le in p.findChildren(QLineEdit):
                if gt.class_name(le) == "LineEdit" and le.text() == label:
                    return p
        return None

    def panel_button(name, what):
        p = panel_of(name)
        if p is None:
            return None
        for b in p.findChildren(QAbstractButton):
            if ("%s the last change" % what) in b.toolTip():
                return b
        return None

    def type_into_panel(name, value):
        """Types value into the first visible, enabled spin box of the node's panel (a parameter on
        the open Controls page) and presses Return, which is how KnobGui pushes a value command.
        Hidden pages hold spin boxes too, e.g. the label's font size on the Node page."""
        p = panel_of(name)
        if not check(p is not None, "found the settings panel of " + name):
            return False
        boxes = [w for w in p.findChildren(QLineEdit)
                 if gt.class_name(w) == "SpinBox" and w.isEnabled() and w.isVisible()]
        if not check(len(boxes) > 0, "the settings panel of %s has a visible, enabled spin box" % name):
            return False
        box = boxes[0]
        box.setFocus()
        box.setText(str(value))
        send_key(box, Qt.Key_Return)
        return True

    def show_page(name, page_label):
        p = panel_of(name)
        if p is None:
            return False
        for tabs in p.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == page_label:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def font_size_box(name):
        p = panel_of(name)
        if p is None:
            return None
        for w in p.findChildren(QLineEdit):
            if gt.class_name(w) == "SpinBox" and w.isVisible() and w.isEnabled() and "Font size" in w.toolTip():
                return w
        return None

    def label_text(name):
        node = app.getNode(name)
        p = node.getParam("userTextArea") if node is not None else None
        return p.getValue() if p is not None else None

    def snapshot(name):
        node = app.getNode(name)
        out = {}
        if node is None:
            return out
        for p in node.getParams():
            getv = getattr(p, "getValue", None)
            if getv is None:
                continue
            try:
                n = p.getNumDimensions()
            except Exception:
                continue
            for d in range(n):
                try:
                    out[(p.getScriptName(), d)] = getv(d)
                except Exception:
                    try:
                        out[(p.getScriptName(), d)] = getv()
                    except Exception:
                        pass
        return out

    def num_keys(name):
        node = app.getNode(name)
        if node is None:
            return -1
        p = node.getParam(KEYED_PARAM)
        return p.getNumKeys(0) if p is not None else -1

    def position(name):
        node = app.getNode(name)
        return tuple(node.getPosition()) if node is not None else None

    def viewer_input_name(viewer_name):
        viewer = app.getNode(viewer_name)
        if viewer is None:
            return None
        src = viewer.getInput(0)
        return src.getScriptName() if src is not None else None

    # ---- graph -----------------------------------------------------------------------------

    def frame_on(graph, name):
        node = app.getNode(name)
        x, y = node.getPosition()
        w, h = node.getSize()
        graph.resetTransform()
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()
        return graph.mapFromScene(QPointF(x + w / 2.0, y + h / 2.0))

    def drag_node(graph, name, dx, dy):
        vp = graph.viewport()
        # QGraphicsItem::isActive() is false in an inactive window, so NodeGraph::hasItemNearbyMouse()
        # skips every node; typing into a panel just before leaves the main window inactive.
        win = graph.window()
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)
        start = frame_on(graph, name)
        x0, y0 = float(start.x()), float(start.y())
        def mouse(kind, x, y, button, buttons):
            QApplication.sendEvent(vp, QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
            QApplication.processEvents()

        mouse(QEvent.MouseMove, x0, y0, Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseButtonPress, x0, y0, Qt.LeftButton, Qt.LeftButton)
        steps_n = 5
        for i in range(1, steps_n + 1):
            mouse(QEvent.MouseMove, x0 + dx * i / steps_n, y0 + dy * i / steps_n, Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, x0 + dx, y0 + dy, Qt.LeftButton, Qt.NoButton)

    # ---- curve editor and dope sheet -------------------------------------------------------

    from PySide6.QtCore import QMetaObject

    def curve_editor_parts():
        # Several CurveEditor wrappers can exist (stale ones from closed panels); keep only live ones.
        from shiboken6 import isValid
        best_tree, best_widget = None, None
        for ce in gt.widgets_of("CurveEditor"):
            if not isValid(ce):
                continue
            trees = [t for t in ce.findChildren(QTreeWidget) if isValid(t) and t.objectName() == "tree"]
            widgets = [w for w in gt.widgets_of("CurveWidget") if isValid(w) and ce.isAncestorOf(w)]
            if trees and widgets:
                if best_tree is None or ce.isVisible():
                    best_tree, best_widget = trees[0], widgets[0]
        return best_tree, best_widget

    def tree_op(method):
        # Fetch the tree and call it in the same expression chain, never holding the wrapper across calls.
        from shiboken6 import isValid
        for ce in gt.widgets_of("CurveEditor"):
            for t in ce.findChildren(QTreeWidget):
                if isValid(t) and t.objectName() == "tree":
                    getattr(t, method)()
                    return True
        return False

    def tree_select_node(node_name):
        # Select only the named node's row and its descendants, so Ctrl+A acts on that node's curves.
        from shiboken6 import isValid
        from PySide6.QtCore import QItemSelectionModel
        for ce in gt.widgets_of("CurveEditor"):
            for t in ce.findChildren(QTreeWidget):
                if not (isValid(t) and t.objectName() == "tree"):
                    continue
                model = t.model()
                sel = t.selectionModel()
                sel.clear()
                def walk(parent):
                    for r in range(model.rowCount(parent)):
                        idx = model.index(r, 0, parent)
                        yield idx
                        for sub in walk(idx):
                            yield sub
                for top in range(model.rowCount()):
                    top_idx = model.index(top, 0)
                    if node_name in str(model.data(top_idx)):
                        flags = QItemSelectionModel.Select | QItemSelectionModel.Rows
                        sel.select(top_idx, flags)
                        for idx in walk(top_idx):
                            sel.select(idx, flags)
                        return True
        return False

    def dope_sheet():
        # DopeSheetEditor has no Q_OBJECT, so it is found through its view; keys the view ignores
        # reach the editor's key handler.
        views = [v for v in gt.widgets_of("DopeSheetView") if v.isVisible()] or gt.widgets_of("DopeSheetView")
        return views[0] if views else None

    def viewer_widget():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        viewers = [n for n in app.getChildren() if n.getPluginID() == "fr.inria.built-in.Viewer"]
        if not viewers:
            viewers = [create("fr.inria.built-in.Viewer", 0, 400)]
        gt.require(len(viewers) > 0 and viewers[0] is not None, "the project has a viewer")
        viewer = viewers[0].getScriptName()
        a = create(BLUR, 0, 0).getScriptName()
        b = create(BLUR, 300, 0).getScriptName()
        c = create(BLUR, 600, 0).getScriptName()
        d = create(BLUR, 900, 0).getScriptName()
        for name in (c, d):
            p = app.getNode(name).getParam(KEYED_PARAM)
            gt.require(p is not None and hasattr(p, "setValueAtTime"), "%s has an animatable %s" % (name, KEYED_PARAM))
            p.setValueAtTime(3.0, 1, 0)
            p.setValueAtTime(9.0, 10, 0)
        app.clearSelection()
        yield ("sleep", SETTLE_MS)
        check(not undo_enabled(), "the history is empty before any user edit")

        a_before = snapshot(a)
        b_pos_before = position(b)
        viewer_before = viewer_input_name(viewer)
        gt.require(viewer_before != b, "the viewer is not already connected to " + b)

        # ---- the five edits, each made in a different pane.
        gt.section("edits")
        type_into_panel(a, 7)
        yield ("sleep", SETTLE_MS)
        gt.require(snapshot(a) != a_before, "1: typing into %s's panel changed a parameter" % a)
        a_after = snapshot(a)
        report("NOTE edit 1 changed %r; the Edit menu offers %r"
               % (sorted(k for k in a_after if a_after.get(k) != a_before.get(k)),
                  edit_menu_action("undo").text() if edit_menu_action("undo") else None))
        check(undo_enabled(), "1: the panel edit is in the history")

        graph = root_graph()
        drag_node(graph, b, 60, 40)
        yield ("sleep", SETTLE_MS)
        gt.require(position(b) != b_pos_before, "2: dragging %s in the graph moved it" % b,
                   "position %r" % (position(b),))
        b_pos_after = position(b)

        ok = yield ("until", lambda: all(x is not None for x in curve_editor_parts()), 10.0)
        yield ("sleep", SETTLE_MS)
        tree, curve_widget = curve_editor_parts()
        gt.require(tree is not None and curve_widget is not None, "found the curve editor's tree and curve widget")
        tree_op("expandAll")
        QApplication.processEvents()
        tree, curve_widget = curve_editor_parts()
        gt.require(tree is not None and curve_widget is not None, "the curve editor's tree and curve widget survive expandAll")
        gt.require(tree_select_node(c), "selected %s in the curve editor tree" % c)
        QApplication.processEvents()
        send_key(curve_widget, Qt.Key_A, Qt.ControlModifier)
        send_key(curve_widget, Qt.Key_Backspace)
        yield ("sleep", SETTLE_MS)
        gt.require(num_keys(c) == 0, "3: Ctrl+A, Backspace in the curve editor removed %s's keys" % c,
                   "%d keys left" % num_keys(c))
        gt.require(num_keys(d) == 2, "3: the curve editor edit left %s's keys alone" % d)

        sheet = dope_sheet()
        gt.require(sheet is not None, "found the dope sheet editor")
        send_key(sheet, Qt.Key_A, Qt.ControlModifier)
        send_key(sheet, Qt.Key_Backspace)
        yield ("sleep", SETTLE_MS)
        gt.require(num_keys(d) == 0, "4: Ctrl+A, Backspace in the dope sheet removed %s's keys" % d,
                   "%d keys left" % num_keys(d))

        app.setSelection([app.getNode(b)])
        QApplication.processEvents()
        send_key(main_window(), Qt.Key_1)
        yield ("sleep", SETTLE_MS)
        gt.require(viewer_input_name(viewer) == b, "5: the 1 key connected the viewer to " + b,
                   "viewer input is %r" % viewer_input_name(viewer))
        app.clearSelection()
        report("NOTE roto shapes are not covered: drawing one needs synthetic pen strokes in the viewer")

        def state():
            return (snapshot(a) == a_after,
                    position(b) == b_pos_after,
                    num_keys(c) == 0,
                    num_keys(d) == 0,
                    viewer_input_name(viewer) == b)

        def expected(done):
            return tuple(i < done for i in range(5))

        check(state() == expected(5), "all five edits are in place")

        # ---- global undo and redo, with the mouse over a different pane each time.
        gt.section("global-order")
        panes = [("node graph", root_graph()), ("curve editor", curve_widget), ("dope sheet", sheet),
                 ("panel of " + a, panel_of(a)), ("viewer", viewer_widget())]
        panes = [(label, w) for (label, w) in panes if w is not None]
        gt.require(len(panes) >= 3, "found at least three panes to hover", "%d" % len(panes))
        for i in range(5):
            label, w = panes[i % len(panes)]
            shortcut(w, redo=False)
            yield ("sleep", SETTLE_MS)
            check(state() == expected(4 - i),
                  "Ctrl+Z #%d over the %s undoes edit %d only (state %r)" % (i + 1, label, 5 - i, state()))
        check(snapshot(a) == a_before, "after undoing everything %s's parameters are back" % a)
        check(position(b) == b_pos_before, "after undoing everything %s is back in place" % b)
        check(num_keys(c) == 2 and num_keys(d) == 2, "after undoing everything the keys are back")
        check(viewer_input_name(viewer) == viewer_before, "after undoing everything the viewer input is back")
        check(not undo_enabled(), "nothing is left to undo")
        for i in range(5):
            label, w = panes[(len(panes) - 1 - i) % len(panes)]
            shortcut(w, redo=True)
            yield ("sleep", SETTLE_MS)
            check(state() == expected(i + 1),
                  "Ctrl+Shift+Z #%d over the %s redoes edit %d only (state %r)" % (i + 1, label, i + 1, state()))
        check(not redo_enabled(), "nothing is left to redo")

        # ---- a panel's buttons step only its own node's edits.
        gt.section("panel-buttons")
        b_before = snapshot(b)
        type_into_panel(b, 5)
        yield ("sleep", SETTLE_MS)
        gt.require(snapshot(b) != b_before, "typing into %s's panel changed a parameter" % b)
        b_after = snapshot(b)

        undo_a = panel_button(a, "Undo")
        redo_a = panel_button(a, "Redo")
        gt.require(undo_a is not None and redo_a is not None, "found the undo/redo buttons of %s's panel" % a)
        check(undo_a.isEnabled(), "%s's undo button is enabled although %s was edited afterwards" % (a, b))
        undo_a.click()
        yield ("sleep", SETTLE_MS)
        check(snapshot(a) == a_before, "%s's undo button reverted %s's edit" % (a, a))
        check(snapshot(b) == b_after, "%s's undo button left %s's later edit alone" % (a, b))
        check(position(b) == b_pos_after and num_keys(c) == 0 and num_keys(d) == 0
              and viewer_input_name(viewer) == b, "%s's undo button left every other edit alone" % a)
        check(redo_a.isEnabled(), "%s's redo button is enabled after its undo" % a)
        redo_a.click()
        yield ("sleep", SETTLE_MS)
        check(snapshot(a) == a_after, "%s's redo button re-applied %s's edit" % (a, a))
        check(snapshot(b) == b_after, "%s's redo button left %s's edit alone" % (a, b))

        # ---- delete A, undo the delete: its panel buttons still step its edits.
        gt.section("delete-then-panel")
        app.setSelection([app.getNode(a)])
        QApplication.processEvents()
        send_key(root_graph(), Qt.Key_Backspace)
        yield ("sleep", SETTLE_MS)
        check(app.getNode(a) is None, "Backspace deleted " + a)
        trigger("undo")
        yield ("sleep", SETTLE_MS)
        gt.require(app.getNode(a) is not None, "undo restored " + a)
        check(snapshot(a) == a_after, "the restored %s keeps its edit" % a)
        undo_a = panel_button(a, "Undo")
        redo_a = panel_button(a, "Redo")
        gt.require(undo_a is not None and redo_a is not None,
                   "found the undo/redo buttons of the restored %s's panel" % a)
        check(undo_a.isEnabled(), "the restored %s's undo button is enabled" % a)
        undo_a.click()
        yield ("sleep", SETTLE_MS)
        check(snapshot(a) == a_before, "the restored %s's undo button reverted its edit" % a)
        check(redo_a.isEnabled(), "the restored %s's redo button is enabled after its undo" % a)
        redo_a.click()
        yield ("sleep", SETTLE_MS)
        check(snapshot(a) == a_after, "the restored %s's redo button re-applied its edit" % a)
        gt.shot("m114-global-undo.png")

        # ---- the label's font size does not render, but it is part of the project.
        gt.section("label-font-size")
        gt.require(show_page(a, "Node"), "showed the Node page of " + a)
        box = font_size_box(a)
        gt.require(box is not None, "found the label's font size box on %s's Node page" % a)
        label_before = label_text(a)
        new_size = 17 if str(box.text()).strip() != "17" else 19
        box.setFocus()
        box.setText(str(new_size))
        send_key(box, Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        label_after = label_text(a)
        gt.require(label_after != label_before and str(new_size) in label_after,
                   "typing %d into the font size box changed %s's label" % (new_size, a),
                   "label %r" % (label_after,))
        check(undo_enabled(), "the font size edit is in the history")
        shortcut(panel_of(a), redo=False)
        yield ("sleep", SETTLE_MS)
        check(label_text(a) == label_before, "Ctrl+Z restored %s's label (%r)" % (a, label_text(a)))
        shortcut(panel_of(a), redo=True)
        yield ("sleep", SETTLE_MS)
        check(label_text(a) == label_after, "Ctrl+Shift+Z re-applied the font size (%r)" % (label_text(a),))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
