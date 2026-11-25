"""A VDB volume read into native geometry reaches the 3D viewer and a Storm render as a UsdVolVolume.

Graph: GeoVDBRead(m417-fog-sphere.vdb) into GeoToUSD, merged with a USDCamera (VolumeCam) looking
at the origin, into USDRender. The fixture is a fog volume sphere of radius 1 at the origin, one
float grid named "density"; $NATRON_M417_VDB overrides its path. The script checks:
  - with Viewer1 on the merge, the scene view draws the stage and lists a "volume" guide, the
    volume's bounding box, and the pixels near it are the guide colour;
  - the volume is drawn: with the grid hidden and the Viewer's gain at 3 f-stops, the centre of the
    framed view differs from the empty view's background;
  - with Viewer1 on the USDRender (Storm) showing alpha, the render's centre differs from its
    corner;
  - packed copies: the volume copied by a GeoCopyToPoints (Pack And Instance on) onto a row of
    points spread along X, through a second GeoToUSD, is drawn at every copy: with the view framed on
    everything, each vertical third of it differs from the background outside the guide colour.
    The stage's PointInstancer instance count and its distinct .vdb file paths are reported when
    pxr is importable. The coverage check repeats on Cycles at a low Max Samples when the build has
    it, and is skipped with a NOTE when it does not.

Screenshots for sign-off: m417-volume-scene, m417-volume-drawn, m417-volume-empty, m417-volume-render,
m417-copies-storm, m417-copies-cycles.

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
PATCH_RADIUS = 4
COPY_COLUMNS = 2
COPY_ROWS = 1
COPY_SPACING = 6.0
COPY_DEPTH = 0.1
# The fraction of a third of the view that must differ from the background for a copy to count as drawn.
MIN_COVERAGE = 0.002
CYCLES_ID = "HdCyclesPlugin"
CYCLES_SAMPLES = 8
CYCLES_TIMEOUT_S = 120.0
VOLUME_GAIN_STOPS = 3.0
# The Viewer's displayChannels entry that shows the alpha channel.
DISPLAY_ALPHA = 5

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
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
    enable_gain = viewer.getParam("enableGain")
    gain = viewer.getParam("gain")
    require(enable_gain is not None and gain is not None, "the Viewer has enableGain and gain parameters")

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

    def median_around(image):
        """Per channel, the median over a patch at the centre, so a guide line through it does not count."""
        cx, cy = image.width() // 2, image.height() // 2
        samples = [rgb(image, cx + dx, cy + dy) for dy in range(-PATCH_RADIUS, PATCH_RADIUS + 1)
                   for dx in range(-PATCH_RADIUS, PATCH_RADIUS + 1)]
        return tuple(sorted(c[i] for c in samples)[len(samples) // 2] for i in range(3))

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

    _held = []

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

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

    def close_menus():
        for _ in range(4):
            p = popup()
            if p is None:
                break
            QTest.keyClick(p, Qt.Key_Escape)
            QApplication.processEvents()

    def open_renderer_submenu(state):
        """Steps that post the view's context menu and open its Renderer submenu; returns the submenu or None."""
        v = view()
        post_context_menu(v, state)
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
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def coverage(image, background):
        """Per vertical third, the fraction of pixels that differ from background and are not guide-coloured."""
        fractions = []
        width = image.width()
        for third in range(3):
            x0, x1 = third * width // 3, (third + 1) * width // 3
            hits = 0
            for y in range(0, image.height(), 2):
                for x in range(x0, x1, 2):
                    c = image.pixelColor(x, y)
                    if not is_guide_colour(c) and differs((c.red(), c.green(), c.blue()), background):
                        hits += 1
            fractions.append(hits / float(max(1, ((x1 - x0 + 1) // 2) * ((image.height() + 1) // 2))))
        return fractions

    def corner_background(image):
        samples = [rgb(image, x, y) for y in range(8, 8 + 2 * PATCH_RADIUS) for x in range(8, 8 + 2 * PATCH_RADIUS)]
        return tuple(sorted(c[i] for c in samples)[len(samples) // 2] for i in range(3))

    def report_stage(source):
        """Reports the PointInstancer instance count and the distinct .vdb paths of the stage source serves."""
        try:
            from pxr import Usd, UsdGeom
        except Exception:
            report("NOTE pxr is not importable here; the stage is not inspected")
            return
        work_dir = os.path.join(OUT, "work")
        if not os.path.isdir(work_dir):
            os.makedirs(work_dir)
        writer = make("USDWrite", 450, 300)
        writer.getParam("exportMode").setValue(1)
        path = os.path.join(work_dir, "m417_copies.usda")
        if os.path.exists(path):
            os.remove(path)
        writer.getParam("filename").setValue(path)
        require(writer.connectInput(0, source), "connect a USDWrite to the copies' GeoToUSD")
        app.renderBlocking(writer, 1, 1)
        if not os.path.isfile(path):
            report("NOTE USDWrite did not write " + path)
            return
        stage = Usd.Stage.Open(path)
        instancers = [p for p in stage.Traverse() if p.IsA(UsdGeom.PointInstancer)]
        counts = []
        for prim in instancers:
            indices = UsdGeom.PointInstancer(prim).GetProtoIndicesAttr().Get()
            counts.append((str(prim.GetPath()), len(indices) if indices is not None else 0))
        files = set()
        for prim in stage.Traverse():
            attr = prim.GetAttribute("filePath")
            value = attr.Get() if attr else None
            if value is not None and str(getattr(value, "path", value)).endswith(".vdb"):
                files.add(str(getattr(value, "path", value)))
        report("NOTE PointInstancers (path, instances) %r, distinct .vdb filePaths %d %r"
               % (counts, len(files), sorted(files)))

    def steps():
        require(viewer.connectInput(0, merge), "connect the Viewer to the merge")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the stage (%r)" % (v.property("natronSceneError") if v else None,))
        v.activateWindow()
        v.setFocus()
        # The grid's ground axes cross at the framed origin, the volume's centre, and are drawn over it.
        if v.property("natronSceneShowGrid"):
            QTest.keyClick(v, Qt.Key_G, Qt.ControlModifier)
        ok = yield ("until", lambda: v.property("natronSceneShowGrid") is False, POLL_TIMEOUT_S)
        require(ok, "Ctrl+G hides the grid")
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

        section("volume drawn")
        # Storm's unlit-by-scene fog is close to the background grey; the gain brightens what Storm
        # rendered and leaves the background as it is.
        enable_gain.setValue(True)
        gain.setValue(VOLUME_GAIN_STOPS)
        yield settle()
        centre = median_around(save(v, "m417-volume-drawn"))
        read.getParam("grids").setValue("^*")
        ok = yield ("until", lambda: scene_drawn() and len(volume_guides()) == 0, POLL_TIMEOUT_S)
        check(ok, "no grid selected leaves no volume guide (%r)" % (list(volume_guides().keys()),))
        yield settle()
        empty = save(v, "m417-volume-empty")
        background = median_around(empty)
        report("NOTE centre with the volume %r, without %r" % (centre, background))
        check(differs(centre, background), "the volume changes the centre of the view")
        read.getParam("grids").setValue("*")
        ok = yield ("until", lambda: scene_drawn() and len(volume_guides()) == 1, POLL_TIMEOUT_S)
        check(ok, "selecting every grid again brings the volume back")
        gain.setValue(0.0)
        enable_gain.setValue(False)

        section("Storm render")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, render), "connect the Viewer to the USDRender")
        ok = yield ("until", lambda: any(w.isVisible() for w in widgets_of("ViewerGL")), POLL_TIMEOUT_S)
        require(ok, "the image viewer is up")
        # Without scene lights Storm's default volume only absorbs, so its colour is black over the
        # black background; the volume shows in the alpha Storm writes, 1 - transmittance.
        channels = viewer.getParam("displayChannels")
        require(channels is not None, "the Viewer has a displayChannels parameter")
        channels.set(DISPLAY_ALPHA)
        yield ("sleep", 3000)
        report("NOTE viewer messages %r" % (viewer_messages(),))
        gl = [w for w in widgets_of("ViewerGL") if w.isVisible()][0]
        image = save(gl, "m417-volume-render")
        mid = rgb(image, image.width() // 2, image.height() // 2)
        corner = rgb(image, image.width() // 2 + image.width() // 4, image.height() // 2 + image.height() // 3)
        report("NOTE render centre %r, off the volume %r" % (mid, corner))
        check(differs(mid, corner), "Storm draws the volume at the centre of the render")

        section("packed copies")
        enable_gain.setValue(True)
        gain.setValue(VOLUME_GAIN_STOPS)
        points = make("GeoPrimitive", 300, 0)
        points.getParam("type").setValue(1)
        points.getParam("gridSize").set(COPY_SPACING * COPY_COLUMNS, COPY_DEPTH)
        points.getParam("gridColumns").setValue(COPY_COLUMNS)
        points.getParam("gridRows").setValue(COPY_ROWS)
        copy = make("GeoCopyToPoints", 300, 120)
        require(copy.connectInput(0, read), "connect the volume to GeoCopyToPoints input 0")
        require(copy.connectInput(1, points), "connect the points to GeoCopyToPoints input 1")
        copy.getParam("packAndInstance").setValue(True)
        copies_usd = make("GeoToUSD", 300, 240)
        require(copies_usd.connectInput(0, copy), "connect the GeoCopyToPoints into a second GeoToUSD")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, copies_usd), "connect the Viewer to the copies' GeoToUSD")
        ok = yield ("until", lambda: any(w.isVisible() for w in widgets_of("SceneViewport")), POLL_TIMEOUT_S)
        require(ok, "the scene view is back")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the copies (%r)" % (v.property("natronSceneError") if v else None,))
        v.activateWindow()
        v.setFocus()
        if v.property("natronSceneShowGrid"):
            QTest.keyClick(v, Qt.Key_G, Qt.ControlModifier)
        QTest.keyClick(v, Qt.Key_A)
        yield settle()
        yield settle()
        report_stage(copies_usd)
        image = save(v, "m417-copies-storm")
        background = corner_background(image)
        fractions = coverage(image, background)
        report("NOTE Storm copies: background %r, coverage per third %r" % (background, fractions))
        for i, fraction in enumerate(fractions):
            check(fraction >= MIN_COVERAGE, "Storm draws a copy in third %d of the view (%.4f of its pixels)" % (i + 1, fraction))

        section("packed copies on Cycles")
        state = {}
        sub = yield from open_renderer_submenu(state)
        labels = [a.text().replace("&", "") for a in sub.actions() if not a.isSeparator()] if sub is not None else []
        cycles = [a for a in sub.actions() if a.text().replace("&", "").startswith("Cycles")] if sub is not None else []
        if not cycles or not cycles[0].isEnabled() or "unavailable" in cycles[0].text():
            yield from finish_menu(state)
            report("NOTE Cycles is not available in this build (%r); the Cycles coverage check is skipped" % (labels,))
            return
        cycles[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == CYCLES_ID, POLL_TIMEOUT_S)
        require(ok, "picking Cycles switches the view (%r)" % (prop("natronSceneRenderer"),))
        check(not prop("natronSceneRendererError"), "no renderer error on Cycles (%r)" % (prop("natronSceneRendererError"),))
        viewer.getParam("maxSamples").setValue(CYCLES_SAMPLES)
        ok = yield ("until", lambda: prop("natronSceneMaxSamples") == CYCLES_SAMPLES, POLL_TIMEOUT_S)
        check(ok, "Max Samples takes %d (%r)" % (CYCLES_SAMPLES, prop("natronSceneMaxSamples")))
        ok = yield ("until", lambda: (prop("natronSceneDrawnSamples") or 0) >= CYCLES_SAMPLES, CYCLES_TIMEOUT_S)
        check(ok, "Cycles draws %d samples (%r)" % (CYCLES_SAMPLES, prop("natronSceneDrawnSamples")))
        yield settle()
        image = save(view(), "m417-copies-cycles")
        background = corner_background(image)
        fractions = coverage(image, background)
        report("NOTE Cycles copies: background %r, coverage per third %r" % (background, fractions))
        for i, fraction in enumerate(fractions):
            check(fraction >= MIN_COVERAGE, "Cycles draws a copy in third %d of the view (%.4f of its pixels)" % (i + 1, fraction))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
