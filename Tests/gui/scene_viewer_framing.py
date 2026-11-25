"""Framing and the camera list reach the view.

Graph: USDRead(scene-reference.usda) into Viewer1. The fixture has a red 2-unit cube at the origin,
a blue ball of radius 0.5 at x 3, and a camera /World/Cam.

- The first sync frames the scene: the free camera's pivot sits inside the scene's bounds, off the
  origin.
- A dolly with the wheel moves the camera; A puts it back where the first framing left it by the
  time the view reads as synced. Where the framing is worked out, and whether the view read as
  unsynced on the way, is not checked.
- The context menu's Look Through lists /World/Cam, and picking it looks through it. A then frames
  the scene from the free camera again.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
CAMERA_PATH = "/World/Cam"

try:
    from PySide6.QtCore import QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import (FIXTURES, check, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    viewer.setPosition(0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def synced():
        v = view()
        return v is not None and bool(v.property("natronSceneSynced"))

    def scene_drawn():
        v = view()
        return (synced() and bool(v.property("natronSceneShown")) and not v.property("natronSceneError")
                and v.property("natronSceneFreeCamera") is not None)

    def camera():
        return [float(x) for x in view().property("natronSceneFreeCamera")]

    def same_camera(a, b):
        return all(abs(x - y) < 1e-3 for x, y in zip(a, b))

    def look_through():
        return str(view().property("natronSceneLookThrough") or "")

    def key(v, k):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k)
        QApplication.processEvents()

    def wheel(v):
        event = QWheelEvent(QPointF(v.rect().center()), QPointF(v.mapToGlobal(v.rect().center())), QPoint(0, 0),
                            QPoint(0, 120), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
        QApplication.sendEvent(v, event)
        QApplication.processEvents()

    def find_action(menu, text):
        for a in menu.actions():
            if a.text().replace("&", "") == text:
                return a
            sub = menu_of(a)
            if sub is not None:
                found = find_action(sub, text)
                if found is not None:
                    return found
        return None

    def open_menu(v):
        """The menu's exec() blocks, so it runs from a timer while the steps keep polling."""
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

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to USDRead")
        ok = yield ("until", lambda: view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))

        section("Framing")
        framed = camera()
        report("NOTE camera after the first sync: %r" % (framed,))
        check(0.5 < framed[0] < 3.5, "the first sync framed the scene: the pivot sits between the cube and the ball")
        check(framed[3] > 0.0, "the first framing set a distance")
        shot("scene-framing-first.png")

        wheel(v)
        ok = yield ("until", lambda: not same_camera(framed, camera()), POLL_TIMEOUT_S)
        check(ok, "the wheel moves the camera off the framing")

        key(v, Qt.Key_A)
        ok = yield ("until", lambda: synced() and same_camera(framed, camera()), POLL_TIMEOUT_S)
        check(ok, "A frames the scene as the first sync did (%r, wanted %r)" % (camera(), framed))
        shot("scene-framing-all.png")

        section("Cameras")
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        require(ok, "the context menu opens")
        action = find_action(QApplication.activePopupWidget(), CAMERA_PATH)
        listed = check(action is not None and action.isEnabled(), "the Look Through menu lists %s" % CAMERA_PATH)
        if listed:
            action.trigger()
            QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        if listed:
            check(look_through() == CAMERA_PATH, "picking it looks through it (%r)" % (look_through(),))
            shot("scene-framing-look-through.png")

            key(v, Qt.Key_A)
            ok = yield ("until", lambda: synced() and look_through() == "", POLL_TIMEOUT_S)
            check(ok, "A leaves the look-through for the free camera")
            check(same_camera(framed, camera()), "A from a look-through frames the scene (%r)" % (camera(),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
