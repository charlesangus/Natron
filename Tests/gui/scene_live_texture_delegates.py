"""
A Natron image on a card in the scene view's CPU renderers: an edit upstream of the material reaches
the delegate, which reads the new image and starts refining again.

Constant1 feeds USDMaterial1's Diffuse Color input, USDMaterial1 feeds USDCard1's Material input, and
USDCard1 and a USDLight1 are merged into Viewer1. For Cycles and then MoonRay: pick the renderer from
the view's context menu, see the card red at the centre of the view, set the Constant to green and
touch nothing else. The view must show the card green, with no error, within the timeout, and its
restart count must have moved. A renderer this build lacks is reported as SKIP.

The project format is small: every image lives in /dev/shm while a delegate may read it, and the 64 MB
of a container's default holds only one 1920x1080 image with its mips.

Screenshots, one file each, with a _viewer.png crop of the scene view beside each:
scene_live_texture_<renderer>_red.png and scene_live_texture_<renderer>_green.png.

Run through Tests/gui/run-gui-test.sh, at 1920x1080. Screenshots and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 90.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
RENDERERS = (("Cycles", "cycles", "HdCyclesPlugin"), ("MoonRay", "moonray", "HdMoonrayRendererPlugin"))

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    app.addFormat("liveTextureCard 256x256")
    project_format = app.getProjectParam("outputFormat")
    format_options = list(project_format.getOptions())
    project_format.setValue(next((i for i, o in enumerate(format_options) if "liveTextureCard" in str(o)), len(format_options) - 1))

    constant = make("Constant", 0, 0)
    material = make("USDMaterial", 0, 100)
    card = make("USDCard", 0, 200)
    light = make("USDLight", 200, 200)
    merge = make("USDMerge", 0, 300)
    color = constant.getParam("color")
    require(color is not None, "Constant1 has a color parameter")
    color.set(1.0, 0.0, 0.0, 1.0)
    intensity = light.getParam("intensity")
    require(intensity is not None, "USDLight1 has an intensity parameter")
    intensity.set(3.0)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 400)

    # A child widget's wrapper dies with its parent's, and a menu's with the event that made it.
    _held = []

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def centre_samples():
        """Colours on a 7x7 grid 48 px across around the view centre, so one stray overlay line does
        not decide the colour."""
        v = view()
        if v is None:
            return []
        QApplication.processEvents()
        centre = v.mapToGlobal(v.rect().center())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        samples = []
        for dy in range(-24, 25, 8):
            for dx in range(-24, 25, 8):
                c = image.pixelColor(centre.x() + dx, centre.y() + dy)
                samples.append((c.red(), c.green(), c.blue()))
        return samples

    def centre_colour():
        samples = centre_samples()
        if not samples:
            return None
        return tuple(sorted(c[i] for c in samples)[len(samples) // 2] for i in range(3))

    def dominated_by(channel):
        samples = centre_samples()
        if not samples:
            return False
        hits = 0
        for c in samples:
            others = [c[i] for i in range(3) if i != channel]
            if c[channel] > 40 and all(c[channel] > 2 * o for o in others):
                hits += 1
        return hits * 4 >= len(samples) * 3

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

    def activate_main_window():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            guis[0].activateWindow()
            guis[0].raise_()
        QApplication.processEvents()

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

    def open_renderer_submenu(state):
        activate_main_window()
        post_context_menu(view(), state)
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
        for _ in range(4):
            p = popup()
            if p is None:
                break
            QTest.keyClick(p, Qt.Key_Escape)
            QApplication.processEvents()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def exercise(label, slug, renderer_id):
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the context menu has a Renderer submenu")
        found = [a for a in sub.actions() if a.text().replace("&", "").lower().startswith(slug)]
        if not found or not found[0].isEnabled() or "unavailable" in found[0].text():
            yield from finish_menu(state)
            report("SKIP %s is not available in this build" % label)
            return
        found[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == renderer_id, POLL_TIMEOUT_S)
        require(ok, "picking %s switches the view (%r)" % (label, prop("natronSceneRenderer")))
        check(not prop("natronSceneRendererError"), "no renderer error on %s (%r)" % (label, prop("natronSceneRendererError")))

        color.set(1.0, 0.0, 0.0, 1.0)
        ok = yield ("until", lambda: scene_drawn() and dominated_by(0), POLL_TIMEOUT_S)
        check(ok, "%s shows the card red (%r, error %r)" % (label, centre_colour(), prop("natronSceneError")))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_%s_red" % slug)

        restarts = prop("natronSceneRestartCount")
        color.set(0.0, 1.0, 0.0, 1.0)
        ok = yield ("until", lambda: scene_drawn() and dominated_by(1), POLL_TIMEOUT_S)
        check(ok, "editing the Constant turns the card green in %s with no viewport input (%r, error %r, material %r)"
              % (label, centre_colour(), prop("natronSceneError"), material.getPersistentMessage()))
        check(prop("natronSceneRestartCount") > restarts,
              "the edit restarted %s's refinement (%r, was %r)" % (label, prop("natronSceneRestartCount"), restarts))
        yield ("sleep", SETTLE_MS)
        save_shots("scene_live_texture_%s_green" % slug)

    def steps():
        require(material.connectInput(0, constant), "connect Constant1 to USDMaterial1's Diffuse Color input")
        require(card.connectInput(0, material), "connect USDMaterial1 to USDCard1's Material input")
        require(merge.connectInput(0, card), "connect USDCard1 to USDMerge1")
        require(merge.connectInput(1, light), "connect USDLight1 to USDMerge1")
        require(viewer.connectInput(0, merge), "connect the Viewer to USDMerge1")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the card")
        for label, slug, renderer_id in RENDERERS:
            yield from exercise(label, slug, renderer_id)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
