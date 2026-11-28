"""Direct painting across a seam and a UDIM tile boundary in the Viewer's 3D view.

Graph: USDRead(texture-paint-seam-cube.usda) into USDTexturePaint and into Viewer1, so the 3D view shows the
cube, with USDTexturePaint's panel open, which makes it the node the view paints into, and USDTexturePaint
into a Write. Paint Model is Direct, Tile Resolution 1K, Viewer 3D Navigation preset 0 (Ctrl+left
tumbles). The default view looks at the cube's front, right and top faces; the front face lies in tile 1001
and the top face in tile 1002, across the front/top edge. A plain left drag from the front face over that
edge onto the top face paints a Solid pure red stroke straight into the texels: the output changes without
any orbit, once the node's hidden bakeGeneration knob counts the paint landing, and the open-stroke overlay
goes once its texels are in the texture. Tiles 1001 and 1002 are rendered through a Write with Output Tile
set to each and read back: both islands hold red (pure red survives any colour transform). A Ctrl+left drag
then orbits, with no bake progress drawn, and the Edit menu's Undo takes the stroke off both tiles. Last,
the reverse drag brings the default view back, and one Buffer stroke over the front face is baked by an
orbit and lands only in the front face's island in tile 1001, tile 1002 staying clean.

Mouse events are sent to the SceneViewport (synthesized window-system events do not reach Natron's
widgets under Xvfb). Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots and results.txt
go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/), one PNG per screenshot. Exit status 0 only
if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
BAKE_TIMEOUT_S = 180.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
CTRL_TUMBLE_PRESET = 0
BUFFER = 0
DIRECT = 1
TILE_1K = 0
MIN_RED_TEXELS = 50
# Where the default view (azimuth 35, elevation 25 degrees) shows the front and top faces' centres, in
# cube half-widths right of and above the view centre.
FRONT_CENTRE = (-0.57, -0.35)
TOP_CENTRE = (0.0, 0.91)
# The faces' islands in st: the front face in tile 1001, the top face in tile 1002 at the same place.
ISLAND = (0.25, 0.45)
# The Orbit section's Ctrl+left drag, in view pixels: azimuth -64 and elevation +24 degrees.
ORBIT = (160, 60)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QImage, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow

    from guitest import (FIXTURES, OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    scene_path = os.path.join(FIXTURES, "texture-paint-seam-cube.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)
    work_dir = os.path.join(OUT, "fixtures", "m315")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()

    import guitest
    end_run = guitest.finish

    def finish_restoring_navigation():
        navigation.setValue(original_navigation)
        end_run()

    # Every end of the run, a failed require() included, goes through finish(), which exits the
    # process at once, so no try/finally in steps() would run on it.
    guitest.finish = finish_restoring_navigation
    finish = finish_restoring_navigation
    navigation.setValue(CTRL_TUMBLE_PRESET)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    paint = app.createNode("USDTexturePaint")
    require(paint is not None, "create USDTexturePaint", plugin_problem("USDTexturePaint"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    paint.setPosition(0, 100)
    viewer.setPosition(0, 200)
    write = app.createWriter(os.path.join(work_dir, "tile.png"))
    require(write is not None, "create a Write for the output tiles")
    write.setPosition(200, 200)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def overlay_count():
        v = view()
        return int(v.property("natronScenePaintOverlayCount") or 0) if v is not None else 0

    def bake_fraction():
        v = view()
        value = v.property("natronScenePaintBakeFraction") if v is not None else None
        return -1. if value is None else float(value)

    def bake_generation():
        return int(paint.getParam("bakeGeneration").getValue())

    def free_camera():
        v = view()
        return list(v.property("natronSceneFreeCamera") or []) if v is not None else []

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons,
                                              modifiers))

    def drag(v, start, delta, modifiers=Qt.NoModifier, steps=10):
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, modifiers)
        for i in range(1, steps + 1):
            mouse(v, QEvent.MouseMove, start + delta * (i / float(steps)), Qt.NoButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, start + delta, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()

    def node_graph_view():
        views = [w for w in widgets_of("NodeGraph") if w.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                    if w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def edit_menu_actions():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                    return menu.actions()
        return []

    def undo_action():
        for a in edit_menu_actions():
            if a.text().replace("&", "").startswith("Undo"):
                return a
        return None

    def read_tile(udim, name):
        """Tile udim of the output rendered through the Write, as (red texels, red texels in ISLAND), or (-1, -1)."""
        path = os.path.join(work_dir, name)
        if os.path.exists(path):
            os.remove(path)
        paint.getParam("outputTile").setValue(udim)
        write.getParam("filename").setValue(path)
        app.renderBlocking(write, 1, 1)
        image = QImage(path)
        if image.isNull():
            return -1, -1
        image = image.convertToFormat(QImage.Format_RGBA8888)
        data = bytes(image.constBits())[:image.sizeInBytes()]
        stride = image.bytesPerLine()
        width = image.width()
        height = image.height()
        # Two texels of seam gutter either side of the island.
        x1 = int(ISLAND[0] * width) - 2
        x2 = int(ISLAND[1] * width) + 2
        y1 = int((1. - ISLAND[1]) * height) - 2
        y2 = int((1. - ISLAND[0]) * height) + 2
        red = 0
        inside = 0
        for y in range(height):
            row = data[y * stride:y * stride + 4 * width]
            for x, (r, g, b, a) in enumerate(zip(row[0::4], row[1::4], row[2::4], row[3::4])):
                if a > 200 and r > 200 and g < 60 and b < 60:
                    red += 1
                    if x1 <= x < x2 and y1 <= y < y2:
                        inside += 1
        return red, inside

    def view_point(v, at):
        """The view pixel showing at, in cube half-widths right of and above the view centre."""
        scale = 0.25 * min(v.width(), v.height())
        centre = v.rect().center()
        return centre + QPoint(int(at[0] * scale), -int(at[1] * scale))

    def steps():
        require(paint.connectInput(0, usd), "connect USDTexturePaint to the USDRead")
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        require(write.connectInput(0, paint), "connect the Write to USDTexturePaint")
        colour = paint.getParam("strokeColorButton")
        for dim, value in enumerate((1., 0., 0.)):
            colour.setValue(value, dim)
        paint.getParam("tileResolution").setValue(TILE_1K)
        paint.getParam("paintModel").setValue(DIRECT)
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        app.selectNode(paint, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(paint) is not None, POLL_TIMEOUT_S)
        require(ok, "open USDTexturePaint's settings panel")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the cube (%r)" % (v.property("natronSceneError") if v else None,))

        section("Before")
        empty = [read_tile(udim, "tile-%d-empty.png" % udim) for udim in (1001, 1002)]
        report("NOTE red texels in 1001 and 1002 before painting: %r" % (empty,))
        check(all(red == 0 for red, _ in empty), "neither tile has red texels before painting")
        yield ("sleep", SETTLE_MS)
        shot("m315-direct-before.png")

        section("Direct stroke")
        v.activateWindow()
        v.setFocus()
        start = view_point(v, FRONT_CENTRE)
        end = view_point(v, TOP_CENTRE)
        report("NOTE view %dx%d, stroke from %r to %r" % (v.width(), v.height(), (start.x(), start.y()),
                                                          (end.x(), end.y())))
        camera = free_camera()
        generation = bake_generation()
        drag(v, start, end - start, steps=40)
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "the stroke's texels landed without an orbit (bakeGeneration %d, was %d)"
              % (bake_generation(), generation))
        check(free_camera() == camera, "the camera did not move")
        ok = yield ("until", lambda: overlay_count() == 0, POLL_TIMEOUT_S)
        check(ok, "the open stroke's overlay goes once its texels are in the texture (%d)" % overlay_count())
        yield ("sleep", SETTLE_MS)
        shot("m315-direct-stroke.png")
        painted = dict((udim, read_tile(udim, "tile-%d-painted.png" % udim)) for udim in (1001, 1002))
        report("NOTE red texels (all, in the island) after the stroke: %r" % (painted,))
        check(painted[1001][1] >= MIN_RED_TEXELS, "the front face's island in tile 1001 holds red (%d, need %d)"
              % (painted[1001][1], MIN_RED_TEXELS))
        check(painted[1002][1] >= MIN_RED_TEXELS, "the top face's island in tile 1002 holds red (%d, need %d)"
              % (painted[1002][1], MIN_RED_TEXELS))

        section("Orbit")
        before = free_camera()
        drag(v, v.rect().center(), QPoint(*ORBIT), Qt.ControlModifier)
        after = free_camera()
        report("NOTE free camera before %r after %r" % (before, after))
        check(len(before) == 7 and len(after) == 7 and before[4:6] != after[4:6], "a Ctrl+left drag orbits the camera")
        fractions = []
        for _ in range(10):
            yield ("sleep", 100)
            fractions.append(bake_fraction())
        report("NOTE bake progress while orbited: %r" % (fractions,))
        check(all(f < 0. for f in fractions), "orbiting in Direct shows no bake progress")
        check(overlay_count() == 0, "no overlay is drawn after the orbit (%d)" % overlay_count())
        shot("m315-direct-orbited.png")

        section("Undo")
        action = undo_action()
        report("NOTE undo action: %r" % (action.text() if action is not None else None,))
        require(action is not None and action.isEnabled(), "the Edit menu offers an enabled Undo")
        action.trigger()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        undone = dict((udim, read_tile(udim, "tile-%d-undone.png" % udim)) for udim in (1001, 1002))
        report("NOTE red texels after the undo: %r" % (undone,))
        check(undone[1001][0] == 0 and undone[1002][0] == 0, "the undo takes the stroke off both tiles")
        yield ("sleep", SETTLE_MS)
        shot("m315-direct-undone.png")

        section("Buffer stroke")
        drag(v, v.rect().center(), QPoint(-ORBIT[0], -ORBIT[1]), Qt.ControlModifier)
        home = free_camera()
        report("NOTE free camera turned back to %r" % (home,))
        require(len(home) == 7 and abs(home[4] - 35.) < 0.5 and abs(home[5] - 25.) < 0.5,
                "the reverse Ctrl+left drag brings back the default view the stroke points are measured in")
        paint.getParam("paintModel").setValue(BUFFER)
        QApplication.processEvents()
        v.activateWindow()
        v.setFocus()
        generation = bake_generation()
        front = view_point(v, FRONT_CENTRE)
        drag(v, front - QPoint(20, 0), QPoint(40, 0))
        ok = yield ("until", lambda: overlay_count() > 0, POLL_TIMEOUT_S)
        check(ok, "the Buffer stroke is drawn over the view until it is baked")
        drag(v, v.rect().center(), QPoint(0, 40), Qt.ControlModifier)
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "the Buffer session's bake landed (bakeGeneration %d, was %d)" % (bake_generation(), generation))
        yield ("until", lambda: overlay_count() == 0, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        mixed = dict((udim, read_tile(udim, "tile-%d-mixed.png" % udim)) for udim in (1001, 1002))
        report("NOTE red texels after the Buffer stroke: %r" % (mixed,))
        check(mixed[1001][0] >= MIN_RED_TEXELS, "the Buffer stroke lands in tile 1001 over the Direct session (%d)"
              % mixed[1001][0])
        check(mixed[1001][1] == mixed[1001][0], "every red texel in tile 1001 lies in the front face's island (%d of %d)"
              % (mixed[1001][1], mixed[1001][0]))
        check(mixed[1002][0] == 0, "tile 1002 stays clean (%d)" % mixed[1002][0])

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
