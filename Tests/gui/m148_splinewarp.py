"""SplineWarp viewer tools: the panel's Role and Linked To columns and warp actions, a destination
drag with a draft frame, open and single-point links, a feathered boundary, a correspondence, and
the Show and Output Warped settings. Then Z smooths a linked curve, the viewer's right-click menu
opens on every click and on a curve runs Duplicate and Link and Link, and the point tool makes a
linked pair of points.

A CheckerBoard feeds a SplineWarp feeding the viewer, with the SplineWarp's settings panel open.
Curves are made through the node's Roto context; the links are made from the curve list's context
menu (Duplicate and Link, Link, Toggle Boundary), which run the same actions as the viewer. The
destination of a closed curve is then dragged in the viewer, with a screenshot taken mid-drag (the
draft frame) and after the pen is up. Saves screenshots of every step.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m148-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m148_splinewarp.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
MENU_MS = 600
# Long enough for an unanswered viewer menu poll to have given up: MENU_MS, then ten 200 ms polls.
MENU_GONE_MS = 3000
DRAG_STEPS = 12
SHOW_SOURCE, SHOW_DESTINATION, SHOW_BOTH = 0, 1, 2
MIN_INK = 40  # pixels on a 2-pixel sampling grid
# The closed curve, a rectangle in canonical coordinates.
RECT = (700.0, 350.0, 1200.0, 750.0)
DRAG_BY = (120, -80)  # viewer pixels: right and up

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QMainWindow, QToolButton, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Python wrappers die with their parent wrapper, so every one used later is held here.
    held = []

    def hold(obj):
        held.append(obj)
        return obj

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
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

    def fresh_tree(node):
        """The node's curve list, found again on every call: a tree wrapper held across a yield can
        be invalidated, and the panel wrapper is held so the tree's wrapper outlives this call."""
        panel = panel_of(node)
        if panel is None:
            return None
        hold(panel)
        return curve_tree(panel)

    def open_panel(view, node):
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")

    def visible_viewer():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    def grab_viewer():
        """A screen grab of the viewer (a widget grab of the GL viewer can come out blank under
        llvmpipe)."""
        w = visible_viewer()
        if w is None:
            return None
        QApplication.processEvents()
        origin = w.mapToGlobal(QPoint(0, 0))
        return QGuiApplication.primaryScreen().grabWindow(
            0, origin.x(), origin.y(), w.width(), w.height()).toImage()

    def coloured_pixels(img, test):
        if img is None:
            return []
        points = []
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                c = img.pixelColor(x, y)
                if test(c.red(), c.green(), c.blue()):
                    points.append((x, y))
        return points

    def is_destination(r, g, b):
        return b > 200 and r < 140 and 100 < g < 200

    def is_source(r, g, b):
        return r > 200 and g < 130 and b < 130

    def is_boundary(r, g, b):
        return r > 200 and g > 170 and b < 110

    def bbox(points):
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        return min(xs), min(ys), max(xs), max(ys)

    def rectangle_edges(img, points, test):
        """The left, top, right and bottom pixel lines of a drawn axis-aligned rectangle. A plain
        bounding box of its ink also takes in the feather bars sticking out of the corners, and a
        press at that box's corner lands on a feather bar instead of the control point."""
        bx0, by0, bx1, by1 = bbox(points)
        rows = {}
        cols = {}
        for y in range(max(0, by0 - 2), min(img.height(), by1 + 3)):
            for x in range(max(0, bx0 - 2), min(img.width(), bx1 + 3)):
                c = img.pixelColor(x, y)
                if test(c.red(), c.green(), c.blue()):
                    rows[y] = rows.get(y, 0) + 1
                    cols[x] = cols.get(x, 0) + 1
        long_rows = [y for y, n in rows.items() if n * 2 >= max(rows.values())]
        long_cols = [x for x, n in cols.items() if n * 2 >= max(cols.values())]
        return min(long_cols), min(long_rows), max(long_cols), max(long_rows)

    def region_difference(a, b, rect):
        """The mean absolute RGB difference of two grabs inside rect (x0, y0, x1, y1)."""
        if a is None or b is None or a.size() != b.size():
            return -1.0
        x0, y0, x1, y1 = rect
        total = 0
        count = 0
        for y in range(max(0, y0), min(a.height(), y1), 2):
            for x in range(max(0, x0), min(a.width(), x1), 2):
                ca = a.pixelColor(x, y)
                cb = b.pixelColor(x, y)
                total += abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue())
                count += 3
        return total / float(count) if count else -1.0

    def send_mouse(widget, kind, pos, button, buttons):
        pos = QPointF(pos)
        QApplication.sendEvent(widget, QMouseEvent(kind, pos, QPointF(widget.mapToGlobal(pos.toPoint())),
                                                   button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def set_param(node, name, *values):
        param = node.getParam(name)
        if param is None:
            report("NOTE %s has no parameter %s" % (node.getScriptName(), name))
            return False
        param.set(*values)
        return True

    def set_output_warped(on):
        """Output Warped is a checkable button, which Python can only trigger, not set, so its
        viewer toolbar button is clicked."""
        for button in QApplication.instance().allWidgets():
            if (isinstance(button, QAbstractButton) and button.isVisible() and button.isCheckable()
                    and "Source unwarped" in button.toolTip()):
                if button.isChecked() != on:
                    button.click()
                QApplication.processEvents()
                return button.isChecked() == on
        report("NOTE no visible Output Warped button")
        return False

    def tree_item(tree, curve):
        found = tree.findItems(curve.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
        return found[0] if found else None

    def select_in_tree(tree, curves):
        tree.clearSelection()
        items = []
        for curve in curves:
            item = tree_item(tree, curve)
            if item is not None:
                item.setSelected(True)
                items.append(item)
        QApplication.processEvents()
        return items

    def run_menu_action(tree, item, text, seen):
        """Right-clicks item in the curve list and triggers the menu action labelled text. The menu
        is modal, so the action is picked from a timer while it is up."""
        def pick():
            popup = QApplication.activePopupWidget()
            if popup is None:
                QTimer.singleShot(200, pick)
                return
            for act in popup.actions():
                if act.text() == text:
                    seen[text] = act.isEnabled()
                    if act.isEnabled():
                        act.trigger()
                    break
            popup.close()

        QTimer.singleShot(MENU_MS, pick)
        # A release on any other column of a selected row is taken as a click on that cell (activation,
        # lock, colour) and opens no menu, so the click lands on the name column.
        header = tree.header()
        x = header.sectionViewportPosition(0) + header.sectionSize(0) // 2
        pos = QPoint(x, tree.visualItemRect(item).center().y())
        send_mouse(tree.viewport(), QEvent.MouseButtonRelease, pos, Qt.RightButton, Qt.NoButton)

    # The curve actions the viewer's right-click menu on a curve shares with the curve list.
    CURVE_MENU_ACTIONS = ("Duplicate and Link", "Link", "Unlink", "Swap Source/Destination", "Toggle Boundary",
                          "Toggle Cookie Cutter", "Link Transform to First", "Unlink Transform")

    def menu_text(action):
        return action.text().replace("&", "").split("\t")[0].strip()

    def run_viewer_menu_action(pos, text, seen):
        """Right-clicks the viewer at pos, on a curve, and triggers the menu action labelled text,
        picked from a timer while the modal menu is up; text None only dismisses the menu. seen gets
        each entry's enabled state. The poll gives up, so a click that opened no menu cannot be
        answered by the menu of a later click."""
        polls = [10]

        def pick():
            popup = QApplication.activePopupWidget()
            if popup is None:
                polls[0] -= 1
                if polls[0] > 0:
                    QTimer.singleShot(200, pick)
                return
            chosen = None
            for act in popup.actions():
                label = menu_text(act)
                if label:
                    seen[label] = act.isEnabled()
                if label == text:
                    chosen = act
            popup.close()
            if chosen is not None and chosen.isEnabled():
                chosen.trigger()

        QTimer.singleShot(MENU_MS, pick)
        gl = visible_viewer()
        send_mouse(gl, QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton)
        send_mouse(gl, QEvent.MouseButtonPress, pos, Qt.RightButton, Qt.RightButton)
        send_mouse(gl, QEvent.MouseButtonRelease, pos, Qt.RightButton, Qt.NoButton)

    def check_menu_opens_repeatedly(pos, what):
        """Right-clicks pos three times with the same entries, dismissing the menu each time."""
        for attempt in (1, 2, 3):
            shown = {}
            run_viewer_menu_action(pos, None, shown)
            yield ("sleep", MENU_GONE_MS)
            check(all(name in shown for name in CURVE_MENU_ACTIONS),
                  "right-click %d on %s opens the viewer menu" % (attempt, what))

    def viewer_key(key):
        """key through the viewer's window to the viewer, as a key press from a person arrives."""
        gl = visible_viewer()
        gl.setFocus(Qt.OtherFocusReason)
        QTest.keyClick(gl.window().windowHandle(), key)
        QApplication.processEvents()

    def select_tool(text):
        """Selects a viewer toolbar tool the way its button does; the tool knobs are checkable, so
        triggering the parameter from a script never changes the tool."""
        for button in QApplication.allWidgets():
            if isinstance(button, QToolButton) and button.isVisible():
                for act in button.actions():
                    if act.text() == text:
                        button.setDefaultAction(act)
                        button.triggered.emit(act)
                        QApplication.processEvents()
                        return True
        report("NOTE no viewer toolbar button offers %s" % text)
        return False

    def tangents(curve):
        return [tuple(curve.getControlPointPosition(i, 1.0)) for i in range(curve.getNumControlPoints())]

    def make_quad(roto, x0, y0, x1, y1):
        curve = hold(roto.createBezier(x0, y0, 1.0))
        for x, y in ((x1, y0), (x1, y1), (x0, y1)):
            curve.addControlPoint(x, y)
        curve.setCurveFinished(True)
        return curve

    def column_of(tree, header):
        h = tree.headerItem()
        for i in range(tree.columnCount()):
            if h.text(i) == header:
                return i
        return -1

    def role_and_partner(tree, curve):
        item = tree_item(tree, curve)
        role_col = column_of(tree, "Role")
        partner_col = column_of(tree, "Linked To")
        if item is None or role_col < 0 or partner_col < 0:
            return None, None
        return item.text(role_col), item.text(partner_col)

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        view = node_graph_view()

        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 600)
        checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
        require(checker is not None, "created a CheckerBoard",
                gt.plugin_problem("net.sf.openfx.CheckerBoardPlugin"))
        checker.setPosition(0, 0)
        warp = app.createNode("SplineWarp")
        require(warp is not None, "created a SplineWarp", gt.plugin_problem("SplineWarp"))
        warp.setPosition(0, 300)
        require(warp.connectInput(0, checker), "connected the CheckerBoard to the SplineWarp")
        require(viewer.connectInput(0, warp), "connected the SplineWarp to the viewer")
        roto = hold(warp.getRotoContext())
        sw = hold(warp.getSplineWarp())
        require(roto is not None and sw is not None, "the SplineWarp has curves and links")

        open_panel(view, warp)
        ok = yield ("until", lambda: panel_of(warp) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the SplineWarp's settings panel")
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        yield ("sleep", RENDER_MS)
        tree = fresh_tree(warp)
        require(tree is not None, "the settings panel holds the curve list")

        gt.section("panel-columns")
        role_col = column_of(tree, "Role")
        partner_col = column_of(tree, "Linked To")
        check(role_col >= 0, "the curve list has a Role column")
        check(partner_col >= 0, "the curve list has a Linked To column")
        check(role_col >= 0 and partner_col >= 0
              and not tree.isColumnHidden(role_col) and not tree.isColumnHidden(partner_col),
              "both columns are shown")

        gt.section("duplicate-and-link")
        x0, y0, x1, y1 = RECT
        closed = hold(roto.createBezier(x0, y0, 1.0))
        require(closed is not None, "created a closed curve")
        for x, y in ((x1, y0), (x1, y1), (x0, y1)):
            closed.addControlPoint(x, y)
        closed.setCurveFinished(True)
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [closed])
        require(len(items) == 1, "selected the closed curve in the curve list")
        seen = {}
        run_menu_action(tree, items[0], "Duplicate and Link", seen)
        ok = yield ("until", lambda: sw.getLink(closed) is not None, POLL_TIMEOUT_S)
        check(seen.get("Duplicate and Link", False), "the context menu offers Duplicate and Link")
        require(ok, "Duplicate and Link linked the closed curve")
        link = hold(sw.getLink(closed))
        dest = hold(link.getDestination())
        check(link.getSource().getScriptName() == closed.getScriptName(), "the original is the source")
        tree = fresh_tree(warp)
        role, partner = role_and_partner(tree, closed)
        check(role == "Source", "the original's Role is Source (%r)" % role)
        check(partner == dest.getLabel(), "the original is linked to the copy (%r)" % partner)
        role, partner = role_and_partner(tree, dest)
        check(role == "Destination", "the copy's Role is Destination (%r)" % role)
        check(partner == closed.getLabel(), "the copy is linked to the original (%r)" % partner)
        yield ("sleep", RENDER_MS)
        gt.shot("m148-panel-columns.png")

        gt.section("drag-destination")
        require(set_param(warp, "show", SHOW_DESTINATION), "set Show to Destination")
        yield ("sleep", RENDER_MS)
        before = grab_viewer()
        blue = coloured_pixels(before, is_destination)
        report("NOTE destination grab has %d blue pixels" % len(blue))
        require(len(blue) > MIN_INK, "the destination curve is drawn blue")
        px0, py0, px1, py1 = rectangle_edges(before, blue, is_destination)
        report("NOTE the destination rectangle spans viewer pixels (%d, %d)-(%d, %d)" % (px0, py0, px1, py1))
        # The top-right corner of the rectangle: canonical y grows upwards, viewer y downwards.
        corner = QPoint(px1, py0)
        target = corner + QPoint(*DRAG_BY)
        report("NOTE dragging the destination corner from viewer pixel (%d, %d)" % (corner.x(), corner.y()))
        start_pos = dest.getControlPointPosition(2, 1.0)
        gl = visible_viewer()
        send_mouse(gl, QEvent.MouseButtonPress, corner, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            send_mouse(gl, QEvent.MouseMove, QPointF(corner) + QPointF(DRAG_BY[0] * f, DRAG_BY[1] * f),
                       Qt.NoButton, Qt.LeftButton)
        yield ("sleep", RENDER_MS)
        draft = grab_viewer()
        gt.shot("m148-drag-draft.png")
        report("NOTE the mid-drag frame is the draft render; compare it with m148-drag-done.png")
        send_mouse(gl, QEvent.MouseButtonRelease, target, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", RENDER_MS)
        after = grab_viewer()
        gt.shot("m148-drag-done.png")
        end_pos = dest.getControlPointPosition(2, 1.0)
        report("NOTE destination corner %r -> %r" % (tuple(start_pos[:2]), tuple(end_pos[:2])))
        check(end_pos[0] > start_pos[0] + 1.0 and end_pos[1] > start_pos[1] + 1.0,
              "the drag moved the destination's corner up and right")
        source_pos = closed.getControlPointPosition(2, 1.0)
        check(abs(source_pos[0] - x1) < 1e-6 and abs(source_pos[1] - y1) < 1e-6, "the source curve did not move")
        inside = (px0, py0, px1, py1)
        d_after = region_difference(before, after, inside)
        d_draft = region_difference(before, draft, inside)
        report("NOTE mean change inside the curve: draft %.2f, final %.2f" % (d_draft, d_after))
        check(d_after > 2.0, "the image inside the curve is warped after the drag")
        check(d_draft > 2.0, "the image is warped during the drag")

        gt.section("show-and-output")
        require(set_param(warp, "show", SHOW_SOURCE), "set Show to Source")
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        check(len(coloured_pixels(img, is_source)) > MIN_INK, "Show Source draws the red source curve")
        check(len(coloured_pixels(img, is_destination)) < MIN_INK, "Show Source hides the destination curve")
        gt.shot("m148-show-source.png")
        require(set_param(warp, "show", SHOW_DESTINATION), "set Show to Destination")
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        check(len(coloured_pixels(img, is_source)) < MIN_INK, "Show Destination hides the source curve")
        check(len(coloured_pixels(img, is_destination)) > MIN_INK, "Show Destination draws the blue curve")
        gt.shot("m148-show-destination.png")
        require(set_param(warp, "show", SHOW_BOTH), "set Show to Both")
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        check(len(coloured_pixels(img, is_source)) > MIN_INK and len(coloured_pixels(img, is_destination)) > MIN_INK,
              "Show Both draws both curves")
        gt.shot("m148-show-both.png")
        warped = img
        require(set_output_warped(False), "turned Output Warped off")
        yield ("sleep", RENDER_MS)
        unwarped = grab_viewer()
        d = region_difference(warped, unwarped, inside)
        report("NOTE Output Warped off changes the inside of the curve by %.2f" % d)
        check(d > 2.0, "Output Warped off shows the Source unwarped")
        gt.shot("m148-output-unwarped.png")
        require(set_output_warped(True), "turned Output Warped back on")
        yield ("sleep", RENDER_MS)
        check(region_difference(warped, grab_viewer(), inside) < 2.0, "Output Warped on warps again")

        gt.section("correspondence")
        before = grab_viewer()
        index = link.addCorrespondence(1.5, 1.5)
        check(index >= 0, "added a correspondence to the closed link")
        check(link.getCorrespondenceCount() == 1, "the link has one correspondence")
        yield ("sleep", RENDER_MS)
        gt.shot("m148-correspondence-added.png")
        link.setCorrespondence(index, 1.5, 1.8, 1.0)
        params = link.getCorrespondences(1.0)
        report("NOTE correspondences after the slide: %r" % (params,))
        check(len(params) == 1 and abs(params[0][1] - 1.8) < 1e-6, "slid the correspondence's destination end")
        yield ("sleep", RENDER_MS)
        check(region_difference(before, grab_viewer(), inside) > 0.5, "the slid correspondence changes the warp")
        gt.shot("m148-correspondence-slid.png")
        report("NOTE the correspondence was added and slid through Python; the viewer tool is not driven")

        gt.section("open-curve-and-point")
        open_a = hold(roto.createBezier(300.0, 200.0, 1.0))
        open_a.addControlPoint(450.0, 300.0)
        open_a.addControlPoint(600.0, 200.0)
        open_b = hold(roto.createBezier(300.0, 150.0, 1.0))
        open_b.addControlPoint(450.0, 80.0)
        open_b.addControlPoint(600.0, 150.0)
        point = hold(roto.createBezier(1500.0, 800.0, 1.0))
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [open_a, open_b])
        require(len(items) == 2, "selected two open curves in the curve list")
        seen = {}
        run_menu_action(tree, items[0], "Link", seen)
        ok = yield ("until", lambda: sw.getLink(open_a) is not None, POLL_TIMEOUT_S)
        check(seen.get("Link", False), "the context menu enables Link for two open curves")
        check(ok, "Link linked the two open curves")
        if ok:
            tree = fresh_tree(warp)
            role_a, partner_a = role_and_partner(tree, open_a)
            role_b, _ = role_and_partner(tree, open_b)
            check(role_a == "Source" and role_b == "Destination",
                  "the first selected is the source (%r, %r)" % (role_a, role_b))
            check(partner_a == open_b.getLabel(), "the open source is linked to the other open curve")
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [point])
        require(len(items) == 1, "selected the single point in the curve list")
        seen = {}
        run_menu_action(tree, items[0], "Duplicate and Link", seen)
        ok = yield ("until", lambda: sw.getLink(point) is not None, POLL_TIMEOUT_S)
        check(ok, "Duplicate and Link linked the single point")
        if ok:
            point_link = hold(sw.getLink(point))
            point_dest = hold(point_link.getDestination())
            point_dest.movePointByIndex(0, 1.0, 60.0, -60.0)
        yield ("sleep", RENDER_MS)
        gt.shot("m148-open-and-point.png")

        gt.section("boundary")
        bx0, by0, bx1, by1 = 550.0, 250.0, 1350.0, 850.0
        boundary = hold(roto.createBezier(bx0, by0, 1.0))
        for x, y in ((bx1, by0), (bx1, by1), (bx0, by1)):
            boundary.addControlPoint(x, y)
        boundary.setCurveFinished(True)
        boundary.setFeatherDistance(60.0, 1.0)
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [boundary])
        require(len(items) == 1, "selected the boundary curve in the curve list")
        before = grab_viewer()
        seen = {}
        run_menu_action(tree, items[0], "Toggle Boundary", seen)
        ok = yield ("until", lambda: sw.isBoundary(boundary), POLL_TIMEOUT_S)
        check(ok, "Toggle Boundary made the curve a boundary")
        tree = fresh_tree(warp)
        role, partner = role_and_partner(tree, boundary)
        check(role == "Boundary" and partner == "", "the boundary's Role is Boundary (%r, %r)" % (role, partner))
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        check(len(coloured_pixels(img, is_boundary)) > MIN_INK, "the boundary is drawn yellow")
        report("NOTE the boundary changes the picture by %.2f" % region_difference(before, img, inside))
        gt.shot("m148-boundary-feather.png")
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [closed])
        seen = {}
        run_menu_action(tree, items[0], "Toggle Boundary", seen)
        yield ("sleep", MENU_MS * 2)
        check(seen.get("Toggle Boundary") is False, "Toggle Boundary is greyed out for a linked curve")
        gt.shot("m148-panel-final.png")

        # Canonical coordinates to viewer pixels, from where the destination rectangle, then on RECT,
        # was drawn before the drag; the viewer has not been panned or zoomed since.
        sx = (px1 - px0) / (RECT[2] - RECT[0])
        sy = (py1 - py0) / (RECT[3] - RECT[1])

        def to_viewer(cx, cy):
            return QPoint(int(round(px0 + (cx - RECT[0]) * sx)), int(round(py1 - (cy - RECT[1]) * sy)))

        gt.section("smooth-key")
        # Smoothing a SplineWarp curve used to hang Natron: Z on a linked curve has to come back.
        warp.getParam("SelectAllTool").trigger()
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [dest])
        require(len(items) == 1, "selected the linked destination in the curve list")
        yield ("sleep", SETTLE_MS)
        before_smooth = tangents(dest)
        send_mouse(visible_viewer(), QEvent.MouseMove, to_viewer(250.0, 950.0), Qt.NoButton, Qt.NoButton)
        viewer_key(Qt.Key_Z)
        ok = yield ("until", lambda: tangents(dest) != before_smooth, POLL_TIMEOUT_S)
        check(ok, "Z smoothed the linked destination and Natron came back")
        if not ok:
            report("NOTE Z did not reach the curve; running Smooth through its parameter")
            warp.getParam("SmoothItemsAction").trigger()
            ok = yield ("until", lambda: tangents(dest) != before_smooth, POLL_TIMEOUT_S)
            check(ok, "Smooth on the linked destination came back and smoothed it")
        report("NOTE destination points before/after smoothing: %r -> %r" % (before_smooth[:1], tangents(dest)[:1]))
        yield ("sleep", RENDER_MS)
        gt.shot("m148-smoothed.png")

        gt.section("viewer-menu")
        q1 = make_quad(roto, 100.0, 600.0, 400.0, 900.0)
        q2 = make_quad(roto, 1450.0, 150.0, 1750.0, 450.0)
        q3 = make_quad(roto, 1450.0, 500.0, 1750.0, 650.0)
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [q1])
        require(len(items) == 1, "selected a new closed curve")
        yield from check_menu_opens_repeatedly(to_viewer(100.0, 750.0), "a curve")
        seen = {}
        run_viewer_menu_action(to_viewer(100.0, 750.0), "Duplicate and Link", seen)
        ok = yield ("until", lambda: sw.getLink(q1) is not None, POLL_TIMEOUT_S)
        report("NOTE the viewer menu on a curve: %r" % (seen,))
        check(all(name in seen for name in CURVE_MENU_ACTIONS), "the viewer menu lists the curve list's warp actions")
        check(seen.get("Duplicate and Link") is True and seen.get("Link") is False,
              "the viewer menu enables Duplicate and Link, not Link, for one curve")
        check(ok, "Duplicate and Link from the viewer menu linked the curve")
        if ok:
            check(sw.getLink(q1).getSource().getScriptName() == q1.getScriptName(),
                  "the clicked curve is the source")
        yield ("sleep", RENDER_MS)
        gt.shot("m148-viewer-menu-duplicate.png")
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [q2, q3])
        require(len(items) == 2, "selected two new closed curves")
        seen = {}
        run_viewer_menu_action(to_viewer(1600.0, 650.0), "Link", seen)
        ok = yield ("until", lambda: sw.getLink(q2) is not None, POLL_TIMEOUT_S)
        report("NOTE the viewer menu on two curves: %r" % (seen,))
        check(seen.get("Link") is True and seen.get("Duplicate and Link") is False,
              "the viewer menu enables Link, not Duplicate and Link, for two curves")
        check(ok, "Link from the viewer menu linked the two curves")
        if ok:
            link23 = sw.getLink(q2)
            report("NOTE the viewer menu's Link made %s -> %s" % (link23.getSource().getScriptName(),
                                                                  link23.getDestination().getScriptName()))
        yield ("sleep", RENDER_MS)
        gt.shot("m148-viewer-menu-link.png")

        gt.section("viewer-point-menu")
        q4 = make_quad(roto, 700.0, 150.0, 1000.0, 350.0)
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [q4])
        require(len(items) == 1, "selected a new closed curve for the point menu")
        yield from check_menu_opens_repeatedly(to_viewer(700.0, 150.0), "a control point")
        seen = {}
        run_viewer_menu_action(to_viewer(700.0, 150.0), "Duplicate and Link", seen)
        ok = yield ("until", lambda: sw.getLink(q4) is not None, POLL_TIMEOUT_S)
        report("NOTE the viewer menu on a control point: %r" % (seen,))
        check(all(name in seen for name in CURVE_MENU_ACTIONS), "the viewer menu on a control point lists the warp actions")
        check(seen.get("Duplicate and Link") is True and seen.get("Link") is False,
              "the point menu enables Duplicate and Link, not Link, for one curve")
        check(ok, "Duplicate and Link from the point menu linked the point's curve")
        yield ("sleep", RENDER_MS)
        gt.shot("m148-viewer-point-menu-duplicate.png")

        gt.section("point-tool")
        select_tool("Point Tool")
        yield ("sleep", SETTLE_MS)
        names_before = set(item.getScriptName() for item in roto.getBaseLayer().getChildren())
        click = to_viewer(1600.0, 800.0)
        gl = visible_viewer()
        send_mouse(gl, QEvent.MouseButtonPress, click, Qt.LeftButton, Qt.LeftButton)
        send_mouse(gl, QEvent.MouseButtonRelease, click, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", RENDER_MS)
        made = [item for item in roto.getBaseLayer().getChildren() if item.getScriptName() not in names_before]
        report("NOTE the point tool made %r" % ([m.getScriptName() for m in made],))
        check(len(made) == 2, "one click with the point tool made two points")
        made_names = set(m.getScriptName() for m in made)
        new_links = [lk for lk in sw.getLinks() if lk.getSource().getScriptName() in made_names]
        check(len(new_links) == 1 and new_links[0].getDestination().getScriptName() in made_names,
              "the two points are linked")
        if len(new_links) == 1:
            point_link = new_links[0]
            src = point_link.getSource()
            dst = point_link.getDestination()
            check(src.getNumControlPoints() == 1 and dst.getNumControlPoints() == 1, "both are single points")
            sp = src.getControlPointPosition(0, 1.0)
            dp = dst.getControlPointPosition(0, 1.0)
            check(abs(sp[0] - dp[0]) < 1e-6 and abs(sp[1] - dp[1]) < 1e-6, "the destination lies on the source")
        select_tool("Select All Tool")
        gt.shot("m148-point-tool.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
