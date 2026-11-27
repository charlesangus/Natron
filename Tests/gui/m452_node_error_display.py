import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
SETTLE_MS = 800
ERROR_COLOUR = "#e63c32"
WARNING_COLOUR = "#f5a514"
ROLES = ("nodeMessageBadge", "nodeMessageBorder", "nodeMessageCaption")
COLOUR_TOLERANCE = 40
SHOT_ZOOM = 1.5
LOW_DETAIL_FACTOR = 0.8
DRAG_STEPS = 6
BAND_MARGIN = 6.0

try:
    from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
    from PySide6.QtGui import QColor, QFontMetrics, QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, plugin_problem, report, require, run, section, widgets_of

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def graph_items():
        view = graph_view()
        if view is None:
            return []
        try:
            return list(view.items())
        except RuntimeError:
            return []

    def node_box(node):
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        return x, y, w, h

    def node_centre(node):
        x, y, w, h = node_box(node)
        return x + w / 2.0, y + h / 2.0

    def role_of(item):
        try:
            value = item.data(0)
        except RuntimeError:
            return None
        return str(value) if value is not None else None

    def message_items(node):
        cx, cy = node_centre(node)
        best = {}
        for item in graph_items():
            role = role_of(item)
            if role not in ROLES:
                continue
            try:
                r = item.sceneBoundingRect()
            except RuntimeError:
                continue
            d = abs(r.center().x() - cx) + abs(r.center().y() - cy)
            if role not in best or d < best[role][0]:
                best[role] = (d, item)
        return dict((role, hold(entry[1])) for role, entry in best.items())

    def caption_text(item):
        if hasattr(item, "text"):
            return str(item.text())
        return ""

    def message_of(node):
        try:
            return str(node.getPersistentMessage() or "")
        except Exception:
            return ""

    def centred_tags():
        found = []
        for item in graph_items():
            try:
                if item.isVisible() and hasattr(item, "text") and str(item.text()).strip() in ("ERROR", "WARNING"):
                    found.append(str(item.text()))
            except RuntimeError:
                continue
        return found

    def view_on(node, zoom):
        view = graph_view()
        view.resetTransform()
        view.scale(zoom, zoom)
        cx, cy = node_centre(node)
        view.centerOn(cx, cy)
        view.viewport().update()
        QApplication.processEvents()

    def viewport_image():
        QApplication.processEvents()
        vp = graph_view().viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        return pix, pix.toImage()

    def save_crop(node, name):
        pix, _img = viewport_image()
        view = graph_view()
        x, y, w, h = node_box(node)
        tl = view.mapFromScene(QPointF(x - 2 * w, y - h))
        br = view.mapFromScene(QPointF(x + 3 * w, y + 3 * h))
        crop = pix.copy(min(tl.x(), br.x()), min(tl.y(), br.y()), abs(br.x() - tl.x()), abs(br.y() - tl.y()))
        if crop.isNull():
            crop = pix
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if crop.save(path) else "NOTE could not save ") + path)

    def count_colour(img, scene_rect, colour_name):
        view = graph_view()
        target = QColor(colour_name)
        tl = view.mapFromScene(scene_rect.topLeft())
        br = view.mapFromScene(scene_rect.bottomRight())
        x0, x1 = sorted((tl.x(), br.x()))
        y0, y1 = sorted((tl.y(), br.y()))
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(img.width() - 1, x1), min(img.height() - 1, y1)
        count = 0
        for py in range(y0, y1 + 1):
            for px in range(x0, x1 + 1):
                c = img.pixelColor(px, py)
                if (abs(c.red() - target.red()) + abs(c.green() - target.green()) + abs(c.blue() - target.blue())) <= COLOUR_TOLERANCE:
                    count += 1
        return count

    def activate_main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                w.activateWindow()
                w.raise_()
                return hold(w)
        return None

    def send_mouse(etype, scene_pos, button, buttons):
        view = graph_view()
        pos = view.mapFromScene(scene_pos)
        QApplication.sendEvent(view.viewport(), QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def rubber_band(scene_rect):
        start, end = scene_rect.topLeft(), scene_rect.bottomRight()
        send_mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            send_mouse(QEvent.MouseMove, start + (end - start) * f, Qt.NoButton, Qt.LeftButton)
        send_mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def selected_names():
        return sorted(n.getScriptName() for n in app.getSelectedNodes())

    def show_on_viewer(viewer, node):
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer shows " + node.getScriptName())

    def check_display(label, node, colour, multi_line):
        items = message_items(node)
        message = message_of(node)
        report("NOTE %s: message %r" % (label, message))
        first_line = message.split("\n")[0].strip()
        if multi_line:
            check("\n" in message.strip(), "%s: the message spans several lines" % label)
        for role in ROLES:
            if not check(role in items, "%s: the node has a %s item" % (label, role)):
                return
        badge, border, caption = items["nodeMessageBadge"], items["nodeMessageBorder"], items["nodeMessageCaption"]
        check(badge.isVisible(), "%s: the badge is shown" % label)
        check(border.isVisible(), "%s: the border is shown" % label)
        check(caption.isVisible(), "%s: the caption is shown" % label)
        for role, item in items.items():
            check(str(item.data(1)) == colour, "%s: the %s is drawn in %s (got %r)" % (label, role, colour, item.data(1)))
        if hasattr(border, "pen"):
            check(border.pen().color().name() == colour, "%s: the border pen is %s (got %s)" % (label, colour, border.pen().color().name()))
            check(abs(border.pen().widthF() - 2.0) < 0.6, "%s: the border is about 2 px wide (got %g)" % (label, border.pen().widthF()))

        x, y, w, h = node_box(node)
        b = badge.sceneBoundingRect()
        check(b.center().x() > x + w / 2.0 and abs(b.center().y() - y) <= h / 2.0,
              "%s: the badge sits at the node's top-right corner (%g,%g vs node %g,%g %gx%g)" % (label, b.center().x(), b.center().y(), x, y, w, h))

        text = caption_text(caption)
        report("NOTE %s: caption %r" % (label, text))
        check("\n" not in text, "%s: the caption is one line" % label)
        elided = text.endswith(u"…") or text.endswith("...")
        stem = text.rstrip(u"…").rstrip(".") if elided else text
        check(bool(text) and (text == first_line or (elided and first_line.startswith(stem))),
              "%s: the caption is the message's first line, elided at most" % label)
        c = caption.sceneBoundingRect()
        check(c.width() <= 2.0 * w + 2.0, "%s: the caption is at most twice the node's width (%g vs %g)" % (label, c.width(), w))
        check(c.top() >= y + h - 1.0, "%s: the caption sits under the node body" % label)
        if multi_line:
            check(text != message.strip(), "%s: the caption leaves the later lines out" % label)
        for role, item in (("badge", badge), ("caption", caption)):
            tip = str(item.toolTip())
            check(bool(first_line) and first_line in tip and (not multi_line or message.strip().split("\n")[-1].strip() in tip),
                  "%s: the %s's tooltip holds the full message" % (label, role))

        tags = centred_tags()
        check(not tags, "%s: no centred ERROR/WARNING tag is shown (got %r)" % (label, tags))

    def steps():
        ok = yield ("until", lambda: bool(widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")

        section("setup")
        read_error = make("Read", 0, 0)
        constant = make("Constant", 300, -150)
        slitscan = make("SlitScan", 300, 0)
        read_long = make("Read", 600, 0)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        viewer.setPosition(300, 250)
        require(slitscan.connectInput(0, constant), "connect the SlitScan to the Constant")
        require(slitscan.getParam("retimeGain") is not None, "SlitScan has retimeGain")
        require(read_error.getParam("filename") is not None, "Read has a filename")
        read_error.getParam("filename").setValue(os.path.join(gt.OUT, "m452_missing.exr"))
        long_name = "m452_" + "a_missing_file_with_a_long_name_" * 3 + "\nsecond_line.exr"
        read_long.getParam("filename").setValue(os.path.join(gt.OUT, long_name))
        app.clearSelection()
        ok = yield ("until", gt.viewer_up, POLL_TIMEOUT_S)
        require(ok, "the viewer is visible")

        section("error")
        show_on_viewer(viewer, read_error)
        ok = yield ("until", lambda: bool(message_of(read_error)), POLL_TIMEOUT_S)
        check(ok, "error: the Read of a missing file posts a message")
        yield ("sleep", SETTLE_MS)
        check_display("error", read_error, ERROR_COLOUR, False)
        view_on(read_error, SHOT_ZOOM)
        yield ("sleep", SETTLE_MS)
        save_crop(read_error, "m452_error.png")

        section("warning")
        slitscan.getParam("retimeGain").setValue(100.0)
        show_on_viewer(viewer, slitscan)
        ok = yield ("until", lambda: bool(message_of(slitscan)), POLL_TIMEOUT_S)
        check(ok, "warning: the SlitScan posts a message")
        yield ("sleep", SETTLE_MS)
        check_display("warning", slitscan, WARNING_COLOUR, False)
        view_on(slitscan, SHOT_ZOOM)
        yield ("sleep", SETTLE_MS)
        save_crop(slitscan, "m452_warning.png")

        section("long message")
        show_on_viewer(viewer, read_long)
        ok = yield ("until", lambda: bool(message_of(read_long)), POLL_TIMEOUT_S)
        check(ok, "long message: the Read of a missing file posts a message")
        yield ("sleep", SETTLE_MS)
        check_display("long message", read_long, ERROR_COLOUR, True)
        view_on(read_long, SHOT_ZOOM)
        yield ("sleep", SETTLE_MS)
        save_crop(read_long, "m452_long_message.png")

        section("rubber band")
        win = activate_main_window()
        ok = yield ("until", lambda: win is not None and win.isActiveWindow(), 10.0)
        check(ok, "rubber band: the main window is active")
        items = message_items(read_long)
        if check("nodeMessageCaption" in items, "rubber band: the long-message node has a caption"):
            view_on(read_long, SHOT_ZOOM)
            yield ("sleep", SETTLE_MS)
            name = read_long.getScriptName()
            x, y, w, h = node_box(read_long)
            caption_rect = QRectF(items["nodeMessageCaption"].sceneBoundingRect())
            check(caption_rect.top() > y + h + 3.0, "rubber band: the caption leaves room under the body (top %g, body bottom %g)" % (caption_rect.top(), y + h))
            m = BAND_MARGIN
            body_only = QRectF(QPointF(x - m, y - m), QPointF(x + w + m, y + h + 2.5))
            app.clearSelection()
            rubber_band(body_only)
            yield ("sleep", SETTLE_MS)
            got = selected_names()
            check(name not in got, "rubber band: enclosing the body without its caption leaves the node unselected (got %r)" % got)
            whole = QRectF(QPointF(x, y), QPointF(x + w, y + h)).united(caption_rect).adjusted(-m, -m, m, m)
            app.clearSelection()
            rubber_band(whole)
            yield ("sleep", SETTLE_MS)
            got = selected_names()
            check(name in got, "rubber band: enclosing the body and its caption selects the node (got %r)" % got)
            app.clearSelection()

        section("zoomed out")
        items = message_items(read_long)
        if check("nodeMessageCaption" in items and "nodeMessageBadge" in items, "zoomed out: the long-message node has its items"):
            caption_rect = QRectF(items["nodeMessageCaption"].sceneBoundingRect())
            badge_rect = QRectF(items["nodeMessageBadge"].sceneBoundingRect())
            view_on(read_long, 1.0)
            yield ("sleep", SETTLE_MS)
            _pix, img = viewport_image()
            at_full = count_colour(img, caption_rect, ERROR_COLOUR)
            report("NOTE zoomed out: %d caption pixels at 1:1" % at_full)
            check(at_full > 0, "zoomed out: the caption is drawn at 1:1 (%d pixels)" % at_full)

            switch_zoom = 4.0 / QFontMetrics(QApplication.font()).height()
            view_on(read_long, switch_zoom * LOW_DETAIL_FACTOR)
            yield ("sleep", SETTLE_MS)
            _pix, img = viewport_image()
            x, y, w, h = node_box(read_long)
            below = QRectF(x - w, y + h + 1.0, 3 * w, caption_rect.bottom() - (y + h) + 2.0)
            in_caption = count_colour(img, below, ERROR_COLOUR)
            on_badge = count_colour(img, badge_rect.adjusted(-2, -2, 2, 2), ERROR_COLOUR)
            report("NOTE zoomed out: %d caption pixels, %d badge pixels" % (in_caption, on_badge))
            check(in_caption == 0, "zoomed out: the caption is hidden (%d pixels)" % in_caption)
            check(on_badge > 0, "zoomed out: the badge is still drawn (%d pixels)" % on_badge)
            view_on(read_error, switch_zoom * LOW_DETAIL_FACTOR)
            graph_view().centerOn(node_centre(slitscan)[0], node_centre(slitscan)[1])
            yield ("sleep", SETTLE_MS)
            pix, _img = viewport_image()
            path = os.path.join(gt.OUT, "m452_zoomed_out.png")
            report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
