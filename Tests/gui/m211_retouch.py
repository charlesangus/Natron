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
clicked S, then draws a second ellipse and checks it keeps the offset. Checks the General tab's
Color is hidden. With nothing selected, Shift+drags with the brush and checks the brush size grows
by half the move right plus up and nothing is painted; sets the shape settings to a Color fill and
paints a second brush that takes them and renders filled. Clicks inside Brush1 with the select tool
and checks it is selected with its own settings loaded, edits them and checks Brush1 alone changes
and renders filled, then deselects and checks the next shape's settings come back. On a second
Retouch over a CheckerBoard, so the source has texture, drags a heal ellipse's source outline,
checks the healed pixels change while the pen is still down, and checks the frame left after the
release matches a fresh render: a third Retouch made with the same shape and the final offset,
which no cache of the dragged node can reach. Screenshots of the panel and
the viewer at each step go to $NATRON_GUI_TEST_OUT.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m211-shots/out -e GUI_TEST_TIMEOUT=420 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m211_retouch.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 420
SETTLE_MS = 500
PLUGIN_ID = "Retouch"
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
CHECKER_ID = "net.sf.openfx.CheckerBoardPlugin"
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
# The Shift+drag that sizes the brush, in widget pixels (x right, y down), and where it starts in
# canonical pixels, clear of every shape.
SIZE_DRAG = (40, -20)
SIZE_DRAG_FROM = (300.0, 950.0)
# The second brush, painted with the shape settings set while nothing is selected.
BRUSH2_FROM = (300.0, 850.0)
BRUSH2_TO = (800.0, 850.0)
BRUSH2_SIZE = 40.0
BRUSH1_EDIT_SIZE = 80.0
ALGORITHM_HEAL = 1
ALGORITHM_FILL = 3
FILL_MODE_COLOR = 1
FILL_GREY = 0.9
MIN_FILLED_GREEN = 120  # the viewer's green over a Fill Color stroke of FILL_GREY, on the 0.02 plate
RENDER_MS = 1500  # between the two grabs that must agree before a frame counts as rendered
COLOUR_SLACK = 3  # 8-bit levels a solve's rounding may move a viewer pixel
SAMPLE_STEP = 6  # widget pixels between samples over a healed disc
MIN_CHANGED_SHARE = 0.1  # of a heal's grey samples that a drag of its source must change

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

        gt.section("general-color")
        color = node.getParam("color")
        check(color is not None and not color.getIsVisible(),
              "the General tab's Color, which a retouch mask ignores, is hidden")
        screen_crop("retouch_panel_shape_settings.png", panel_of(node))

        def deselect_all():
            t = curve_tree(panel_of(node))
            require(t is not None, "found the curve list")
            t.selectionModel().select(QModelIndex(), QItemSelectionModel.Clear)
            QApplication.processEvents()

        def green_at(p):
            """The median green in a 5x5 box: the overlay's red has little green, the plate less."""
            img = grab_viewer()
            if img is None:
                return -1
            values = sorted(img.pixelColor(int(p[0]) + i, int(p[1]) + j).green()
                            for i in range(-2, 3) for j in range(-2, 3))
            return values[len(values) // 2]

        gt.section("brush-size")
        deselect_all()
        yield ("sleep", SETTLE_MS)
        check(not node.getParam("shapeScale").getIsEnabled(0),
              "with nothing selected, Shape Scale, which belongs to a shape, is disabled")
        node.getParam("DrawBrushTool").trigger()
        yield ("sleep", SETTLE_MS)
        w = visible_viewer()
        size_before = node.getParam("brushSize").getValue()
        names_before = item_names(ctx)
        from_w = to_widget(SIZE_DRAG_FROM)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyPress, Qt.Key_Shift, Qt.ShiftModifier))
        drag(w, from_w, (from_w[0] + SIZE_DRAG[0], from_w[1] + SIZE_DRAG[1]), Qt.ShiftModifier)
        QApplication.sendEvent(w, QKeyEvent(QEvent.KeyRelease, Qt.Key_Shift, Qt.NoModifier))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        size_after = node.getParam("brushSize").getValue()
        # Half the pen's move right plus up, in canonical pixels.
        want = size_before + (SIZE_DRAG[0] - SIZE_DRAG[1]) / scale / 2.0
        report("NOTE brush size %r -> %r, expected %r" % (size_before, size_after, want))
        check(abs(size_after - want) <= 1.0 / scale, "Shift+drag with the brush sizes the brush, as in RotoPaint")
        check(abs(node.getParam("shapeBrushSize").getValue() - size_after) <= 1e-6,
              "the Shape Brush Size shows the new size")
        check(item_names(ctx) == names_before, "the Shift+drag paints nothing")

        gt.section("shape-settings")
        node.getParam("currentScale").setValue(0)
        node.getParam("shapeBrushSize").setValue(BRUSH2_SIZE)
        node.getParam("shapeAlgorithm").setValue(ALGORITHM_FILL)
        node.getParam("shapeFillMode").setValue(FILL_MODE_COLOR)
        node.getParam("shapeFillColor").set(FILL_GREY, FILL_GREY, FILL_GREY, 1.0)
        yield ("sleep", SETTLE_MS)
        names_before = item_names(ctx)
        drag(visible_viewer(), to_widget(BRUSH2_FROM), to_widget(BRUSH2_TO), Qt.NoModifier, 16)
        yield ("sleep", 2 * SETTLE_MS)
        made = sorted(item_names(ctx) - names_before)
        report("NOTE the second stroke made %r" % (made,))
        require(len(made) == 1, "the second stroke makes one brush")
        brush2 = ctx.getItemByName(made[0])
        require(brush2 is not None, "the second brush has a Python wrapper")
        check(brush2.getParam("shapeBrushSize").getValue() == BRUSH2_SIZE
              and brush2.getParam("shapeAlgorithm").getValue() == ALGORITHM_FILL
              and brush2.getParam("shapeScale").getValue() == 0,
              "with nothing selected, the shape settings apply to the new stroke")
        brush2_mid = to_widget(((BRUSH2_FROM[0] + BRUSH2_TO[0]) / 2.0, BRUSH2_FROM[1]))
        ok = yield ("until", lambda: green_at(brush2_mid) >= MIN_FILLED_GREEN, POLL_TIMEOUT_S)
        check(ok, "the second brush fills with its colour (green %d)" % green_at(brush2_mid))

        node.getParam("SelectAllTool").trigger()
        yield ("sleep", SETTLE_MS)
        brush1 = ctx.getItemByName("Brush1")
        require(brush1 is not None, "Brush1 has a Python wrapper")
        mid1 = to_widget(((BRUSH_FROM[0] + BRUSH_TO[0]) / 2.0, (BRUSH_FROM[1] + BRUSH_TO[1]) / 2.0))
        # The stroke bowed 5 widget pixels up in its middle.
        mid1 = (mid1[0], mid1[1] - 5.0)
        green_before = green_at(mid1)
        w = visible_viewer()
        send_mouse(w, QEvent.MouseButtonPress, mid1, Qt.LeftButton, Qt.LeftButton)
        send_mouse(w, QEvent.MouseButtonRelease, mid1, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        check(node.getParam("shapeScale").getIsEnabled(0), "a click inside Brush1 selects it")
        check(node.getParam("shapeAlgorithm").getValue() == ALGORITHM_HEAL
              and node.getParam("shapeBrushSize").getValue() == BRUSH_SIZE,
              "selecting the stroke loads its settings (%r, %r)"
              % (node.getParam("shapeAlgorithm").getValue(), node.getParam("shapeBrushSize").getValue()))
        screen_crop("retouch_panel_brush_selected.png", panel_of(node))

        node.getParam("shapeBrushSize").setValue(BRUSH1_EDIT_SIZE)
        node.getParam("shapeAlgorithm").setValue(ALGORITHM_FILL)
        node.getParam("shapeFillMode").setValue(FILL_MODE_COLOR)
        node.getParam("shapeFillColor").set(FILL_GREY, FILL_GREY, FILL_GREY, 1.0)
        node.getParam("shapeScale").setValue(0)
        yield ("sleep", SETTLE_MS)
        check(brush1.getParam("shapeBrushSize").getValue() == BRUSH1_EDIT_SIZE
              and brush1.getParam("shapeAlgorithm").getValue() == ALGORITHM_FILL,
              "editing the shape settings changes the selected stroke")
        check(brush2.getParam("shapeBrushSize").getValue() == BRUSH2_SIZE,
              "editing them leaves the other stroke alone")
        check(node.getParam("brushSize").getValue() == BRUSH2_SIZE,
              "editing them leaves the next shape's settings alone")
        ok = yield ("until", lambda: green_at(mid1) >= MIN_FILLED_GREEN, POLL_TIMEOUT_S)
        report("NOTE green inside Brush1 %d before the edit, %d after" % (green_before, green_at(mid1)))
        check(ok, "the edited stroke renders its new settings")
        screen_crop("retouch_viewer_brush_edited.png", visible_viewer())

        deselect_all()
        yield ("sleep", SETTLE_MS)
        check(node.getParam("shapeBrushSize").getValue() == BRUSH2_SIZE
              and not node.getParam("shapeScale").getIsEnabled(0),
              "deselecting brings back the next shape's settings")
        check(brush1.getParam("shapeBrushSize").getValue() == BRUSH1_EDIT_SIZE,
              "the stroke keeps its edit after the deselection")

        gt.section("heal-drag")
        gt.close_all_panels()
        app.clearSelection()
        checker = app.createNode(CHECKER_ID)
        require(checker is not None, "create a CheckerBoard", gt.plugin_problem(CHECKER_ID))
        checker.setPosition(600, 0)

        def textured_heal(x):
            n = app.createNode(PLUGIN_ID)
            require(n is not None, "create Retouch", gt.plugin_problem(PLUGIN_ID))
            n.setPosition(x, 200)
            require(n.connectInput(0, checker), "Retouch reads the CheckerBoard")
            n.getParam("algorithm").setValue(ALGORITHM_HEAL)
            return n

        def heal_ellipse(n, ox, oy):
            c = n.getRotoContext()
            require(c is not None, "the Retouch node hosts curves")
            e = c.createEllipse(CENTRE[0], CENTRE[1], 2 * RADIUS, True, 1.0)
            require(e is not None, "created an ellipse")
            n.getParam("sourceOffset").set(ox, oy)
            o = e.getParam("sourceOffset")
            if (o.getValue(0), o.getValue(1)) != (ox, oy):
                o.set(ox, oy)
            check(o.getValue(0) == ox and o.getValue(1) == oy, "the ellipse's Source Offset reads (%r, %r)" % (ox, oy))
            return e

        def disc_samples(centre, radius, grey_only=False):
            """Viewer colours on a SAMPLE_STEP grid over a disc in widget pixels; with grey_only, None
            where the colour is not grey, as the overlay's lines are not."""
            img = grab_viewer()
            if img is None:
                return None
            out = []
            r = int(radius)
            for dy in range(-r, r + 1, SAMPLE_STEP):
                for dx in range(-r, r + 1, SAMPLE_STEP):
                    x, y = int(centre[0] + dx), int(centre[1] + dy)
                    if dx * dx + dy * dy > r * r or not (0 <= x < img.width() and 0 <= y < img.height()):
                        continue
                    c = img.pixelColor(x, y)
                    rgb = (c.red(), c.green(), c.blue())
                    if grey_only and (abs(rgb[0] - rgb[1]) > 12 or abs(rgb[1] - rgb[2]) > 12):
                        rgb = None
                    out.append(rgb)
            return out

        def differs(a, b):
            # Two overlay samples agree: the dashed line to the source crosses the disc and stays put.
            if a is None or b is None:
                return (a is None) != (b is None)
            return any(abs(x - y) > COLOUR_SLACK for x, y in zip(a, b))

        def settled(sample):
            """Waits until two samples RENDER_MS apart agree, and returns the last one, or None when
            the viewer never stabilises."""
            previous = None
            for _ in range(int(POLL_TIMEOUT_S * 1000 / RENDER_MS)):
                yield ("sleep", RENDER_MS)
                now = sample()
                if now is not None and previous is not None and len(now) == len(previous) \
                        and not any(differs(a, b) for a, b in zip(now, previous)):
                    return now
                previous = now
            return None

        def show(n, what):
            # connectInput refuses an occupied input.
            if viewer.getInput(0) is not None:
                viewer.disconnectInput(0)
            require(viewer.connectInput(0, n), what)

        healer = textured_heal(600)
        show(healer, "the viewer shows the textured Retouch")
        yield ("sleep", SETTLE_MS)
        plain = red_pixels(grab_viewer())
        app.selectNode(healer, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(healer) is not None, POLL_TIMEOUT_S)
        require(ok, "the textured Retouch's panel opened")
        healed = heal_ellipse(healer, OFFSET_X, 0.0)
        healed_offset = healed.getParam("sourceOffset")
        found = {"red": []}

        def healed_overlay():
            found["red"] = sorted(red_pixels(grab_viewer()) - plain)
            return len(found["red"]) > MIN_OUTLINE_PIXELS

        ok = yield ("until", healed_overlay, POLL_TIMEOUT_S)
        require(ok, "the textured heal's outlines are drawn")
        hred = found["red"]
        hmid = (min(p[1] for p in hred) + max(p[1] for p in hred)) / 2.0
        hcols = sorted(set(p[0] for p in hred if abs(p[1] - hmid) > 3))
        hruns = [[hcols[0], hcols[0]]]
        for x in hcols[1:]:
            if x - hruns[-1][1] > OUTLINE_GAP:
                hruns.append([x, x])
            else:
                hruns[-1][1] = x
        require(len(hruns) >= 2, "the textured heal's source outline is drawn apart from it")
        h_left, h_right = hruns[-1]
        h_ys = [p[1] for p in hred if h_left <= p[0] <= h_right]
        h_scale = ((h_right - h_left) + (max(h_ys) - min(h_ys))) / (4.0 * RADIUS)
        h_source = ((h_left + h_right) / 2.0, (min(h_ys) + max(h_ys)) / 2.0)
        # The shape's centre is the source's less the offset; widget y runs down.
        h_centre = (h_source[0] - OFFSET_X * h_scale, h_source[1])
        h_radius = 0.85 * RADIUS * h_scale
        report("NOTE textured heal centre (%.0f, %.0f), radius %.0f widget px" % (h_centre[0], h_centre[1], h_radius))
        require(h_radius > 3 * SAMPLE_STEP, "the textured heal is large enough on screen to sample")
        before = yield from settled(lambda: disc_samples(h_centre, h_radius, True))
        require(before is not None, "the textured heal renders before the drag")
        screen_crop("retouch_heal_drag_before.png", visible_viewer())

        def changed_share(now):
            pairs = [(a, b) for a, b in zip(before, now or []) if a is not None and b is not None]
            if not pairs:
                return 0.0
            return sum(1 for a, b in pairs if differs(a, b)) / float(len(pairs))

        w = hold(visible_viewer())
        send_mouse(w, QEvent.MouseButtonPress, h_source, Qt.LeftButton, Qt.LeftButton)
        offsets = []
        shares = []
        for half in (1, 2):
            for i in range(1, 5):
                f = (4 * (half - 1) + i) / 8.0
                w = hold(visible_viewer())
                require(w is not None, "the viewer stays on screen during the drag")
                send_mouse(w, QEvent.MouseMove, (h_source[0] + f * DRAG[0], h_source[1] + f * DRAG[1]),
                           Qt.NoButton, Qt.LeftButton)
            offsets.append((healed_offset.getValue(0), healed_offset.getValue(1)))
            ok = yield ("until", lambda: changed_share(disc_samples(h_centre, h_radius, True)) >= MIN_CHANGED_SHARE,
                        POLL_TIMEOUT_S)
            shares.append(changed_share(disc_samples(h_centre, h_radius, True)))
            screen_crop("retouch_heal_drag_%d.png" % half, visible_viewer())
        report("NOTE offsets during the drag %r, changed shares %r" % (offsets, shares))
        check(offsets[0] != (OFFSET_X, 0.0) and offsets[1] != offsets[0], "the source offset follows the pen during the drag")
        check(all(x >= MIN_CHANGED_SHARE for x in shares), "the healed pixels change while the pen is down")
        w = hold(visible_viewer())
        send_mouse(w, QEvent.MouseButtonRelease, (h_source[0] + DRAG[0], h_source[1] + DRAG[1]), Qt.LeftButton, Qt.NoButton)
        final_offset = (healed_offset.getValue(0), healed_offset.getValue(1))
        report("NOTE source offset after the release %r" % (final_offset,))

        # No overlay in either grab, so every sample is the render's.
        gt.close_all_panels()
        released = yield from settled(lambda: disc_samples(h_centre, 1.2 * RADIUS * h_scale))
        require(released is not None, "the frame after the release settles")
        screen_crop("retouch_heal_drag_released.png", visible_viewer())
        fresh = textured_heal(800)
        heal_ellipse(fresh, final_offset[0], final_offset[1])
        gt.close_all_panels()
        show(fresh, "the viewer shows the fresh Retouch")
        reference = yield from settled(lambda: disc_samples(h_centre, 1.2 * RADIUS * h_scale))
        require(reference is not None, "the fresh render settles")
        screen_crop("retouch_heal_drag_fresh.png", visible_viewer())
        mismatched = sum(1 for a, b in zip(released, reference) if differs(a, b))
        report("NOTE %d of %d samples differ from the fresh render" % (mismatched, len(reference)))
        check(len(released) == len(reference) and mismatched == 0,
              "the frame after the release matches a fresh render of the final offset")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
