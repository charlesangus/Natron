"""M449 - USD Layer Save Paths: a USDWrite in Layered mode writes each named layer region to its own
file, nested by sublayers, with the region's metadata on it.

Fixture: Tests/fixtures/m52-layer-stack.usda (World/Cam, World/Asset/Geo).

Graph: USDRead(fixture) -> A -> B -> USDConfigureLayer(Preceding Layers, lighting.usda) ->
USDLayerBegin -> USDConfigureLayer(Following Layers, key/lights_key.usda) -> C -> USDLayerEnd -> D
-> USDWrite(shot.usda). A to D are USDSetAttributes, each setting its own float on
/World/Asset/Geo.

Checks:
 1. The write produces shot.usda, lighting.usda and key/lights_key.usda, and nothing else.
 2. shot.usda holds D and sublayers key/lights_key.usda, then lighting.usda, strongest first.
 3. lighting.usda holds A and B and sublayers the fixture, with the region's comment and custom
    data row. lights_key.usda holds C and has up axis Z.
 4. A USDRead of shot.usda draws without an error.

Screenshots: m449_readback_viewer.png (scene view crop).

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
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
    # A fresh directory per run, so that files an earlier run wrote are not counted.
    out_dir = os.path.join(OUT, "work", "m449-%d" % int(time.time() * 1000))
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    shot_path = os.path.join(out_dir, "shot.usda")
    lighting_path = os.path.join(out_dir, "lighting.usda")
    key_path = os.path.join(out_dir, "key", "lights_key.usda")

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

    def connect(node, input_node):
        require(node.connectInput(0, input_node), "connect %s to %s" % (input_node.getLabel(), node.getLabel()))

    # Every node below is held in a variable for the whole run, since PySide child wrappers die with
    # temporary parents.
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(400, 1000)

    y = [0]

    def chain(plugin_id, input_node):
        y[0] += 100
        node = make(plugin_id, 0, y[0])
        if input_node is not None:
            connect(node, input_node)
        return node

    def edit(input_node, name, value):
        node = chain("USDSetAttribute", input_node)
        param(node, "primPattern").setValue("/World/Asset/Geo")
        param(node, "attributeName").setValue("userProperties:" + name)
        param(node, "value").setValue(value)
        return node

    def configure(input_node, applies_to, save_path):
        node = chain("USDConfigureLayer", input_node)
        require(set_choice(param(node, "appliesTo"), applies_to), "Applies To offers " + applies_to)
        param(node, "savePath").setValue(save_path)
        return node

    read = make("USDRead", 0, 0)
    param(read, "filename").setValue(layer_path)
    a = edit(read, "a", 1.0)
    b = edit(a, "b", 2.0)
    lighting = configure(b, "preceding", "lighting.usda")
    param(lighting, "comment").setValue("lighting pass")
    param(lighting, "commentAuthored").setValue(True)
    begin = chain("USDLayerBegin", lighting)
    key = configure(begin, "following", "key/lights_key.usda")
    require(set_choice(param(key, "upAxis"), "Z"), "Up Axis offers Z")
    param(key, "upAxisAuthored").setValue(True)
    c = edit(key, "c", 3.0)
    end = chain("USDLayerEnd", c)
    d = edit(end, "d", 4.0)
    writer = chain("USDWrite", d)
    readback = make("USDRead", 200, y[0])

    # The custom data table has no Python parameter type; the row is set where one exists.
    custom_data = lighting.getParam("customData")
    if custom_data is not None and hasattr(custom_data, "setTable"):
        custom_data.setTable([["department", "String", "lighting"]])
    else:
        report("NOTE customData has no Python parameter: the custom data row is not checked")
        custom_data = None

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def save_view(name):
        v = view()
        if v is None:
            return
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        image = image.copy(origin.x(), origin.y(), v.width(), v.height())
        path = os.path.join(OUT, name + ".png")
        image.save(path)
        report("SHOT " + path)

    def show(node):
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to %s" % node.getLabel())

    def written_files():
        files = []
        for root, _dirs, names in os.walk(out_dir):
            for name in names:
                files.append(os.path.relpath(os.path.join(root, name), out_dir))
        return sorted(files)

    def sublayers(layer, path):
        base = os.path.dirname(path)
        return [os.path.normpath(os.path.join(base, p)) for p in layer.subLayerPaths] if layer else None

    def has_edit(layer, name):
        return layer is not None and bool(layer.GetAttributeAtPath("/World/Asset/Geo.userProperties:" + name))

    def steps():
        # 1. The file set.
        writer.getParam("filename").setValue(shot_path)
        require(set_choice(param(writer, "exportMode"), "layered"), "Export Mode offers layered")
        app.renderBlocking(writer, 1, 1)
        files = written_files()
        check(files == sorted(["shot.usda", "lighting.usda", os.path.join("key", "lights_key.usda")]),
              "the write produces shot.usda, lighting.usda and key/lights_key.usda (%r)" % (files,))
        shot = Sdf.Layer.FindOrOpen(shot_path)
        lighting_layer = Sdf.Layer.FindOrOpen(lighting_path)
        key_layer = Sdf.Layer.FindOrOpen(key_path)
        require(shot is not None and lighting_layer is not None and key_layer is not None, "the written files open")

        # 2. The root file.
        subs = sublayers(shot, shot_path)
        check(subs == [os.path.normpath(key_path), os.path.normpath(lighting_path)],
              "shot.usda sublayers the key file, then the lighting file (%r)" % (subs,))
        check(has_edit(shot, "d") and not has_edit(shot, "a") and not has_edit(shot, "c"), "shot.usda holds D alone")

        # 3. The region files.
        subs = sublayers(lighting_layer, lighting_path)
        check(subs == [os.path.normpath(layer_path)], "lighting.usda sublayers the fixture (%r)" % (subs,))
        check(has_edit(lighting_layer, "a") and has_edit(lighting_layer, "b") and not has_edit(lighting_layer, "c"),
              "lighting.usda holds A and B")
        check(lighting_layer.comment == "lighting pass", "lighting.usda has the region's comment (%r)" % lighting_layer.comment)
        if custom_data is not None:
            data = lighting_layer.customLayerData
            check(data.get("department") == "lighting", "lighting.usda has the custom data row (%r)" % (data,))
        check(has_edit(key_layer, "c") and not has_edit(key_layer, "b") and not has_edit(key_layer, "d"),
              "lights_key.usda holds C")
        check(list(key_layer.subLayerPaths) == [], "lights_key.usda has no sublayers")
        axis = key_layer.pseudoRoot.GetInfo("upAxis") if key_layer.pseudoRoot.HasInfo("upAxis") else None
        check(axis == "Z", "lights_key.usda has up axis Z (%r)" % (axis,))

        # 4. Read back.
        param(readback, "filename").setValue(shot_path)
        show(readback)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        check(ok, "a USDRead of shot.usda draws without an error")
        yield ("sleep", SETTLE_MS)
        save_view("m449_readback_viewer")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
