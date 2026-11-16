"""Live geometry selection in the 3D view: mode-switch conversion, grow, shrink and convert.

Graph: GeoPrimitive (grid, 10 x 10 faces, 121 points) -> Viewer1, later GeoPrimitive -> GeoDelete
(points 0-60) -> Viewer1. The view is put in the top orthographic view (C) and framed (A), so the
grid is a square centred in it and its centre is a point.

Checks:
  - Points: a click at the centre selects one point; Grow (Ctrl+Up) twice gives the 13-point diamond
    and Shrink (Ctrl+Down) walks back through 5 points to the original single point; growing again
    returns the same diamond;
  - the diamond in Primitives mode (F11) keeps only the 4 faces whose corners are all selected, and
    Points mode (F9) turns those into their 9 corners;
  - Convert to Edges (Ctrl+F10) from those 9 points gives the 12 edges between them, Convert to
    Primitives (Ctrl+F11) the 12 faces using one of those edges, and Convert to Points (Ctrl+F9)
    the 21 points of those faces; the pick mode follows each conversion;
  - Objects mode (F8) clears the selection;
  - with every point selected, viewing the GeoDelete output (fewer points) drops the numbers past
    its count and the view shows no error.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
PICK_TIMEOUT_S = 3.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
DEFAULT_NAVIGATION_PRESET = 0
MODES = ("objects", "points", "edges", "primitives")
GRID_POINTS = 121
KEPT_POINTS = GRID_POINTS - 61

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    grid = app.createNode("GeoPrimitive")
    require(grid is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(10)
    grid.getParam("gridColumns").setValue(10)
    delete = app.createNode("GeoDelete")
    require(delete is not None, "create GeoDelete", plugin_problem("GeoDelete"))
    require(delete.connectInput(0, grid), "connect the GeoDelete to the GeoPrimitive")
    delete.getParam("groupType").setValue(0)
    delete.getParam("group").setValue("0-60")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    grid.setPosition(0, 0)
    delete.setPosition(0, 100)
    viewer.setPosition(0, 220)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def count():
        return int(prop("natronGeoSelectionCount", 0))

    def pattern():
        return str(prop("natronGeoSelection", ""))

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def row():
        rows = [w for w in QApplication.allWidgets() if w.objectName() == "geoDisplayRow"]
        return rows[0] if rows else None

    def control(name):
        r = row()
        if r is None:
            return None
        found = [w for w in r.children() if hasattr(w, "objectName") and w.objectName() == name]
        return found[0] if found else None

    def framebuffer():
        v = view()
        image = v.grab().toImage()
        if image.isNull():
            origin = v.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(), v.height()).toImage()
        return image

    def differs(a, b, step=2):
        if a.size() != b.size():
            return True
        for y in range(0, a.height(), step):
            for x in range(0, a.width(), step):
                if a.pixel(x, y) != b.pixel(x, y):
                    return True
        return False

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers))

    def click(pos, modifiers=Qt.NoModifier):
        v = view()
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()

    def marquee(start, end, modifiers=Qt.NoModifier, steps=10):
        v = view()
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, modifiers)
        for i in range(1, steps + 1):
            mouse(v, QEvent.MouseMove, start + (end - start) * (float(i) / steps), Qt.NoButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()

    def key(k, modifiers=Qt.NoModifier):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def pick_until(positions, want, modifiers=Qt.NoModifier):
        """Clicks each position in turn until want() holds after the click is served."""
        for pos in positions:
            click(pos, modifiers)
            ok = yield ("until", want, PICK_TIMEOUT_S)
            if ok:
                return pos
        return None

    def around(pos, radius=6, step=3):
        yield pos
        for r in range(step, radius + 1, step):
            for dx, dy in ((-r, r), (-r, 0), (0, r), (r, r), (r, 0), (0, -r), (-r, -r), (r, -r), (-r, r)):
                yield pos + QPoint(dx, dy)

    def set_mode(index):
        key((Qt.Key_F8, Qt.Key_F9, Qt.Key_F10, Qt.Key_F11)[index])
        return ("until", lambda: prop("natronGeoPickMode") == MODES[index], PICK_TIMEOUT_S)

    def whole_view():
        v = view()
        return QPoint(3, 3), QPoint(v.width() - 4, v.height() - 4)

    def act(k, modifiers, want, label):
        key(k, modifiers)
        ok = yield ("until", want, PICK_TIMEOUT_S)
        check(ok, label + "")
        return ok

    def selection_is(n, mode):
        return lambda: count() == n and prop("natronGeoPickMode") == mode

    def steps():
        require(viewer.connectInput(0, grid), "connect the Viewer to the GeoPrimitive")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the grid (%r)" % (prop("natronSceneError"),))
        key(Qt.Key_C)
        yield ("until", lambda: prop("natronSceneViewMode") == "top", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()

        ok = yield set_mode(1)
        require(ok, "Points mode is on")
        yield settle()
        centre = QPoint(view().width() // 2, view().height() // 2)
        hit = yield from pick_until(around(centre), lambda: count() == 1)
        require(hit is not None, "a click at the grid's centre selects one point (%r)" % pattern())
        single = pattern()
        yield settle()
        shot("m361-selection-point.png")

        ok = yield from act(Qt.Key_Up, Qt.ControlModifier, selection_is(5, "points"), "Grow gives the point and its 4 neighbours")
        yield settle()
        shot("m361-selection-grow-once.png")
        ok = yield from act(Qt.Key_Up, Qt.ControlModifier, selection_is(13, "points"), "Grow again gives the 13-point diamond")
        diamond = pattern()
        yield settle()
        shot("m361-selection-diamond.png")

        yield from act(Qt.Key_Down, Qt.ControlModifier, selection_is(5, "points"), "Shrink returns to the 5 points")
        yield from act(Qt.Key_Down, Qt.ControlModifier, selection_is(1, "points"), "Shrink again returns to the single point")
        check(pattern() == single, "and it is the point first picked (%r, was %r)" % (pattern(), single))
        yield settle()
        shot("m361-selection-shrunk.png")
        key(Qt.Key_Up, Qt.ControlModifier)
        key(Qt.Key_Up, Qt.ControlModifier)
        ok = yield ("until", selection_is(13, "points"), PICK_TIMEOUT_S)
        check(ok and pattern() == diamond, "growing twice again gives the same diamond (%r)" % pattern())

        ok = yield set_mode(3)
        check(ok and count() == 4, "in Primitives mode only the 4 fully covered faces remain (%d)" % count())
        yield settle()
        shot("m361-selection-primitives.png")
        ok = yield set_mode(1)
        check(ok and count() == 9, "back in Points mode they give their 9 corners (%d)" % count())
        yield settle()
        shot("m361-selection-corners.png")

        yield from act(Qt.Key_F10, Qt.ControlModifier, selection_is(12, "edges"),
                       "Convert to Edges gives the 12 edges between the 9 points and picks Edges")
        yield settle()
        shot("m361-selection-edges.png")
        yield from act(Qt.Key_F11, Qt.ControlModifier, selection_is(12, "primitives"),
                       "Convert to Primitives gives the 12 faces using one of the edges and picks Primitives")
        yield from act(Qt.Key_F9, Qt.ControlModifier, selection_is(21, "points"),
                       "Convert to Points gives the 21 points of those faces and picks Points")
        yield settle()
        shot("m361-selection-converted-points.png")

        ok = yield set_mode(0)
        check(ok and count() == 0, "Objects mode clears the selection (%d)" % count())

        ok = yield set_mode(1)
        require(ok, "Points mode is on")
        start, end = whole_view()
        marquee(start, end)
        ok = yield ("until", lambda: count() == GRID_POINTS, PICK_TIMEOUT_S)
        check(ok, "a marquee over the view selects all %d points (%d)" % (GRID_POINTS, count()))
        yield settle()
        shot("m361-selection-all.png")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, delete), "connect the Viewer to the GeoDelete")
        ok = yield ("until", lambda: 0 < count() <= KEPT_POINTS, POLL_TIMEOUT_S)
        check(ok, "numbers past the new point count are dropped (%d of %d points left)" % (count(), KEPT_POINTS))
        yield settle()
        # Every point of the new detail is encoded as "*", which names no number past its count.
        if pattern() == "*":
            check(count() == KEPT_POINTS, "no remaining number reaches the new count (%r, %d)" % (pattern(), count()))
        else:
            numbers = [int(n) for n in re.findall(r"\d+", pattern())]
            check(bool(numbers) and max(numbers) < KEPT_POINTS, "no remaining number reaches the new count (%r)" % pattern())
        check(prop("natronGeoPickMode") == "points" and not prop("natronSceneError"), "the mode is kept and the view shows no error (%r)" % (prop("natronSceneError"),))
        shot("m361-selection-deleted.png")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
