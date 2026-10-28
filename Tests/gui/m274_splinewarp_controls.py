"""SplineWarp's remaining controls: the Controls, Transform and Render pages fit at 1920x1080, a soft
boundary, a drag of the viewer transform overlay, the cookie mask in the Output Mask layer, and
Crop To Format on and off.

A CheckerBoard feeds a Transform feeding a SplineWarp feeding the viewer, with the SplineWarp's
settings panel open. Curves, links and roles are made through Python; the overlay is dragged in
the viewer. The viewer's canonical-to-pixel mapping is calibrated from the blue destination curve.
Saves screenshots of every step.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m274-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m274_splinewarp_controls.py build/fast/App/Natron
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
DRAG_STEPS = 12
SHOW_SOURCE, SHOW_DESTINATION, SHOW_BOTH = 0, 1, 2
MIN_INK = 40  # pixels on a 2-pixel sampling grid
PAGES = ("Controls", "Transform", "Render")
# Canonical rectangles (x0, y0, x1, y1).
SOURCE_RECT = (700.0, 350.0, 1200.0, 750.0)
SHIFT = 120.0  # the destination is the source moved right
COOKIE_RECT = (250.0, 650.0, 550.0, 950.0)
# The crop link pulls from left of the format, which the Transform shifts the checkerboard into.
CROP_SHIFT = -400.0
CROP_SOURCE_RECT = (-300.0, 380.0, -100.0, 580.0)
CROP_DEST_RECT = (100.0, 380.0, 300.0, 580.0)
OVERLAY_CENTRE = (1500.0, 220.0)
DRAG_BY = (80, -60)  # viewer pixels: right and up

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QScrollArea, QTabWidget, QTreeWidget

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

    def page_tabs(panel):
        for tabs in panel.findChildren(QTabWidget):
            names = [tabs.tabText(i) for i in range(tabs.count())]
            if all(p in names for p in PAGES):
                return tabs
        return None

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

    def bbox(points):
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        return min(xs), min(ys), max(xs), max(ys)

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

    def mean_level(img, rect):
        """The mean of the largest channel of a grab inside rect (x0, y0, x1, y1)."""
        if img is None:
            return -1.0
        x0, y0, x1, y1 = rect
        total = 0
        count = 0
        for y in range(max(0, y0), min(img.height(), y1), 2):
            for x in range(max(0, x0), min(img.width(), x1), 2):
                c = img.pixelColor(x, y)
                total += max(c.red(), c.green(), c.blue())
                count += 1
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

    def make_rect(roto, rect):
        x0, y0, x1, y1 = rect
        curve = hold(roto.createBezier(x0, y0, 1.0))
        if curve is None:
            return None
        for x, y in ((x1, y0), (x1, y1), (x0, y1)):
            curve.addControlPoint(x, y)
        curve.setCurveFinished(True)
        return curve

    def shifted(rect, dx):
        return (rect[0] + dx, rect[1], rect[2] + dx, rect[3])

    def tree_item(tree, curve):
        found = tree.findItems(curve.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
        return found[0] if found else None

    def column_of(tree, header):
        h = tree.headerItem()
        for i in range(tree.columnCount()):
            if h.text(i) == header:
                return i
        return -1

    def cell(tree, curve, header):
        item = tree_item(tree, curve)
        column = column_of(tree, header)
        if item is None or column < 0:
            return None
        return item.text(column)

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

    def viewer_layer_combo():
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and "Layer:" in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def combo_items(combo):
        try:
            return [str(i) for i in (combo.property("natronItems") or [])]
        except RuntimeError:
            return []

    def choose(combo, text):
        items = combo_items(combo)
        index = next((n for n, i in enumerate(items) if text in i), -1)
        if index < 0:
            return False
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
        return True

    class Mapping(object):
        """Canonical coordinates to viewer pixels, from a canonical rectangle and its pixel box."""

        def __init__(self, rect, box):
            x0, y0, x1, y1 = rect
            px0, py0, px1, py1 = box
            self.sx = (px1 - px0) / (x1 - x0)
            self.sy = (py1 - py0) / (y1 - y0)
            self.x0, self.y0, self.px0, self.py1 = x0, y0, px0, py1

        def point(self, x, y):
            return QPoint(int(round(self.px0 + (x - self.x0) * self.sx)),
                          int(round(self.py1 - (y - self.y0) * self.sy)))

        def rect(self, rect, inset=0.0):
            """The pixel box of a canonical rectangle, shrunk by inset of its size on each side."""
            x0, y0, x1, y1 = rect
            dx, dy = (x1 - x0) * inset, (y1 - y0) * inset
            a = self.point(x0 + dx, y1 - dy)
            b = self.point(x1 - dx, y0 + dy)
            return a.x(), a.y(), b.x(), b.y()

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
        viewer.setPosition(0, 900)
        checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
        require(checker is not None, "created a CheckerBoard",
                gt.plugin_problem("net.sf.openfx.CheckerBoardPlugin"))
        checker.setPosition(0, 0)
        shift = app.createNode("net.sf.openfx.TransformPlugin")
        require(shift is not None, "created a Transform", gt.plugin_problem("net.sf.openfx.TransformPlugin"))
        shift.setPosition(0, 300)
        warp = app.createNode("SplineWarp")
        require(warp is not None, "created a SplineWarp", gt.plugin_problem("SplineWarp"))
        warp.setPosition(0, 600)
        require(shift.connectInput(0, checker), "connected the CheckerBoard to the Transform")
        require(warp.connectInput(0, shift), "connected the Transform to the SplineWarp")
        viewer.disconnectInput(0)
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
        panel = hold(panel_of(warp))
        tree = hold(curve_tree(panel))
        require(tree is not None, "the settings panel holds the curve list")

        gt.section("pages-fit")
        tabs = hold(page_tabs(panel))
        require(tabs is not None, "the settings panel has Controls, Transform and Render pages")
        screen = QGuiApplication.primaryScreen().geometry()
        report("NOTE screen %dx%d" % (screen.width(), screen.height()))
        top_left = panel.mapToGlobal(QPoint(0, 0))
        panel_rect = QRect(top_left, panel.size())
        check(screen.contains(panel_rect), "the settings panel is on screen (%r)" % (panel_rect.getRect(),))
        for name in PAGES:
            index = next(i for i in range(tabs.count()) if tabs.tabText(i) == name)
            tabs.setCurrentIndex(index)
            yield ("sleep", SETTLE_MS)
            page = hold(tabs.widget(index))
            areas = [page] if isinstance(page, QScrollArea) else [a for a in page.findChildren(QScrollArea) if a.isVisible()]
            require(len(areas) > 0, "the %s page scrolls in a scroll area" % name)
            area = hold(areas[0])
            vmax = area.verticalScrollBar().maximum()
            hmax = area.horizontalScrollBar().maximum()
            report("NOTE %s page: viewport %dx%d, scroll ranges v %d h %d" % (
                name, area.viewport().width(), area.viewport().height(), vmax, hmax))
            check(vmax == 0, "the %s page fits without scrolling vertically" % name)
            check(hmax == 0, "the %s page fits without scrolling horizontally" % name)
            gt.shot("m274-page-%s.png" % name.lower())
        tabs.setCurrentIndex(next(i for i in range(tabs.count()) if tabs.tabText(i) == "Controls"))
        yield ("sleep", SETTLE_MS)

        gt.section("calibrate")
        source = make_rect(roto, SOURCE_RECT)
        dest_rect = shifted(SOURCE_RECT, SHIFT)
        dest = make_rect(roto, dest_rect)
        require(source is not None and dest is not None, "created the source and destination curves")
        link = hold(sw.link(source, dest))
        require(link is not None, "linked the source curve to the destination")
        require(set_param(warp, "show", SHOW_DESTINATION), "set Show to Destination")
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        blue = coloured_pixels(img, is_destination)
        report("NOTE destination grab has %d blue pixels" % len(blue))
        require(len(blue) > MIN_INK, "the destination curve is drawn blue")
        mapping = Mapping(dest_rect, bbox(blue))
        report("NOTE canonical to viewer pixels: scale %.4f x %.4f" % (mapping.sx, mapping.sy))
        require(mapping.sx > 0.05 and mapping.sy > 0.05, "the viewer mapping is usable")
        require(set_param(warp, "show", SHOW_BOTH), "set Show to Both")
        yield ("sleep", RENDER_MS)

        gt.section("soft-boundary")
        # An open line just right of the destination, where the shift moves the picture most.
        lx = dest_rect[2] + 40.0
        line = hold(roto.createBezier(lx, dest_rect[1] - 100.0, 1.0))
        require(line is not None, "created an open curve")
        line.addControlPoint(lx, (dest_rect[1] + dest_rect[3]) / 2.0)
        line.addControlPoint(lx, dest_rect[3] + 100.0)
        yield ("sleep", RENDER_MS)
        near = mapping.rect((lx - 60.0, dest_rect[1], lx + 60.0, dest_rect[3]))
        before = grab_viewer()
        sw.setSoftBoundary(line, True)
        check(sw.isSoftBoundary(line), "the open curve is a soft boundary")
        check(not sw.isBoundary(line), "a soft boundary is not a hard boundary")
        yield ("sleep", RENDER_MS)
        role = cell(tree, line, "Role")
        check(role == "Soft Boundary", "the curve list shows Soft Boundary (%r)" % role)
        after = grab_viewer()
        d = region_difference(before, after, near)
        report("NOTE the soft boundary changes the picture near it by %.2f" % d)
        check(d > 1.0, "the soft boundary holds the picture along the curve")
        gt.shot("m274-soft-boundary.png")

        gt.section("transform-overlay")
        centre = dest.getParam("center")
        translate = dest.getParam("translate")
        require(centre is not None and translate is not None, "the destination has center and translate")
        centre.set(OVERLAY_CENTRE[0], OVERLAY_CENTRE[1])
        check(abs(centre.getValue(0) - OVERLAY_CENTRE[0]) < 1e-6 and abs(centre.getValue(1) - OVERLAY_CENTRE[1]) < 1e-6,
              "moved the destination's transform centre")
        items = select_in_tree(tree, [dest])
        require(len(items) == 1, "selected the destination alone in the curve list")
        yield ("sleep", RENDER_MS)
        gt.shot("m274-overlay-shown.png")
        before = grab_viewer()
        start = mapping.point(*OVERLAY_CENTRE)
        report("NOTE dragging the transform centre from viewer pixel (%d, %d)" % (start.x(), start.y()))
        gl = visible_viewer()
        win.activateWindow()
        send_mouse(gl, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            send_mouse(gl, QEvent.MouseMove, QPointF(start) + QPointF(DRAG_BY[0] * f, DRAG_BY[1] * f),
                       Qt.NoButton, Qt.LeftButton)
        yield ("sleep", RENDER_MS)
        gt.shot("m274-overlay-drag.png")
        send_mouse(gl, QEvent.MouseButtonRelease, start + QPoint(*DRAG_BY), Qt.LeftButton, Qt.NoButton)
        yield ("sleep", RENDER_MS)
        gt.shot("m274-overlay-done.png")
        tx, ty = translate.getValue(0), translate.getValue(1)
        report("NOTE destination translate after the drag (%.2f, %.2f), expected about (%.2f, %.2f)" % (
            tx, ty, DRAG_BY[0] / mapping.sx, -DRAG_BY[1] / mapping.sy))
        check(tx > 1.0 and ty > 1.0, "the overlay drag translated the destination up and right")
        node_translate = warp.getParam("translate")
        check(node_translate is not None and abs(node_translate.getValue(0) - tx) < 1e-3,
              "the Transform page shows the selected curve's translate")
        moved = mapping.rect(shifted(dest_rect, tx))
        d = region_difference(before, grab_viewer(), moved)
        report("NOTE the drag changes the warped picture by %.2f" % d)
        check(d > 2.0, "the translated destination changes the warp")
        tree.clearSelection()
        QApplication.processEvents()
        yield ("sleep", RENDER_MS)

        gt.section("cookie-mask")
        cookie = make_rect(roto, COOKIE_RECT)
        require(cookie is not None, "created the cookie cutter curve")
        mask = warp.getParam("outputMask")
        require(mask is not None, "the SplineWarp has an Output Mask parameter")
        check(not mask.getIsEnabled(), "Output Mask is disabled with no cookie cutter")
        sw.setCookieCutter(cookie, True)
        check(sw.isCookieCutter(cookie), "the closed curve is a cookie cutter")
        yield ("sleep", RENDER_MS)
        check(mask.getIsEnabled(), "Output Mask is enabled with a cookie cutter")
        ok = yield ("until", lambda: viewer_layer_combo() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer's layer menu is on screen")
        combo = hold(viewer_layer_combo())
        ok = yield ("until", lambda: any("SplineWarpMask" in i for i in combo_items(combo)), POLL_TIMEOUT_S)
        require(ok, "the viewer's layer menu lists SplineWarpMask (%r)" % combo_items(combo))
        require(choose(combo, "SplineWarpMask"), "chose the SplineWarpMask layer in the viewer")
        yield ("sleep", RENDER_MS)
        img = grab_viewer()
        inside = mean_level(img, mapping.rect(COOKIE_RECT, 0.2))
        outside = mean_level(img, mapping.rect((COOKIE_RECT[0], COOKIE_RECT[1] - 250.0, COOKIE_RECT[2], COOKIE_RECT[1] - 50.0)))
        report("NOTE the mask layer is %.1f inside the cookie cutter and %.1f below it" % (inside, outside))
        check(inside > outside + 40.0, "the Output Mask layer holds the cookie cutter's shape")
        gt.shot("m274-cookie-mask.png")
        require(choose(combo, "rgba"), "chose the rgba layer in the viewer again")
        sw.setCookieCutter(cookie, False)
        yield ("sleep", RENDER_MS)

        gt.section("crop-to-format")
        require(set_param(warp, "boundaryBBox", False), "turned Boundary BBox off")
        require(set_param(shift, "translate", CROP_SHIFT, 0.0), "moved the checkerboard left of the format")
        value = shift.getParam("translate").getValue(0)
        check(abs(value - CROP_SHIFT) < 1e-6, "the Transform's translate is %.1f (%.1f)" % (CROP_SHIFT, value))
        crop_source = make_rect(roto, CROP_SOURCE_RECT)
        crop_dest = make_rect(roto, CROP_DEST_RECT)
        require(crop_source is not None and crop_dest is not None, "created the crop link's curves")
        crop_link = hold(sw.link(crop_source, crop_dest))
        require(crop_link is not None, "linked a curve left of the format to one inside it")
        require(set_param(warp, "cropToFormat", True), "turned Crop To Format on")
        yield ("sleep", RENDER_MS)
        cropped = grab_viewer()
        gt.shot("m274-crop-on.png")
        require(set_param(warp, "cropToFormat", False), "turned Crop To Format off")
        yield ("sleep", RENDER_MS)
        uncropped = grab_viewer()
        gt.shot("m274-crop-off.png")
        region = mapping.rect(CROP_DEST_RECT, 0.2)
        level_on, level_off = mean_level(cropped, region), mean_level(uncropped, region)
        d = region_difference(cropped, uncropped, region)
        report("NOTE inside the crop destination: level on %.1f, off %.1f, difference %.2f" % (level_on, level_off, d))
        check(d > 5.0, "Crop To Format off pulls in the Source outside the format")
        check(level_off > level_on, "Crop To Format on pulls in black from outside the format")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
