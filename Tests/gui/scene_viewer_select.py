"""Selection and framing in the scene view: click, Shift+click, Ctrl+click and marquee selection,
then F (frame selection) and A (frame all).

Graph: USDRead(scene-reference.usda) into Viewer1, with the Nuke navigation preset. The script
first frames everything with A and finds where /World/Box and /World/Ball are drawn by clicking a
grid of points and reading the view's natronSceneSelection property (there is no Python access to
the view's picking). /World/Cam is a camera: the script hides the camera and light guides
(Ctrl+Shift+G) so that it draws nothing and cannot be clicked; scene_viewer_guides.py covers them.

Checks:
  - a click selects the prim under it, Shift+click adds, Ctrl+click toggles, a click on empty
    space clears;
  - a marquee from empty space selects what it encloses, an empty marquee clears, and a Shift
    marquee adds to the selection;
  - a plain left drag that starts on the box is a marquee too (as in Nuke, where Ctrl+drag
    tumbles): dragged past the ball it selects both, and the camera stays;
  - F with the ball selected frames it: the pivot moves and a click at the centre picks the ball;
  - F with nothing selected frames everything, the same camera as A;
  - the context menu lists Frame Selection and Frame All.

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
GRID_COLUMNS = 24
GRID_ROWS = 14
NUKE_PRESET = 0
BOX = "/World/Box"
BALL = "/World/Ball"

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
    navigation.setValue(NUKE_PRESET)

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
        return a is not None and b is not None and len(a) == len(b) and all(abs(x - y) <= 1e-6 * max(1., abs(x)) for x, y in zip(a, b))

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def click(v, pos, modifiers=Qt.NoModifier):
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()
        return selection()

    def drag_to(v, start, end, modifiers=Qt.NoModifier, steps=10):
        """Presses at start and moves to end with the left button held, without releasing."""
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton, modifiers)
        for i in range(1, steps + 1):
            p = start + (end - start) * (float(i) / steps)
            mouse(v, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton, modifiers)
        QApplication.processEvents()

    def marquee(v, start, end, modifiers=Qt.NoModifier):
        drag_to(v, start, end, modifiers)
        mouse(v, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()
        return selection()

    def key(v, k):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k)
        QApplication.processEvents()

    def key_with(v, k, modifiers):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def settle():
        """Asks for a paint, so natronSceneFreeCamera reflects the camera."""
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def nearest(points, target):
        return min(points, key=lambda p: (p - target).manhattanLength())

    def centre_of(points):
        x = sum(p.x() for p in points) / float(len(points))
        y = sum(p.y() for p in points) / float(len(points))
        return nearest(points, QPoint(int(x), int(y)))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        check(v.property("natronSceneSelection") is not None and selection() == [], "nothing is selected at first")
        key_with(v, Qt.Key_G, Qt.ControlModifier | Qt.ShiftModifier)
        yield settle()
        require(v.property("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")

        key(v, Qt.Key_A)
        yield settle()
        framed_all = camera()
        report("NOTE camera after A: %r" % (framed_all,))
        shot("scene-select-frame-all.png")

        # Where each prim is drawn, by clicking a grid of points.
        sx = max(1, v.width() // GRID_COLUMNS)
        sy = max(1, v.height() // GRID_ROWS)
        hits = {}
        for row in range(GRID_ROWS):
            for column in range(GRID_COLUMNS):
                p = QPoint(sx // 2 + column * sx, sy // 2 + row * sy)
                picked = click(v, p)
                hits.setdefault(picked[0] if len(picked) == 1 else "", []).append(p)
            yield ("sleep", 0)
        report("NOTE grid hits: %r" % dict((k, len(p)) for k, p in hits.items()))
        require(BOX in hits and BALL in hits and "" in hits, "the grid finds the box, the ball and empty space")
        box = centre_of(hits[BOX])
        ball = centre_of(hits[BALL])
        empty = nearest(hits[""], QPoint(0, 0))

        # Clicks.
        check(click(v, box) == [BOX], "a click on the box selects it (got %r)" % (selection(),))
        yield settle()
        shot("scene-select-box.png")
        check(click(v, ball, Qt.ShiftModifier) == sorted([BOX, BALL]), "Shift+click on the ball adds it (got %r)" % (selection(),))
        yield settle()
        shot("scene-select-box-and-ball.png")
        check(click(v, box, Qt.ControlModifier) == [BALL], "Ctrl+click on the box toggles it off (got %r)" % (selection(),))
        check(click(v, box, Qt.ControlModifier) == sorted([BOX, BALL]), "Ctrl+click on the box toggles it back on (got %r)" % (selection(),))
        check(click(v, empty) == [], "a click on empty space clears the selection (got %r)" % (selection(),))
        check(same_camera(framed_all, camera()), "selection clicks leave the camera alone")

        # Marquees.
        corner = QPoint(2, 2)
        far = QPoint(v.width() - 3, v.height() - 3)
        drag_to(v, corner, far)
        yield settle()
        shot("scene-select-marquee-drawn.png")
        mouse(v, QEvent.MouseButtonRelease, far, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()
        check(selection() == sorted([BOX, BALL]), "a marquee over the whole view selects the box and the ball (got %r)" % (selection(),))
        yield settle()
        shot("scene-select-marquee-result.png")
        check(same_camera(framed_all, camera()), "a plain left drag is a marquee and leaves the camera alone")
        check(marquee(v, corner, corner + QPoint(10, 10)) == [], "an empty marquee clears the selection (got %r)" % (selection(),))

        # A drag from the box past the ball: a click on the box would select the box alone.
        def clamp(value, low, high):
            return max(low, min(high, value))
        beyond = QPoint(clamp(ball.x() + (sx if ball.x() >= box.x() else -sx), 2, v.width() - 3),
                        clamp(ball.y() + (sy if ball.y() >= box.y() else -sy), 2, v.height() - 3))
        from_box = marquee(v, box, beyond)
        report("NOTE marquee from the box %r -> %r selects %r" % ((box.x(), box.y()), (beyond.x(), beyond.y()), from_box))
        check(from_box == sorted([BOX, BALL]), "a plain drag starting on the box is a marquee: past the ball it selects both (got %r)" % (from_box,))
        check(same_camera(framed_all, camera()), "a plain drag from a prim leaves the camera alone")
        click(v, empty)

        # A marquee around the ball alone: its corners are grid points one step out, known to be empty.
        xs = [p.x() for p in hits[BALL]]
        ys = [p.y() for p in hits[BALL]]
        around = [QPoint(x, y) for x in (min(xs) - sx, max(xs) + sx) for y in (min(ys) - sy, max(ys) + sy)]
        starts = [p for p in around if p in hits[""]]
        if check(bool(starts), "a grid point next to the ball is empty, to start a marquee from"):
            start = starts[0]
            end = QPoint(2 * ball.x() - start.x(), 2 * ball.y() - start.y())
            enclosed = marquee(v, start, end)
            report("NOTE marquee around the ball %r -> %r selects %r" % ((start.x(), start.y()), (end.x(), end.y()), enclosed))
            check(BALL in enclosed, "a marquee around the ball selects it")
            if BOX not in enclosed:
                click(v, box)
                added = marquee(v, start, end, Qt.ShiftModifier)
                check(added == sorted([BOX, BALL]), "a Shift marquee around the ball adds it to the box (got %r)" % (added,))
            else:
                report("NOTE the marquee around the ball also encloses the box; Shift marquee not checked")

        # Frame selection.
        click(v, ball)
        key(v, Qt.Key_F)
        yield settle()
        on_ball = camera()
        report("NOTE camera after F on the ball: %r" % (on_ball,))
        check(on_ball is not None and framed_all is not None and on_ball[0] > framed_all[0] + 1e-3,
              "F moves the pivot towards the ball")
        shot("scene-select-frame-ball.png")
        centre = v.rect().center()
        check(click(v, centre) == [BALL], "after F, a click at the centre picks the ball (got %r)" % (selection(),))
        check(click(v, corner) == [], "a click in the corner clears the selection (got %r)" % (selection(),))
        key(v, Qt.Key_F)
        yield settle()
        check(same_camera(framed_all, camera()), "F with nothing selected frames everything, as A does (%r)" % (camera(),))
        shot("scene-select-frame-empty.png")

        # The context menu.
        menu = {}

        def open_menu():
            mouse(v, QEvent.MouseButtonPress, centre, Qt.RightButton, Qt.RightButton)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            menu["closed"] = True

        QTimer.singleShot(0, open_menu)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if check(ok, "a right-click opens the context menu"):
            popup = QApplication.activePopupWidget()
            texts = dict((a.text().replace("&", ""), a.shortcut().toString()) for a in popup.actions())
            report("NOTE context menu: %r" % (texts,))
            check(texts.get("Frame Selection") == "F", "the menu lists Frame Selection with F")
            check(texts.get("Frame All") == "A", "the menu lists Frame All with A")
            shot("scene-select-context-menu.png")
            QTest.keyClick(popup, Qt.Key_Escape)
        ok = yield ("until", lambda: menu.get("closed", False), POLL_TIMEOUT_S)
        check(ok, "Esc closes the context menu")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
