"""Paint laid live over a UsdPreviewSurface material by USDLayerMaterial, seen in the Viewer's 3D view.

Graph: USDRead(m312-layer-material-quad.usda) into USDAssignMaterial, which binds a blue USDPreviewSurface
to /World/Quad. USDAssignMaterial feeds the Scene input of USDLayerMaterial and a USDTexturePaint in Paint
Only mode, which feeds its Paint input. Viewer1 shows USDLayerMaterial. USDTexturePaint's panel is open, so the
view paints into it. Its colour plane carries the diffuseColor purpose, which USDLayerMaterial's Diffuse
Color Layer (From Metadata) takes. A plain left drag paints a pure red Solid stroke. A Ctrl+left drag
tumbles the camera, which closes the session and queues its bake. Once the node's hidden bakeGeneration
knob counts it and the unbaked overlay is gone, the quad shows red where the stroke is, through the
layered material, and blue elsewhere. USDRead draws the quad in full, the view lights it with the headlight
and the grid and guides are hidden, since their axis lines are red and blue.

Mouse events are sent to the SceneViewport (synthesized window-system events do not reach Natron's
widgets under Xvfb). Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots and results.txt
go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
BAKE_TIMEOUT_S = 180.0
SHOW_TIMEOUT_S = 60.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
NAVIGATION_MODE_0 = 0
PAINT_ONLY = 1
MIN_RED_PIXELS = 100
MIN_BLUE_PIXELS = 200
TEXTURE_BUDGET_MB = 256
INITIAL_DRAW_FULL = 3
HEADLIGHT = 2

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import (FIXTURES, check, finish, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    scene_path = os.path.join(FIXTURES, "m312-layer-material-quad.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NAVIGATION_MODE_0)

    # Software GL reports a few MB free, under the automatic budget's headroom, so that budget would
    # load no texture and the paint would read its texture's transparent fallback.
    budget = NatronEngine.natron.getSettings().getParam("gpuTextureBudget")
    require(budget is not None, "Preferences have a gpuTextureBudget parameter")
    original_budget = budget.getValue()
    budget.setValue(TEXTURE_BUDGET_MB)

    # The scene has no light, and the default Scene Lights mode would show the lit quad black.
    lighting = NatronEngine.natron.getSettings().getParam("viewer3DLighting")
    require(lighting is not None, "Preferences have a viewer3DLighting parameter")
    original_lighting = lighting.getValue()
    lighting.setValue(HEADLIGHT)

    def create(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        return node

    usd = create("USDRead")
    usd.getParam("filename").setValue(scene_path)
    # Full: the quad draws with its material, not as the outline of its bounds.
    initial_draw = usd.getParam("initialDraw")
    require(initial_draw is not None, "USDRead has an initialDraw parameter")
    initial_draw.setValue(INITIAL_DRAW_FULL)
    surface = create("USDPreviewSurface")
    assign = create("USDAssignMaterial")
    paint = create("USDTexturePaint")
    layered = create("USDLayerMaterial")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    surface.setPosition(200, 0)
    assign.setPosition(0, 100)
    paint.setPosition(200, 200)
    layered.setPosition(0, 300)
    viewer.setPosition(0, 400)

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

    def key(k, modifiers=Qt.NoModifier):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
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

    def framebuffer():
        v = view()
        if v is None:
            return None
        image = v.grab().toImage()
        if image.isNull():
            origin = v.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(),
                                                               v.height()).toImage()
        return None if image.isNull() else image.convertToFormat(QImage.Format_RGBA8888)

    def counts():
        """The view's (red, blue) pixel counts, sampled every other pixel."""
        image = framebuffer()
        if image is None:
            return -1, -1
        red = blue = 0
        for y in range(0, image.height(), 2):
            for x in range(0, image.width(), 2):
                c = image.pixelColor(x, y)
                if c.red() > 120 and c.red() > 2 * c.green() and c.red() > 2 * c.blue():
                    red += 1
                elif c.blue() > 80 and c.blue() > 2 * c.red() and c.blue() > 2 * c.green():
                    blue += 1
        return red, blue

    state = {"red": 0, "blue": 0}

    def shows_paint():
        state["red"], state["blue"] = counts()
        return state["red"] >= MIN_RED_PIXELS and state["blue"] >= MIN_BLUE_PIXELS

    def steps():
        require(assign.connectInput(0, usd), "connect USDAssignMaterial's Scene input to the USDRead")
        require(assign.connectInput(1, surface), "connect USDAssignMaterial's Material input to the USDPreviewSurface")
        require(paint.connectInput(0, assign), "connect USDTexturePaint to USDAssignMaterial")
        require(layered.connectInput(0, assign), "connect USDLayerMaterial's Scene input to USDAssignMaterial")
        require(layered.connectInput(1, paint), "connect USDLayerMaterial's Paint input to USDTexturePaint")
        require(viewer.connectInput(0, layered), "connect the Viewer to USDLayerMaterial")
        assign.getParam("primPattern").setValue("/World/Quad")
        layered.getParam("primPattern").setValue("/World/Quad")
        diffuse = surface.getParam("diffuseColor")
        for dim, value in enumerate((0., 0., 1.)):
            diffuse.setValue(value, dim)
        paint.getParam("outputMode").setValue(PAINT_ONLY)
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
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        # The grid's axis lines are red and blue, so they would count as paint and material.
        key(Qt.Key_G, Qt.ControlModifier | Qt.ShiftModifier)
        key(Qt.Key_G, Qt.ControlModifier)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(view().property("natronSceneLighting") == "headlight",
                "the view lights with the headlight (%r)" % view().property("natronSceneLighting"))
        require(ok and view().property("natronSceneShowGrid") is False
                and view().property("natronSceneShowGuides") is False, "Ctrl+G and Ctrl+Shift+G hide the grid and guides")
        yield ("sleep", SETTLE_MS)
        red, blue = counts()
        report("NOTE before painting: %d red, %d blue pixels" % (red, blue))
        check(blue >= MIN_BLUE_PIXELS and red < MIN_RED_PIXELS,
              "before painting the quad shows its blue material (%d blue, %d red)" % (blue, red))
        shot("m312-layer-material-before.png")

        section("Paint")
        v.activateWindow()
        v.setFocus()
        centre = v.rect().center()
        generation = bake_generation()
        drag(v, centre - QPoint(120, 40), QPoint(240, 80))
        ok = yield ("until", lambda: overlay_count() > 0, POLL_TIMEOUT_S)
        check(ok, "the unbaked stroke is drawn over the view")
        shot("m312-layer-material-session.png")

        section("Bake")
        drag(v, centre, QPoint(160, 60), Qt.ControlModifier)
        ok = yield ("until", lambda: bake_generation() > generation, BAKE_TIMEOUT_S)
        check(ok, "the session's bake landed (bakeGeneration %d, was %d)" % (bake_generation(), generation))
        ok = yield ("until", lambda: overlay_count() == 0, POLL_TIMEOUT_S)
        check(ok, "the unbaked buffer is gone once the bake has landed")

        section("Layered material")
        started = time.time()
        ok = yield ("until", lambda: scene_drawn() and shows_paint(), SHOW_TIMEOUT_S)
        report("NOTE after the bake (%.1f s): %d red, %d blue pixels" % (time.time() - started, state["red"],
                                                                         state["blue"]))
        check(ok, "the quad shows the red paint over its blue material (%d red, %d blue; need %d and %d)"
              % (state["red"], state["blue"], MIN_RED_PIXELS, MIN_BLUE_PIXELS))
        yield ("sleep", SETTLE_MS)
        shot("m312-layer-material-layered.png")

        navigation.setValue(original_navigation)
        budget.setValue(original_budget)
        lighting.setValue(original_lighting)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
