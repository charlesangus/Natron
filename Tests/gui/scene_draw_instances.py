"""Draw states reach native instances, point instancers and the Lobby's Geometry row.

A native instance draws through an instancer under /UsdNiPropagatedPrototypes, not at its own
path, so the draw filter turns instances off in that instancer's mask. Graph: USDRead into
Viewer1, first on heavy_instances.usda (tools/bench/heavy_scene_make.py --instances-only: 64
native instances under /World/Instanced, a point instancer of as many balls at /World/Scatter),
then on the Lobby. Each change waits for the frame that shows it (natronSceneDrawPresented), and
each image is compared with the one before the change. The script checks:
  - BBox on one instance, on the whole /World/Instanced row and Hidden on it each change pixels,
    the row more than the one instance, and natronSceneDrawStats counts the masked instances;
  - BBox on /World/Scatter, a point instancer's own row, changes pixels;
  - clearing each change gives back the first image exactly;
  - in the Lobby, /World/assembly_Lobby/max_Lobby/Geometry lies inside the base building's opaque
    shell, so from the framed view it covers no pixel. With the base building Hidden, BBox on
    Geometry changes pixels, and clearing it gives back the image before.

Files: $NATRON_INSTANCES_FILE (by default the script writes it under $NATRON_GUI_TEST_OUT) and
$LOBBY_FILE (default .local/World_Lobby.usd); the Lobby section is skipped, and says so, when the Lobby is absent.

Screenshots for sign-off: scene-instances-shown, scene-instances-one-boxed,
scene-instances-row-boxed, scene-instances-row-hidden, scene-instances-scatter-boxed,
scene-lobby-base-hidden, scene-lobby-geometry-boxed.

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
INSTANCE = "/World/Instanced/Inst0"
INSTANCED = "/World/Instanced"
SCATTER = "/World/Scatter"
INSTANCE_COUNT = 64
BASE_BUILDING = "/World/assembly_BaseBuildingL0"
GEOMETRY = "/World/assembly_Lobby/max_Lobby/Geometry"

try:
    from PySide6.QtCore import Q_ARG, QMetaObject, Qt
    from PySide6.QtGui import QGuiApplication, QImage
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    instances_path = os.environ.get("NATRON_INSTANCES_FILE")
    if not instances_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "scene_draw_instances"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_instances(fixture_dir, INSTANCE_COUNT)
        instances_path = os.path.join(fixture_dir, "heavy_instances.usda")
    require(os.path.isfile(instances_path), "fixture exists: %s" % instances_path)
    lobby_path = os.environ.get("LOBBY_FILE", os.path.join(ROOT, ".local", "World_Lobby.usd"))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(instances_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def draw_stats():
        v = view()
        stats = v.property("natronSceneDrawStats") if v is not None else None
        return dict(stats) if stats else {}

    def presented():
        v = view()
        return int(v.property("natronSceneDrawPresented") or 0) if v is not None else 0

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

    def change(label, method, *args):
        """Calls a draw-state slot and waits until its frame is presented and settled."""
        before = int(draw_stats().get("posted", 0))
        require(QMetaObject.invokeMethod(view(), method, *args), "%s: the scene view has %s" % (label, method))
        posted = int(draw_stats().get("posted", 0))
        if posted == before:
            check(False, "%s: the draw state changed something" % label)
            return
        ok = yield ("until", lambda: presented() >= posted, POLL_TIMEOUT_S)
        check(ok, "%s: reached the screen" % label)
        yield ("sleep", SETTLE_MS)

    def set_state(row, state):
        yield from change("%s %s" % (state, row), "setDrawState", Q_ARG("QString", row), Q_ARG("QString", state))

    def clear_state(row):
        yield from change("clear " + row, "clearDrawState", Q_ARG("QString", row))

    def open_scene(path):
        usd.getParam("filename").setValue(path)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows %s (%r)" % (path, v.property("natronSceneError") if v else None))
        yield from frame_all()

    def changes_and_restores(row, state, name, base):
        """Pixels changed by state on row, after checking that clearing it restores base."""
        yield from set_state(row, state)
        image = save(name)
        changed = changed_pixels(base, image)
        masked = int(draw_stats().get("maskedInstances", 0))
        report("NOTE %s %s: %r pixels changed, %d instances masked" % (state, row, changed, masked))
        check(bool(changed), "%s %s changes the image" % (state, row))
        yield from clear_state(row)
        restored = changed_pixels(base, grab())
        check(restored == 0, "clearing %s gives back the first image (%r pixels differ)" % (row, restored))
        return changed or 0, masked

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the instances")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        yield from frame_all()

        section("native instances")
        base = save("scene-instances-shown")
        one, masked = yield from changes_and_restores(INSTANCE, "bbox", "scene-instances-one-boxed", base)
        check(masked == 1, "BBox on one instance masks one instance (%d)" % masked)
        row, masked = yield from changes_and_restores(INSTANCED, "bbox", "scene-instances-row-boxed", base)
        check(masked == INSTANCE_COUNT, "BBox on the row masks all %d instances (%d)" % (INSTANCE_COUNT, masked))
        check(row > one, "the row changes more pixels than one instance (%d, %d)" % (row, one))
        hidden, masked = yield from changes_and_restores(INSTANCED, "hidden", "scene-instances-row-hidden", base)
        check(masked == INSTANCE_COUNT, "Hidden on the row masks all %d instances (%d)" % (INSTANCE_COUNT, masked))
        check(int(draw_stats().get("maskedInstances", -1)) == 0, "no instance stays masked once the rows are clear")

        section("point instancer")
        yield from changes_and_restores(SCATTER, "bbox", "scene-instances-scatter-boxed", base)

        section("Lobby Geometry")
        if not os.path.isfile(lobby_path):
            report("SKIP the Lobby is not at %s (set LOBBY_FILE)" % lobby_path)
            return
        yield from open_scene(lobby_path)
        lobby = grab()
        yield from set_state(GEOMETRY, "bbox")
        occluded = changed_pixels(lobby, grab())
        report("NOTE BBox on Geometry inside the base building changes %r pixels (its shell hides it)" % (occluded,))
        yield from clear_state(GEOMETRY)
        yield from set_state(BASE_BUILDING, "hidden")
        opened = save("scene-lobby-base-hidden")
        check(bool(changed_pixels(lobby, opened)), "Hidden on the base building changes the image")
        changed, _ = yield from changes_and_restores(GEOMETRY, "bbox", "scene-lobby-geometry-boxed", opened)
        report("NOTE with the base building Hidden, BBox on Geometry changes %d pixels" % changed)
        yield from clear_state(BASE_BUILDING)
        restored = changed_pixels(lobby, grab())
        check(restored == 0, "clearing the base building gives back the Lobby's first image (%r pixels differ)" % (restored,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
