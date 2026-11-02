"""A right-click in the scene view opens its context menu and leaves no camera drag behind: after
the menu is dismissed with Esc, hover moves do not dolly the camera.

Graph: USDRead(scene-reference.usda) into Viewer1. Once the scene is drawn, the script sends a
right press and a context-menu event to the SceneViewport (synthesized events through the window
system do not reach Natron's widgets under Xvfb), waits for the menu, presses Esc, then sends
button-less mouse moves. The free camera, read from the view's natronSceneFreeCamera property,
must be unchanged. A second case presses Alt+right, which starts a dolly under the Maya navigation
preset the script selects, and then moves with no button held, as when the release went to another
window: the camera must not move either.

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

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import FIXTURES, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(1)  # Maya: Alt+right dollies.

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    filename = usd.getParam("filename")
    require(filename is not None, "USDRead has a filename parameter")
    filename.setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    viewer.setPosition(0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError") and v.property("natronSceneFreeCamera") is not None)

    def camera():
        v = view()
        value = v.property("natronSceneFreeCamera") if v is not None else None
        return [float(x) for x in value] if value else None

    def same_camera(a, b):
        return a is not None and b is not None and len(a) == len(b) and all(abs(x - y) <= 1e-9 * max(1., abs(x)) for x, y in zip(a, b))

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def hover_moves(v, start):
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + QPoint(12 * i, 9 * i), Qt.NoButton, Qt.NoButton)
            QApplication.processEvents()

    def repainted_camera():
        """Asks for a paint, so the property reflects the camera after the moves."""
        v = view()
        if v is not None:
            v.update()

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        yield ("sleep", SETTLE_MS)
        before = camera()
        report("NOTE camera before: %r" % (before,))
        centre = v.rect().center()

        # A plain right-click: the press, then the context menu the window system would send.
        menu = {}

        def open_menu():
            mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            menu["closed"] = True

        QTimer.singleShot(0, open_menu)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        check(ok, "a right-click opens the context menu")
        if ok:
            shot("m20-scene-context-menu.png")
            QTest.keyClick(QApplication.activePopupWidget(), Qt.Key_Escape)
        ok = yield ("until", lambda: menu.get("closed", False), POLL_TIMEOUT_S)
        require(ok, "Esc closes the context menu")
        check(QApplication.activePopupWidget() is None, "no popup is left open")

        hover_moves(v, centre)
        repainted_camera()
        yield ("sleep", SETTLE_MS)
        after = camera()
        report("NOTE camera after the menu and hover moves: %r" % (after,))
        check(same_camera(before, after), "hover moves after a dismissed context menu leave the camera unchanged")

        # Alt+right starts a dolly; a move with no button held means its release went elsewhere.
        mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton, Qt.AltModifier)
        QApplication.processEvents()
        hover_moves(v, centre)
        repainted_camera()
        yield ("sleep", SETTLE_MS)
        lost = camera()
        report("NOTE camera after Alt+right press and button-less moves: %r" % (lost,))
        check(same_camera(before, lost), "moves with the dolly button no longer held leave the camera unchanged")

        # The dolly itself still works while the button is held.
        mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton, Qt.AltModifier)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, centre + QPoint(12 * i, 9 * i), Qt.NoButton, Qt.RightButton, Qt.AltModifier)
        mouse(v, QEvent.MouseButtonRelease, centre + QPoint(120, 90), Qt.RightButton, Qt.NoButton, Qt.AltModifier)
        repainted_camera()
        yield ("sleep", SETTLE_MS)
        dollied = camera()
        report("NOTE camera after an Alt+right drag: %r" % (dollied,))
        check(dollied is not None and before is not None and abs(dollied[3] - before[3]) > 1e-6,
              "an Alt+right drag still dollies the camera")
        shot("m20-scene-after-dolly.png")
        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
