"""Evidence for the direction badges round the pointer in global mode, in the real application.

Run with:
    docker exec natron-dev bash -c 'cd <tree> && \
        NATRON_GUI_TEST_OUT=build/m400-shots GUI_TEST_TIMEOUT=900 \
        Tests/gui/run-gui-test.sh Tests/gui/m400_pointer_badges.py <Natron binary or AppImage>'

Sections, each with numbers and captioned screenshots (a NOTE "caption" line precedes each SHOT):
- crosshair-only: with the Crosshair only feedback, Grade gain in S, T, V and C, Blur size and
  Transform rotate, each engaged with Ctrl+E and read with the pointer mid-graph and near the
  bottom-right corner of the screen. The pointer window ("ColorTrackballHudWindow") shows the
  target's four direction pills, read from their "trackballPillText" property, centred on the
  pointer a small gap from it, with the hint block under the bottom pill; in C only the block,
  right of and below the pointer. Near the corner the whole group is shifted on screen.
- stroke: in Crosshair only the pills stay up and centred on the pointer mid-stroke, for a colour
  and a value target.
- switch: Q/W/E/R/T on a Transform and Ctrl+Down/Up across its knobs relabel the pills live.
- reveal: in Crosshair + reveal the pills are up between strokes, the window hides while the
  reveal is up mid-stroke and comes back on the release.
- widget-cursor: Widget cursor never shows the window; switching the preference mid-engage to
  Crosshair only brings it up with the pills, and back hides it; Esc hides it.

Screen grabs leave the cursor out, so every pointer screenshot marks the pointer with a thin red
cross. Edge warps happen only on real (spontaneous) stroke moves, which a script cannot send while
a synthetic button is held, so they are left to the unit tests.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 850
SETTLE_MS = 400
PANEL_MS = 900
# The X server delivers the move from QCursor.setPos() asynchronously. Arriving after a
# synthetic press, that buttonless real move reads as a release global mode never saw, which
# ends the stroke before it starts.
POINTER_SETTLE_MS = 300
CENTRE_TOLERANCE_PX = 2
CORNER_INSET_PX = 12
POINTER_BADGE_REACH = 20.0
HUD_FOLLOW_OFFSET = 16.0

BLUR_ID = "Blur"
TRANSFORM_ID = "Transform"
GRADE_ID = "Grade"
GAIN = "white"

FEEDBACK_CROSSHAIR = 0
FEEDBACK_WIDGET = 1
FEEDBACK_CROSSHAIR_ONLY = 2

MINUS = u"−"
EMPTY = ("", "", "", "")
COLOUR_LABELS = {
    "S": ("+Hue", MINUS + "Hue", "+Sat", MINUS + "Sat"),
    "T": ("Warmer", "Cooler", "Less green", "Greener"),
    "V": ("+Val", MINUS + "Val", "+Val", MINUS + "Val"),
    "C": EMPTY,
}
KIND_LABELS = {
    "translate": ("+X", MINUS + "X", "+Y", MINUS + "Y"),
    "rotate": ("CCW", "CW", "CCW", "CW"),
    "scale": ("Scale up", "Scale down", "Scale up", "Scale down"),
    "size": ("Larger", "Smaller", "Larger", "Smaller"),
    "skew": ("+Skew X", MINUS + "Skew X", "+Skew Y", MINUS + "Skew Y"),
    "generic": ("+", MINUS, "+", MINUS),
}
TRANSFORM_KNOB_KINDS = {
    "translate": "translate",
    "center": "translate",
    "rotate": "rotate",
    "scale": "scale",
    "skewX": "skew",
    "skewY": "skew",
}
PILL_NAMES = ("ColorTrackballPointerPillRight", "ColorTrackballPointerPillLeft",
              "ColorTrackballPointerPillTop", "ColorTrackballPointerPillBottom")
RIGHT, LEFT, TOP, BOTTOM = range(4)

try:
    import faulthandler
    import gc

    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QColor, QCursor, QGuiApplication, QKeyEvent, QMouseEvent, QPainter, QPen
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QScrollArea, QWidget

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def dpi_scale():
        return QGuiApplication.primaryScreen().logicalDotsPerInchX() / 96.0

    def dpi_px(v):
        return int(round(v * dpi_scale()))

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def is_inside(widget, ancestor):
        if widget is None or ancestor is None:
            return False
        target = ptr(ancestor)
        w = widget
        while w is not None:
            if ptr(w) == target:
                return True
            w = w.parentWidget()
        return False

    def global_rect(w):
        return QRect(w.mapToGlobal(QPoint(0, 0)), w.size())

    def fmt_rect(r):
        return "(%d,%d %dx%d)" % (r.x(), r.y(), r.width(), r.height())

    def fmt_pt(p):
        return "(%d,%d)" % (p.x(), p.y())

    def screen_rect():
        return QGuiApplication.primaryScreen().geometry()

    # ---- application widgets -------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def first_named(name, visible_only=False):
        for w in QApplication.allWidgets():
            try:
                if w.objectName() != name:
                    continue
                if visible_only and not w.isVisible():
                    continue
                return w
            except RuntimeError:
                continue
        return None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QWidget):
                    if gt.class_name(w) == "LineEdit" and w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def raise_panel(node):
        view = node_graph_view()
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def live_children(root):
        out = []
        try:
            kids = root.findChildren(QWidget)
        except RuntimeError:
            return out
        for w in kids:
            try:
                w.objectName()
                out.append(w)
            except RuntimeError:
                continue
        return out

    def tagged(w, name):
        try:
            return ("<b>%s</b>" % name) in (w.toolTip() or "")[:200]
        except RuntimeError:
            return False

    def knob_widget(panel, name):
        """A visible, non-label widget of the knob, scrolled into view."""
        for w in live_children(panel):
            if not tagged(w, name) or isinstance(w, QLabel) or w.width() < 4 or w.height() < 4:
                continue
            a = w.parentWidget()
            while a is not None:
                if isinstance(a, QScrollArea):
                    a.ensureWidgetVisible(w)
                    break
                a = a.parentWidget()
            QApplication.processEvents()
            if w.isVisible() and not w.visibleRegion().isEmpty():
                return w
        return None

    def widget_centre(w):
        return w.mapToGlobal(w.rect().center())

    def hover_knob(panel, name):
        w = knob_widget(panel, name)
        if w is None:
            return False
        g = widget_centre(w)
        QCursor.setPos(g)
        QApplication.processEvents()
        return is_inside(QApplication.widgetAt(g), panel)

    def set_pref(name, value):
        NatronEngine.natron.getSettings().getParam(name).set(value)
        QApplication.processEvents()

    def drop_stale_wrappers():
        gc.collect()

    # ---- input -------------------------------------------------------------------------------

    def gmouse(kind, g, button, buttons, mods=Qt.NoModifier):
        w = main_window()
        local = QPointF(w.mapFromGlobal(g))
        QApplication.sendEvent(w, QMouseEvent(kind, local, QPointF(w.mapToGlobal(local)), button, buttons, mods))
        QApplication.processEvents()

    def send_key(key, mods=Qt.NoModifier):
        target = main_window()
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, key, mods))
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyRelease, key, mods))
        QApplication.processEvents()

    def shortcut_action():
        for a in main_window().actions():
            if a.shortcut().toString() == "Ctrl+E" or "Hovered or Last-Touched" in a.text():
                return a
        return None

    def engaged():
        return QApplication.overrideCursor() is not None

    def engage_with_ctrl_e():
        win = main_window()
        win.activateWindow()
        QApplication.processEvents()
        QTest.keyClick(win, Qt.Key_E, Qt.ControlModifier)
        QApplication.processEvents()
        if not engaged():
            a = shortcut_action()
            if a is not None:
                a.trigger()
            QApplication.processEvents()
            report("NOTE the Ctrl+E shortcut did not engage; the action was triggered instead")
        return engaged()

    def leave():
        if engaged():
            send_key(Qt.Key_Escape)
        QApplication.processEvents()

    def engage_knob(node, knob_name, colour=False):
        """Hovers the knob in the node's panel and engages it with Ctrl+E."""
        raise_panel(node)
        panel = panel_of(node)
        if panel is None or not hover_knob(panel, knob_name):
            report("NOTE could not hover %s.%s" % (node.getLabel(), knob_name))
            return False
        engage_with_ctrl_e()
        title = hud_knob()
        if colour:
            ok = engaged() and bool(title) and ": " not in title
        else:
            ok = engaged() and title == "%s: %s" % (node.getScriptName(), knob_name)
        return check(ok, "Ctrl+E over %s.%s engages it (HUD title %r)" % (node.getLabel(), knob_name, title))

    # ---- global mode readouts ------------------------------------------------------------------

    def pointer_hud():
        return first_named("ColorTrackballHudWindow")

    def hud_snapshot():
        """Everything read off the pointer window in one pass, from freshly found widgets; None when it is
        absent or deleted mid-read. Wrappers never outlive this call: the C++ side recreates the window."""
        try:
            w = pointer_hud()
            if w is None:
                return None
            texts, shown, rects = [], [], []
            for n in PILL_NAMES:
                p = w.findChild(QWidget, n)
                texts.append(None if p is None else (p.property("trackballPillText") or ""))
                shown.append(bool(p is not None and p.isVisible()))
                rects.append(None if p is None else global_rect(p))
            b = w.findChild(QWidget, "ColorTrackballHudBlock")
            return {
                "shown": w.isVisible(),
                "knob": w.property("trackballHudKnob"),
                "mode": w.property("trackballHudMode") or None,
                "window": global_rect(w),
                "texts": tuple(texts),
                "visible": tuple(shown),
                "rects": rects,
                "block": global_rect(b) if b is not None and b.isVisible() else None,
            }
        except RuntimeError:
            return None

    def hud_shown():
        s = hud_snapshot()
        return s is not None and s["shown"]

    def hud_knob():
        s = hud_snapshot()
        return s["knob"] if s is not None else None

    def hud_mode():
        s = hud_snapshot()
        return s["mode"] if s is not None else None

    def pill_texts():
        s = hud_snapshot()
        return s["texts"] if s is not None else (None,) * 4

    def pill_visible():
        s = hud_snapshot()
        return s["visible"] if s is not None else (False,) * 4

    def hud_block_rect():
        s = hud_snapshot()
        return s["block"] if s is not None else None

    def reveal_window():
        for w in QApplication.topLevelWidgets():
            try:
                if gt.class_name(w) != "QWidget":
                    continue
                if any(gt.class_name(c) == "ColorWheelWidget" for c in w.children() if isinstance(c, QWidget)):
                    return w
            except RuntimeError:
                continue
        return None

    def reveal_shown():
        r = reveal_window()
        return r is not None and r.isVisible()

    def val(node, name):
        p = node.getParam(name)
        return tuple(round(p.getValue(d), 4) for d in range(p.getNumDimensions()))

    # ---- pointer -------------------------------------------------------------------------------

    def graph_point():
        return widget_centre(node_graph_view())

    def corner_point():
        """Near the bottom-right corner of the screen, still over the main window."""
        r = global_rect(main_window()).intersected(screen_rect())
        return QPoint(r.x() + r.width() - 1 - CORNER_INSET_PX, r.y() + r.height() - 1 - CORNER_INSET_PX)

    def move_pointer(g):
        QCursor.setPos(g)
        yield ("sleep", POINTER_SETTLE_MS)
        QApplication.processEvents()

    def badge_centre(snap):
        """The point the four pills are laid out round, from their global rects."""
        if snap is None or not all(snap["visible"]):
            return None
        r = snap["rects"]
        x = (r[LEFT].x() + r[LEFT].width() + r[RIGHT].x()) / 2.0
        y = (r[TOP].y() + r[TOP].height() + r[BOTTOM].y()) / 2.0
        return QPointF(x, y)

    def near(a, b, tol=CENTRE_TOLERANCE_PX):
        return a is not None and b is not None and abs(a.x() - b.x()) <= tol and abs(a.y() - b.y()) <= tol

    def check_badges(tag, expected, pointer, at_corner):
        """The pointer window's pills and block for a target with labels expected, the pointer at pointer."""
        if not check(hud_shown(), "%s: the pointer window is up" % tag):
            return
        snap = hud_snapshot()
        if not check(snap is not None, "%s: the pointer window can be read" % tag):
            return
        texts = snap["texts"]
        check(texts == expected, "%s: pill texts right/left/top/bottom %r (expected %r)" % (tag, texts, expected))
        want_visible = tuple(bool(t) for t in expected)
        check(snap["visible"] == want_visible, "%s: pills shown %r (expected %r)" % (tag, snap["visible"], want_visible))
        block = snap["block"]
        if not check(block is not None, "%s: the hint block is in the window" % tag):
            return
        window = snap["window"]
        check(screen_rect().contains(window), "%s: the window %s is on screen %s" % (tag, fmt_rect(window), fmt_rect(screen_rect())))
        if not any(want_visible):
            off = dpi_px(HUD_FOLLOW_OFFSET)
            if at_corner:
                check(block.x() + block.width() <= pointer.x() and block.y() + block.height() <= pointer.y(),
                      "%s: near the corner the block alone %s flips above and left of the pointer %s" % (tag, fmt_rect(block), fmt_pt(pointer)))
            else:
                check(near(block.topLeft(), pointer + QPoint(off, off), 1),
                      "%s: the block alone sits at the pointer %s plus (%d, %d), as before (block %s)"
                      % (tag, fmt_pt(pointer), off, off, fmt_rect(block)))
            return
        centre = badge_centre(snap)
        if at_corner:
            check(centre is not None and not near(centre, QPointF(pointer)),
                  "%s: near the corner the group is shifted as one (pills centred on %s, pointer %s)"
                  % (tag, "none" if centre is None else "(%.1f,%.1f)" % (centre.x(), centre.y()), fmt_pt(pointer)))
            return
        check(near(centre, QPointF(pointer)),
              "%s: the pills are centred on the pointer %s (%s)"
              % (tag, fmt_pt(pointer), "none" if centre is None else "(%.1f,%.1f)" % (centre.x(), centre.y())))
        reach = dpi_px(POINTER_BADGE_REACH)
        clear = QRect(pointer.x() - reach + 1, pointer.y() - reach + 1, 2 * reach - 2, 2 * reach - 2)
        rects = snap["rects"]
        check(not any(r.intersects(clear) for r in rects),
              "%s: no pill covers the %d px square round the pointer" % (tag, 2 * reach))
        # The top and bottom pills keep clear of the side pills' half height when it exceeds reach.
        reach_y = max(reach, (rects[RIGHT].height() + 1) // 2, (rects[LEFT].height() + 1) // 2)
        gaps = (rects[RIGHT].x() - pointer.x(),
                pointer.x() - (rects[LEFT].x() + rects[LEFT].width()),
                pointer.y() - (rects[TOP].y() + rects[TOP].height()),
                rects[BOTTOM].y() - pointer.y())
        check(all(g <= reach_y + CENTRE_TOLERANCE_PX for g in gaps),
              "%s: the pills sit close to the pointer, not at a ring radius (gaps right/left/top/bottom %r, reach %d)"
              % (tag, gaps, reach))
        bottom = rects[BOTTOM]
        check(block.y() >= bottom.y() + bottom.height(),
              "%s: the block %s is under the bottom pill %s" % (tag, fmt_rect(block), fmt_rect(bottom)))
        check(abs(block.x() + block.width() / 2.0 - pointer.x()) <= CENTRE_TOLERANCE_PX + 1,
              "%s: the block is centred under the pointer" % tag)

    # ---- screenshots ---------------------------------------------------------------------------

    def save_shot(r, name, caption, pointer=None):
        r = r.intersected(screen_rect())
        pix = QGuiApplication.primaryScreen().grabWindow(0, r.x(), r.y(), r.width(), r.height())
        if pointer is not None:
            painter = QPainter(pix)
            painter.setPen(QPen(QColor(255, 40, 40), 1))
            p = pointer - r.topLeft()
            painter.drawLine(p.x() - 8, p.y(), p.x() + 8, p.y())
            painter.drawLine(p.x(), p.y() - 8, p.x(), p.y() + 8)
            painter.end()
        path = os.path.join(gt.OUT, name + ".png")
        report("NOTE caption %s: %s" % (name, caption))
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def shoot_pointer(name, caption, pointer, pad=40):
        """The pointer window and the pointer, with a red cross where the pointer is."""
        QApplication.processEvents()
        r = QRect(pointer.x() - 30, pointer.y() - 30, 60, 60)
        snap = hud_snapshot()
        if snap is not None and snap["shown"]:
            r = r.united(snap["window"])
        r = QRect(r.x() - pad, r.y() - pad, r.width() + 2 * pad, r.height() + 2 * pad)
        save_shot(r, name, caption + "; the red cross marks the pointer", pointer)

    def shoot_widget(w, name, caption, pad=30):
        QApplication.processEvents()
        if w is None or not w.isVisible():
            report("NOTE %s: nothing visible to shoot (%s)" % (name, caption))
            return
        r = global_rect(w)
        save_shot(QRect(r.x() - pad, r.y() - pad, r.width() + 2 * pad, r.height() + 2 * pad), name, caption)

    # ---- sequences -----------------------------------------------------------------------------

    def mid_and_corner(tag, name, expected, what):
        """With global mode engaged: the pills with the pointer mid-graph, then near the corner."""
        mid = graph_point()
        yield from move_pointer(mid)
        yield ("sleep", SETTLE_MS)
        check_badges(tag + ", mid-graph", expected, QCursor.pos(), False)
        shoot_pointer(name + "-mid", "Crosshair only, %s, pointer mid-graph" % what, QCursor.pos())
        corner = corner_point()
        yield from move_pointer(corner)
        yield ("sleep", SETTLE_MS)
        check(QCursor.pos() == corner, "%s: the pointer is at the corner %s (%s)" % (tag, fmt_pt(corner), fmt_pt(QCursor.pos())))
        check_badges(tag + ", corner", expected, QCursor.pos(), True)
        shoot_pointer(name + "-corner", "Crosshair only, %s, pointer %d px from the bottom-right corner of the screen"
                      % (what, CORNER_INSET_PX), QCursor.pos())
        yield from move_pointer(mid)

    def stroke_with_badges(tag, name, expected, read, what):
        """A synthetic stroke from the graph's centre: the pills stay up, centred on the pointer."""
        start = graph_point()
        yield from move_pointer(start)
        before = read()
        gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        check_badges(tag + ", pressed", expected, QCursor.pos(), False)
        for k in range(1, 9):
            gmouse(QEvent.MouseMove, QPoint(start.x() + 6 * k, start.y() - 3 * k), Qt.NoButton, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        check(not reveal_shown(), "%s: no reveal mid-stroke" % tag)
        # Synthetic moves leave the real pointer at the press point, and the window follows the
        # real pointer on every sync.
        check_badges(tag + ", mid-stroke", expected, QCursor.pos(), False)
        shoot_pointer(name, "Crosshair only, %s, mid-stroke 48 px right and 24 px up of the press" % what, QCursor.pos())
        gmouse(QEvent.MouseButtonRelease, QPoint(start.x() + 48, start.y() - 24), Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        after = read()
        check(after != before, "%s: the stroke edited the knob (%s -> %s)" % (tag, before, after))
        check(hud_shown() and pill_texts() == expected, "%s: the pills stay after the release" % tag)
        send_key(Qt.Key_Z, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        check(read() == before, "%s: one Ctrl+Z restores the knob (%s)" % (tag, read()))

    def kind_of_title(title):
        knob = (title or "").split(": ")[-1]
        return TRANSFORM_KNOB_KINDS.get(knob)

    def steps():
        gt.section("setup")
        ok = yield ("until", lambda: node_graph_view() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        win = main_window()
        win.activateWindow()
        win.raise_()
        yield ("until", lambda: win.isActiveWindow(), 10.0)
        report("NOTE binary %s, screen %s, main window %s, DPI scale %.3f"
               % (QApplication.applicationFilePath(), fmt_rect(screen_rect()), fmt_rect(global_rect(win)), dpi_scale()))
        gt.close_all_panels()
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR_ONLY)
        check(NatronEngine.natron.getSettings().getParam("colorTrackballFeedback").getValue() == FEEDBACK_CROSSHAIR_ONLY,
              "the colorTrackballFeedback preference takes Crosshair only")

        blur = create(BLUR_ID, 0, 0)
        transform = create(TRANSFORM_ID, 200, 0)
        grade = create(GRADE_ID, 400, 0)
        yield ("until", lambda: all(panel_of(n) is not None for n in (blur, transform, grade)), 20.0)
        yield ("sleep", PANEL_MS)
        for n in (blur, transform, grade):
            require(panel_of(n) is not None, "%s panel opened" % n.getLabel())
        blur.getParam("size").set(10.0, 10.0)
        QApplication.processEvents()
        check(not hud_shown(), "the pointer window is not up before Ctrl+E")

        # ---- crosshair-only -------------------------------------------------------------------
        gt.section("crosshair-only")
        if engage_knob(grade, GAIN, colour=True):
            check(hud_mode() == "C", "a fresh colour engage starts in C (HUD %r)" % hud_mode())
            for key, mode, what in ((Qt.Key_S, "S", "Grade gain in S (hue and saturation)"),
                                    (Qt.Key_T, "T", "Grade gain in T (temperature and tint)"),
                                    (Qt.Key_V, "V", "Grade gain in V (value)"),
                                    (Qt.Key_C, "C", "Grade gain in C: no pills, the hint block alone")):
                send_key(key)
                yield ("sleep", SETTLE_MS)
                check(hud_mode() == mode, "%s picks mode %s (HUD %r)" % (chr(key), mode, hud_mode()))
                yield from mid_and_corner("Grade gain %s" % mode, "co-grade-%s" % mode.lower(), COLOUR_LABELS[mode], what)
        leave()
        yield ("sleep", SETTLE_MS)
        check(not hud_shown(), "Esc hides the pointer window")
        drop_stale_wrappers()

        if engage_knob(blur, "size"):
            yield from mid_and_corner("Blur size", "co-blur-size", KIND_LABELS["size"], "Blur size")
        leave()
        yield ("sleep", SETTLE_MS)

        if engage_knob(transform, "rotate"):
            yield from mid_and_corner("Transform rotate", "co-transform-rotate", KIND_LABELS["rotate"], "Transform engaged on rotate")
        leave()
        yield ("sleep", SETTLE_MS)
        drop_stale_wrappers()

        # ---- stroke ---------------------------------------------------------------------------
        gt.section("stroke")
        if engage_knob(grade, GAIN, colour=True):
            send_key(Qt.Key_S)
            yield ("sleep", SETTLE_MS)
            yield from stroke_with_badges("Grade gain S", "co-grade-s-stroke", COLOUR_LABELS["S"],
                                          lambda: val(grade, GAIN), "Grade gain in S")
        leave()
        yield ("sleep", SETTLE_MS)
        drop_stale_wrappers()
        if engage_knob(blur, "size"):
            yield from stroke_with_badges("Blur size", "co-blur-size-stroke", KIND_LABELS["size"],
                                          lambda: val(blur, "size"), "Blur size")
        leave()
        yield ("sleep", SETTLE_MS)

        # ---- switch ---------------------------------------------------------------------------
        gt.section("switch")
        if engage_knob(transform, "rotate"):
            yield from move_pointer(graph_point())
            for key, kind in ((Qt.Key_Q, "translate"), (Qt.Key_E, "scale"), (Qt.Key_R, "skew"),
                              (Qt.Key_T, "translate"), (Qt.Key_W, "rotate")):
                send_key(key)
                yield ("sleep", 250)
                check(hud_shown() and pill_texts() == KIND_LABELS[kind],
                      "Transform: %s relabels the pills for %s (%r)" % (chr(key), kind, pill_texts()))
            first = hud_knob()
            send_key(Qt.Key_Down, Qt.ControlModifier)
            yield ("sleep", 250)
            stepped = hud_knob()
            kind = kind_of_title(stepped)
            check(stepped != first, "Ctrl+Down steps off %r (now %r)" % (first, stepped))
            if kind is not None:
                check(pill_texts() == KIND_LABELS[kind], "after Ctrl+Down to %r the pills read %s (%r)" % (stepped, kind, pill_texts()))
            else:
                report("NOTE Ctrl+Down reached %r, which has no label set in this script" % stepped)
            send_key(Qt.Key_Up, Qt.ControlModifier)
            yield ("sleep", 250)
            check(hud_knob() == first and pill_texts() == KIND_LABELS["rotate"],
                  "Ctrl+Up back to %r relabels the pills for rotate (%r, %r)" % (first, hud_knob(), pill_texts()))
        leave()
        yield ("sleep", SETTLE_MS)
        drop_stale_wrappers()

        # ---- reveal ---------------------------------------------------------------------------
        gt.section("reveal")
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)
        if engage_knob(blur, "size"):
            start = graph_point()
            yield from move_pointer(start)
            yield ("sleep", SETTLE_MS)
            check(not reveal_shown(), "Crosshair + reveal: no reveal between strokes")
            check_badges("Crosshair + reveal, idle", KIND_LABELS["size"], QCursor.pos(), False)
            shoot_pointer("cr-blur-size-idle", "Crosshair + reveal, Blur size between strokes", QCursor.pos())
            before = val(blur, "size")
            gmouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
            yield ("sleep", SETTLE_MS)
            check(reveal_shown(), "Crosshair + reveal: the reveal comes up on the press")
            check(not hud_shown(), "Crosshair + reveal: the pointer window hides while the reveal is up")
            for k in range(1, 9):
                gmouse(QEvent.MouseMove, QPoint(start.x() + 6 * k, start.y() - 3 * k), Qt.NoButton, Qt.LeftButton)
            yield ("sleep", SETTLE_MS)
            check(reveal_shown() and not hud_shown(), "Crosshair + reveal: mid-stroke the reveal is up and the pointer window is not")
            shoot_widget(reveal_window(), "cr-blur-size-stroke",
                         "Crosshair + reveal, Blur size mid-stroke: the reveal, unchanged, with its own pills")
            gmouse(QEvent.MouseButtonRelease, QPoint(start.x() + 48, start.y() - 24), Qt.LeftButton, Qt.NoButton)
            yield ("sleep", SETTLE_MS)
            check(not reveal_shown(), "Crosshair + reveal: the reveal goes on the release")
            check(hud_shown() and pill_texts() == KIND_LABELS["size"], "Crosshair + reveal: the pills come back on the release")
            send_key(Qt.Key_Z, Qt.ControlModifier)
            yield ("sleep", SETTLE_MS)
            check(val(blur, "size") == before, "one Ctrl+Z restores Blur size (%s)" % (val(blur, "size"),))
        leave()
        yield ("sleep", SETTLE_MS)
        check(not hud_shown(), "Crosshair + reveal: Esc hides the pointer window")

        # ---- widget-cursor --------------------------------------------------------------------
        gt.section("widget-cursor")
        set_pref("colorTrackballFeedback", FEEDBACK_WIDGET)
        if engage_knob(blur, "size"):
            yield from move_pointer(graph_point())
            yield ("sleep", SETTLE_MS)
            check(not hud_shown(), "Widget cursor: no pointer window")
            check(reveal_shown(), "Widget cursor: the widget cursor's disc is up")
            set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR_ONLY)
            yield ("sleep", SETTLE_MS)
            check(hud_shown() and pill_texts() == KIND_LABELS["size"],
                  "switching to Crosshair only mid-engage brings up the pills (%r)" % (pill_texts(),))
            check(not reveal_shown(), "switching to Crosshair only mid-engage drops the disc")
            set_pref("colorTrackballFeedback", FEEDBACK_WIDGET)
            yield ("sleep", SETTLE_MS)
            check(not hud_shown(), "switching back to Widget cursor mid-engage hides the pointer window")
        leave()
        yield ("sleep", SETTLE_MS)
        check(not hud_shown(), "the pointer window is down after leaving")
        set_pref("colorTrackballFeedback", FEEDBACK_CROSSHAIR)
        check(not engaged(), "the run ends with global mode left")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
            f.write("FAIL script error: " + traceback.format_exc() + "\n")
        os._exit(1)
