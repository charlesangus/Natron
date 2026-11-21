"""A progressive delegate's frames get the Viewer's gain, view and lighting mode, as Storm's do.

Graph: USDRead of a stage the script writes into Viewer1, drawn with Cycles (picked from the view's
Renderer submenu; the script reports SKIP and passes when this build has no Cycles). Guides and the
grid are hidden. Max Samples is low so each render converges quickly.

Stages: an emissive card (no lights), read once at an emission of 0.3 for reference and then at
0.15; a grey diffuse card under a dim distant light. The script checks:
  - at gain 2 (the gain knob at 1 f-stop) the 0.15 card shows as the 0.3 card did at gain 1, so the
    frame is graded in scene-linear, and the four background corners keep their colour;
  - another of the Viewer's views, when the OpenColorIO config offers one that changes the card,
    changes its pixel;
  - neither the gain nor the view change restarts the render (natronSceneRestartCount);
  - on the lit card, the Headlight lighting mode restarts the render, changes the card's pixel and
    shows as natronSceneLighting "headlight"; Auto, the stage having a light, renders as Scene
    Lights without a restart; Scene Lights again brings back the first pixel.

Screenshots for sign-off, each with a CAPTION line in results.txt: m365-reference, m365-plain,
m365-gain-2, m365-view, m365-scene-lights, m365-headlight, m365-auto.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
REFINE_TIMEOUT_S = 180.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
CYCLES_ID = "HdCyclesPlugin"
MAX_SAMPLES = 16
HUD_MARGIN = 28
# Screen pixels are 8-bit, and a converged path-traced frame keeps a little noise.
COLOUR_SLACK = 6
BACKGROUND_SLACK = 2
CARD_DIFFERENCE = 12
CORNER_INSET = 12
MIN_PIXELS = 40
GAIN_STEPS = (0.25, 0.5, 0.75, 1.0)
LIGHTING_AUTO, LIGHTING_SCENE, LIGHTING_HEADLIGHT = 0, 1, 2

EMISSIVE_STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Card" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        rel material:binding = </World/Looks/Glow>
    }

    def Scope "Looks"
    {
        def Material "Glow"
        {
            token outputs:surface.connect = </World/Looks/Glow/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0, 0, 0)
                float inputs:roughness = 1
                color3f inputs:emissiveColor = (%(grey)s, %(grey)s, %(grey)s)
                token outputs:surface
            }
        }
    }
}
"""

LIT_STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Card" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        rel material:binding = </World/Looks/Grey>
    }

    def Scope "Looks"
    {
        def Material "Grey"
        {
            token outputs:surface.connect = </World/Looks/Grey/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
                float inputs:roughness = 1
                token outputs:surface
            }
        }
    }

    def DistantLight "Sun"
    {
        float inputs:intensity = 0.25
        float3 xformOp:rotateXYZ = (20, 20, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
}
"""

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    import NatronEngine
    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    def write_stage(name, text):
        path = os.path.join(OUT, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    reference_path = write_stage("m365-reference.usda", EMISSIVE_STAGE % {"grey": "0.3"})
    card_path = write_stage("m365-card.usda", EMISSIVE_STAGE % {"grey": "0.15"})
    lit_path = write_stage("m365-lit.usda", LIT_STAGE)

    lighting = NatronEngine.natron.getSettings().getParam("viewer3DLighting")
    require(lighting is not None, "Preferences have a viewer3DLighting parameter")
    original_lighting = lighting.getValue()
    lighting.setValue(LIGHTING_SCENE)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(reference_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 150)
    knobs = {}
    for name in ("enableGain", "gain", "enableGamma", "gamma", "view", "maxSamples"):
        knobs[name] = viewer.getParam(name)
        require(knobs[name] is not None, "the Viewer has a %s parameter" % name)

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier
    _held = []

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        return bool(prop("natronSceneSynced")) and bool(prop("natronSceneShown")) and not prop("natronSceneError")

    def restarts():
        return int(prop("natronSceneRestartCount", 0) or 0)

    def key(v, k, modifiers=Qt.NoModifier):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def painted_since(t0_ms):
        return lambda: (prop("natronSceneLastPaintMs", 0) or 0) >= t0_ms

    def repaint():
        t0 = time.time() * 1000.0
        v = view()
        if v is not None:
            v.update()
        ok = yield ("until", painted_since(t0), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        return ok

    def converged_after(restart):
        """Steps that wait for a render after restart number restart to converge and be drawn."""
        ok = yield ("until", lambda: restarts() > restart and bool(prop("natronSceneRefineConverged"))
                    and (prop("natronSceneDrawnSamples", -1) or -1) >= 1, REFINE_TIMEOUT_S)
        yield from repaint()
        return ok

    def load(path, what):
        before = restarts()
        t0 = time.time() * 1000.0
        usd.getParam("filename").setValue(path)
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) or 0) >= t0 and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the view syncs " + what)
        ok = yield from converged_after(before)
        require(ok, "Cycles converges on %s (restarts %r, samples %r)" % (what, restarts(), prop("natronSceneSamples")))

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

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

    def pick_cycles():
        """Steps that pick Cycles from the Renderer submenu; return False when the build has none."""
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()
        state = {}
        post_context_menu(view(), state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        require(ok, "the view's context menu opens")
        menu = popup()
        _held.append(menu)
        found = [a for a in menu.actions() if a.text().replace("&", "") == "Renderer" and a.menu() is not None]
        require(bool(found), "the context menu has a Renderer submenu")
        action = found[0]
        _held.append(action)
        sub = action.menu()
        _held.append(sub)
        menu.setActiveAction(action)
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        cycles = [a for a in sub.actions() if a.text().replace("&", "").startswith("Cycles")]
        usable = bool(cycles) and cycles[0].isEnabled() and "unavailable" not in cycles[0].text()
        if usable:
            cycles[0].trigger()
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        return usable

    def crop():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def capture(name, caption):
        shot(name + ".png")
        image = crop()
        if image is not None:
            path = os.path.join(OUT, name + "_view.png")
            image.save(path)
            report("SHOT " + path)
        report("CAPTION %s: %s" % (name, caption))
        return image

    def rgb(image, x, y):
        c = image.pixelColor(x, y)
        return (c.red(), c.green(), c.blue())

    def corners(image):
        w, h = image.width(), image.height()
        top, bottom = HUD_MARGIN + CORNER_INSET, h - HUD_MARGIN - CORNER_INSET
        return [(CORNER_INSET, top), (w - 1 - CORNER_INSET, top), (CORNER_INSET, bottom), (w - 1 - CORNER_INSET, bottom)]

    def differs(a, b, slack):
        return any(abs(p - q) > slack for p, q in zip(a, b))

    def card_centre(image, background):
        xs, ys = 0, 0
        n = 0
        for y in range(HUD_MARGIN, image.height() - HUD_MARGIN, 2):
            for x in range(0, image.width(), 2):
                if differs(rgb(image, x, y), background, CARD_DIFFERENCE):
                    xs += x
                    ys += y
                    n += 1
        if n == 0:
            return None, 0
        return (xs // n, ys // n), n

    def mean_around(image, centre, radius=3):
        total = [0, 0, 0]
        n = 0
        for y in range(centre[1] - radius, centre[1] + radius + 1):
            for x in range(centre[0] - radius, centre[0] + radius + 1):
                for c, value in enumerate(rgb(image, x, y)):
                    total[c] += value
                n += 1
        return tuple(t / float(n) for t in total)

    def check_corners(before, after, what):
        for x, y in corners(before):
            a, b = rgb(before, x, y), rgb(after, x, y)
            check(not differs(a, b, BACKGROUND_SLACK), "%s leaves the background at (%d, %d) as it was (%r -> %r)"
                  % (what, x, y, a, b))

    def set_gain(stops):
        knobs["enableGain"].setValue(stops != 0.0)
        knobs["gain"].setValue(stops)
        QApplication.processEvents()

    def set_lighting(index):
        lighting.setValue(index)
        QApplication.processEvents()

    def steps():
        set_gain(0.0)
        knobs["enableGamma"].setValue(False)
        knobs["maxSamples"].setValue(MAX_SAMPLES)
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        key(v, Qt.Key_G, CTRL | SHIFT)
        yield ("sleep", SETTLE_MS)
        require(prop("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")
        key(v, Qt.Key_G, CTRL)
        yield ("sleep", SETTLE_MS)
        require(prop("natronSceneShowGrid") is False, "Ctrl+G hides the grid")
        key(v, Qt.Key_A)
        yield from repaint()

        before = restarts()
        usable = yield from pick_cycles()
        if not usable:
            report("SKIP Cycles is not available in this build")
            return
        ok = yield ("until", lambda: prop("natronSceneRenderer") == CYCLES_ID, POLL_TIMEOUT_S)
        require(ok, "picking Cycles switches the view (%r)" % (prop("natronSceneRenderer"),))
        ok = yield from converged_after(before - 1)
        require(ok, "Cycles converges on the 0.3 card (samples %r)" % (prop("natronSceneSamples"),))

        section("reference")
        reference = capture("m365-reference", "Cycles, the card emitting 0.3 at gain 1: what gain 2 must make of the 0.15 card")
        require(reference is not None, "the scene view can be captured")
        background = rgb(reference, *corners(reference)[0])
        for x, y in corners(reference)[1:]:
            require(not differs(rgb(reference, x, y), background, BACKGROUND_SLACK),
                    "the corners show the background (%r at (%d, %d), %r at the first)" % (rgb(reference, x, y), x, y, background))
        centre, card_pixels = card_centre(reference, background)
        require(centre is not None and card_pixels >= MIN_PIXELS, "the card is drawn (%d pixels)" % card_pixels)
        twice = mean_around(reference, centre)
        report("NOTE background %r, 0.3 card %r at %r" % (background, twice, centre))

        yield from load(card_path, "the 0.15 card")
        plain = capture("m365-plain", "Cycles, the card emitting 0.15 at gain 1")
        once = mean_around(plain, centre)
        require(differs(once, twice, COLOUR_SLACK * 2), "the 0.15 card shows darker than the 0.3 one (%r vs %r)" % (once, twice))
        check_corners(reference, plain, "changing the card's emission")

        section("gain")
        settled = restarts()
        for stops in GAIN_STEPS:
            set_gain(stops)
            yield from repaint()
        gained = capture("m365-gain-2", "Cycles at gain 2 (1 f-stop): the 0.15 card matches the 0.3 reference, the background is unchanged")
        doubled = mean_around(gained, centre)
        check(not differs(doubled, twice, COLOUR_SLACK), "gain 2 shows the 0.15 card as the 0.3 one at gain 1 (%r vs %r)"
              % (doubled, twice))
        check_corners(plain, gained, "gain 2")
        check(restarts() == settled, "gain restarts no render (%d -> %d)" % (settled, restarts()))
        set_gain(0.0)
        yield from repaint()

        section("view")
        view_knob = knobs["view"]
        original_view = view_knob.get()
        options = [i for i in range(view_knob.getNumOptions())
                   if i != original_view and str(view_knob.getOption(i)) not in ("projectDefault", "none")]
        report("NOTE views %r, the first is %r" % ([str(view_knob.getOption(i)) for i in range(view_knob.getNumOptions())],
                                                   str(view_knob.getOption(original_view))))
        changed = None
        for index in options:
            view_knob.set(index)
            QApplication.processEvents()
            yield from repaint()
            image = crop()
            if image is not None and differs(mean_around(image, centre), once, COLOUR_SLACK * 2):
                changed = index
                break
        if changed is None:
            check(False, "some other view of this config changes the card (%d tried)" % len(options))
        else:
            shown = capture("m365-view", "Cycles in the %s view: the card's pixel changes" % view_knob.getOption(changed))
            check(differs(mean_around(shown, centre), once, COLOUR_SLACK * 2), "the %s view changes the card (%r -> %r)"
                  % (view_knob.getOption(changed), once, mean_around(shown, centre)))
        check(restarts() == settled, "changing the view restarts no render (%d -> %d)" % (settled, restarts()))
        view_knob.set(original_view)
        QApplication.processEvents()
        yield from repaint()

        section("lighting")
        yield from load(lit_path, "the lit card")
        check(prop("natronSceneLighting") == "scene", "the view lights with the scene's lights (%r)" % (prop("natronSceneLighting"),))
        scene_lit_image = capture("m365-scene-lights", "Cycles, Scene Lights: the grey card under the stage's dim distant light")
        scene_lit = mean_around(scene_lit_image, centre)

        before = restarts()
        set_lighting(LIGHTING_HEADLIGHT)
        ok = yield ("until", lambda: prop("natronSceneLighting") == "headlight", POLL_TIMEOUT_S)
        check(ok, "the view switches to the headlight (%r)" % (prop("natronSceneLighting"),))
        ok = yield from converged_after(before)
        check(ok, "the headlight restarts the render (%d -> %d)" % (before, restarts()))
        headlit_image = capture("m365-headlight", "Cycles, Headlight: the stage's light off, a light at the camera on")
        headlit = mean_around(headlit_image, centre)
        check(differs(headlit, scene_lit, COLOUR_SLACK * 2), "the headlight changes the lit card (%r -> %r)" % (scene_lit, headlit))

        before = restarts()
        set_lighting(LIGHTING_SCENE)
        ok = yield ("until", lambda: prop("natronSceneLighting") == "scene", POLL_TIMEOUT_S)
        check(ok, "the view switches back to the scene's lights (%r)" % (prop("natronSceneLighting"),))
        ok = yield from converged_after(before)
        check(ok, "Scene Lights restarts the render (%d -> %d)" % (before, restarts()))
        again = mean_around(crop(), centre)
        check(not differs(again, scene_lit, COLOUR_SLACK), "Scene Lights brings back the first pixel (%r vs %r)" % (again, scene_lit))

        before = restarts()
        set_lighting(LIGHTING_AUTO)
        ok = yield ("until", lambda: prop("natronSceneLighting") == "auto", POLL_TIMEOUT_S)
        check(ok, "the view switches to auto (%r)" % (prop("natronSceneLighting"),))
        yield ("sleep", SETTLE_MS * 3)
        yield from repaint()
        auto_image = capture("m365-auto", "Cycles, Auto on a stage with a light: as Scene Lights")
        auto = mean_around(auto_image, centre)
        check(restarts() == before, "auto on a stage with a light restarts no render (%d -> %d)" % (before, restarts()))
        check(not differs(auto, scene_lit, COLOUR_SLACK), "auto renders as Scene Lights (%r vs %r)" % (auto, scene_lit))

    def all_steps():
        try:
            yield from steps()
        finally:
            lighting.setValue(original_lighting)

    run(all_steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
