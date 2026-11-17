"""The scene stand-in: boxes over a cleared view in place of the scene, as the Preferences say.

Graph: USDRead of a ten-block heavy_city.usda (tools/bench/heavy_scene_make.py) into Viewer1, every
payload unloaded. The city's models are /World and its blocks (group) and the buildings (assembly). The
script checks:
  - under the default During Interaction mode, a still view shows a full frame, not the stand-in, and
    publishes that frame's cost (natronSceneLastFullFrameMs);
  - sceneStandInMode Always shows the stand-in (natronSceneStandIn): one box or marker per model, the
    image changes, the view counts as synced, and orbiting it keeps it up;
  - sceneStandInBoxes Per Draw State Row, with no draw state set, draws one box for /World;
  - Off again shows a full frame, with no stand-in.

File: $NATRON_CITY_FILE, by default a city of ten blocks the script writes under $NATRON_GUI_TEST_OUT.

Screenshots for sign-off: stand-in-full-frame, stand-in-models, stand-in-rows.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 1000
POLL_MS = 5
BLOCKS = 10
BUILDINGS_PER_BLOCK = 12
CITY_FLOORS = 16
CITY_PRIMS = BLOCKS * BUILDINGS_PER_BLOCK * 20 + BLOCKS + 1
MODELS = 1 + BLOCKS + BLOCKS * BUILDINGS_PER_BLOCK
MODE_OFF, MODE_INTERACTION, MODE_ALWAYS = 0, 1, 2
BOXES_MODELS, BOXES_ROWS = 0, 1

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    city_path = os.environ.get("NATRON_CITY_FILE")
    if not city_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "m336_stand_in"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, BLOCKS, BUILDINGS_PER_BLOCK, CITY_FLOORS)
        city_path = os.path.join(fixture_dir, "heavy_city.usda")
    require(os.path.isfile(city_path), "fixture exists: %s" % city_path)

    settings = NatronEngine.natron.getSettings()
    mode_param = settings.getParam("sceneStandInMode")
    boxes_param = settings.getParam("sceneStandInBoxes")
    require(mode_param is not None and boxes_param is not None, "Preferences have sceneStandInMode and sceneStandInBoxes")
    original_mode = mode_param.getValue()
    original_boxes = boxes_param.getValue()
    check(original_mode == MODE_INTERACTION, "the stand-in mode defaults to During Interaction (%r)" % original_mode)
    check(original_boxes == BOXES_MODELS, "the stand-in boxes default to Per Model (%r)" % original_boxes)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(city_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def stand_in_boxes():
        return int(prop("natronSceneStandInBoxes", 0))

    def full_frame_shown():
        return scene_drawn() and prop("natronSceneStandIn") is False

    def grab():
        v = view()
        if v is None:
            return QImage()
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height()).convertToFormat(QImage.Format_RGBX8888)

    def save(name):
        shot(name + ".png")
        image = grab()
        path = os.path.join(OUT, name + "_view.png")
        image.save(path)
        report("SHOT " + path)
        return image

    def changed_pixels(first, second):
        if first.isNull() or first.size() != second.size():
            return None
        a = bytes(first.constBits())[:first.sizeInBytes()]
        b = bytes(second.constBits())[:second.sizeInBytes()]
        if a == b:
            return 0
        return sum(1 for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3])

    def frame_all():
        view().activateWindow()
        ok = yield ("until", lambda: view() is not None and view().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the scene view's window is active before the key press")
        v = view()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, Qt.Key_A)
        QApplication.processEvents()
        yield ("sleep", 2 * SETTLE_MS)

    def orbit(seconds):
        """Tumbles the view for seconds; returns how many of its paints showed the stand-in, and how many did not."""
        navigation = settings.getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        navigation.setValue(1)
        v = view()
        v.setFocus()
        QApplication.processEvents()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier))

        counts = [0, 0]
        last = int(prop("natronScenePaintCount", 0))
        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        step = 0
        for _ in range(int(seconds * 1000 / POLL_MS)):
            step += 1
            mouse(QEvent.MouseMove, centre + QPoint((step % 40) * 4 - 80, (step % 20) * 2 - 20), Qt.NoButton, Qt.LeftButton)
            yield ("sleep", POLL_MS)
            count = int(prop("natronScenePaintCount", 0))
            if count != last:
                counts[0 if prop("natronSceneStandIn") else 1] += 1
            last = count
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        navigation.setValue(original)
        return counts

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the city")
        yield from frame_all()

        section("During Interaction, still")
        ok = yield ("until", full_frame_shown, POLL_TIMEOUT_S)
        check(ok, "a still view shows a full frame (natronSceneStandIn %r)" % (prop("natronSceneStandIn"),))
        cost = prop("natronSceneLastFullFrameMs")
        check(cost is not None and float(cost) > 0.0, "the last full frame's cost is published (%r)" % (cost,))
        base = save("stand-in-full-frame")

        section("Always, per model")
        mode_param.setValue(MODE_ALWAYS)
        ok = yield ("until", lambda: bool(prop("natronSceneStandIn")) and stand_in_boxes() == MODELS and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "Always shows %d stand-in boxes and markers, one per model, and is synced (stand-in %r, %d drawn, synced %r)"
              % (MODELS, prop("natronSceneStandIn"), stand_in_boxes(), prop("natronSceneSynced")))
        yield ("sleep", SETTLE_MS)
        image = save("stand-in-models")
        check(bool(changed_pixels(base, image)), "the stand-in changes the image")
        frames_before = prop("natronSceneStormFrames")
        stand_in, full = yield from orbit(1.0)
        check(stand_in > 0 and full == 0, "orbiting under Always paints only the stand-in (%d stand-in, %d full paints)" % (stand_in, full))
        yield ("sleep", SETTLE_MS)
        check(prop("natronSceneStormFrames") == frames_before, "no full frame was rendered under Always")
        check(bool(prop("natronSceneStandIn")) and stand_in_boxes() == MODELS, "the stand-in stays up after the orbit (%d drawn)" % stand_in_boxes())

        section("Always, per draw state row")
        boxes_param.setValue(BOXES_ROWS)
        ok = yield ("until", lambda: stand_in_boxes() == 1, POLL_TIMEOUT_S)
        check(ok, "with no draw state set, Per Draw State Row draws one box, for /World (%d drawn)" % stand_in_boxes())
        yield ("sleep", SETTLE_MS)
        save("stand-in-rows")
        boxes_param.setValue(original_boxes)

        section("Off")
        mode_param.setValue(MODE_OFF)
        ok = yield ("until", full_frame_shown, LOAD_TIMEOUT_S)
        check(ok, "Off shows a full frame again (natronSceneStandIn %r)" % (prop("natronSceneStandIn"),))
        check(stand_in_boxes() == 0, "Off draws no stand-in box (%d)" % stand_in_boxes())
        mode_param.setValue(original_mode)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
