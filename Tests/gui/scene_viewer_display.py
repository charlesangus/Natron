"""The 3D viewer's display controls: lighting mode, purposes, draw-mode keys and the grid key.

Graph: USDRead of a stage the script writes (a grey box, and a blue sphere of render purpose
beside it) merged with a red USDLight rect light in front of the box, into Viewer1. Guides are
hidden (Ctrl+Shift+G) so that only the lit geometry counts. The script checks:
  - Auto lighting uses the stage's lights when it has any: natronSceneSceneLit is true and the
    box renders reddish;
  - Display > Lighting > Headlight lights with the headlight instead: SceneLit false, no reddish
    pixels; Display > Lighting > Scene Lights brings the red light back;
  - natronScenePurposes starts at [proxy, render, guide] = [on, off, off] and the render-purpose
    sphere is not drawn; Display > Purposes > Render draws it;
  - Ctrl+4, Ctrl+5 and Ctrl+6 select the wireframe, shaded and unshaded draw modes, and Ctrl+G
    toggles the grid (natronSceneDrawMode, natronSceneShowGrid).

Screenshots for sign-off: scene-display-auto, scene-display-lighting-menu, scene-display-headlight,
scene-display-purposes-menu, scene-display-render-purpose, scene-display-scene-lights,
scene-display-wireframe, scene-display-shaded, scene-display-unshaded, scene-display-no-grid.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
NUKE_PRESET = 0
HUD_MARGIN = 28
# Pixels of a colour below this count are noise (antialiasing, the HUD).
MIN_PIXELS = 40

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "Box"
    {
        double size = 2
        color3f[] primvars:displayColor = [(0.7, 0.7, 0.7)]
    }

    def Sphere "RenderBall"
    {
        uniform token purpose = "render"
        double radius = 0.6
        double3 xformOp:translate = (1.8, 0, 0.5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        color3f[] primvars:displayColor = [(0.1, 0.3, 0.9)]
    }
}
"""

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import (OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    scene_path = os.path.join(OUT, "scene-display.usda")
    with open(scene_path, "w") as f:
        f.write(STAGE)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE_PRESET)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("filename").setValue(scene_path)
    # A rect light shines down its -Z axis: from in front of the box, onto its front face.
    light = make("USDLight", 150, 0)
    light.setScriptName("RedLight")
    light.getParam("lightType").setValue(2)
    light.getParam("color").set(1.0, 0.0, 0.0, 1.0)
    light.getParam("intensity").setValue(20.0)
    light.getParam("width").setValue(2.0)
    light.getParam("height").setValue(2.0)
    light.getParam("translate").set(0.0, 0.0, 3.0)
    merge = make("USDMerge", 75, 120)
    require(merge.connectInput(0, usd), "connect the USDRead into the merge")
    require(merge.connectInput(1, light), "connect RedLight into the merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(75, 260)

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

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

    def reddish(c):
        return c.red() > 30 and c.red() > c.green() + 25 and c.red() > c.blue() + 25

    def bluish(c):
        return c.blue() > 60 and c.blue() > c.red() + 40 and c.blue() > c.green() + 20

    def open_menu(v):
        state = {}

        def go():
            centre = v.rect().center()
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Keyboard, centre, v.mapToGlobal(centre),
                                                        Qt.NoModifier))
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

    def find_action(menu, submenu_title, object_name, path=()):
        """The action named object_name in the submenu titled submenu_title, and the submenus
        leading to it."""
        for a in menu.actions():
            sub = menu_of(a)
            if sub is None:
                if (path and path[-1].title().replace("&", "") == submenu_title and a.objectName() == object_name):
                    return a, path
                continue
            found = find_action(sub, submenu_title, object_name, path + (sub,))
            if found[0] is not None:
                return found
        return None, ()

    def close_menus():
        for _ in range(4):
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()

    def with_menu(v, submenu_title, object_name, screenshot):
        """Opens the context menu down to submenu_title, screenshots it, triggers the item named
        object_name there, and closes the menus."""
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if not check(ok, "the context menu opens"):
            return False
        popup = QApplication.activePopupWidget()
        action, submenus = find_action(popup, submenu_title, object_name)
        triggered = check(action is not None and action.isEnabled(),
                          "the context menu has %s > %s" % (submenu_title, object_name))
        if triggered:
            titles = [s.title().replace("&", "") for s in submenus]
            check(titles == ["Display", submenu_title], "%s is under Display (%r)" % (submenu_title, titles))
            parent = popup
            for sub in submenus:
                sub.popup(parent.mapToGlobal(parent.rect().topRight()))
                QApplication.processEvents()
                parent = sub
            yield ("sleep", SETTLE_MS)
            shot(screenshot + ".png")
            action.trigger()
            QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield settle()
        return triggered

    def purposes():
        value = view().property("natronScenePurposes")
        return [bool(x) for x in value] if value is not None else None

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        key(v, Qt.Key_G, CTRL | SHIFT)
        yield settle()
        require(v.property("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")
        key(v, Qt.Key_A)
        yield settle()

        section("lighting")
        check(v.property("natronSceneLighting") == "auto", "lighting starts at auto (%r)" % v.property("natronSceneLighting"))
        ok = yield ("until", lambda: v.property("natronSceneSceneLit") is True, POLL_TIMEOUT_S)
        check(ok, "auto lighting uses the stage's lights when it has one")
        image = save_view("scene-display-auto")
        red = count(image, reddish)
        report("NOTE reddish pixels under auto: %d" % red)
        check(red >= MIN_PIXELS, "the red rect light tints the box (%d reddish pixels)" % red)

        yield from with_menu(v, "Lighting", "headlight", "scene-display-lighting-menu")
        check(v.property("natronSceneLighting") == "headlight", "Display > Lighting > Headlight selects headlight")
        ok = yield ("until", lambda: v.property("natronSceneSceneLit") is False, POLL_TIMEOUT_S)
        check(ok, "the headlight replaces the stage's lights")
        image = save_view("scene-display-headlight")
        red = count(image, reddish)
        check(red < MIN_PIXELS, "the box renders neutral under the headlight (%d reddish pixels)" % red)

        section("purposes")
        check(purposes() == [True, False, False], "purposes start at proxy on, render off, guide off (%r)" % (purposes(),))
        blue = count(image, bluish)
        check(blue < MIN_PIXELS, "the render-purpose sphere is not drawn (%d bluish pixels)" % blue)
        yield from with_menu(v, "Purposes", "render", "scene-display-purposes-menu")
        check(purposes() == [True, True, False], "Display > Purposes > Render shows render purpose (%r)" % (purposes(),))
        key(v, Qt.Key_A)
        yield settle()
        image = save_view("scene-display-render-purpose")
        blue = count(image, bluish)
        check(blue >= MIN_PIXELS, "the render-purpose sphere is drawn (%d bluish pixels)" % blue)

        yield from with_menu(v, "Lighting", "scene", "scene-display-lighting-menu-scene")
        check(v.property("natronSceneLighting") == "scene", "Display > Lighting > Scene Lights selects scene")
        ok = yield ("until", lambda: v.property("natronSceneSceneLit") is True, POLL_TIMEOUT_S)
        check(ok, "scene lights are used again")
        image = save_view("scene-display-scene-lights")
        red = count(image, reddish)
        check(red >= MIN_PIXELS, "the red light is back (%d reddish pixels)" % red)

        section("draw modes and grid")
        for k, digit, name in ((Qt.Key_4, "4", "wireframe"), (Qt.Key_5, "5", "shaded"), (Qt.Key_6, "6", "unshaded")):
            key(v, k, CTRL)
            yield settle()
            mode = v.property("natronSceneDrawMode")
            check(mode == name, "Ctrl+%s selects %s (got %r)" % (digit, name, mode))
            save_view("scene-display-" + name)
        key(v, Qt.Key_5, CTRL)
        yield settle()
        grid = v.property("natronSceneShowGrid")
        key(v, Qt.Key_G, CTRL)
        yield settle()
        check(v.property("natronSceneShowGrid") is (not grid), "Ctrl+G toggles the grid (%r to %r)"
              % (grid, v.property("natronSceneShowGrid")))
        save_view("scene-display-no-grid" if grid else "scene-display-grid")
        key(v, Qt.Key_G, CTRL)
        yield settle()
        check(v.property("natronSceneShowGrid") is grid, "Ctrl+G again restores the grid")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
