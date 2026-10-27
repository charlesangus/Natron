"""The Retouch node's panel and its viewer interaction.

Creates a Retouch node on a dark Constant with 3 scales and Current Scale 2, and adds a heal
ellipse by script, which takes the node's Heal and scale 2. Opens the panel and checks that it fits
a 1920x1080 screen, that its curve list has the "Scale / Algorithm" column reading "S2 / Heal",
and that the viewer draws the shape's dashed source outline. Drags the outline in the viewer and
checks that the shape's Source Offset follows the drag. With Current Scale 3, right-clicks the
shape in the curve list, checks the menu offers "Copy to Scale 3" and "Move to Scale 3", moves
the shape and checks its scale and column, then undoes the move with Edit > Undo. Screenshots of
the panel and the viewer at each step go to $NATRON_GUI_TEST_OUT.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m211-shots/out -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m211_retouch.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
PLUGIN_ID = "Retouch"
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
MAX_WIDTH = 1920
MAX_HEIGHT = 1080
COLUMN_HEADER = "Scale / Algorithm"
# The ellipse, in canonical pixels: centre, radius, and the source offset set by script (dy 0 so
# the shape and its source share their rows in the viewer).
CENTRE = (500.0, 540.0)
RADIUS = 150.0
OFFSET_X = 700.0
DRAG = (80, -50)  # widget pixels, y down
MIN_OUTLINE_PIXELS = 100

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QScrollArea, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

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

    def column_of(tree, header):
        h = tree.headerItem()
        for c in range(tree.columnCount()):
            if h.text(c) == header:
                return c
        return -1

    def tree_item(tree, name):
        def walk(item):
            if item.text(0) == name:
                return item
            for i in range(item.childCount()):
                found = walk(item.child(i))
                if found is not None:
                    return found
            return None
        for i in range(tree.topLevelItemCount()):
            found = walk(tree.topLevelItem(i))
            if found is not None:
                return found
        return None

    def shape_column_text(node, name):
        panel = panel_of(node)
        tree = curve_tree(panel) if panel is not None else None
        if tree is None:
            return None
        col = column_of(tree, COLUMN_HEADER)
        item = tree_item(tree, name)
        if col < 0 or item is None:
            return None
        return item.text(col)

    def content_height(panel):
        heights = [a.widget().sizeHint().height() for a in panel.findChildren(QScrollArea) if a.widget() is not None]
        return max(heights) if heights else panel.sizeHint().height()

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def visible_viewer():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    def grab_viewer():
        w = visible_viewer()
        if w is None:
            return None
        QApplication.processEvents()
        origin = w.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), w.width(), w.height()).toImage()

    def red_pixels(img):
        """The roto overlay's red, drawn at full or half alpha over the dark plate."""
        points = set()
        if img is None:
            return points
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.red() > 90 and c.green() < 70 and c.blue() < 70 and c.red() - c.green() > 50:
                    points.add((x, y))
        return points

    def send_key(widget, key):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def send_mouse(widget, kind, pos, button, buttons):
        local = QPointF(pos[0], pos[1])
        glob = QPointF(widget.mapToGlobal(QPoint(int(pos[0]), int(pos[1]))))
        QApplication.sendEvent(widget, QMouseEvent(kind, local, glob, button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().replace("&", "").startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().replace("&", "").startswith(prefix):
                        return a
        return None

    menu_seen = []

    def pick_from_popup(wanted):
        """Records the open context menu's action texts and triggers the one named wanted."""
        popup = QApplication.activePopupWidget()
        if popup is None:
            menu_seen.append(None)
            return
        texts = [a.text() for a in popup.actions()]
        menu_seen.append(texts)
        for a in popup.actions():
            if a.text() == wanted:
                a.trigger()
                break
        popup.close()

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        constant = app.createNode(CONSTANT_ID)
        require(constant is not None, "create a Constant", gt.plugin_problem(CONSTANT_ID))
        constant.getParam("color").set(0.02, 0.02, 0.02, 1.0)
        constant.setPosition(0, 0)
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create Retouch", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 200)
        require(node.connectInput(0, constant), "Retouch reads the Constant")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 400)
        require(viewer.connectInput(0, node), "the viewer shows Retouch")
        node.getParam("scales").setValue(3)
        node.getParam("currentScale").setValue(2)

        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        send_key(visible_viewer(), Qt.Key_F)
        yield ("sleep", SETTLE_MS)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "the Retouch panel opened")
        yield ("sleep", SETTLE_MS)
        baseline = red_pixels(grab_viewer())
        report("NOTE red pixels before any shape: %d" % len(baseline))

        ctx = node.getRotoContext()
        require(ctx is not None, "the Retouch node hosts curves")
        shape = ctx.createEllipse(CENTRE[0], CENTRE[1], 2 * RADIUS, True, 1.0)
        require(shape is not None, "created an ellipse")
        name = shape.getScriptName()
        check(shape.getParam("shapeAlgorithm").getValue() == 1, "the new shape takes the node's Heal")
        check(shape.getParam("shapeScale").getValue() == 2, "the new shape takes Current Scale 2")
        offset = shape.getParam("sourceOffset")
        require(offset is not None, "the shape has a Source Offset")
        check(offset.getValue(0) == 0.0 and offset.getValue(1) == 0.0, "a shape made by script has no source offset")
        offset.set(OFFSET_X, 0.0)
        yield ("sleep", 2 * SETTLE_MS)

        gt.section("panel")
        panel = panel_of(node)
        require(panel is not None, "the Retouch panel is open")
        height = content_height(panel)
        report("NOTE panel content %dx%d" % (panel.width(), height))
        check(panel.width() <= MAX_WIDTH and height <= MAX_HEIGHT, "the panel fits a 1920x1080 screen")
        tree = curve_tree(panel)
        check(tree is not None and column_of(tree, COLUMN_HEADER) >= 0,
              "the curve list has the %s column" % COLUMN_HEADER)
        text = shape_column_text(node, name)
        report("NOTE column text %r" % (text,))
        check(text == "S2 / Heal", "the shape's row reads S2 / Heal")
        screen_crop("retouch_panel.png", panel)
        gt.shot("retouch_screen.png")

        gt.section("source-drag")
        img = grab_viewer()
        red = sorted(red_pixels(img) - baseline)
        report("NOTE overlay pixels %d" % len(red))
        require(len(red) > MIN_OUTLINE_PIXELS, "the shape and its dashed source outline are drawn")
        xs = [p[0] for p in red]
        ys = [p[1] for p in red]
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        scale = (right - left) / (OFFSET_X + 2 * RADIUS)
        source_centre = (right - scale * RADIUS, (top + bottom) / 2.0)
        report("NOTE overlay box (%d, %d)-(%d, %d), %.4f widget px per canonical px" % (left, top, right, bottom, scale))
        require(scale > 0.05, "the viewer shows the shape at a usable zoom")
        screen_crop("retouch_viewer.png", visible_viewer())

        w = visible_viewer()
        start = source_centre
        end = (start[0] + DRAG[0], start[1] + DRAG[1])
        send_mouse(w, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 9):
            f = i / 8.0
            send_mouse(w, QEvent.MouseMove, (start[0] + f * DRAG[0], start[1] + f * DRAG[1]), Qt.NoButton, Qt.LeftButton)
        send_mouse(w, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        got = (offset.getValue(0), offset.getValue(1))
        want = (OFFSET_X + DRAG[0] / scale, -DRAG[1] / scale)
        tol = 3.0 / scale
        report("NOTE source offset after the drag %r, expected %r within %.1f" % (got, want, tol))
        check(abs(got[0] - want[0]) <= tol and abs(got[1] - want[1]) <= tol,
              "dragging the dashed outline moves the source")
        screen_crop("retouch_viewer_dragged.png", visible_viewer())

        undo = edit_menu_action("undo")
        if check(undo is not None and undo.isEnabled(), "Edit > Undo is enabled after the source drag"):
            undo.trigger()
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            back = (offset.getValue(0), offset.getValue(1))
            check(abs(back[0] - OFFSET_X) <= 0.5 and abs(back[1]) <= 0.5,
                  "one undo puts the source offset back (%r)" % (back,))
            redo = edit_menu_action("redo")
            if check(redo is not None and redo.isEnabled(), "Edit > Redo is enabled after the undo"):
                redo.trigger()
                QApplication.processEvents()
                yield ("sleep", SETTLE_MS)
                again = (offset.getValue(0), offset.getValue(1))
                check(abs(again[0] - got[0]) <= 0.5 and abs(again[1] - got[1]) <= 0.5,
                      "redo moves the source again (%r)" % (again,))

        gt.section("move-to-scale")
        node.getParam("currentScale").setValue(3)
        yield ("sleep", SETTLE_MS)
        tree = curve_tree(panel_of(node))
        require(tree is not None, "found the curve list")
        item = tree_item(tree, name)
        require(item is not None, "the shape has a row in the curve list")
        tree.clearSelection()
        item.setSelected(True)
        tree.scrollToItem(item)
        QApplication.processEvents()
        rect = tree.visualItemRect(item)
        click = QPoint(tree.header().sectionViewportPosition(0) + 10, rect.center().y())
        del menu_seen[:]
        QTimer.singleShot(400, lambda: pick_from_popup("Move to Scale 3"))
        QTest.mouseClick(tree.viewport(), Qt.RightButton, Qt.NoModifier, click)
        ok = yield ("until", lambda: len(menu_seen) > 0, POLL_TIMEOUT_S)
        check(ok and menu_seen[0] is not None, "right-clicking the shape opened its menu")
        texts = menu_seen[0] if menu_seen and menu_seen[0] else []
        report("NOTE menu %r" % (texts,))
        check("Copy to Scale 3" in texts, "the menu offers Copy to Scale 3")
        check("Move to Scale 3" in texts, "the menu offers Move to Scale 3")
        yield ("sleep", SETTLE_MS)
        check(shape.getParam("shapeScale").getValue() == 3, "Move to Scale 3 put the shape on scale 3")
        ok = yield ("until", lambda: shape_column_text(node, name) == "S3 / Heal", POLL_TIMEOUT_S)
        check(ok, "the shape's row reads S3 / Heal (%r)" % (shape_column_text(node, name),))
        screen_crop("retouch_panel_moved.png", panel_of(node))

        undo = edit_menu_action("undo")
        if check(undo is not None and undo.isEnabled(), "Edit > Undo is enabled after the move"):
            undo.trigger()
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            check(shape.getParam("shapeScale").getValue() == 2, "undo puts the shape back on scale 2")
            ok = yield ("until", lambda: shape_column_text(node, name) == "S2 / Heal", POLL_TIMEOUT_S)
            check(ok, "after undo the row reads S2 / Heal (%r)" % (shape_column_text(node, name),))
        gt.shot("retouch_screen_undone.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
