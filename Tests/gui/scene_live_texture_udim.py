"""Every tile of a UDIM live texture shows in the 3D view, and an edit re-renders the tiles live.

Graph: Constant1 into MetadataModify1 (which declares the tiles 1001 and 1002) into
USDEmissiveMaterial1's Emissive input; USDRead1 reads a quad whose texture coordinates run 0 to 2
in u, USDMerge1 merges it with the material, Viewer1 shows the merge. The Constant's green is 1
only in tile 1001 and its red only in tile 1002 (expressions on udim.tile), so once the scene view
shows the quad its left half must be green and its right half red.

Then the red expression is changed to 0, and nothing else is touched: the right half must stop being
red, and the store's render count of each tile (the scene view's natronLiveTextureRenders property)
must have risen by the same amount, with no render of a single, non-UDIM texture. A Constant does not
tell tiles apart, so its edit moves every tile; that an edit one tile owns renders only that tile is
checked at the store, in Tests/Scene/LiveTexture_Test.cpp.

Screenshots: scene_live_texture_udim_before.png and scene_live_texture_udim_after.png (full window),
each with a _viewer.png crop of the scene view.

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

QUAD = """#usda 1.0
(
    upAxis = "Y"
    metersPerUnit = 1
)
def Mesh "Quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -0.5, 0), (1, -0.5, 0), (1, 0.5, 0), (-1, 0.5, 0)]
    texCoord2f[] primvars:st = [(0, 0), (2, 0), (2, 1), (0, 1)] (
        interpolation = "vertex"
    )
    uniform token subdivisionScheme = "none"
    rel material:binding = </Live>
}
"""

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    os.makedirs(OUT, exist_ok=True)
    quad_path = os.path.join(OUT, "scene_live_texture_udim_quad.usda")
    with open(quad_path, "w") as f:
        f.write(QUAD)

    constant = make("Constant", 0, 0)
    modify = make("MetadataModify", 0, 100)
    material = make("USDEmissiveMaterial", 0, 200)
    reader = make("USDRead", 200, 200)
    merge = make("USDMerge", 100, 300)
    require(constant.getParam("color").setExpression("1 if (udim.tile or 0) == 1002 else 0", False, 0),
            "Constant1 red reads the tile")
    require(constant.getParam("color").setExpression("1 if (udim.tile or 0) == 1001 else 0", False, 1),
            "Constant1 green reads the tile")
    for name, value in (("operation1", 1), ("type1", 2)):
        modify.getParam(name).set(value)
    modify.getParam("key1").set("natron/udimTiles")
    modify.getParam("stringValue1").set("1001,1002")
    reader.getParam("filename").set(quad_path)
    material.getParam("primPath").set("/Live")
    use_alpha = material.getParam("useAlpha")
    if use_alpha is not None:
        use_alpha.set(False)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(100, 400)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def tile_renders():
        v = view()
        counts = v.property("natronLiveTextureRenders") if v is not None else None
        return dict(counts) if counts else {}

    def dominant(c, channel):
        others = [c[i] for i in range(3) if i != channel]
        return c[channel] > 80 and all(c[channel] > 2 * o for o in others)

    def halves():
        """Mean x of the green and of the red pixels, and how many of each, along a row through the
        view's centre; the row sits off the centre line, where the world axes would cross the quad."""
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        y = top_left.y() + v.height() // 2 - 12
        found = {0: [], 1: []}
        for x in range(top_left.x(), top_left.x() + v.width()):
            c = image.pixelColor(x, y)
            rgb = (c.red(), c.green(), c.blue())
            for channel in (0, 1):
                if dominant(rgb, channel):
                    found[channel].append(x)
        mean = lambda xs: sum(xs) / len(xs) if xs else None
        return {"red": (len(found[0]), mean(found[0])), "green": (len(found[1]), mean(found[1]))}

    def both_tiles_show():
        h = halves()
        return (h is not None and h["red"][0] >= 20 and h["green"][0] >= 20 and h["green"][1] < h["red"][1])

    def red_gone():
        h = halves()
        return h is not None and h["red"][0] < 5 and h["green"][0] >= 20

    def save_shots(name):
        shot(name + ".png")
        v = view()
        if v is None:
            return
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        path = os.path.join(OUT, name + "_viewer.png")
        image.copy(top_left.x(), top_left.y(), v.width(), v.height()).save(path)
        report("SHOT " + path)

    def steps():
        require(modify.connectInput(0, constant), "connect Constant1 to MetadataModify1")
        require(material.connectInput(0, modify), "connect MetadataModify1 to USDEmissiveMaterial1's Emissive input")
        require(merge.connectInput(0, reader), "connect USDRead1 to USDMerge1")
        require(merge.connectInput(1, material), "connect USDEmissiveMaterial1 to USDMerge1")
        require(viewer.connectInput(0, merge), "connect the Viewer to USDMerge1")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the quad")
        ok = yield ("until", both_tiles_show, POLL_TIMEOUT_S)
        check(ok, "the quad is green over tile 1001 and red over tile 1002 (%r)" % (halves(),))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_udim_before")
        before = tile_renders()
        check(before.get("1001", 0) >= 1 and before.get("1002", 0) >= 1, "each tile was rendered (%r)" % (before,))
        check(before.get("0", 0) == 0, "no single texture was rendered for the set (%r)" % (before,))

        require(constant.getParam("color").setExpression("0", False, 0), "Constant1 red is set to 0")
        ok = yield ("until", lambda: scene_drawn() and red_gone(), POLL_TIMEOUT_S)
        check(ok, "editing the Constant removes the red of tile 1002 with no viewport input (%r)" % (halves(),))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_udim_after")
        after = tile_renders()
        added = {t: after.get(t, 0) - before.get(t, 0) for t in ("1001", "1002")}
        check(added["1001"] >= 1 and added["1001"] == added["1002"],
              "the edit re-rendered each tile the same number of times (%r)" % (added,))
        check(after.get("0", 0) == 0, "still no single texture rendered for the set (%r)" % (after,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
