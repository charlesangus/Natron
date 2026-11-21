"""USDProject3D in the scene view's CPU renderers: a Constant projected through a USDCamera onto a
USDCard shows on the card, follows the camera when it moves, and follows an edit of the Constant.

Constant1 feeds USDProject3D1's Image input, USDCamera1 its Camera input, and USDProject3D1 feeds
USDCard1's Material input, which feeds Viewer1. The camera's long lens frames only the middle of the
card, so with Crop on and Outside = Black the plate is a red patch on a black card. For Cycles and
then MoonRay:
  - pick the renderer from the view's context menu; the card shows a red patch;
  - move USDCamera1 right: the red patch moves right in the view;
  - set the Constant to green, touching nothing else: the patch turns green.
A renderer this build lacks is reported as SKIP.

The project format is small: every image lives in /dev/shm while a delegate may read it, and the 64 MB
of a container's default holds only one 1920x1080 image with its mips.

Screenshots, one file each, with a _viewer.png crop of the scene view beside each:
scene_projection_<renderer>_plate.png, scene_projection_<renderer>_moved.png and
scene_projection_<renderer>_edited.png.

Run through Tests/gui/run-gui-test.sh, at 1920x1080. Screenshots and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 90.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
RENDERERS = (("Cycles", "cycles", "HdCyclesPlugin"), ("MoonRay", "moonray", "HdMoonrayRendererPlugin"))
# At 5 units a 360 mm lens on the 36 mm back frames 0.5 units of the unit card, so the crop edge
# shows and a camera move shifts it.
CAMERA_DISTANCE = 5.0
FOCAL_LENGTH = 360.0
CAMERA_SHIFT = 0.2
# The view frames the card at a few hundred pixels across; a 0.2 unit move is tens of pixels.
MIN_SHIFT_PX = 12.0
RED = (1.0, 0.0, 0.0, 1.0)
GREEN = (0.0, 1.0, 0.0, 1.0)

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    app.addFormat("projectionPlate 256x256")
    project_format = app.getProjectParam("outputFormat")
    format_options = list(project_format.getOptions())
    project_format.setValue(next((i for i, o in enumerate(format_options) if "projectionPlate" in str(o)), len(format_options) - 1))

    constant = make("Constant", 0, 0)
    camera = make("USDCamera", 200, 0)
    project = make("USDProject3D", 0, 100)
    card = make("USDCard", 0, 200)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)

    color = param(constant, "color")
    translate = param(camera, "translate")
    param(camera, "focalLength").set(FOCAL_LENGTH)
    param(project, "crop").set(True)
    param(project, "outside").set(0)

    # A child widget's wrapper dies with its parent's, and a menu's with the event that made it.
    _held = []

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def grab_view():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(origin.x(), origin.y(), v.width(), v.height())

    def hue(c):
        """'r', 'g', or None for a pixel that is neither clearly red nor clearly green."""
        r, g, b = c.red(), c.green(), c.blue()
        if r > 60 and r > 2 * g and r > 2 * b:
            return "r"
        if g > 60 and g > 2 * r and g > 2 * b:
            return "g"
        return None

    def patch(want, step=6):
        """The (count, centroid x, centroid y) of the view's pixels of hue want on a step-px grid,
        keeping only those with two such neighbours: the view's axes are thin lines, not regions."""
        image = grab_view()
        if image is None:
            return None
        hits = set()
        for y in range(0, image.height(), step):
            for x in range(0, image.width(), step):
                if hue(image.pixelColor(x, y)) == want:
                    hits.add((x, y))
        kept = [(x, y) for x, y in hits
                if sum((x + dx, y + dy) in hits for dx, dy in ((step, 0), (-step, 0), (0, step), (0, -step))) >= 2]
        if len(kept) < 20:
            return None
        return (len(kept), sum(p[0] for p in kept) / float(len(kept)), sum(p[1] for p in kept) / float(len(kept)))

    def save_shots(name):
        shot(name + ".png")
        image = grab_view()
        if image is not None:
            path = os.path.join(OUT, name + "_viewer.png")
            image.save(path)
            report("SHOT " + path)

    def activate_main_window():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            guis[0].activateWindow()
            guis[0].raise_()
        QApplication.processEvents()

    def popup():
        return QApplication.activePopupWidget()

    def post_context_menu(v, state):
        centre = v.rect().center()

        def post():
            press = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(v.mapToGlobal(centre)),
                                Qt.RightButton, Qt.RightButton, Qt.NoModifier)
            QApplication.sendEvent(v, press)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            state["closed"] = True

        QTimer.singleShot(0, post)

    def open_renderer_submenu(state):
        activate_main_window()
        post_context_menu(view(), state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        if not ok:
            return None
        menu = popup()
        _held.append(menu)
        found = [a for a in menu.actions() if a.text().replace("&", "") == "Renderer" and a.menu() is not None]
        if not found:
            return None
        _held.append(found[0])
        sub = found[0].menu()
        _held.append(sub)
        menu.setActiveAction(found[0])
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        return sub

    def finish_menu(state):
        for _ in range(4):
            p = popup()
            if p is None:
                break
            QTest.keyClick(p, Qt.Key_Escape)
            QApplication.processEvents()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def moved_right(before):
        after = patch("r")
        return after is not None and after[1] - before[1] >= MIN_SHIFT_PX

    def exercise(label, slug, renderer_id):
        translate.set(0.0, 0.0, CAMERA_DISTANCE)
        color.set(*RED)
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the context menu has a Renderer submenu")
        found = [a for a in sub.actions() if a.text().replace("&", "").lower().startswith(slug)]
        if not found or not found[0].isEnabled() or "unavailable" in found[0].text():
            yield from finish_menu(state)
            report("SKIP %s is not available in this build" % label)
            return
        found[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == renderer_id, POLL_TIMEOUT_S)
        require(ok, "picking %s switches the view (%r)" % (label, prop("natronSceneRenderer")))
        check(not prop("natronSceneRendererError"), "no renderer error on %s (%r)" % (label, prop("natronSceneRendererError")))

        ok = yield ("until", lambda: scene_drawn() and patch("r") is not None, POLL_TIMEOUT_S)
        check(ok, "%s shows the projected plate as a red patch on the card (error %r, projector %r)"
              % (label, prop("natronSceneError"), project.getPersistentMessage()))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_projection_%s_plate" % slug)
        before = patch("r")
        if before is None:
            return

        restarts = prop("natronSceneRestartCount")
        translate.set(CAMERA_SHIFT, 0.0, CAMERA_DISTANCE)
        ok = yield ("until", lambda: scene_drawn() and moved_right(before), POLL_TIMEOUT_S)
        check(ok, "moving USDCamera1 right moves the patch right in %s (from %r to %r)" % (label, before, patch("r")))
        check(prop("natronSceneRestartCount") > restarts,
              "the camera move restarted %s's refinement (%r, was %r)" % (label, prop("natronSceneRestartCount"), restarts))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_projection_%s_moved" % slug)
        moved = patch("r")

        restarts = prop("natronSceneRestartCount")
        color.set(*GREEN)
        ok = yield ("until", lambda: scene_drawn() and patch("g") is not None and patch("r") is None, POLL_TIMEOUT_S)
        check(ok, "editing the Constant turns the patch green in %s (green %r, red %r, error %r)"
              % (label, patch("g"), patch("r"), prop("natronSceneError")))
        green = patch("g")
        if moved is not None and green is not None:
            check(abs(green[1] - moved[1]) < MIN_SHIFT_PX and abs(green[2] - moved[2]) < MIN_SHIFT_PX,
                  "the green patch is where the red one was in %s (%r, %r)" % (label, moved, green))
        check(prop("natronSceneRestartCount") > restarts,
              "the edit restarted %s's refinement (%r, was %r)" % (label, prop("natronSceneRestartCount"), restarts))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_projection_%s_edited" % slug)

    def steps():
        require(project.connectInput(0, constant), "connect Constant1 to USDProject3D1's Image input")
        require(project.connectInput(1, camera), "connect USDCamera1 to USDProject3D1's Camera input")
        require(card.connectInput(0, project), "connect USDProject3D1 to USDCard1's Material input")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, card), "connect the Viewer to USDCard1")
        translate.set(0.0, 0.0, CAMERA_DISTANCE)
        color.set(*RED)
        ok = yield ("until", lambda: scene_drawn() and patch("r") is not None, STARTUP_TIMEOUT_S)
        require(ok, "Storm shows the projected plate on the card")
        for label, slug, renderer_id in RENDERERS:
            yield from exercise(label, slug, renderer_id)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
