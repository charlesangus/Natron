"""The Retouch node's panel and its viewer interaction.

Creates a Retouch node on a dark Constant with 3 scales and Current Scale 2, and adds a heal
ellipse by script, which takes the node's Heal and scale 2. Opens the panel and checks that it fits
a 1920x1080 screen, that its curve list has the "Scale / Algorithm" column reading "S2 / Heal",
and that the viewer draws the shape's dashed source outline. Drags the outline in the viewer and
checks that the shape's Source Offset follows the drag. With Current Scale 3, right-clicks the
shape in the curve list, checks the menu offers "Copy to Scale 3" and "Move to Scale 3", moves
the shape and checks its scale and column, then undoes the move with Edit > Undo. Adds a
rectangle by script, presses Z in the viewer and checks every corner gets the equal, opposite
handles of the circle through the corners, then undoes it. Picks the Brush tool, drags a brush
stroke across the viewer and checks it becomes a "Brush1" shape in the curve list with the node's
Brush Size, drawn with its dashed source line, and that the brush outline stays drawn around the
pen in the middle of the stroke. Sets a small carried offset with a Ctrl+drag before the brush stroke so its source line is visible, presses Reset Clone Offset later, then sets the source offset as on a
RotoPaint clone stroke: Ctrl held, the pen dragged from S to D with the Ellipse Tool. Draws an
ellipse and checks its Source Offset is S - D, so a shape drawn from D takes its source at the
clicked S, then draws a second ellipse and checks it keeps the offset. Screenshots of the panel and
the viewer at each step go to $NATRON_GUI_TEST_OUT.

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
MIN_BRUSH_LINE_PIXELS = 40
BRUSH_SOURCE_PIXELS = 8  # widget pixels between a brush stroke and its source
BRUSH_BAND = 15  # widget pixels around the stroke's path that its source line may fall in
OUTLINE_GAP = 8  # widget pixels between the shape's handles and the source outline's columns
# The rectangle Z smooths: its corner and side, in canonical pixels.
SQUARE = (1300.0, 300.0)
SQUARE_SIZE = 300.0
# The brush stroke, in canonical pixels, clear of the ellipse, the square and their sources.
BRUSH_FROM = (300.0, 150.0)
BRUSH_TO = (900.0, 230.0)
BRUSH_SIZE = 60.0
MIN_OUTLINE_ANGLES = 0.6  # the share of directions around the pen where the brush outline is found
# The Ctrl+drag that sets the source offset, and the ellipses drawn after it, in canonical pixels.
OFFSET_FROM = (1500.0, 800.0)
OFFSET_TO = (1200.0, 700.0)
ELLIPSE_DRAGS = (((1100.0, 650.0), (1180.0, 600.0)), ((1600.0, 300.0), (1680.0, 250.0)))

try:
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMetaObject, QModelIndex, QPoint, QPointF, QRect, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QScrollArea, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

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
                        return hold(panel)
                except RuntimeError:
                    continue
        return None

    def curve_tree(panel):
        for tree in panel.findChildren(QTreeWidget):
            parent = tree.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return hold(tree)
                parent = parent.parentWidget()
        return None

    def column_of(tree, header):
        m = hold(tree.model())
        for c in range(m.columnCount()):
            if m.headerData(c, Qt.Horizontal) == header:
                return c
        return -1

    def row_index(tree, name):
        """The column-0 index of name's row, read through the model: Natron rebuilds the tree's
        items, so a QTreeWidgetItem wrapper kept across a rebuild is already deleted."""
        m = hold(tree.model())

        def walk(parent):
            for r in range(m.rowCount(parent)):
                index = m.index(r, 0, parent)
                if index.data() == name:
                    return index
                found = walk(index)
                if found is not None:
                    return found
            return None
        return walk(QModelIndex())

    def shape_column_text(node, name):
        try:
            panel = panel_of(node)
            tree = curve_tree(panel) if panel is not None else None
            if tree is None:
                return None
            col = column_of(tree, COLUMN_HEADER)
            index = row_index(tree, name)
            if col < 0 or index is None:
                return None
            return index.siblingAtColumn(col).data()
        except RuntimeError:
            return None

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

    def dim_red_pixels(img):
        """The overlay's red at half alpha too, as an unselected shape's antialiased dashes are
        drawn: too dark for red_pixels, still well clear of the grey plate."""
        points = set()
        if img is None:
            return points
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.red() > 35 and c.red() - c.green() > 20 and c.red() - c.blue() > 20:
                    points.add((x, y))
        return points

    def send_key(widget, key):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def send_mouse(widget, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        local = QPointF(pos[0], pos[1])
        glob = QPointF(widget.mapToGlobal(QPoint(int(pos[0]), int(pos[1]))))
        QApplication.sendEvent(widget, QMouseEvent(kind, local, glob, button, buttons, modifiers))
        QApplication.processEvents()

    def drag(widget, start, end, modifiers=Qt.NoModifier, steps=8):
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, modifiers)
        for i in range(1, steps + 1):
            f = i / float(steps)
            p = (start[0] + f * (end[0] - start[0]), start[1] + f * (end[1] - start[1]))
            send_mouse(widget, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton, modifiers)
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, modifiers)

    def outline_angles(img, centre, radius):
        """The share of 72 directions from centre with a near-white pixel within 3 widget pixels
        of radius: the brush outline, drawn white over the dark plate."""
        import math
        if img is None:
            return 0.0
        hits = 0
        for k in range(72):
            a = 2.0 * math.pi * k / 72.0
            for dr in range(-3, 4):
                x = int(round(centre[0] + (radius + dr) * math.cos(a)))
                y = int(round(centre[1] + (radius + dr) * math.sin(a)))
                if 0 <= x < img.width() and 0 <= y < img.height():
                    c = img.pixelColor(x, y)
                    if min(c.red(), c.green(), c.blue()) > 150:
                        hits += 1
                        break
        return hits / 72.0

    def item_names(ctx):
        names = set()
        pending = list(ctx.getBaseLayer().getChildren())
        while pending:
            item = pending.pop()
            names.add(item.getScriptName())
            if hasattr(item, "getChildren"):
                pending.extend(item.getChildren())
        return names

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
        # The context selects the new shape, slaving its knobs to the node's knobs of the same name:
        # the shape's knob reads the node's value, so the offset is set on the node's knob.
        node.getParam("sourceOffset").set(OFFSET_X, 0.0)
        check(offset.getValue(0) == OFFSET_X and offset.getValue(1) == 0.0, "the shape's Source Offset reads (%r, 0)" % OFFSET_X)
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
        overlay = {"red": []}

        def overlay_drawn():
            overlay["red"] = sorted(red_pixels(grab_viewer()) - baseline)
            return len(overlay["red"]) > MIN_OUTLINE_PIXELS

        yield ("until", overlay_drawn, POLL_TIMEOUT_S)
        red = overlay["red"]
        report("NOTE overlay pixels %d" % len(red))
        require(len(red) > MIN_OUTLINE_PIXELS, "the shape and its dashed source outline are drawn")
        xs = [p[0] for p in red]
        ys = [p[1] for p in red]
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        report("NOTE overlay box (%d, %d)-(%d, %d)" % (left, top, right, bottom))
        # The source outline is the rightmost run of red columns, leaving out the rows of the dashed
        # line joining the two centres. The shape's tangent handles stick out past its outline, so
        # only the source outline gives the zoom.
        mid = (top + bottom) / 2.0
        columns = sorted(set(p[0] for p in red if abs(p[1] - mid) > 3))
        runs = [[columns[0], columns[0]]] if columns else []
        for x in columns[1:]:
            if x - runs[-1][1] > OUTLINE_GAP:
                runs.append([x, x])
            else:
                runs[-1][1] = x
        report("NOTE red column runs %r" % (runs,))
        require(len(runs) >= 2, "the dashed source outline is drawn apart from the shape")
        s_left, s_right = runs[-1]
        s_ys = [p[1] for p in red if s_left <= p[0] <= s_right]
        s_top, s_bottom = min(s_ys), max(s_ys)
        scale = ((s_right - s_left) + (s_bottom - s_top)) / (4.0 * RADIUS)
        source_centre = ((s_left + s_right) / 2.0, (s_top + s_bottom) / 2.0)
        report("NOTE source outline (%d, %d)-(%d, %d), %.4f widget px per canonical px"
               % (s_left, s_top, s_right, s_bottom, scale))
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
        index = row_index(tree, name)
        require(index is not None, "the shape has a row in the curve list")
        tree.selectionModel().select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        tree.scrollTo(index)
        QApplication.processEvents()
        index = row_index(tree, name)
        require(index is not None, "the shape keeps its row in the curve list")
        rect = tree.visualRect(index)
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

        gt.section("smooth")
        square = ctx.createRectangle(SQUARE[0], SQUARE[1], SQUARE_SIZE, 1.0)
        require(square is not None, "created a rectangle")
        count = square.getNumControlPoints()
        corners = [square.getControlPointPosition(i, 1.0) for i in range(count)]
        report("NOTE rectangle points %r" % (corners,))
        check(all(c[2] == c[0] and c[3] == c[1] and c[4] == c[0] and c[5] == c[1] for c in corners),
              "the rectangle's corners are cusps")
        yield ("sleep", SETTLE_MS)
        send_key(visible_viewer(), Qt.Key_Z)
        yield ("sleep", SETTLE_MS)
        smoothed = [square.getControlPointPosition(i, 1.0) for i in range(count)]
        report("NOTE smoothed points %r" % (smoothed,))
        radius = SQUARE_SIZE / 2.0 * 2.0 ** 0.5
        handle = 4.0 / 3.0 * (2.0 ** 0.5 - 1.0) * radius

        def symmetric(c):
            lx, ly, rx, ry = c[2] - c[0], c[3] - c[1], c[4] - c[0], c[5] - c[1]
            return abs(lx + rx) < 1e-6 and abs(ly + ry) < 1e-6 and abs((rx * rx + ry * ry) ** 0.5 - handle) < 1e-6

        check(len(smoothed) == count and all(symmetric(c) for c in smoothed),
              "Z gives every corner of the selected rectangle equal, opposite handles %.3f long" % handle)
        screen_crop("retouch_viewer_smoothed.png", visible_viewer())
        undo = edit_menu_action("undo")
        if check(undo is not None and undo.isEnabled(), "Edit > Undo is enabled after the smooth"):
            undo.trigger()
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            back = [square.getControlPointPosition(i, 1.0) for i in range(count)]
            check(all(abs(a - b) < 1e-6 for c, d in zip(back, corners) for a, b in zip(c, d)),
                  "one undo puts the cusps back")

        gt.section("brush")
        # Move to Scale 3 left the node on Current Scale 3, the scale new shapes go on.
        node.getParam("currentScale").setValue(2)
        node.getParam("brushSize").setValue(BRUSH_SIZE)
        tool = node.getParam("DrawBrushTool")
        require(tool is not None, "the Retouch toolbar has the Draw Brush Tool")
        tool.trigger()
        yield ("sleep", SETTLE_MS)
        # Canonical to widget pixels from the measured source outline, which undo put back.
        origin = (CENTRE[0] + OFFSET_X, CENTRE[1])

        def to_widget(p):
            return (source_centre[0] + (p[0] - origin[0]) * scale, source_centre[1] - (p[1] - origin[1]) * scale)

        w = visible_viewer()
        start = to_widget(BRUSH_FROM)
        names_before_offset = item_names(ctx)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
        drag(w, (start[0] + BRUSH_SOURCE_PIXELS, start[1] + BRUSH_SOURCE_PIXELS), start, Qt.ControlModifier)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(item_names(ctx) == names_before_offset, "the Ctrl+drag before the stroke makes no shape")
        before = dim_red_pixels(grab_viewer())
        end = to_widget(BRUSH_TO)
        send_mouse(w, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 17):
            f = i / 16.0
            p = (start[0] + f * (end[0] - start[0]), start[1] + f * (end[1] - start[1]) - 20.0 * f * (1.0 - f))
            send_mouse(w, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
            if i == 8:
                # Mid-stroke, with the button still down.
                yield ("sleep", SETTLE_MS)
                mid = grab_viewer()
                share = outline_angles(mid, p, BRUSH_SIZE / 2.0 * scale)
                report("NOTE brush outline mid-stroke around %r: %.2f of directions" % (p, share))
                check(share >= MIN_OUTLINE_ANGLES, "the brush outline stays drawn around the pen while painting")
                if mid is not None:
                    mid.save(os.path.join(gt.OUT, "retouch_viewer_mid_stroke.png"))
        send_mouse(w, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", 2 * SETTLE_MS)
        brush_text = shape_column_text(node, "Brush1")
        report("NOTE brush column text %r" % (brush_text,))
        check(brush_text is not None, "the stroke is the Brush1 shape in the curve list")
        check(brush_text == "S2 / Heal", "the brush takes the node's Heal and scale 2")
        brush = ctx.getItemByName("Brush1")
        if brush is not None and brush.getParam("shapeBrushSize") is not None:
            check(brush.getParam("shapeBrushSize").getValue() == BRUSH_SIZE, "the brush takes the node's Brush Size")
        else:
            report("NOTE the brush item has no Python wrapper with params")
        # The brush is not selected, so its source line is dashed at half alpha, offset from the
        # stroke by the carried source offset.
        band_x = (min(start[0], end[0]) - BRUSH_BAND, max(start[0], end[0]) + BRUSH_BAND)
        band_y = (min(start[1], end[1]) - BRUSH_BAND, max(start[1], end[1]) + BRUSH_BAND)
        added = [p for p in dim_red_pixels(grab_viewer()) - before
                 if band_x[0] <= p[0] <= band_x[1] and band_y[0] <= p[1] <= band_y[1]]
        report("NOTE red pixels added along the brush: %d" % len(added))
        check(len(added) > MIN_BRUSH_LINE_PIXELS, "the brush's dashed source line is drawn")
        screen_crop("retouch_viewer_brush.png", visible_viewer())
        gt.shot("retouch_screen_brush.png")

        gt.section("source-offset")
        reset = node.getParam("resetCloneOffsetButton")
        require(reset is not None, "the Retouch viewer toolbar has Reset Clone Offset")
        reset.trigger()
        ellipse_tool = node.getParam("DrawEllipseTool")
        require(ellipse_tool is not None, "the Retouch toolbar has the Ellipse Tool")
        ellipse_tool.trigger()
        yield ("sleep", SETTLE_MS)
        w = visible_viewer()
        names_before = item_names(ctx)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
        drag(w, to_widget(OFFSET_FROM), to_widget(OFFSET_TO), Qt.ControlModifier)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(item_names(ctx) == names_before, "the Ctrl+drag sets the offset and makes no shape")
        want = (OFFSET_FROM[0] - OFFSET_TO[0], OFFSET_FROM[1] - OFFSET_TO[1])
        tol = 3.0 / scale
        for k, (a, b) in enumerate(ELLIPSE_DRAGS):
            ellipse_tool.trigger()
            yield ("sleep", SETTLE_MS)
            before = item_names(ctx)
            drag(visible_viewer(), to_widget(a), to_widget(b))
            yield ("sleep", SETTLE_MS)
            made = sorted(item_names(ctx) - before)
            report("NOTE ellipse %d made %r" % (k + 1, made))
            if not check(len(made) == 1, "drawing ellipse %d makes one shape" % (k + 1)):
                continue
            made_offset = ctx.getItemByName(made[0]).getParam("sourceOffset")
            got = (made_offset.getValue(0), made_offset.getValue(1))
            report("NOTE ellipse %d source offset %r, expected %r within %.1f" % (k + 1, got, want, tol))
            if k == 0:
                check(abs(got[0] - want[0]) <= tol and abs(got[1] - want[1]) <= tol,
                      "the shape's source lands at the Ctrl+drag's press, as on a RotoPaint clone stroke")
            else:
                check(abs(got[0] - want[0]) <= tol and abs(got[1] - want[1]) <= tol,
                      "the next shape keeps the offset")
        screen_crop("retouch_viewer_source_offset.png", visible_viewer())

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
