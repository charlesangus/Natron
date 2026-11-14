"""Painting on a UV-mapped quad in the Viewer's 3D view, orbiting, and the texture the node outputs.

Graph: USDRead(texture-paint-quad.usda) into TexturePaint and into Viewer1, so the 3D view shows the
scene, with TexturePaint's panel open, which makes it the node the view paints into, and TexturePaint
into a Write. Viewer 3D Navigation set to the Nuke preset (Ctrl+left tumbles). A plain left drag paints a Solid
stroke in pure red through the view camera. A Ctrl+left drag tumbles the camera, which closes the
session and queues its bake; the node's hidden bakeGeneration knob counts the bakes that landed.
Once it has, the node's output (UDIM tile 1001) is rendered through a Write to a PNG and read back:
the stroke's texels are red (pure red survives any colour transform). Then Bake Output writes the
texture files and pushes an undo entry; the Edit menu's Undo takes it back, and the output tile still
holds the stroke, rebuilt from the strokes now that their session is live again.

Mouse events are sent to the SceneViewport (synthesized window-system events do not reach Natron's
widgets under Xvfb). Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots and results.txt
go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
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
MIN_RED_TEXELS = 100

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QImage, QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow

    from guitest import (FIXTURES, OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    scene_path = os.path.join(FIXTURES, "texture-paint-quad.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)
    work_dir = os.path.join(OUT, "fixtures", "m53")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    paint = app.createNode("TexturePaint")
    require(paint is not None, "create TexturePaint", plugin_problem("TexturePaint"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    paint.setPosition(0, 100)
    viewer.setPosition(0, 200)
    tile_path = os.path.join(work_dir, "tile.png")
    write = app.createWriter(tile_path)
    require(write is not None, "create a Write for the output tile")
    write.setPosition(200, 200)
    bake_pattern = os.path.join(work_dir, "baked.<UDIM>.exr")

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

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons,
                                              modifiers))

    def drag(v, start, delta, modifiers=Qt.NoModifier):
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, modifiers)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + delta * (i / 10.), Qt.NoButton, Qt.LeftButton, modifiers)
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

    def free_camera():
        v = view()
        return list(v.property("natronSceneFreeCamera") or []) if v is not None else []

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

    def read_tile(name):
        """The output tile rendered through the Write, as (red texel count, image), or (-1, None)."""
        path = os.path.join(work_dir, name)
        if os.path.exists(path):
            os.remove(path)
        write.getParam("filename").setValue(path)
        app.renderBlocking(write, 1, 1)
        image = QImage(path)
        if image.isNull():
            return -1, None
        image = image.convertToFormat(QImage.Format_RGBA8888)
        red = 0
        for y in range(image.height()):
            for x in range(image.width()):
                c = image.pixelColor(x, y)
                if c.alpha() > 200 and c.red() > 200 and c.green() < 60 and c.blue() < 60:
                    red += 1
        return red, image

    def steps():
        require(paint.connectInput(0, usd), "connect TexturePaint to the USDRead")
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        require(write.connectInput(0, paint), "connect the Write to TexturePaint")
        colour = paint.getParam("strokeColorButton")
        for dim, value in enumerate((1., 0., 0.)):
            colour.setValue(value, dim)
        paint.getParam("bakeOutputPath").setValue(bake_pattern)
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        app.selectNode(paint, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(paint) is not None, POLL_TIMEOUT_S)
        require(ok, "open TexturePaint's settings panel")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))

        section("Paint")
        v.activateWindow()
        v.setFocus()
        red, _ = read_tile("tile-empty.png")
        check(red <= 0, "the output tile has no red texel before painting (%d)" % red)
        centre = v.rect().center()
        generation = bake_generation()
        drag(v, centre - QPoint(120, 40), QPoint(240, 80))
        ok = yield ("until", lambda: overlay_count() > 0, POLL_TIMEOUT_S)
        check(ok, "the unbaked stroke is drawn over the view")
        shot("m53-paint-session.png")

        section("Orbit")
        before = free_camera()
        drag(v, centre, QPoint(160, 60), Qt.ControlModifier)
        after = free_camera()
        report("NOTE free camera before %r after %r" % (before, after))
        check(len(before) == 7 and len(after) == 7 and before[4:6] != after[4:6],
              "a Ctrl+left drag tumbles the camera")
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "the session's bake landed (bakeGeneration %d, was %d)" % (bake_generation(), generation))
        ok = yield ("until", lambda: overlay_count() == 0, POLL_TIMEOUT_S)
        check(ok, "the unbaked buffer is gone once the bake has landed")
        yield ("sleep", SETTLE_MS)
        shot("m53-paint-orbited.png")

        section("Texture")
        red, image = read_tile("tile-after-orbit.png")
        require(image is not None, "the output tile rendered to " + tile_path)
        report("NOTE tile %dx%d, %d red texels" % (image.width(), image.height(), red))
        check(red >= MIN_RED_TEXELS, "tile 1001 holds the red stroke (%d texels, need %d)" % (red, MIN_RED_TEXELS))

        section("Bake Output")
        baked_before = paint.getParam("bakedOutput").getValue()
        generation = bake_generation()
        paint.getParam("bakeOutput").trigger()
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "Bake Output landed (bakeGeneration %d, was %d)" % (bake_generation(), generation))
        baked_after = paint.getParam("bakedOutput").getValue()
        report("NOTE Baked Output: %r" % (baked_after,))
        check(bool(baked_after) and baked_after != baked_before, "Baked Output reports what was written")
        files = sorted(f for f in os.listdir(work_dir) if f.startswith("baked."))
        report("NOTE baked files: %r" % (files,))
        check(any("1001" in f for f in files), "the tile 1001 file was written")
        yield ("sleep", SETTLE_MS)
        red, _ = read_tile("tile-after-bake.png")
        check(red >= MIN_RED_TEXELS, "tile 1001 still holds the stroke from the baked files (%d)" % red)
        shot("m53-paint-bake-output.png")

        section("Undo")
        action = undo_action()
        report("NOTE undo action: %r" % (action.text() if action is not None else None,))
        require(action is not None and action.isEnabled(), "the Edit menu offers an enabled Undo")
        generation = bake_generation()
        action.trigger()
        QApplication.processEvents()
        yield ("until", lambda: bake_generation() > generation, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        baked_undone = paint.getParam("bakedOutput").getValue()
        report("NOTE Baked Output after undo: %r" % (baked_undone,))
        check(baked_undone != baked_after, "undoing Bake Output restores the Baked Output state")
        red, _ = read_tile("tile-after-undo.png")
        check(red >= MIN_RED_TEXELS, "after the undo tile 1001 still holds the stroke, rebuilt from its strokes (%d)" % red)
        shot("m53-paint-undone.png")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
