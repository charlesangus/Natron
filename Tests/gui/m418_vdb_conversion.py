"""Surfaces and points turned into VDB volumes and back into polygons reach the 3D viewer.

Graph: a GeoPrimitive sphere into VDBFromPolygons, and a GeoPrimitive sphere's points into
VDBFromParticles, each into a ConvertVDB set to Polygons; one of the two feeds a GeoToUSD, merged
with a USDCamera looking at the origin, into Viewer1. The script checks:
  - both VDB nodes report their grids' active voxels and memory in their info;
  - mesh to SDF to polygons: the view shows no volume guide (the volume became polygons) and the
    centre of the framed view differs from its corner;
  - with ConvertVDB's group excluding every grid, the VDB passes through and one volume guide is
    drawn;
  - particles to SDF to polygons: the same polygon checks on the particle branch.

Screenshots for sign-off: m418-mesh-roundtrip, m418-volume-passthrough, m418-particles-roundtrip.

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
# How far apart two colours must be, summed over the channels, to count as different.
COLOUR_DIFFERENCE = 24
SPHERE = 3

try:
    import NatronEngine
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getPluginID(), name))
        return p

    mesh = make("GeoPrimitive", 0, 0)
    param(mesh, "type").set(SPHERE)
    from_polygons = make("VDBFromPolygons", 0, 120)
    require(from_polygons.connectInput(0, mesh), "connect the sphere into VDBFromPolygons")
    param(from_polygons, "voxelSize").set(0.05)
    mesh_convert = make("ConvertVDB", 0, 240)
    require(mesh_convert.connectInput(0, from_polygons), "connect VDBFromPolygons into ConvertVDB")

    points = make("GeoPrimitive", 300, 0)
    param(points, "type").set(SPHERE)
    param(points, "sphereRows").set(8)
    param(points, "sphereColumns").set(8)
    from_particles = make("VDBFromParticles", 300, 120)
    require(from_particles.connectInput(0, points), "connect the sphere's points into VDBFromParticles")
    param(from_particles, "voxelSize").set(0.05)
    param(from_particles, "radiusScale").set(0.3)
    particles_convert = make("ConvertVDB", 300, 240)
    require(particles_convert.connectInput(0, from_particles), "connect VDBFromParticles into ConvertVDB")

    to_usd = make("GeoToUSD", 150, 360)
    cam = make("USDCamera", 300, 360)
    cam.getParam("translate").set(0.0, 0.0, 6.0)
    merge = make("USDMerge", 150, 480)
    require(merge.connectInput(0, to_usd), "connect the GeoToUSD into the merge")
    require(merge.connectInput(1, cam), "connect the camera into the merge")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(150, 600)

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
        v = view()
        entries = (v.property("natronSceneGuides") if v is not None else None) or []
        return [str(e[0]) for e in entries if str(e[1]) == "volume"]

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

    def message(node):
        return str(node.getPersistentMessage() or "")

    def frame(v):
        v.activateWindow()
        v.setFocus()
        QTest.keyClick(v, Qt.Key_A)
        return settle()

    def polygons_shown(name):
        v = view()
        image = save(v, name)
        centre = rgb(image, image.width() // 2, image.height() // 2)
        corner = rgb(image, image.width() // 10, image.height() // 10)
        report("NOTE %s centre %r, corner %r" % (name, centre, corner))
        check(differs(centre, corner), "the polygons cover the centre of the view (" + name + ")")

    def steps():
        require(to_usd.connectInput(0, mesh_convert), "connect the mesh branch into the GeoToUSD")
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the stage (%r)" % (v.property("natronSceneError") if v else None,))
        yield frame(v)

        section("mesh to SDF to polygons")
        ok = yield ("until", lambda: "active voxels" in message(from_polygons), POLL_TIMEOUT_S)
        report("NOTE VDBFromPolygons info %r" % (message(from_polygons),))
        check(ok, "VDBFromPolygons reports its grid's voxels and memory")
        check("surface" in message(from_polygons), "the info names the surface grid")
        check(not volume_guides(), "the volume became polygons, so no volume guide (%r)" % (volume_guides(),))
        polygons_shown("m418-mesh-roundtrip")

        section("volume passes through")
        param(mesh_convert, "group").setValue("^*")
        ok = yield ("until", lambda: scene_drawn() and len(volume_guides()) == 1, POLL_TIMEOUT_S)
        check(ok, "with no grid selected the VDB passes through as a volume (%r)" % (volume_guides(),))
        yield settle()
        save(view(), "m418-volume-passthrough")
        param(mesh_convert, "group").setValue("*")
        ok = yield ("until", lambda: scene_drawn() and not volume_guides(), POLL_TIMEOUT_S)
        check(ok, "selecting every grid again turns it back into polygons")

        section("particles to SDF to polygons")
        # connectInput refuses an input that is already connected.
        to_usd.disconnectInput(0)
        require(to_usd.connectInput(0, particles_convert), "connect the particle branch into the GeoToUSD")
        ok = yield ("until", lambda: "active voxels" in message(from_particles), POLL_TIMEOUT_S)
        report("NOTE VDBFromParticles info %r" % (message(from_particles),))
        check(ok, "VDBFromParticles reports its grid's voxels and memory")
        ok = yield ("until", lambda: scene_drawn() and not volume_guides(), POLL_TIMEOUT_S)
        check(ok, "the particle volume became polygons")
        yield frame(view())
        polygons_shown("m418-particles-roundtrip")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
