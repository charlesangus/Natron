"""Point selection in the 3D view picks every point whose centre projects into the box, whatever
size the points are drawn at and whatever is in front of them, and a click picks by distance to
the point's centre.

Graph: GeoPrimitive (card) -> GeoTransform -> GeoScatter (COUNT points, pscale large enough that
the drawn points overlap one another) -> Viewer1. The view is put in the front orthographic view
(Z) and framed (A), in Points mode (F9).

Checks:
  - a box over the whole view selects all COUNT points;
  - for several split positions, the boxes left and right of the split select disjoint sets that
    together are every point: a point is in exactly one box, whatever the box sizes;
  - the same holds for the four quadrants of a box about the centre, which select exactly that box's
    points between them;
  - nested boxes about the centre select nested sets;
  - a click selects at most one point, one inside a box of the click's pick radius around it, and
    does select one when a box well inside that radius holds any.

Mouse and key events are sent to the SceneViewport. Run through Tests/gui/run-gui-test.sh.
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status
0 only if every check passed.
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
COUNT = 400
PSCALE = 0.15
CLICK_RADIUS = 6
INNER_RADIUS = 3

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    card = app.createNode("GeoPrimitive")
    require(card is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    card.getParam("type").setValue(0)
    transform = app.createNode("GeoTransform")
    require(transform is not None, "create GeoTransform", plugin_problem("GeoTransform"))
    transform.getParam("translate").set(0.25, 0.1, 0.)
    require(transform.connectInput(0, card), "connect the GeoTransform to the card")
    scatter = app.createNode("GeoScatter")
    require(scatter is not None, "create GeoScatter", plugin_problem("GeoScatter"))
    scatter.getParam("generateBy").setValue(0)
    scatter.getParam("count").setValue(COUNT)
    scatter.getParam("pscale").setValue(PSCALE)
    require(scatter.connectInput(0, transform), "connect the GeoScatter to the GeoTransform")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    card.setPosition(0, 0)
    transform.setPosition(0, 100)
    scatter.setPosition(0, 200)
    viewer.setPosition(0, 320)

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

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers))

    def click(pos):
        v = view()
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def drag(start, end, steps=8):
        v = view()
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            mouse(v, QEvent.MouseMove, start + (end - start) * (float(i) / steps), Qt.NoButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def key(k):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k)
        QApplication.processEvents()

    def point_numbers(text):
        """The point numbers of a points pattern ("3 5", "0-6", "0-6:2"); every point for "*"."""
        if text.strip() == "*":
            return set(range(COUNT))
        numbers = set()
        for token in text.split():
            m = re.match(r"^(\d+)(?:-(\d+)(?::(\d+))?)?$", token)
            if m is None:
                continue
            first = int(m.group(1))
            last = int(m.group(2)) if m.group(2) else first
            numbers.update(range(first, last + 1, int(m.group(3)) if m.group(3) else 1))
        return numbers

    def served(before):
        return lambda: pattern() != before

    def box(left, top, right, bottom):
        """Selects with a marquee over the pixels left..right, top..bottom inclusive and returns
        the points selected. A marquee that selects what the last one did is read after the wait."""
        before = pattern()
        drag(QPoint(left, top), QPoint(right, bottom))
        yield ("until", served(before), PICK_TIMEOUT_S)
        return point_numbers(pattern())

    def point(pos):
        before = pattern()
        click(pos)
        yield ("until", served(before), PICK_TIMEOUT_S)
        return point_numbers(pattern())

    def steps():
        require(viewer.connectInput(0, scatter), "connect the Viewer to the GeoScatter")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the points (%r)" % (prop("natronSceneError"),))
        key(Qt.Key_Z)
        yield ("until", lambda: prop("natronSceneViewMode") == "front", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()
        key(Qt.Key_F9)
        ok = yield ("until", lambda: prop("natronGeoPickMode") == "points", PICK_TIMEOUT_S)
        require(ok, "Points mode is on")
        yield settle()
        require(bool(prop("natronGeoComponents")), "components are pickable on the scattered points")
        shot("m559-points.png")

        v = view()
        x0, y0, x1, y1 = 2, 2, v.width() - 3, v.height() - 3
        everything = yield from box(x0, y0, x1, y1)
        check(len(everything) == COUNT, "a box over the whole view selects every point (%d of %d)" % (len(everything), COUNT))
        shot("m559-points-all.png")

        for fraction in (0.3, 0.45, 0.5, 0.62, 0.8):
            split = int(x0 + (x1 - x0) * fraction)
            left = yield from box(x0, y0, split - 1, y1)
            right = yield from box(split, y0, x1, y1)
            check(not (left & right), "split at x=%d: no point is in both boxes (%d shared)" % (split, len(left & right)))
            check(len(left) + len(right) == COUNT,
                  "split at x=%d: the two boxes hold every point (%d + %d of %d)" % (split, len(left), len(right), COUNT))

        cx, cy = v.width() // 2, v.height() // 2
        nested = []
        for half in (20, 45, 90, min(cx, cy) - 4):
            inside = yield from box(cx - half, cy - half, cx + half - 1, cy + half - 1)
            nested.append((half, inside))
        for (small_half, small), (big_half, big) in zip(nested, nested[1:]):
            check(small <= big, "the box of half size %d selects a subset of the one of half size %d (%d of %d)"
                  % (small_half, big_half, len(small), len(big)))
        half, whole = nested[-1]
        quadrants = []
        for left, top, right, bottom in ((cx - half, cy - half, cx - 1, cy - 1), (cx, cy - half, cx + half - 1, cy - 1),
                                         (cx - half, cy, cx - 1, cy + half - 1), (cx, cy, cx + half - 1, cy + half - 1)):
            quadrant = yield from box(left, top, right, bottom)
            quadrants.append(quadrant)
        union = set().union(*quadrants)
        check(sum(len(q) for q in quadrants) == len(union), "no point is in two quadrants")
        check(union == whole, "the quadrants select exactly the points of their box (%d vs %d)" % (len(union), len(whole)))

        clicked = 0
        for fx in (0.35, 0.42, 0.5, 0.58, 0.65):
            for fy in (0.35, 0.5, 0.65):
                pos = QPoint(int(x0 + (x1 - x0) * fx), int(y0 + (y1 - y0) * fy))
                near = yield from box(pos.x() - CLICK_RADIUS, pos.y() - CLICK_RADIUS, pos.x() + CLICK_RADIUS, pos.y() + CLICK_RADIUS)
                close = yield from box(pos.x() - INNER_RADIUS, pos.y() - INNER_RADIUS, pos.x() + INNER_RADIUS, pos.y() + INNER_RADIUS)
                picked = yield from point(pos)
                check(len(picked) <= 1, "a click at %d,%d selects at most one point (%d)" % (pos.x(), pos.y(), len(picked)))
                check(picked <= near, "a click at %d,%d selects a point within its pick radius (%r)" % (pos.x(), pos.y(), sorted(picked)))
                if close:
                    check(len(picked) == 1, "a click at %d,%d with a point's centre %d pixels away selects one"
                          % (pos.x(), pos.y(), INNER_RADIUS))
                    clicked += 1
        report("NOTE %d clicks landed close to a point centre" % clicked)
        check(clicked > 0, "some clicks landed close to a point centre")
        shot("m559-points-click.png")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
