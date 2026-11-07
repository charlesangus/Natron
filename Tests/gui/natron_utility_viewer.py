"""Natron Utility in the 3D viewer: the scene view's Renderer submenu switches to the delegate, which
draws the scene, and Display > Purposes > Render shows a render-purpose prim through it.

Graph: USDRead into Viewer1, of a stage with a grey default-purpose box and a blue render-purpose ball
in front of it. The viewer's purposes start with Render off, so the ball must be absent until the
toggle and absent again after it, under Natron Utility and then under Storm.

Only geometry may reach the colour counts: the grid (Ctrl+G) is hidden, as its blue axis line counts as
blue and its lines as grey, and the view uses the headlight, as the default Scene Lights leaves Storm's
surfaces black on this stage, which has no light.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
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
