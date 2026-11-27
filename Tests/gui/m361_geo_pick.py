"""Point, edge and primitive picking in the 3D view showing geometry.

Graph: GeoPrimitive (cube, one division per side) -> Viewer1, then GeoPrimitive -> GeoToUSD ->
USDToGeo (unpack off, prim pattern /geo/mesh_0) -> Viewer1 for packed USD, then GeoToUSD ->
Viewer1 for a scene. The view is put in the front orthographic view (Z) and framed (A), so the
cube is a square centred in it.
The square's extent is read from the framebuffer, since there is no Python access to the view's
camera.

Checks:
  - F8 to F11 switch the pick mode (natronGeoPickMode) and the toolbar buttons follow, and a
    toolbar button switches the mode back;
  - Points: a click on the square's corner selects one point and the view draws it; Shift+click
    on it keeps one point; Ctrl+click on it toggles it off; a marquee over the view selects points;
  - switching to Edges converts the picked points to the edges whose two ends are both picked;
  - Edges: a click on the middle of the square's right side selects one edge, a point pair
    ("p<a>-<b>"); a marquee over the view selects edges;
  - Primitives: a click at the centre selects one primitive; a marquee over the view selects
    primitives;
  - packed USD from USDToGeo picks as one whole primitive, and not in Edges mode;
  - downstream of GeoToUSD the view is a scene: no component picking (natronGeoComponents false),
    the pick mode buttons are disabled and a click selects a prim (natronSceneSelection).

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
CUBE_MESH = "/geo/mesh_0"
MODES = (("objects", "geoPickObjects"), ("points", "geoPickPoints"), ("edges", "geoPickEdges"),
         ("primitives", "geoPickPrimitives"))

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QWidget

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    primitive = app.createNode("GeoPrimitive")
    require(primitive is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    primitive.getParam("type").setValue(2)
    primitive.getParam("cubeSubdivisions").set(1, 1, 1)
    to_usd = app.createNode("GeoToUSD")
    require(to_usd is not None, "create GeoToUSD", plugin_problem("GeoToUSD"))
    require(to_usd.connectInput(0, primitive), "connect the GeoToUSD to the GeoPrimitive")
    to_geo = app.createNode("USDToGeo")
    require(to_geo is not None, "create USDToGeo", plugin_problem("USDToGeo"))
    to_geo.getParam("unpack").setValue(False)
    to_geo.getParam("primPattern").setValue(CUBE_MESH)
    require(to_geo.connectInput(0, to_usd), "connect the USDToGeo to the GeoToUSD")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    primitive.setPosition(0, 0)
    to_usd.setPosition(0, 100)
    to_geo.setPosition(0, 200)
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

    def row():
        rows = [w for w in QApplication.allWidgets() if w.objectName() == "sceneToolBar"]
        return rows[0] if rows else None

    def control(name):
        r = row()
        if r is None:
            return None
        found = [w for w in r.findChildren(QWidget) if w.objectName() == name]
        return found[0] if found else None

    def framebuffer():
        v = view()
        image = v.grab().toImage()
        if image.isNull():
            origin = v.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(), v.height()).toImage()
        return image

    def differs(a, b, step=2, around=None, radius=12):
        """Whether the frames differ, sampling every step pixels, or every pixel within radius of around."""
        if a.size() != b.size():
            return True
        xs, ys = range(0, a.width(), step), range(0, a.height(), step)
        if around is not None:
            xs = range(max(0, around.x() - radius), min(a.width(), around.x() + radius + 1))
            ys = range(max(0, around.y() - radius), min(a.height(), around.y() + radius + 1))
        for y in ys:
            for x in xs:
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

    def square():
        """The cube's square in the front view: its centre and its half extent, from the pixels
        along the centre row that differ from the view's corner."""
        image = framebuffer()
        background = image.pixel(2, 2)
        cy = image.height() // 2
        cx = image.width() // 2
        right = cx
        for x in range(cx, image.width() - 2):
            if image.pixel(x, cy - 3) != background:
                right = x
        top = cy
        for y in range(cy, 1, -1):
            if image.pixel(cx + 3, y) != background:
                top = y
        return QPoint(cx, cy), right - cx, cy - top

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
        return ("until", lambda: prop("natronGeoPickMode") == MODES[index][0], PICK_TIMEOUT_S)

    def point_numbers(text):
        """The point numbers of a points pattern ("3 5", "0-6", "0-6:2"), or None for every point ("*")."""
        if text.strip() == "*":
            return None
        numbers = set()
        for token in text.split():
            m = re.match(r"^(\d+)(?:-(\d+)(?::(\d+))?)?$", token)
            if m is None:
                continue
            first = int(m.group(1))
            last = int(m.group(2)) if m.group(2) else first
            numbers.update(range(first, last + 1, int(m.group(3)) if m.group(3) else 1))
        return numbers

    def edge_ends(text):
        """The edges of an edges pattern, whose tokens are paths of point numbers ("p0-1-3")."""
        edges = []
        for token in text.split():
            path = [int(n) for n in token.lstrip("p").split("-") if n.isdigit()]
            edges.extend(zip(path, path[1:]))
        return edges

    def whole_view():
        v = view()
        return QPoint(3, 3), QPoint(v.width() - 4, v.height() - 4)

    def steps():
        require(viewer.connectInput(0, primitive), "connect the Viewer to the GeoPrimitive")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the cube (%r)" % (prop("natronSceneError"),))
        key(Qt.Key_Z)
        yield ("until", lambda: prop("natronSceneViewMode") == "front", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()

        r = row()
        check(r is not None and r.isVisible(), "the 3D view's tool bar shows")
        check(bool(prop("natronGeoComponents")) is False, "in Objects mode no components are picked")
        for index in (1, 2, 3, 0):
            ok = yield set_mode(index)
            if check(ok, "F%d switches the pick mode to %s" % (8 + index, MODES[index][0])):
                button = control(MODES[index][1])
                check(button is not None and button.isChecked(), "the toolbar button %s follows" % MODES[index][1])
        button = control("geoPickEdges")
        if check(button is not None, "the toolbar has a %s button" % "geoPickEdges"):
            button.click()
            QApplication.processEvents()
            ok = yield ("until", lambda: prop("natronGeoPickMode") == "edges", PICK_TIMEOUT_S)
            check(ok, "the toolbar Edges button switches the pick mode")

        ok = yield set_mode(1)
        require(ok, "Points mode is on")
        yield settle()
        check(bool(prop("natronGeoComponents")), "components are pickable on geometry")
        centre, half_w, half_h = square()
        report("NOTE cube square centre %d,%d half %dx%d" % (centre.x(), centre.y(), half_w, half_h))
        require(10 < half_w < view().width() // 2 - 4 and 10 < half_h < view().height() // 2 - 4,
                "the cube draws as a square inside the front view")
        corner = centre + QPoint(half_w, -half_h)
        before = framebuffer()
        hit = yield from pick_until(around(corner), lambda: count() == 1)
        if check(hit is not None, "a click on the cube's corner selects one point (%r)" % pattern()):
            check(pattern() != "" and not pattern().startswith("p"), "the selection is a point number (%r)" % pattern())
            yield settle()
            check(differs(before, framebuffer(), around=hit), "the selected point is drawn highlighted")
            shot("m361-point.png")
            picked = pattern()
            click(hit, Qt.ShiftModifier)
            yield ("sleep", SETTLE_MS)
            check(count() == 1 and pattern() == picked, "Shift+click on it keeps the one point (%r)" % pattern())
            click(hit, Qt.ControlModifier)
            ok = yield ("until", lambda: count() == 0, PICK_TIMEOUT_S)
            check(ok, "Ctrl+click on it toggles the point off")
        start, end = whole_view()
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 1, PICK_TIMEOUT_S)
        report("NOTE points marquee %r" % pattern())
        check(ok and count() <= 8, "a marquee over the view selects the cube's points (%d)" % count())

        picked_points = point_numbers(pattern())
        ok = yield set_mode(2)
        require(ok, "Edges mode is on")
        edges = edge_ends(pattern())
        report("NOTE points %r converted to edges %r" % (sorted(picked_points) if picked_points is not None else "*", pattern()))
        if picked_points is None:
            check(count() == 12, "switching to Edges with every point picked gives the cube's 12 edges (%d)" % count())
        else:
            check(len(edges) == count() and all(a in picked_points and b in picked_points for a, b in edges),
                  "switching to Edges keeps only edges whose ends were both picked (%r)" % pattern())
        side = centre + QPoint(half_w, 0)
        hit = yield from pick_until(around(side, radius=4, step=2), lambda: count() == 1)
        if check(hit is not None, "a click on the cube's right side selects one edge (%r)" % pattern()):
            check(pattern().startswith("p") and pattern().count("-") == 1,
                  "the edge is a point pair (%r)" % pattern())
            yield settle()
            shot("m361-edge.png")
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 1, PICK_TIMEOUT_S)
        report("NOTE edges marquee %r" % pattern())
        check(ok and count() <= 12, "a marquee over the view selects the cube's edges (%d)" % count())

        ok = yield set_mode(3)
        require(ok, "Primitives mode is on")
        hit = yield from pick_until(around(centre), lambda: count() == 1)
        if check(hit is not None, "a click at the cube's centre selects one primitive (%r)" % pattern()):
            yield settle()
            shot("m361-primitive.png")
        marquee(start, end)
        ok = yield ("until", lambda: count() >= 1, PICK_TIMEOUT_S)
        report("NOTE primitives marquee %r" % pattern())
        check(ok and count() <= 6, "a marquee over the view selects the cube's primitives (%d)" % count())

        viewer.disconnectInput(0)
        require(viewer.connectInput(0, to_geo), "connect the Viewer to the USDToGeo")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the packed USD (%r)" % (prop("natronSceneError"),))
        yield settle()
        check(bool(prop("natronGeoComponents")), "packed USD from USDToGeo is geometry")
        hit = yield from pick_until(around(centre), lambda: count() == 1)
        if check(hit is not None, "a click on the packed USD selects one primitive (%r)" % pattern()):
            check(pattern() == "0", "it is the whole packed primitive 0 (%r)" % pattern())
            yield settle()
            shot("m361-packed.png")
        ok = yield set_mode(2)
        require(ok, "Edges mode is on")
        click(centre)
        yield ("sleep", SETTLE_MS)
        check(count() == 0, "a packed primitive has no edges to pick (%r)" % pattern())

        viewer.disconnectInput(0)
        require(viewer.connectInput(0, to_usd), "connect the Viewer to the GeoToUSD")
        ok = yield ("until", lambda: scene_drawn() and not prop("natronGeoComponents"), POLL_TIMEOUT_S)
        check(ok, "downstream of GeoToUSD no components are pickable")
        button = control("geoPickPoints")
        check(button is not None and not button.isEnabled(), "the pick mode buttons are disabled for a scene")
        yield settle()
        click(centre)
        ok = yield ("until", lambda: bool(prop("natronSceneSelection")), PICK_TIMEOUT_S)
        check(ok, "a click selects a prim (%r)" % (prop("natronSceneSelection"),))
        check(count() == 0, "and no components")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
