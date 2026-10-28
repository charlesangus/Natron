"""A GUI test of native-node overlay draw lists: the test-only Overlay node (registered when
NATRON_GUI_TEST_NODES=1, which run-gui-test.sh sets) draws one of every primitive type around its
Center knob. With its panel open and the node in the viewer, every primitive must be on screen,
hovering the centre handle must highlight it, dragging it must move Center by the drag in canonical
units, and Edit > Undo must put Center back.

Run in the natron-dev container:
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m165-overlay/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m165_overlay_primitives.py <AppImage or build/fast/App/Natron>'

Canonical coordinates are mapped to viewer widget pixels through the ViewerGL's
natronCanonicalToWidget property [origin x, origin y, pixels per unit x, pixels per unit y], and
pixels are read from screen grabs: a widget grab of the GL viewer can come out blank under llvmpipe.
The overlay geometry checked here is the node's (Engine/Nodes/Test/OverlayTestNode.cpp).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 200
SETTLE_MS = 500
OVERLAY_ID = "test.natron.built-in.Overlay"
VIEWER_ID = "fr.inria.built-in.Viewer"
DRAG_PX = (50.0, -30.0)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # PySide wrappers of widgets used across steps; a wrapper that dies takes its children's with it.
    held = {}

    # ---- plumbing --------------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def choose(combo, text):
        """Selects the first item containing text (case-insensitive); False if there is none."""
        items = [str(i) for i in (combo.property("natronItems") or [])]
        for index, item in enumerate(items):
            if text.lower() in item.lower():
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
                QApplication.processEvents()
                return True
        return False

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().replace("&", "").lower().startswith(prefix):
                        return a
        return None

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        value = held["viewer"].property("natronCanonicalToWidget")
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def to_widget(x, y):
        ox, oy, sx, sy = mapping()
        return ox + sx * x, oy + sy * y

    def to_canonical(wx, wy):
        ox, oy, sx, sy = mapping()
        return (wx - ox) / sx, (wy - oy) / sy

    def grab():
        QApplication.processEvents()
        return QGuiApplication.primaryScreen().grabWindow(0).toImage()

    def pixel(image, wx, wy):
        g = held["viewer"].mapToGlobal(QPoint(int(round(wx)), int(round(wy))))
        if not (0 <= g.x() < image.width() and 0 <= g.y() < image.height()):
            return (0, 0, 0)
        c = image.pixelColor(g.x(), g.y())
        return (c.red(), c.green(), c.blue())

    def box(image, wx, wy, radius):
        return [pixel(image, wx + dx, wy + dy) for dy in range(-radius, radius + 1) for dx in range(-radius, radius + 1)]

    def any_in_box(image, wx, wy, radius, predicate):
        return any(predicate(p) for p in box(image, wx, wy, radius))

    def is_grey_bright(p):
        return min(p) > 150 and max(p) - min(p) < 50

    def is_red(p):
        return p[0] > 150 and p[1] < 90 and p[2] < 90

    def is_orange(p):
        return p[0] > 180 and 90 < p[1] < 210 and p[2] < 90

    def is_green(p):
        return p[1] > 180 and p[0] < 100 and p[2] < 100

    def is_blue_fill(p):
        return p[2] > p[0] + 25 and p[2] > p[1] + 10

    def centre_value():
        v = held["node"].getParam("center").get()
        return (v.x, v.y)

    def canonical_at(dx, dy):
        cx, cy = centre_value()
        return to_widget(cx + dx, cy + dy)

    def mouse(kind, x, y, button, buttons):
        QApplication.sendEvent(held["viewer"], QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window is up")
        win = main_window()
        held["win"] = win
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        gt.require(OVERLAY_ID in gt._plugin_ids, "the test Overlay node is registered (NATRON_GUI_TEST_NODES=%r)"
                   % os.environ.get("NATRON_GUI_TEST_NODES"))
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode(VIEWER_ID)
        gt.require(viewer is not None, "the project has a viewer", gt.plugin_problem(VIEWER_ID))
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(OVERLAY_ID, -1, None, props)
        gt.require(node is not None, "create the Overlay test node", gt.plugin_problem(OVERLAY_ID))
        held["node"] = node
        held["viewer_node"] = viewer
        node.setPosition(0, 0)
        viewer.setPosition(0, 200)
        gt.require(viewer.connectInput(0, node), "the Overlay node feeds the viewer")
        gt.require(node.getParam("center") is not None, "the Overlay node has a Center parameter")

        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "a viewer is up")
        held["viewer"] = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()][0]

        view_combo = widget_with_tip("ComboBox", "View:")
        zoom_combo = widget_with_tip("ComboBox", "Zoom:")
        held["view_combo"] = view_combo
        held["zoom_combo"] = zoom_combo
        gt.require(view_combo is not None and zoom_combo is not None, "found the viewer's View and Zoom menus")
        check(choose(view_combo, "Un-tone-mapped") or choose(view_combo, "Raw"),
              "the viewer shows the Un-tone-mapped (or Raw) view (items %r)" % (view_combo.property("natronItems"),))
        check(choose(zoom_combo, "100%"), "the viewer zooms to 100%")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the viewer publishes natronCanonicalToWidget")
        report("NOTE canonical to widget mapping %r" % (mapping(),))

        # Centre the overlay in the viewer, on whole canonical units.
        v = held["viewer"]
        cx, cy = to_canonical(v.width() / 2.0, v.height() / 2.0)
        node.getParam("center").set(float(round(cx)), float(round(cy)))
        yield ("sleep", SETTLE_MS)

        # ---- every primitive on screen -------------------------------------------------------
        gt.section("primitives")
        gt.shot("m165-overlay-primitives.png")
        img = grab()
        sx = abs(mapping()[2])
        check(any_in_box(img, *canonical_at(-20, -20), 1, is_blue_fill),
              "the filled polygon is drawn (pixel %r)" % (pixel(img, *canonical_at(-20, -20)),))
        check(not is_blue_fill(pixel(img, *canonical_at(20, 20))),
              "the polygon's concave notch is left empty (pixel %r)" % (pixel(img, *canonical_at(20, 20)),))
        check(any_in_box(img, *canonical_at(30, 60), 1, is_grey_bright),
              "the closed polyline is drawn (top edge)")
        check(any_in_box(img, *canonical_at(-60, 0), 1, is_grey_bright),
              "the closed polyline is drawn (left edge)")
        check(any_in_box(img, *canonical_at(-70, 0), 1, is_red), "the red line is drawn")
        check(any_in_box(img, *canonical_at(70, 0), 1, is_grey_bright), "the segment list's first segment is drawn")
        check(any_in_box(img, *canonical_at(0, 70), 1, is_grey_bright), "the segment list's second segment is drawn")
        check(any_in_box(img, *canonical_at(0, -90), 1, is_orange), "the wide stroke is drawn (middle)")
        check(any_in_box(img, *canonical_at(-60, -90), 1, is_orange), "the wide stroke is drawn (start)")
        corner = sum(1 for p in box(img, *canonical_at(60, 60), 2) if is_grey_bright(p))
        edge = sum(1 for p in box(img, *canonical_at(30, 60), 2) if is_grey_bright(p))
        check(corner >= edge + 5, "a point sits on the polyline's corner (%d bright pixels there, %d mid-edge)" % (corner, edge))
        mx, my = canonical_at(60, 90)
        marker = sum(1 for p in box(img, mx, my, 3) if is_grey_bright(p))
        check(marker >= 12, "the round marker handle is drawn (%d bright pixels)" % marker)
        hx, hy = canonical_at(0, 0)
        check(is_grey_bright(pixel(img, hx, hy)), "the centre handle is drawn, not highlighted (pixel %r)" % (pixel(img, hx, hy),))
        text = 0
        for ty in range(int(hy) - 24, int(hy) - 7):
            for tx in range(int(hx) + 8, int(hx) + 60):
                if is_grey_bright(pixel(img, tx, ty)):
                    text += 1
        check(text >= 15, "the 'center' label is drawn up and right of the handle (%d bright pixels)" % text)
        report("NOTE %.3f widget pixels per canonical unit" % sx)

        # ---- hover -------------------------------------------------------------------------
        gt.section("hover")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        check(ok, "the main window is active")
        hx, hy = canonical_at(0, 0)
        mouse(QEvent.MouseMove, hx + 30, hy + 30, Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseMove, hx, hy, Qt.NoButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        gt.shot("m165-overlay-hover.png")
        img = grab()
        check(is_green(pixel(img, hx, hy)), "hovering the centre handle highlights it (pixel %r)" % (pixel(img, hx, hy),))
        mx, my = canonical_at(60, 90)
        check(not any_in_box(img, mx, my, 2, is_green), "the marker handle is not highlighted with it")

        # ---- drag and undo -----------------------------------------------------------------
        gt.section("drag")
        before = centre_value()
        mouse(QEvent.MouseButtonPress, hx, hy, Qt.LeftButton, Qt.LeftButton)
        steps_n = 5
        for i in range(1, steps_n + 1):
            mouse(QEvent.MouseMove, hx + DRAG_PX[0] * i / steps_n, hy + DRAG_PX[1] * i / steps_n, Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, hx + DRAG_PX[0], hy + DRAG_PX[1], Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS)
        gt.shot("m165-overlay-dragged.png")
        ox, oy, msx, msy = mapping()
        expected = (before[0] + DRAG_PX[0] / msx, before[1] + DRAG_PX[1] / msy)
        after = centre_value()
        tolerance = 1.0 / min(abs(msx), abs(msy))
        check(abs(after[0] - expected[0]) <= tolerance and abs(after[1] - expected[1]) <= tolerance,
              "dragging the centre handle by %r px moves Center from %r to %r (expected %r, tolerance %.3f)"
              % (DRAG_PX, before, after, expected, tolerance))
        img = grab()
        nx, ny = canonical_at(0, 0)
        check(any_in_box(img, nx, ny, 1, lambda p: is_grey_bright(p) or is_green(p)),
              "the overlay is redrawn at the new Center")

        undo = edit_menu_action("undo")
        held["undo"] = undo
        gt.require(undo is not None and undo.isEnabled(), "Edit > Undo is enabled after the drag")
        report("NOTE undo action text: %r" % undo.text())
        undo.trigger()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        restored = centre_value()
        check(abs(restored[0] - before[0]) < 1e-6 and abs(restored[1] - before[1]) < 1e-6,
              "one Edit > Undo puts Center back to %r (got %r)" % (before, restored))
        gt.shot("m165-overlay-undone.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
