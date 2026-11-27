"""Painting across two faces of a box laid out by GeoUVFlatten and GeoUVLayout, through GeoToUSD.

Graph: GeoPrimitive (cube, size 1, one division per side) -> GeoUVFlatten (Preserve Seams, so each face
is an island) -> GeoUVLayout (UDIM Tiles, Scale Islands To Match Surface Area, Fixed Scale 0.9, so each
face fills most of a tile of its own, 1001 to 1006) -> GeoToUSD -> USDTexturePaint -> Write, with the
Viewer on GeoToUSD and USDTexturePaint's panel open, which makes it the node the view paints into.
Viewer 3D Navigation set to the Nuke preset (Ctrl+left tumbles). The camera is first tumbled so two side
faces of the box face it, then a plain left drag paints a Solid pure red stroke across the middle of the
view, over both faces. A second tumble closes the session and queues its bake; the node's hidden
bakeGeneration knob counts the bakes that landed. Each tile is then rendered through the Write with
Output Tile set to it and read back: at least two tiles, so two islands, hold red texels (pure red
survives any colour transform).

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
NUKE = 0
CUBE = 2
UDIM_TILES = 1
FIXED = 1
TILE_1K = 0
TILES = range(1001, 1007)
MIN_RED_TEXELS = 50

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QImage, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import (OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    work_dir = os.path.join(OUT, "fixtures", "m428")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE)

    primitive = app.createNode("GeoPrimitive")
    require(primitive is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    primitive.getParam("type").setValue(CUBE)
    primitive.getParam("cubeSize").set(1, 1, 1)
    primitive.getParam("cubeSubdivisions").set(1, 1, 1)
    flatten = app.createNode("GeoUVFlatten")
    require(flatten is not None, "create GeoUVFlatten", plugin_problem("GeoUVFlatten"))
    flatten.getParam("preserveSeams").setValue(True)
    uv_layout = app.createNode("GeoUVLayout")
    require(uv_layout is not None, "create GeoUVLayout", plugin_problem("GeoUVLayout"))
    uv_layout.getParam("packInto").setValue(UDIM_TILES)
    uv_layout.getParam("scaleIslandsToMatchSurfaceArea").setValue(True)
    uv_layout.getParam("scale").setValue(FIXED)
    uv_layout.getParam("fixedScale").setValue(0.9)
    to_usd = app.createNode("GeoToUSD")
    require(to_usd is not None, "create GeoToUSD", plugin_problem("GeoToUSD"))
    paint = app.createNode("USDTexturePaint")
    require(paint is not None, "create USDTexturePaint", plugin_problem("USDTexturePaint"))
    paint.getParam("tileResolution").setValue(TILE_1K)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    for i, node in enumerate((primitive, flatten, uv_layout, to_usd, paint, viewer)):
        node.setPosition(0, 100 * i)
    write = app.createWriter(os.path.join(work_dir, "tile.png"))
    require(write is not None, "create a Write for the output tiles")
    write.setPosition(200, 500)

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

    def read_tile(udim):
        """Tile udim of the node's output rendered through the Write, as its red texel count, or -1."""
        path = os.path.join(work_dir, "tile-%d.png" % udim)
        if os.path.exists(path):
            os.remove(path)
        paint.getParam("outputTile").setValue(udim)
        write.getParam("filename").setValue(path)
        app.renderBlocking(write, 1, 1)
        image = QImage(path)
        if image.isNull():
            return -1
        image = image.convertToFormat(QImage.Format_RGBA8888)
        data = bytes(image.constBits())[:image.sizeInBytes()]
        stride = image.bytesPerLine()
        red = 0
        for y in range(image.height()):
            row = data[y * stride:y * stride + 4 * image.width()]
            for r, g, b, a in zip(row[0::4], row[1::4], row[2::4], row[3::4]):
                if a > 200 and r > 200 and g < 60 and b < 60:
                    red += 1
        return red

    def steps():
        require(flatten.connectInput(0, primitive), "connect GeoUVFlatten to the GeoPrimitive")
        require(uv_layout.connectInput(0, flatten), "connect GeoUVLayout to GeoUVFlatten")
        require(to_usd.connectInput(0, uv_layout), "connect GeoToUSD to GeoUVLayout")
        require(paint.connectInput(0, to_usd), "connect USDTexturePaint to GeoToUSD")
        require(viewer.connectInput(0, to_usd), "connect the Viewer to GeoToUSD")
        require(write.connectInput(0, paint), "connect the Write to USDTexturePaint")
        colour = paint.getParam("strokeColorButton")
        for dim, value in enumerate((1., 0., 0.)):
            colour.setValue(value, dim)
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
        require(ok, "the scene view shows the box (%r)" % (v.property("natronSceneError") if v else None,))
        for node in (flatten, uv_layout):
            message = node.getPersistentMessage()
            check(not message, "%s reports no warning (%r)" % (node.getPluginID(), message))

        section("Empty tiles")
        empty = [read_tile(udim) for udim in TILES]
        report("NOTE red texels per tile before painting: %r" % (empty,))
        check(all(0 <= red for red in empty), "every tile from 1001 to 1006 renders")
        check(all(red == 0 for red in empty), "no tile has red texels before painting")

        section("Tumble")
        v.activateWindow()
        v.setFocus()
        centre = v.rect().center()
        before = free_camera()
        drag(v, centre, QPoint(140, 0), Qt.ControlModifier)
        after = free_camera()
        report("NOTE free camera before %r after %r" % (before, after))
        check(before != after, "a Ctrl+left drag tumbles the camera to show two faces")
        yield ("sleep", SETTLE_MS)
        shot("m428-box-tumbled.png")

        section("Paint")
        generation = bake_generation()
        half = int(v.width() * 0.4)
        drag(v, centre - QPoint(half, 0), QPoint(2 * half, 0), steps=40)
        ok = yield ("until", lambda: overlay_count() > 0, POLL_TIMEOUT_S)
        check(ok, "the unbaked stroke is drawn over the view")
        shot("m428-stroke-across-two-faces.png")
        drag(v, centre, QPoint(0, 40), Qt.ControlModifier)
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "the session's bake landed (bakeGeneration %d, was %d)" % (bake_generation(), generation))
        yield ("until", lambda: overlay_count() == 0, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        shot("m428-stroke-baked.png")

        section("Tiles")
        painted = [read_tile(udim) for udim in TILES]
        report("NOTE red texels per tile after painting: %r" % (dict(zip(TILES, painted)),))
        islands = [udim for udim, red in zip(TILES, painted) if red >= MIN_RED_TEXELS]
        check(len(islands) >= 2, "the stroke lands on at least two islands, each in its own tile (%r, need %d texels "
                                 "each)" % (islands, MIN_RED_TEXELS))
        if islands:
            paint.getParam("outputTile").setValue(islands[0])
            yield ("sleep", SETTLE_MS)
        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
