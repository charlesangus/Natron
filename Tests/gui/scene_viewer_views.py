"""Orthographic views, clip planes, the view lock and Create Camera from View in the scene view.

Graph: USDRead(scene-reference.usda) and a USDCamera (LockCam, at z 8 looking down -Z) into a
USDMerge, into Viewer1. The fixture has a red 2-unit cube at the origin, a blue ball of radius 0.5
at x 3, and a camera /World/Cam read from the file.

- Views: Z, Shift+Z, X, Shift+X, C, Shift+C and V set the natronSceneViewMode property. In the
  Right view (looking down -X) the ball is in front of the cube; orthographic, the ball's height
  on screen is half the cube's (in perspective it would look bigger, being nearer). Front shows the
  cube left of the ball, Back the ball left of the cube.
- Clipping: the context menu's Near Clip box set beyond the scene hides it; Automatic Clipping
  brings it back. natronSceneClip follows.
- Lock: looking through LockCam with Ctrl+L locked, a Ctrl+left drag (Nuke's tumble) turns the
  camera by editing its Rotate and Translate; Edit > Undo puts both back in one step. While the
  button is held the view moves but the knobs are not written and no scene is handed to the
  view's sync (natronSceneSyncRequests stays put); the release writes them once. Locked to
  /World/Cam, which no Camera node made, a drag leaves the view alone and the HUD says why.
- One-step lock: from the free camera, Ctrl+L with two cameras in the scene opens a menu of them
  and picking LockCam locks to it; the Look Through menu's "Lock to /World/Cam" looks through and
  locks in one pick; with the USDRead alone in view (one camera) Ctrl+L locks to /World/Cam
  without asking.
- Create Camera from View: from a tumbled free view, the menu entry makes a USDCamera; merged in
  and looked through, it shows what the free view showed.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 600
STARTUP_TIMEOUT_S = 60.0
NUKE = 0
# The HUD lines at the top and bottom of the view are left out of image comparisons.
HUD_MARGIN = 28

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QDoubleSpinBox, QMainWindow

    from guitest import (FIXTURES, OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run,
                         section, shot, widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    usd = make("USDRead", 0, 0)
    usd.getParam("filename").setValue(scene_path)
    lock_cam = make("USDCamera", 200, 0)
    lock_cam.setScriptName("LockCam")
    lock_cam.getParam("translate").set(0.0, 0.0, 8.0)
    merge = make("USDMerge", 100, 120)
    require(merge.connectInput(0, usd), "connect the USDRead into the merge")
    require(merge.connectInput(1, lock_cam), "connect LockCam into the merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(100, 260)
    lock_cam_path = lock_cam.getParam("primPath").getValue()
    report("NOTE LockCam prim path %r" % (lock_cam_path,))

    L = Qt.LeftButton
    CTRL, SHIFT, NONE = Qt.ControlModifier, Qt.ShiftModifier, Qt.NoModifier

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError") and v.property("natronSceneFreeCamera") is not None)

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def focus(v):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()

    def key(v, k, modifiers=NONE):
        focus(v)
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def mouse(v, kind, pos, button, buttons, modifiers=NONE):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def drag(v, button, modifiers, delta=QPoint(60, 30)):
        start = v.rect().center()
        mouse(v, QEvent.MouseButtonPress, start, button, button, modifiers)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + delta * (i / 10.), Qt.NoButton, button, modifiers)
        mouse(v, QEvent.MouseButtonRelease, start + delta, button, Qt.NoButton, modifiers)
        QApplication.processEvents()

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

    def classify(c):
        r, g, b = c.red(), c.green(), c.blue()
        if r > g + 40 and r > b + 40:
            return "red"
        if b > r + 40 and b > g + 20:
            return "blue"
        return None

    def extents(image, step=2):
        """Per colour: (min x, max x, min y, max y, count) of the cube's and the ball's pixels."""
        found = {}
        if image is None:
            return found
        for y in range(HUD_MARGIN, image.height() - HUD_MARGIN, step):
            for x in range(0, image.width(), step):
                kind = classify(image.pixelColor(x, y))
                if kind is None:
                    continue
                e = found.get(kind)
                if e is None:
                    found[kind] = [x, x, y, y, 1]
                else:
                    e[0], e[1] = min(e[0], x), max(e[1], x)
                    e[2], e[3] = min(e[2], y), max(e[3], y)
                    e[4] += 1
        return found

    def mean_difference(a, b, step=3):
        """The mean absolute channel difference between two same-sized images, HUD lines left out."""
        if a is None or b is None or a.size() != b.size():
            return None
        total, n = 0, 0
        for y in range(HUD_MARGIN, a.height() - HUD_MARGIN, step):
            for x in range(0, a.width(), step):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                total += abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue())
                n += 3
        return total / float(max(n, 1))

    # ---- the context menu ----------------------------------------------------------------------
    def open_menu(v):
        state = {}

        def go():
            centre = v.rect().center()
            QApplication.sendEvent(v, QContextMenuEvent(QContextMenuEvent.Keyboard, centre, v.mapToGlobal(centre), NONE))
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

    def post_key(v, k, modifiers=NONE):
        """key() from a timer, for a key that may open a modal menu: the menu's exec() then runs
        outside the steps, which keep polling and can drive and close it."""
        state = {}

        def go():
            key(v, k, modifiers)
            state["closed"] = True

        QTimer.singleShot(0, go)
        return state

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

    def close_menus():
        for _ in range(4):
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()

    def with_menu(v, action_text, screenshot=None):
        """Opens the context menu, triggers the action labelled action_text, and closes the menu."""
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if not check(ok, "the context menu opens"):
            return False
        popup = QApplication.activePopupWidget()
        if screenshot:
            shot(screenshot)
        action = find_action(popup, action_text)
        triggered = check(action is not None and action.isEnabled(), "the context menu has an enabled %r" % action_text)
        if triggered:
            action.trigger()
            QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        return triggered

    def edit_undo():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or class_name(win) != "Gui" or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                    for a in menu.actions():
                        if a.text().replace("&", "").lower().startswith("undo") and a.isEnabled():
                            report("NOTE undoing %r" % (a.text(),))
                            a.trigger()
                            QApplication.processEvents()
                            return True
        return False

    def sync_requests(v):
        return int(v.property("natronSceneSyncRequests") or 0)

    def look_through(v):
        return str(v.property("natronSceneLookThrough") or "")

    def lock_menu():
        popup = QApplication.activePopupWidget()
        return popup if popup is not None and popup.objectName() == "SceneLockCameraMenu" else None

    def values(node, name):
        p = node.getParam(name)
        return [p.getValue(i) for i in range(3)]

    def near(a, b, tol=1e-4):
        return all(abs(x - y) <= tol * max(1., abs(x)) for x, y in zip(a, b))

    VIEW_KEYS = [
        ("front", "Z", Qt.Key_Z, NONE), ("back", "Shift+Z", Qt.Key_Z, SHIFT), ("right", "X", Qt.Key_X, NONE),
        ("left", "Shift+X", Qt.Key_X, SHIFT), ("top", "C", Qt.Key_C, NONE), ("bottom", "Shift+C", Qt.Key_C, SHIFT),
        ("perspective", "V", Qt.Key_V, NONE),
    ]

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        key(v, Qt.Key_A)
        yield settle()

        section("views")
        images = {}
        for name, label, k, mods in VIEW_KEYS:
            key(v, k, mods)
            key(v, Qt.Key_A)
            yield settle()
            mode = v.property("natronSceneViewMode")
            check(mode == name, "%s selects the %s view (got %r)" % (label, name, mode))
            images[name] = save_view("scene-views-%s" % name)
        right = extents(images.get("right"))
        report("NOTE right view extents %r" % (right,))
        if check("red" in right and "blue" in right, "the Right view shows the cube and the ball"):
            ratio = (right["blue"][3] - right["blue"][2]) / float(max(1, right["red"][3] - right["red"][2]))
            check(abs(ratio - 0.5) < 0.08, "the Right view is orthographic: the ball looks half the cube's height (%.3f)" % ratio)
        front = extents(images.get("front"))
        back = extents(images.get("back"))
        report("NOTE front %r back %r" % (front, back))
        check("red" in front and "blue" in front and front["red"][0] < front["blue"][0], "the Front view shows the cube left of the ball")
        check("red" in back and "blue" in back and back["blue"][0] < back["red"][0], "the Back view shows the ball left of the cube")
        top = extents(images.get("top"))
        check("red" in top and "blue" in top and top["red"][0] < top["blue"][0], "the Top view shows the cube left of the ball")

        drag(v, L, CTRL)
        yield settle()
        check(v.property("natronSceneViewMode") == "perspective", "tumbling an orthographic view turns it to perspective")

        section("clipping")
        key(v, Qt.Key_Z)
        key(v, Qt.Key_A)
        yield settle()
        state = open_menu(v)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        require(ok, "the context menu opens")
        popup = QApplication.activePopupWidget()
        boxes = popup.findChildren(QDoubleSpinBox, "nearClip")
        require(len(boxes) == 1, "the context menu has a Near Clip box")
        camera = [float(x) for x in v.property("natronSceneFreeCamera")]
        boxes[0].setValue(camera[3] + 5.0)  # beyond everything: the scene is within a few units of the pivot
        QApplication.processEvents()
        clip = v.property("natronSceneClip")
        report("NOTE clip after Near Clip %r" % (clip,))
        check(clip is not None and not clip[0] and abs(clip[1] - (camera[3] + 5.0)) < 1e-3, "Near Clip makes the clip planes absolute")
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield settle()
        clipped = extents(save_view("scene-views-near-clipped"))
        check("red" not in clipped and "blue" not in clipped, "a near clip beyond the scene hides it (%r)" % (clipped,))
        ok = yield from with_menu(v, "Automatic Clipping")
        yield settle()
        clip = v.property("natronSceneClip")
        check(clip is not None and bool(clip[0]), "Automatic Clipping turns the automatic planes back on (%r)" % (clip,))
        restored = extents(save_view("scene-views-auto-clip"))
        check("red" in restored, "automatic clipping shows the scene again")

        section("lock")
        key(v, Qt.Key_V)
        ok = yield from with_menu(v, lock_cam_path)
        yield settle()
        check(v.property("natronSceneLockView") is False, "the view starts unlocked")
        key(v, Qt.Key_L, CTRL)
        check(bool(v.property("natronSceneLockView")), "Ctrl+L locks the view to the camera")
        before = save_view("scene-views-locked-before")
        t0, r0 = values(lock_cam, "translate"), values(lock_cam, "rotate")
        requests0 = sync_requests(v)
        # The drag in two halves: the view during it, then the release.
        start, delta, moves = v.rect().center(), QPoint(60, 30), 10
        mouse(v, QEvent.MouseButtonPress, start, L, L, CTRL)
        for i in range(1, moves + 1):
            mouse(v, QEvent.MouseMove, start + delta * (float(i) / moves), Qt.NoButton, L, CTRL)
            QApplication.processEvents()
        yield settle()
        during = save_view("scene-views-locked-during")
        check(sync_requests(v) == requests0, "a locked drag hands no scene to the view while the button is held (%r -> %r)" %
              (requests0, sync_requests(v)))
        check(near(t0, values(lock_cam, "translate")) and near(r0, values(lock_cam, "rotate")),
              "a locked drag writes LockCam's knobs only on release")
        diff = mean_difference(before, during)
        check(diff is not None and diff > 1.0, "the view follows a locked drag while the button is held (mean difference %r)" % (diff,))
        mouse(v, QEvent.MouseButtonRelease, start + delta, L, Qt.NoButton, CTRL)
        QApplication.processEvents()
        ok = yield ("until", lambda: sync_requests(v) > requests0 and bool(v.property("natronSceneSynced")), POLL_TIMEOUT_S)
        check(ok, "the release hands the written move to the view")
        report("NOTE scenes handed to the view by the locked drag: %d" % (sync_requests(v) - requests0,))
        check(sync_requests(v) - requests0 < moves, "the locked drag syncs the scene fewer times than it moved (%d)" %
              (sync_requests(v) - requests0,))
        yield settle()
        diff = mean_difference(during, crop())
        check(diff is not None and diff < 2.0, "the view stays where the drag left it once the move is written (mean difference %r)" % (diff,))
        t1, r1 = values(lock_cam, "translate"), values(lock_cam, "rotate")
        report("NOTE LockCam translate %r -> %r, rotate %r -> %r" % (t0, t1, r0, r1))
        check(not near(r0, r1), "a locked tumble turns LockCam's Rotate")
        check(not near(t0, t1), "a locked tumble moves LockCam's Translate about the scene")
        save_view("scene-views-locked-after")
        check(edit_undo(), "Edit > Undo is enabled after the locked move")
        yield settle()
        t2, r2 = values(lock_cam, "translate"), values(lock_cam, "rotate")
        check(near(t0, t2) and near(r0, r2), "one undo puts LockCam back (%r, %r)" % (t2, r2))

        ok = yield from with_menu(v, "/World/Cam")
        yield settle()
        before = crop()
        drag(v, L, CTRL)
        yield settle()
        message = v.property("natronSceneLockMessage")
        report("NOTE lock message %r" % (message,))
        check(bool(message) and "/World/Cam" in message, "locked to /World/Cam, the HUD says it has no Camera node")
        diff = mean_difference(before, save_view("scene-views-locked-file-camera"))
        check(diff is not None and diff < 1.0, "a drag locked to /World/Cam leaves the view alone (mean difference %r)" % (diff,))
        key(v, Qt.Key_L, CTRL)
        check(v.property("natronSceneLockView") is False, "Ctrl+L unlocks the view")

        section("one-step lock")
        ok = yield from with_menu(v, "Free Camera")
        yield settle()
        check(look_through(v) == "" and v.property("natronSceneLockView") is False, "back on the free camera, unlocked")
        state = post_key(v, Qt.Key_L, CTRL)
        ok = yield ("until", lambda: lock_menu() is not None, POLL_TIMEOUT_S)
        menu = lock_menu()
        if check(ok and menu is not None, "with two cameras and none looked through, Ctrl+L offers a menu of them"):
            shot("scene-views-lock-menu.png")
            texts = sorted(a.text().replace("&", "") for a in menu.actions() if a.text())
            check(texts == sorted([lock_cam_path, "/World/Cam"]), "the menu lists the scene's cameras (%r)" % (texts,))
            action = find_action(menu, lock_cam_path)
            if check(action is not None, "the menu has LockCam"):
                action.trigger()
                QApplication.processEvents()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield settle()
        check(look_through(v) == lock_cam_path and bool(v.property("natronSceneLockView")),
              "picking LockCam looks through it and locks (%r)" % (look_through(v),))
        key(v, Qt.Key_L, CTRL)
        check(v.property("natronSceneLockView") is False and look_through(v) == lock_cam_path,
              "Ctrl+L unlocks and keeps looking through LockCam")

        ok = yield from with_menu(v, "Lock to /World/Cam")
        yield settle()
        check(look_through(v) == "/World/Cam" and bool(v.property("natronSceneLockView")),
              "Look Through > Lock to /World/Cam looks through it and locks in one pick (%r)" % (look_through(v),))
        ok = yield from with_menu(v, "Free Camera")
        yield settle()
        check(v.property("natronSceneLockView") is False, "the free camera has no lock")

        requests0 = sync_requests(v)
        require(viewer.connectInput(0, usd), "view the USDRead alone")
        ok = yield ("until", lambda: sync_requests(v) > requests0 and scene_drawn(), STARTUP_TIMEOUT_S)
        require(ok, "the view shows the USDRead alone")
        state = post_key(v, Qt.Key_L, CTRL)
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        check(QApplication.activePopupWidget() is None, "with one camera Ctrl+L asks nothing")
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        check(look_through(v) == "/World/Cam" and bool(v.property("natronSceneLockView")),
              "with one camera Ctrl+L locks to it (%r)" % (look_through(v),))
        key(v, Qt.Key_L, CTRL)
        ok = yield from with_menu(v, "Free Camera")
        requests0 = sync_requests(v)
        require(viewer.connectInput(0, merge), "view the merge again")
        ok = yield ("until", lambda: sync_requests(v) > requests0 and scene_drawn(), STARTUP_TIMEOUT_S)
        require(ok, "the view shows the merge again")

        section("create camera")
        ok = yield from with_menu(v, "Free Camera")
        key(v, Qt.Key_A)
        drag(v, L, CTRL, QPoint(40, -25))
        yield settle()
        free_image = save_view("scene-views-free-before-create")
        before_nodes = set(n.getScriptName() for n in app.getChildren())
        ok = yield from with_menu(v, "Create Camera from View")
        require(ok, "Create Camera from View ran")
        created = [n for n in app.getChildren() if n.getScriptName() not in before_nodes]
        require(len(created) == 1 and created[0].getPluginID() == "USDCamera", "it made one USDCamera node (%r)" %
                ([n.getScriptName() for n in created],))
        new_cam = created[0]
        report("NOTE new camera %s translate %r rotate %r focal %r" % (new_cam.getScriptName(), values(new_cam, "translate"),
                                                                      values(new_cam, "rotate"), new_cam.getParam("focalLength").getValue()))
        require(merge.connectInput(2, new_cam), "connect the new camera into the merge")
        new_path = new_cam.getParam("primPath").getValue()
        ok = yield ("until", lambda: scene_drawn() and v.property("natronSceneSynced"), POLL_TIMEOUT_S)
        yield settle()
        ok = yield from with_menu(v, new_path)
        yield settle()
        yield settle()
        through_image = save_view("scene-views-through-created-camera")
        diff = mean_difference(free_image, through_image)
        free_e, through_e = extents(free_image), extents(through_image)
        report("NOTE free %r through %r mean difference %r" % (free_e, through_e, diff))
        check(diff is not None and diff < 6.0, "looking through the new camera shows what the free view showed (mean difference %r)" % (diff,))
        if "red" in free_e and "red" in through_e:
            check(abs(free_e["red"][4] - through_e["red"][4]) <= 0.05 * free_e["red"][4] + 20,
                  "the cube covers as many pixels through the new camera (%d vs %d)" % (free_e["red"][4], through_e["red"][4]))

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
