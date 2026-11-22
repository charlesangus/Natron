"""A VDB volume read into native geometry reaches the 3D viewer and a Storm render as a UsdVolVolume.

Graph: GeoVDBRead(m417-fog-sphere.vdb) into GeoToUSD, merged with a USDCamera (VolumeCam) looking
at the origin, into USDRender. The fixture is a fog volume sphere of radius 1 at the origin, one
float grid named "density"; $NATRON_M417_VDB overrides its path. The script checks:
  - with Viewer1 on the merge, the scene view draws the stage and lists a "volume" guide, the
    volume's bounding box, and the pixels near it are the guide colour;
  - the volume is drawn: the centre of the framed view differs from the empty view's background;
  - with Viewer1 on the USDRender (Storm), the render's centre differs from its corner.

Screenshots for sign-off: m417-volume-scene, m417-volume-empty, m417-volume-render.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 800
STARTUP_TIMEOUT_S = 60.0
SEARCH_RADIUS = 4
# How far apart two colours must be, summed over the channels, to count as different.
COLOUR_DIFFERENCE = 24

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import (FIXTURES, OUT, check, finish, plugin_problem, report, require, run, section, shot,
                         viewer_up, viewer_messages, widgets_of)

    vdb_path = os.environ.get("NATRON_M417_VDB", os.path.join(FIXTURES, "m417-fog-sphere.vdb"))
    require(os.path.isfile(vdb_path), "fixture exists: " + vdb_path)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    read = make("GeoVDBRead", 0, 0)
    require(read.getParam("file") is not None and read.getParam("grids") is not None,
            "GeoVDBRead has file and grids parameters")
    read.getParam("file").setValue(vdb_path)
    to_usd = make("GeoToUSD", 0, 120)
    require(to_usd.connectInput(0, read), "connect the GeoVDBRead into the GeoToUSD")
    cam = make("USDCamera", 150, 120)
    cam.setScriptName("VolumeCam")
    cam.getParam("translate").set(0.0, 0.0, 6.0)
    merge = make("USDMerge", 75, 240)
    require(merge.connectInput(0, to_usd), "connect the GeoToUSD into the merge")
    require(merge.connectInput(1, cam), "connect VolumeCam into the merge")
    render = make("USDRender", 75, 360)
    require(render.connectInput(0, merge), "connect the USDRender to the merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(250, 360)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if not views:
            return None
        if not views[0].property("natronSceneRecordGuides"):
            views[0].setProperty("natronSceneRecordGuides", True)
            views[0].update()
        return views[0]

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def volume_guides():
        """path -> QPoint of the volume guides the last paint drew."""
        v = view()
        found = {}
        for entry in (v.property("natronSceneGuides") if v is not None else None) or []:
            if str(entry[1]) == "volume":
                found[str(entry[0])] = QPoint(int(round(float(entry[2]))), int(round(float(entry[3]))))
        return found

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def grab(widget):
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), widget.width(), widget.height())

    def save(widget, name):
        shot(name + ".png")
        image = grab(widget)
        path = os.path.join(OUT, name + "_view.png")
        image.save(path)
        report("SHOT " + path)
        return image

    def rgb(image, x, y):
        c = image.pixelColor(x, y)
        return (c.red(), c.green(), c.blue())

    def differs(a, b):
        return sum(abs(p - q) for p, q in zip(a, b)) > COLOUR_DIFFERENCE

    # Non-camera guides are drawn in the light guide colour (0.95, 0.9, 0.55).
    def is_guide_colour(c):
        return c.red() > 200 and c.green() > 190 and 90 < c.blue() < 190

    def colour_near(image, point, test):
        for dy in range(-SEARCH_RADIUS, SEARCH_RADIUS + 1):
            for dx in range(-SEARCH_RADIUS, SEARCH_RADIUS + 1):
                x, y = point.x() + dx, point.y() + dy
                if 0 <= x < image.width() and 0 <= y < image.height() and test(image.pixelColor(x, y)):
                    return True
        return False

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the stage (%r)" % (v.property("natronSceneError") if v else None,))
        v.activateWindow()
        v.setFocus()
        QTest.keyClick(v, Qt.Key_A)
        yield settle()

        section("bounding-box guide")
        ok = yield ("until", lambda: len(volume_guides()) == 1, POLL_TIMEOUT_S)
        guides = volume_guides()
        report("NOTE volume guides %r" % (dict((p, (q.x(), q.y())) for p, q in guides.items()),))
        require(ok, "the view draws one volume guide (%r)" % (list(guides.keys()),))
        image = save(v, "m417-volume-scene")
        point = list(guides.values())[0]
        check(colour_near(image, point, is_guide_colour), "the volume guide is drawn in the guide colour")
        centre = rgb(image, image.width() // 2, image.height() // 2)

        section("volume drawn")
        read.getParam("grids").setValue("^*")
        ok = yield ("until", lambda: scene_drawn() and len(volume_guides()) == 0, POLL_TIMEOUT_S)
        check(ok, "no grid selected leaves no volume guide (%r)" % (list(volume_guides().keys()),))
        yield settle()
        empty = save(v, "m417-volume-empty")
        background = rgb(empty, empty.width() // 2, empty.height() // 2)
        report("NOTE centre with the volume %r, without %r" % (centre, background))
        check(differs(centre, background), "the volume changes the centre of the view")
        read.getParam("grids").setValue("*")
        ok = yield ("until", lambda: scene_drawn() and len(volume_guides()) == 1, POLL_TIMEOUT_S)
        check(ok, "selecting every grid again brings the volume back")

        section("Storm render")
        require(viewer.connectInput(0, render), "connect the Viewer to the USDRender")
        ok = yield ("until", lambda: any(w.isVisible() for w in widgets_of("ViewerGL")), POLL_TIMEOUT_S)
        require(ok, "the image viewer is up")
        yield ("sleep", 3000)
        report("NOTE viewer messages %r" % (viewer_messages(),))
        gl = [w for w in widgets_of("ViewerGL") if w.isVisible()][0]
        image = save(gl, "m417-volume-render")
        mid = rgb(image, image.width() // 2, image.height() // 2)
        corner = rgb(image, image.width() // 2 + image.width() // 4, image.height() // 2 + image.height() // 3)
        report("NOTE render centre %r, off the volume %r" % (mid, corner))
        check(differs(mid, corner), "Storm draws the volume at the centre of the render")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
