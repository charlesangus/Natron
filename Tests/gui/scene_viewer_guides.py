"""The 3D viewer draws camera and light guides, picks them, and hides them on request.

Graph: USDRead(scene-reference.usda), a USDCamera (GuideCam) and a sphere USDLight (GuideLight)
merged into Viewer1. Once the scene is drawn and framed, the view's natronSceneGuides property
lists each guide it drew as [path, kind, x, y], (x, y) a point on the guide in widget pixels. The
script checks:
  - GuideCam is drawn as a "camera" guide and GuideLight as a "sphereLight" guide, and the pixels
    near each reported point are the guide colours (camera cyan, light yellow);
  - a click on the camera guide selects the camera prim, and its guide turns the selection orange;
    a click on the light guide selects the light prim;
  - Ctrl+Shift+G hides the guides: natronSceneShowGuides is false, no guides are listed, and the
    guide pixels are gone; Display > Show Guides in the context menu shows them again.

Screenshots for sign-off: scene-guides-shown, scene-guides-camera-selected,
scene-guides-light-selected, scene-guides-hidden, scene-guides-display-menu.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
NUKE_PRESET = 0
# Half the side of the square searched around a reported guide point.
SEARCH_RADIUS = 4

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import (FIXTURES, OUT, check, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

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
    # Both sit in front of the box, inside the framed bounds of the box and the ball.
    cam = make("USDCamera", 150, 0)
    cam.setScriptName("GuideCam")
    cam.getParam("translate").set(-1.5, 0.5, 2.5)
    light = make("USDLight", 300, 0)
    light.setScriptName("GuideLight")
    light.getParam("lightType").setValue(1)
    light.getParam("radius").setValue(0.3)
    light.getParam("translate").set(1.5, 1.0, 1.5)
    merge = make("USDMerge", 150, 120)
    require(merge.connectInput(0, usd), "connect the USDRead into the merge")
    require(merge.connectInput(1, cam), "connect GuideCam into the merge")
    require(merge.connectInput(2, light), "connect GuideLight into the merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(150, 260)
    cam_path = cam.getParam("primPath").getValue()
    light_path = light.getParam("primPath").getValue()
    report("NOTE prim paths: camera %r, light %r" % (cam_path, light_path))

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def guides():
        """path -> (kind, QPoint) of the guides the last paint drew."""
        v = view()
        value = v.property("natronSceneGuides") if v is not None else None
        found = {}
        for entry in value or []:
            found[str(entry[0])] = (str(entry[1]), QPoint(int(round(float(entry[2]))), int(round(float(entry[3])))))
        return found

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def click(v, pos):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

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

    # The guide colours SceneViewport draws: cameras (0.45, 0.85, 0.85), lights (0.95, 0.9, 0.55),
    # the selected guide (1, 0.8, 0.1).
    def is_camera_colour(c):
        return c.red() < 170 and c.green() > 170 and c.blue() > 170

    def is_light_colour(c):
        return c.red() > 200 and c.green() > 190 and 90 < c.blue() < 190

    def is_selected_colour(c):
        return c.red() > 220 and 150 < c.green() < 235 and c.blue() < 90

    def colour_near(image, point, test):
        """Whether a pixel within SEARCH_RADIUS of point passes test."""
        if image is None:
            return False
        for dy in range(-SEARCH_RADIUS, SEARCH_RADIUS + 1):
            for dx in range(-SEARCH_RADIUS, SEARCH_RADIUS + 1):
                x, y = point.x() + dx, point.y() + dy
                if 0 <= x < image.width() and 0 <= y < image.height() and test(image.pixelColor(x, y)):
                    return True
        return False

    def open_menu(v):
        state = {}

        def go():
            centre = v.rect().center()
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Keyboard, centre, v.mapToGlobal(centre),
                                                        Qt.NoModifier))
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

    def find_action(menu, text, path=()):
        """The action labelled text, and the submenus leading to it."""
        for a in menu.actions():
            if a.text().replace("&", "") == text:
                return a, path
            sub = menu_of(a)
            if sub is not None:
                found = find_action(sub, text, path + (sub,))
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

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        key(v, Qt.Key_A)
        yield settle()

        section("guides drawn")
        check(v.property("natronSceneShowGuides") is True, "guides are shown by default")
        ok = yield ("until", lambda: cam_path in guides() and light_path in guides(), POLL_TIMEOUT_S)
        drawn = guides()
        report("NOTE guides %r" % (dict((p, (k, (q.x(), q.y()))) for p, (k, q) in drawn.items()),))
        require(ok, "the view draws a guide for GuideCam and GuideLight")
        check(drawn[cam_path][0] == "camera", "GuideCam's guide is a camera guide (%r)" % drawn[cam_path][0])
        check(drawn[light_path][0] == "sphereLight", "GuideLight's guide is a sphere light guide (%r)" % drawn[light_path][0])
        image = save_view("scene-guides-shown")
        cam_point, light_point = drawn[cam_path][1], drawn[light_path][1]
        check(colour_near(image, cam_point, is_camera_colour), "the camera guide is drawn in the camera colour")
        check(colour_near(image, light_point, is_light_colour), "the light guide is drawn in the light colour")

        section("picking")
        check(selection() == [], "nothing is selected at first")
        click(v, cam_point)
        yield settle()
        check(selection() == [cam_path], "a click on the camera guide selects the camera (%r)" % (selection(),))
        image = save_view("scene-guides-camera-selected")
        check(colour_near(image, guides().get(cam_path, ("", cam_point))[1], is_selected_colour),
              "the selected camera guide is drawn in the selection colour")
        check(colour_near(image, light_point, is_light_colour), "the unselected light guide keeps the light colour")
        click(v, light_point)
        yield settle()
        check(selection() == [light_path], "a click on the light guide selects the light (%r)" % (selection(),))
        save_view("scene-guides-light-selected")

        section("hiding")
        key(v, Qt.Key_G, CTRL | SHIFT)
        yield settle()
        check(v.property("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")
        ok = yield ("until", lambda: guides() == {}, POLL_TIMEOUT_S)
        check(ok, "no guides are drawn once hidden (%r)" % (list(guides().keys()),))
        image = save_view("scene-guides-hidden")
        check(not colour_near(image, cam_point, is_camera_colour) and not colour_near(image, cam_point, is_selected_colour),
              "the camera guide's pixels are gone")
        check(not colour_near(image, light_point, is_light_colour) and not colour_near(image, light_point, is_selected_colour),
              "the light guide's pixels are gone")
        click(v, cam_point)
        yield settle()
        check(cam_path not in selection(), "a hidden camera guide cannot be clicked (%r)" % (selection(),))

        section("menu")
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if check(ok, "the context menu opens"):
            popup = QApplication.activePopupWidget()
            action, submenus = find_action(popup, "Show Guides")
            if check(action is not None and action.isCheckable() and not action.isChecked(),
                     "Display has an unchecked Show Guides item"):
                check(len(submenus) == 1 and submenus[0].title().replace("&", "") == "Display",
                      "Show Guides is in the Display submenu")
                check(action.shortcut().toString() == "Ctrl+Shift+G",
                      "Show Guides lists Ctrl+Shift+G (%r)" % action.shortcut().toString())
                for sub in submenus:
                    sub.popup(popup.mapToGlobal(popup.rect().topRight()))
                    QApplication.processEvents()
                yield settle()
                shot("scene-guides-display-menu.png")
                action.trigger()
                QApplication.processEvents()
            close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield settle()
        check(v.property("natronSceneShowGuides") is True, "Display > Show Guides shows the guides again")
        ok = yield ("until", lambda: cam_path in guides() and light_path in guides(), POLL_TIMEOUT_S)
        check(ok, "both guides are drawn again")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
