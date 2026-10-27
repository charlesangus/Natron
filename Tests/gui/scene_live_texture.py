"""A Constant feeding a USDMaterial's Diffuse Color input colours the card in the 3D view, live.

Graph: Constant1 (red) into USDMaterial1's Diffuse Color input, USDMaterial1 into USDCard1's
Material input, USDCard1 into Viewer1. Once the scene view shows the card, the centre of the view
must be red. Then the Constant's colour is set to green, and nothing else is touched: the view must
re-sync and show the card green, with no error, within the timeout.

Screenshots: scene_live_texture_red.png and scene_live_texture_green.png (full window), each with a
_viewer.png crop of the scene view.

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

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    constant = make("net.sf.openfx.ConstantPlugin", 0, 0)
    material = make("USDMaterial", 0, 100)
    card = make("USDCard", 0, 200)
    color = constant.getParam("color")
    require(color is not None, "Constant1 has a color parameter")
    color.setValue(1.0, 0.0, 0.0, 1.0)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def centre_colour():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        centre = v.mapToGlobal(v.rect().center())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        c = image.pixelColor(centre.x(), centre.y())
        return (c.red(), c.green(), c.blue())

    def dominated_by(channel):
        c = centre_colour()
        if c is None:
            return False
        others = [c[i] for i in range(3) if i != channel]
        return c[channel] > 40 and all(c[channel] > 2 * o for o in others)

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
        require(material.connectInput(0, constant), "connect Constant1 to USDMaterial1's Diffuse Color input")
        require(card.connectInput(0, material), "connect USDMaterial1 to USDCard1's Material input")
        require(viewer.connectInput(0, card), "connect the Viewer to USDCard1")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the card")
        ok = yield ("until", lambda: dominated_by(0), POLL_TIMEOUT_S)
        check(ok, "the card is red at the centre of the view (%r)" % (centre_colour(),))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_red")

        color.setValue(0.0, 1.0, 0.0, 1.0)
        ok = yield ("until", lambda: scene_drawn() and dominated_by(1), POLL_TIMEOUT_S)
        v = view()
        error = v.property("natronSceneError") if v is not None else None
        check(ok, "editing the Constant turns the card green with no viewport input (%r, error %r)" % (centre_colour(), error))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_green")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
