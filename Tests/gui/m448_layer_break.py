"""M448 - USD Layer Break: a USDLayerBreak leaves the viewer's scene unchanged, and a USDWrite after it
writes only the edits made after the break.

Fixture: Tests/fixtures/m52-layer-stack.usda (World/Cam, World/Asset/Geo).

Graph: USDRead(fixture) -> USDLayerBreak -> USDPrune(/World/Cam, deactivate) -> USDWrite.

Checks:
 1. The viewer draws the break's scene without an error, as it draws the USDRead's, and the prune
    after it.
 2. A layered write with Strip Layers Above Breaks on (the default) holds no sublayers, an over that
    deactivates /World/Cam, and no spec for the fixture's /World/Asset. A USDRead of it draws.
 3. A flattened write holds the same over and no /World/Asset.
 4. With the toggle off, the layered write sublayers the fixture and the flattened one defines
    /World/Asset/Geo.

Screenshots: m448_break_viewer.png, m448_readback_viewer.png (scene view crops).

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 40.0
SETTLE_MS = 800
STARTUP_TIMEOUT_S = 60.0

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication
    from pxr import Sdf

    from guitest import FIXTURES, OUT, check, finish, plugin_problem, report, require, run, widgets_of

    layer_path = os.path.join(FIXTURES, "m52-layer-stack.usda")
    require(os.path.isfile(layer_path), "fixture exists: " + layer_path)
    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    def set_choice(p, entry_id):
        for i in range(p.getNumOptions()):
            if p.getOption(i) == entry_id:
                p.setValue(i)
                return True
        return False

    # Every node below is held in a variable for the whole run, since PySide child wrappers die with
    # temporary parents.
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 500)

    read = make("USDRead", 0, 0)
    param(read, "filename").setValue(layer_path)
    brk = make("USDLayerBreak", 0, 100)
    prune = make("USDPrune", 0, 200)
    writer = make("USDWrite", 0, 300)
    readback = make("USDRead", 200, 300)
    require(brk.connectInput(0, read), "connect USDRead1 to the USDLayerBreak")
    require(prune.connectInput(0, brk), "connect the USDLayerBreak to the USDPrune")
    require(writer.connectInput(0, prune), "connect the USDPrune to the USDWrite")
    param(prune, "primPattern").setValue("/World/Cam")
    require(set_choice(param(prune, "mode"), "deactivate"), "the USDPrune Mode menu offers deactivate")
    strip = param(writer, "stripLayerBreaks")
    counter = [0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

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

    def differing_pixels(a, b, step=4):
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), step):
            for x in range(0, a.width(), step):
                pa, pb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(pa.red() - pb.red()) + abs(pa.green() - pb.green()) + abs(pa.blue() - pb.blue()) > 30:
                    count += 1
        return count

    def save_view(name):
        image = grab_view()
        if image is not None:
            path = os.path.join(OUT, name + ".png")
            image.save(path)
            report("SHOT " + path)
        return image

    def show(node):
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to %s" % node.getLabel())

    def write(mode, stripped):
        """Writes the prune through the USDWrite (0 layered, 1 flattened) to a fresh directory and
        returns the written layer."""
        counter[0] += 1
        out_dir = os.path.join(work_dir, "write%d" % counter[0])
        if not os.path.isdir(out_dir):
            os.makedirs(out_dir)
        path = os.path.join(out_dir, "lighting.usda")
        writer.getParam("filename").setValue(path)
        writer.getParam("exportMode").setValue(mode)
        strip.setValue(stripped)
        app.renderBlocking(writer, 1, 1)
        require(os.path.isfile(path), "the USDWrite wrote " + path)
        return path, Sdf.Layer.FindOrOpen(path)

    def holds_only_the_prune(layer, label):
        cam = layer.GetPrimAtPath("/World/Cam") if layer else None
        check(bool(cam) and cam.specifier == Sdf.SpecifierOver and cam.HasInfo("active") and not cam.active,
              "%s: an over deactivates /World/Cam" % label)
        check(layer is not None and not layer.GetPrimAtPath("/World/Asset"),
              "%s: no spec for the fixture's /World/Asset" % label)

    def sublayers(layer, path):
        base = os.path.dirname(path)
        return [os.path.normpath(os.path.join(base, p)) for p in layer.subLayerPaths] if layer else None

    def steps():
        # 1. The viewer.
        show(read)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the USDRead", repr(v.property("natronSceneError")) if v else "no scene view")
        yield ("sleep", SETTLE_MS)
        unbroken = grab_view()
        show(brk)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the scene view shows the USDLayerBreak without an error")
        yield ("sleep", SETTLE_MS)
        broken = save_view("m448_break_viewer")
        diff = differing_pixels(unbroken, broken)
        check(0 <= diff < 5, "the break's scene draws as the USDRead's (%d differing samples)" % diff)
        show(prune)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the scene view shows the USDPrune after the break without an error")

        # 2. Layered, stripped.
        check(strip.getValue(), "Strip Layers Above Breaks is on by default")
        path, layer = write(0, True)
        check(layer is not None and list(layer.subLayerPaths) == [],
              "layered: no sublayers (%r)" % (list(layer.subLayerPaths) if layer else None,))
        holds_only_the_prune(layer, "layered")
        param(readback, "filename").setValue(path)
        show(readback)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "a USDRead of the layered write draws without an error")
        yield ("sleep", SETTLE_MS)
        save_view("m448_readback_viewer")

        # 3. Flattened, stripped.
        path, layer = write(1, True)
        holds_only_the_prune(layer, "flattened")

        # 4. The toggle off.
        path, layer = write(0, False)
        subs = sublayers(layer, path)
        check(subs is not None and os.path.normpath(layer_path) in subs,
              "layered, toggle off: the fixture is sublayered (%r)" % (subs,))
        path, layer = write(1, False)
        check(layer is not None and bool(layer.GetPrimAtPath("/World/Asset/Geo")),
              "flattened, toggle off: /World/Asset/Geo is baked in")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
