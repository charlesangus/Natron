"""Natron Utility in the 3D viewer: the scene view's Renderer submenu switches to the delegate, which
draws the scene, and Display > Purposes > Render shows a render-purpose prim through it.

Graph: USDRead into Viewer1, of a stage with a grey default-purpose box and a blue render-purpose ball
in front of it. The viewer's purposes start with Render off, so the ball must be absent until the
toggle and absent again after it, under Natron Utility and then under Storm.

Under Natron Utility it also measures the longest gap between ticks of a main-thread timer while a
USDXformPrim is edited repeatedly over a heavy mesh in a large view, once the mesh's first sync is done.
That first sync reads the whole stage under the stage lock and cannot be cut short, so it is not measured.

Only geometry may reach the colour counts: the grid (Ctrl+G) is hidden, as its blue axis line counts as
blue and its lines as grey, and the view uses the headlight, as the default Scene Lights leaves Storm's
surfaces black on this stage, which has no light.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
NUKE_PRESET = 0
HEADLIGHT = 2
HUD_MARGIN = 28
UTILITY_ID = "HdNatronUtilityRendererPlugin"
STORM_ID = "HdStormRendererPlugin"
# Pixels of a colour below this count are noise (antialiasing, the HUD).
MIN_PIXELS = 40
HEAVY_GRID = 1000
STALL_BOUND_MS = 500.0
TICK_MS = 5
STALL_EDITS = 8
STALL_EDIT_GAP_MS = 150

# The ball sits inside the box's silhouette because the view frames the box alone while the render
# purpose is off, and the ball must be in that frame when it appears.
STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "Box"
    {
        double size = 4
        color3f[] primvars:displayColor = [(0.7, 0.7, 0.7)]
    }

    def Sphere "RenderBall"
    {
        uniform token purpose = "render"
        double radius = 0.8
        double3 xformOp:translate = (0, 0, 2.6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        color3f[] primvars:displayColor = [(0.1, 0.3, 0.9)]
    }
}
"""

try:
    import NatronEngine
    from PySide6.QtCore import Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import (OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    scene_path = os.path.join(OUT, "natron-utility-viewer.usda")
    with open(scene_path, "w") as f:
        f.write(STAGE)

    def write_heavy_stage(path, n):
        with open(path, "w") as out:
            out.write('#usda 1.0\n(\n    defaultPrim = "Heavy"\n    upAxis = "Y"\n)\n\n')
            out.write('def Mesh "Heavy"\n{\n')
            out.write("    int[] faceVertexCounts = [%s]\n" % ", ".join(["4"] * (n * n)))
            out.write("    int[] faceVertexIndices = [")
            for y in range(n):
                row = ["%d, %d, %d, %d" % (c, c + 1, c + n + 2, c + n + 1)
                       for c in (y * (n + 1) + x for x in range(n))]
                out.write(("" if y == 0 else ", ") + ", ".join(row))
            out.write("]\n    point3f[] points = [")
            for y in range(n + 1):
                row = ["(%g, %g, 0)" % (-2.0 + 4.0 * x / n, -2.0 + 4.0 * y / n) for x in range(n + 1)]
                out.write(("" if y == 0 else ", ") + ", ".join(row))
            out.write("]\n    color3f[] primvars:displayColor = [(0.7, 0.7, 0.7)]\n}\n\n")
            # Something under the edited prim, or the edit changes no pixel and the delegate keeps its frame.
            out.write('def Xform "Rig"\n{\n    def Cube "Marker"\n    {\n        double size = 1\n    }\n}\n')

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE_PRESET)
    lighting = NatronEngine.natron.getSettings().getParam("viewer3DLighting")
    require(lighting is not None, "Preferences have a viewer3DLighting parameter")
    original_lighting = lighting.getValue()
    lighting.setValue(HEADLIGHT)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def show_in_viewer(node):
        # connectInput refuses an occupied input.
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def key(v, k, modifiers=Qt.NoModifier):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def crop():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def save_view(name):
        shot(name + ".png")
        image = crop()
        if image is not None:
            path = os.path.join(OUT, name + "_view.png")
            image.save(path)
            report("SHOT " + path)
        return image

    def count(image, test, step=2):
        n = 0
        if image is None:
            return n
        for y in range(HUD_MARGIN, image.height() - HUD_MARGIN, step):
            for x in range(0, image.width(), step):
                if test(image.pixelColor(x, y)):
                    n += 1
        return n

    def bluish(c):
        return c.blue() > 60 and c.blue() > c.red() + 40 and c.blue() > c.green() + 20

    def greyish(c):
        return c.red() > 100 and abs(c.red() - c.green()) < 12 and abs(c.green() - c.blue()) < 12

    def open_menu(v):
        state = {}

        def go():
            centre = v.rect().center()
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Keyboard, centre, v.mapToGlobal(centre),
                                                        Qt.NoModifier))
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

    def close_menus():
        for _ in range(4):
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()

    def plain(action):
        return action.text().replace("&", "")

    def submenu(menu, title):
        for action in menu.actions():
            sub = menu_of(action)
            if sub is not None and plain(action) == title:
                return sub
        return None

    def pick_renderer(v, label, renderer_id):
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        require(ok, "the context menu opens")
        popup = QApplication.activePopupWidget()
        renderers = submenu(popup, "Renderer")
        require(renderers is not None, "the context menu has a Renderer submenu")
        entries = [a for a in renderers.actions() if plain(a).startswith(label)]
        require(bool(entries) and entries[0].isEnabled(), "the Renderer submenu lists %s" % label)
        renderers.popup(popup.mapToGlobal(popup.rect().topRight()))
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        shot("natron-utility-renderer-menu-%s.png" % renderer_id)
        entries[0].trigger()
        QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == renderer_id and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "%s draws the scene (renderer %r, scene error %r)" % (label, prop("natronSceneRenderer"), prop("natronSceneError")))
        check(not prop("natronSceneRendererError"), "%s has no renderer error (%r)" % (label, prop("natronSceneRendererError")))
        yield settle()

    def set_render_purpose(v, shown):
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        require(ok, "the context menu opens")
        display = submenu(QApplication.activePopupWidget(), "Display")
        require(display is not None, "the context menu has a Display submenu")
        purposes = submenu(display, "Purposes")
        require(purposes is not None, "Display has a Purposes submenu")
        entries = [a for a in purposes.actions() if a.objectName() == "render"]
        require(bool(entries), "Purposes has Render")
        entries[0].trigger()
        QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        ok = yield ("until", lambda: bool(prop("natronScenePurposes")[1]) == shown and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "Display > Purposes > Render turns the render purpose %s" % ("on" if shown else "off"))
        yield settle()

    def check_purpose_toggle(v, label, tag):
        image = save_view("natron-utility-%s-default" % tag)
        grey = count(image, greyish)
        blue = count(image, bluish)
        report("NOTE %s with Render off: %d grey, %d blue" % (label, grey, blue))
        check(grey >= MIN_PIXELS, "%s draws the default-purpose box (%d grey pixels)" % (label, grey))
        check(blue < MIN_PIXELS, "%s hides the render-purpose ball (%d blue pixels)" % (label, blue))

        yield from set_render_purpose(v, True)
        image = save_view("natron-utility-%s-render-purpose" % tag)
        blue = count(image, bluish)
        report("NOTE %s with Render on: %d blue" % (label, blue))
        check(blue >= MIN_PIXELS, "%s shows the render-purpose ball (%d blue pixels)" % (label, blue))

        yield from set_render_purpose(v, False)
        blue = count(crop(), bluish)
        check(blue < MIN_PIXELS, "%s hides the ball again (%d blue pixels)" % (label, blue))

    class StallMeter:
        """The longest gap between ticks of a main-thread timer."""

        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(TICK_MS)
            self.timer.timeout.connect(self.tick)
            self.last = 0.0
            self.longest_ms = 0.0

        def start(self):
            self.last = time.monotonic()
            self.longest_ms = 0.0
            self.timer.start()

        def tick(self):
            now = time.monotonic()
            self.longest_ms = max(self.longest_ms, (now - self.last) * 1000.0)
            self.last = now

        def stop(self):
            self.tick()
            self.timer.stop()

    def measure_edit_stall(v):
        heavy_path = os.path.join(OUT, "natron-utility-viewer-heavy.usda")
        write_heavy_stage(heavy_path, HEAVY_GRID)
        rig = app.createNode("USDXformPrim")
        require(rig is not None, "create USDXformPrim", plugin_problem("USDXformPrim"))
        rig.setPosition(0, 75)
        rig.getParam("primPath").setValue("/Rig")
        require(rig.connectInput(0, usd), "connect the USDRead into the USDXformPrim")
        require(show_in_viewer(rig), "connect the Viewer to the USDXformPrim")
        usd.getParam("filename").setValue(heavy_path)
        before = prop("natronSceneRefineFrames") or 0
        ok = yield ("until", lambda: (prop("natronSceneRefineFrames") or 0) > before and bool(prop("natronSceneRefineConverged")),
                    STARTUP_TIMEOUT_S * 5)
        check(ok, "the heavy mesh's first frame finishes (scene error %r)" % (prop("natronSceneError"),))

        meter = StallMeter()
        restarts_before = prop("natronSceneRestartCount") or 0
        frames_before = prop("natronSceneRefineFrames") or 0
        meter.start()
        for i in range(STALL_EDITS):
            rig.getParam("rotate").set(0.0, float(i + 1), 0.0)
            yield ("sleep", STALL_EDIT_GAP_MS)
        ok = yield ("until", lambda: (prop("natronSceneRestartCount") or 0) > restarts_before and bool(prop("natronSceneRefineConverged")),
                    POLL_TIMEOUT_S)
        meter.stop()
        check(ok, "the render finishes after the edits")
        report("NOTE main-thread stall during edits of a %d-triangle render: %.1f ms; %d frames published" % (
            HEAVY_GRID * HEAVY_GRID * 2, meter.longest_ms, (prop("natronSceneRefineFrames") or 0) - frames_before))
        check(meter.longest_ms < STALL_BOUND_MS,
              "edits during a large render stall the main thread under %d ms (longest gap %.1f ms)"
              % (STALL_BOUND_MS, meter.longest_ms))

        require(show_in_viewer(usd), "connect the Viewer back to the USDRead")
        usd.getParam("filename").setValue(scene_path)
        rig.destroy()
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the view is back on the small stage")
        yield settle()

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        check(prop("natronSceneRenderer") == STORM_ID, "the view starts on Storm (%r)" % (prop("natronSceneRenderer"),))
        check(not bool(prop("natronScenePurposes")[1]), "the view starts with Render off")
        check(prop("natronSceneLighting") == "headlight", "the view uses the headlight (%r)" % (prop("natronSceneLighting"),))
        key(v, Qt.Key_G, Qt.ControlModifier)
        check(not prop("natronSceneShowGrid"), "the grid is hidden")
        key(v, Qt.Key_A)
        yield settle()

        section("Natron Utility")
        yield from pick_renderer(v, "Natron Utility", UTILITY_ID)
        yield from check_purpose_toggle(v, "Natron Utility", "utility")
        section("Natron Utility: main-thread stall during an edit in a large render")
        yield from measure_edit_stall(v)

        section("Storm")
        yield from pick_renderer(v, "Storm", STORM_ID)
        yield from check_purpose_toggle(v, "Storm", "storm")

        navigation.setValue(original_navigation)
        lighting.setValue(original_lighting)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
