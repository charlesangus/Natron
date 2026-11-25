"""USDPrune's Prune Unselected and Mode change what the 3D viewer draws, each on its own.

Graph: USDRead of a stage the script writes to $NATRON_GUI_TEST_OUT/scene_prune_isolate.usda, into
USDPrune, into Viewer1, with the USDPrune's settings panel open. The stage holds three cubes side by
side, each its own colour: /World/A red, /World/B green and /World/C blue. Guides and the grid are
hidden and the view draws unshaded, so each cube's colour on screen says whether it is drawn.

With Prim Pattern /World/A, the script toggles Prune Unselected on, off and on in Deactivate, then
switches Mode to Hide and toggles it off and on again. After each knob change it waits, with no
refresh of its own, for the view to draw:
  - with Prune Unselected on, only /World/A, the one prim the pattern selects;
  - with it off, /World/B and /World/C: the pattern's prim is pruned, as without isolating;
and checks that the four Keep knobs are enabled, in the parameter and on the panel, only while
Prune Unselected is on. An empty pattern first draws all three cubes.

Screenshot for sign-off: scene-prune-isolate-panel, the settings panel and the view with Prune
Unselected on in Deactivate. Every other state is saved too, as scene-prune-isolate-<state>.png.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 700
READ_INTERVAL_MS = 200
HUD_MARGIN = 28
# Fewer pixels of a colour than this are noise: antialiasing, the HUD.
MIN_PIXELS = 40
MODE_DEACTIVATE = 0
MODE_HIDE = 1
NUKE_PRESET = 0
PATTERN = "/World/A"
KEEPS = ["keepLights", "keepCameras", "keepMaterials", "keepRenderSettings"]

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "A"
    {
        double size = 2
        double3 xformOp:translate = (-3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        color3f[] primvars:displayColor = [(0.9, 0.05, 0.05)]
    }

    def Cube "B"
    {
        double size = 2
        color3f[] primvars:displayColor = [(0.05, 0.9, 0.05)]
    }

    def Cube "C"
    {
        double size = 2
        double3 xformOp:translate = (3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        color3f[] primvars:displayColor = [(0.05, 0.05, 0.9)]
    }
}
"""

try:
    import NatronEngine
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import OUT, check, class_name, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(NUKE_PRESET)

    scene_path = os.path.join(OUT, "scene_prune_isolate.usda")
    with open(scene_path, "w") as f:
        f.write(STAGE)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    prune = app.createNode("USDPrune", -1, None, props)
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    pattern = prune.getParam("primPattern")
    mode = prune.getParam("mode")
    isolate = prune.getParam("pruneUnselected")
    require(pattern is not None, "USDPrune has a primPattern parameter")
    require(mode is not None, "USDPrune has a mode parameter")
    require(isolate is not None, "USDPrune has a pruneUnselected parameter")
    keeps = [prune.getParam(n) for n in KEEPS]
    require(all(p is not None for p in keeps), "USDPrune has the %s parameters" % ", ".join(KEEPS))
    require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    prune.setPosition(0, 100)
    viewer.setPosition(0, 200)

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def key(v, k, modifiers=Qt.NoModifier):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def prune_panel():
        label = prune.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                return hold(panel)
        return None

    def knob_label(panel, text):
        for w in panel.findChildren(QLabel):
            try:
                if class_name(w) == "ClickableLabel" and w.text().replace("&", "").strip() == text:
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def crop():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def red(c):
        return c.red() > c.green() + 60 and c.red() > c.blue() + 60

    def green(c):
        return c.green() > c.red() + 60 and c.green() > c.blue() + 60

    def blue(c):
        return c.blue() > c.red() + 60 and c.blue() > c.green() + 60

    CUBES = (("/World/A", red), ("/World/B", green), ("/World/C", blue))

    def counts(image, step=3):
        n = [0, 0, 0]
        if image is None:
            return n
        for y in range(HUD_MARGIN, image.height() - HUD_MARGIN, step):
            for x in range(0, image.width(), step):
                c = image.pixelColor(x, y)
                for i, (_, test) in enumerate(CUBES):
                    if test(c):
                        n[i] += 1
                        break
        return n

    def drawn_of(n):
        return [CUBES[i][0] for i in range(len(CUBES)) if n[i] >= MIN_PIXELS]

    def wait_drawn(expected, what):
        """Steps that read the view until it draws exactly expected on two reads in a row, or time
        out; the view is never asked to repaint."""
        deadline = time.time() + POLL_TIMEOUT_S
        last = None
        matched = 0
        while True:
            yield ("sleep", READ_INTERVAL_MS)
            if scene_drawn():
                last = counts(crop())
                matched = matched + 1 if drawn_of(last) == expected else 0
                if matched >= 2:
                    break
            if time.time() > deadline:
                break
        v = view()
        error = v.property("natronSceneError") if v is not None else None
        check(matched >= 2, "%s: the view draws %s (drawn %s; red, green, blue pixels %r; error %r)"
              % (what, " and ".join(expected) or "nothing", drawn_of(last) if last else None, last, error))

    def check_keeps(enabled, what):
        wrong = []
        panel = prune_panel()
        for p in keeps:
            if bool(p.getIsEnabled(0)) != enabled:
                wrong.append("%s parameter enabled=%r" % (p.getScriptName(), p.getIsEnabled(0)))
            w = knob_label(panel, p.getLabel()) if panel is not None else None
            if w is None:
                wrong.append("%s has no label on the panel" % p.getScriptName())
            elif bool(w.property("readOnly")) == enabled:
                wrong.append("%s label readOnly=%r" % (p.getScriptName(), w.property("readOnly")))
        check(not wrong, "%s: the Keep knobs are %s (%s)" % (what, "enabled" if enabled else "disabled", "; ".join(wrong)))

    def set_isolate(on, expected, what, slug):
        isolate.setValue(on)
        yield from wait_drawn(expected, what)
        check_keeps(on, what)
        shot("scene-prune-isolate-%s.png" % slug)

    def steps():
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        ok = yield ("until", lambda: prune_panel() is not None, POLL_TIMEOUT_S)
        require(ok, "the USDPrune's settings panel is open")
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        key(v, Qt.Key_G, CTRL | SHIFT)
        key(v, Qt.Key_G, CTRL)
        key(v, Qt.Key_6, CTRL)
        key(v, Qt.Key_A)
        yield ("sleep", SETTLE_MS)
        require(v.property("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")
        require(v.property("natronSceneShowGrid") is False, "Ctrl+G hides the grid")
        require(v.property("natronSceneDrawMode") == "unshaded",
                "Ctrl+6 draws unshaded (%r)" % v.property("natronSceneDrawMode"))

        section("setup")
        check(not isolate.getValue(), "Prune Unselected starts off")
        check(mode.getValue() == MODE_DEACTIVATE, "Mode starts at Deactivate")
        check(all(p.getValue() for p in keeps), "the Keep knobs start on")
        all_cubes = [c[0] for c in CUBES]
        yield from wait_drawn(all_cubes, "empty pattern")
        check_keeps(False, "empty pattern")
        shot("scene-prune-isolate-empty-pattern.png")
        pattern.setValue(PATTERN)
        yield from wait_drawn(["/World/B", "/World/C"], "pattern %s, Prune Unselected off" % PATTERN)
        shot("scene-prune-isolate-pruned.png")

        section("deactivate")
        yield from set_isolate(True, ["/World/A"], "Deactivate, Prune Unselected on", "panel")
        yield from set_isolate(False, ["/World/B", "/World/C"], "Deactivate, Prune Unselected off", "deactivate-off")
        yield from set_isolate(True, ["/World/A"], "Deactivate, Prune Unselected on again", "deactivate-on-again")

        section("hide")
        mode.setValue(MODE_HIDE)
        yield from wait_drawn(["/World/A"], "Hide, Prune Unselected on")
        check_keeps(True, "Hide, Prune Unselected on")
        shot("scene-prune-isolate-hide-on.png")
        yield from set_isolate(False, ["/World/B", "/World/C"], "Hide, Prune Unselected off", "hide-off")
        yield from set_isolate(True, ["/World/A"], "Hide, Prune Unselected on again", "hide-on-again")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
