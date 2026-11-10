"""The 3D view showing geometry: a GeoPrimitive chain viewed directly, the geometry display toggles,
and a time-varying upstream scrubbed without resyncing the viewer's copy of the geometry.

Graph: GeoPrimitive (sphere) -> GeoGroup (point group "picked", points 0-40) -> GeoTransform
(translate y keyed 0 at frame 1, 2 at frame 10) -> Viewer1.

Checks:
  - the viewer switches to the 3D view for geometry and draws it, with no error;
  - the geometry display row shows while geometry is viewed, its controls follow the viewer knobs;
  - Point Markers, Normals and Highlight Group each change what the view draws;
  - Point Numbers and Primitive Numbers paint labels (natronGeoLabels), within the per-frame cap;
  - scrubbing frames refreshes the viewer's geometry in place each frame (natronGeoRefreshes
    rises) and resyncs no prim (natronGeoRefreshResyncs stays 0);
  - selecting the sphere's prim maps it to its primitive numbers (natronGeoPick).

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
LABEL_CAP = 1000
TOGGLES = ("pointMarkers", "pointNumbers", "primitiveNumbers", "normals")

try:
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    try:
        app.getProjectParam("frameRange").set(1, 10)
    except Exception:
        report("NOTE could not set the project frame range: " + traceback.format_exc().splitlines()[-1])

    primitive = app.createNode("GeoPrimitive")
    require(primitive is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    primitive.getParam("type").setValue(3)
    group = app.createNode("GeoGroup")
    require(group is not None, "create GeoGroup", plugin_problem("GeoGroup"))
    group.getParam("groupName").setValue("picked")
    group.getParam("groupType").setValue(0)
    group.getParam("usePattern").setValue(True)
    group.getParam("pattern").setValue("0-40")
    transform = app.createNode("GeoTransform")
    require(transform is not None, "create GeoTransform", plugin_problem("GeoTransform"))
    translate = transform.getParam("translate")
    require(translate is not None, "GeoTransform has a translate parameter")
    translate.setValueAtTime(0., 1, 1)
    translate.setValueAtTime(2., 10, 1)
    require(group.connectInput(0, primitive), "connect the GeoGroup to the GeoPrimitive")
    require(transform.connectInput(0, group), "connect the GeoTransform to the GeoGroup")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    primitive.setPosition(0, 0)
    group.setPosition(0, 100)
    transform.setPosition(0, 200)
    viewer.setPosition(0, 320)
    py_viewer = {}

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def row():
        rows = [w for w in QApplication.allWidgets() if w.objectName() == "geoDisplayRow"]
        return rows[0] if rows else None

    def control(name):
        r = row()
        if r is None:
            return None
        found = [w for w in r.children() if hasattr(w, "objectName") and w.objectName() == name]
        return found[0] if found else None

    def framebuffer():
        v = view()
        image = v.grab().toImage()
        if image.isNull():
            origin = v.mapToGlobal(QPoint(0, 0))
            image = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), v.width(), v.height()).toImage()
        return image

    def differs(a, b, step=4):
        if a.size() != b.size():
            return True
        for y in range(0, a.height(), step):
            for x in range(0, a.width(), step):
                if a.pixel(x, y) != b.pixel(x, y):
                    return True
        return False

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def seek(frame):
        if "v" not in py_viewer:
            py_viewer["v"] = app.getViewer(viewer.getScriptName())
        py_viewer["v"].seek(frame)

    def set_knob(name, value):
        viewer.getParam(name).setValue(value)

    def all_off():
        for name in TOGGLES:
            set_knob(name, False)
        set_knob("highlightGroup", "")

    def steps():
        require(viewer.connectInput(0, transform), "connect the Viewer to the GeoTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the geometry (%r)" % (prop("natronSceneError"),))
        seek(1)
        yield settle()
        shot("m376-geometry.png")

        r = row()
        check(r is not None and r.isVisible(), "the geometry display row shows while geometry is viewed")
        for name in TOGGLES + ("normalScale", "highlightGroup"):
            check(viewer.getParam(name) is not None, "the Viewer has a %s parameter" % name)
            check(control(name) is not None, "the geometry display row has a %s control" % name)

        all_off()
        yield settle()
        plain = framebuffer()

        section_toggles = [("pointMarkers", True, "m376-point-markers.png"),
                           ("normals", True, "m376-normals.png"),
                           ("highlightGroup", "picked", "m376-highlight-group.png")]
        for name, value, picture in section_toggles:
            set_knob(name, value)
            box = control(name)
            if name == "highlightGroup":
                check(box is not None and box.text() == "picked", "the Highlight Group field follows its knob")
            else:
                check(box is not None and box.isChecked(), "the %s toggle follows its knob" % name)
            yield settle()
            ok = yield ("until", lambda: differs(plain, framebuffer()), POLL_TIMEOUT_S)
            check(ok, "turning %s on changes what the view draws" % name)
            shot(picture)
            set_knob(name, "" if name == "highlightGroup" else False)
            yield settle()

        set_knob("normals", True)
        set_knob("normalScale", 0.5)
        yield settle()
        shot("m376-normals-long.png")
        set_knob("normals", False)
        set_knob("normalScale", 0.1)

        for name, picture in (("pointNumbers", "m376-point-numbers.png"), ("primitiveNumbers", "m376-primitive-numbers.png")):
            set_knob(name, True)
            ok = yield ("until", lambda: int(prop("natronGeoLabels", 0)) > 0, POLL_TIMEOUT_S)
            labels = int(prop("natronGeoLabels", 0))
            check(ok, "%s paints labels (%d)" % (name, labels))
            check(labels <= LABEL_CAP, "%s paints at most %d labels (%d)" % (name, LABEL_CAP, labels))
            shot(picture)
            set_knob(name, False)
            ok = yield ("until", lambda: int(prop("natronGeoLabels", 0)) == 0, POLL_TIMEOUT_S)
            check(ok, "turning %s off removes its labels" % name)

        set_knob("pointMarkers", True)
        yield settle()
        refreshes = int(prop("natronGeoRefreshes", 0))
        resyncs = int(prop("natronGeoRefreshResyncs", 0))
        report("NOTE before the scrub: %d refreshes, %d resyncs" % (refreshes, resyncs))
        for frame in range(2, 11):
            seek(frame)
            ok = yield ("until", lambda: int(prop("natronGeoRefreshes", 0)) > refreshes, POLL_TIMEOUT_S)
            check(ok, "frame %d refreshes the viewer's geometry in place" % frame)
            refreshes = int(prop("natronGeoRefreshes", 0))
        yield settle()
        check(int(prop("natronGeoRefreshResyncs", 0)) == 0,
              "scrubbing resyncs no prim (%r)" % (prop("natronGeoRefreshResyncs"),))
        check(not prop("natronSceneError"), "no error after the scrub (%r)" % (prop("natronSceneError"),))
        shot("m376-scrubbed-frame-10.png")
        set_knob("pointMarkers", False)

        # The camera framed the sphere once, at frame 1; at frame 10 it is translated off the
        # view centre the click aims at.
        refreshes = int(prop("natronGeoRefreshes", 0))
        seek(1)
        ok = yield ("until", lambda: int(prop("natronGeoRefreshes", 0)) > refreshes, POLL_TIMEOUT_S)
        check(ok, "back at frame 1 the viewer's geometry refreshes")
        yield settle()

        v = view()
        centre = v.rect().center()
        for kind, button, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton, Qt.LeftButton),
                                      (QEvent.MouseButtonRelease, Qt.LeftButton, Qt.NoButton)):
            QApplication.sendEvent(v, QMouseEvent(kind, QPointF(centre), QPointF(v.mapToGlobal(centre)), button, buttons, Qt.NoModifier))
        QApplication.processEvents()
        ok = yield ("until", lambda: bool(prop("natronGeoPick", [])), POLL_TIMEOUT_S)
        picks = prop("natronGeoPick", [])
        report("NOTE selection %r, pick %r" % (prop("natronSceneSelection"), picks))
        if check(ok, "a click on the sphere maps its prim back to primitives"):
            path, primitives = picks[-1][0], picks[-1][1]
            check(len(primitives) > 0, "the picked prim %s is made of primitives (%d)" % (path, len(primitives)))
            check(list(primitives) == sorted(primitives), "the picked primitive numbers are ascending")
        shot("m376-picked.png")

        all_off()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
