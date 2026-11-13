"""The Viewer's gain and gamma act on what the 3D view renders, not on its background or grid.

Graph: USDRead of a stage the script writes (one unshaded grey cube) into Viewer1. Guides and the
grid are hidden and the view draws unshaded, so the cube shows its display colour flat. The cube is
read once at a display colour of 0.3 for reference, then at 0.15; every later check is on the 0.15
cube. The script checks:
  - at gain 2 (the gain knob at 1 f-stop, set in steps as a slider drag sets it) the cube shows as
    the 0.3 cube did at gain 1, so gain doubles it in scene-linear before the display transform,
    and the four background corners keep their gain 1 colour;
  - the gain steps cause no scene sync (natronSceneSyncRequests) and make no engine
    (natronSceneEngineCount);
  - at gamma 2 the cube's screen value v becomes 255 (v / 255)^(1/2), and the corners keep their
    colour;
  - with the grid shown, gain 2 changes no more pixels than it does without the grid, and the grid
    does show;
  - a half-transparent 0.15 cube (displayOpacity 0.5, so seen through both its faces) at gain 2
    shows as a half-transparent 0.3 cube does at gain 1, over the background by an alpha read off
    the 0.3 cube; at gamma 2 its colour is graded unpremultiplied and laid over the ungraded
    background by that alpha, which grading the premultiplied colour would not give.

Progressive delegates (Cycles) are not checked: their frames reach the view already encoded, and
their grading belongs to the delegate display transform work. Looks and views that lift black are
checked in SceneViewport_Test, which builds its own OpenColorIO config.

Screenshots for sign-off, each with a CAPTION line in results.txt: scene-gain-reference,
scene-gain-plain, scene-gain-2, scene-gain-gamma-2, scene-gain-grid-plain, scene-gain-grid-2,
scene-gain-translucent-reference, scene-gain-translucent-2, scene-gain-translucent-gamma-2.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
HUD_MARGIN = 28
# Screen pixels are 8-bit.
COLOUR_SLACK = 5
BACKGROUND_SLACK = 2
# A pixel differing from the background by more than this belongs to the cube.
CUBE_DIFFERENCE = 12
CORNER_INSET = 12
# Pixels of a colour below this count are noise (antialiasing, the HUD).
MIN_PIXELS = 40
GAIN_STEPS = (0.25, 0.5, 0.75, 1.0)

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "Card"
    {
        double size = 2
        color3f[] primvars:displayColor = [(%(grey)s, %(grey)s, %(grey)s)]
%(opacity)s    }
}
"""

try:
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    def write_stage(name, grey, opacity=None):
        path = os.path.join(OUT, name)
        authored = "        float[] primvars:displayOpacity = [%s]\n" % opacity if opacity else ""
        with open(path, "w") as f:
            f.write(STAGE % {"grey": grey, "opacity": authored})
        return path

    reference_path = write_stage("scene-gain-reference.usda", "0.3")
    card_path = write_stage("scene-gain-card.usda", "0.15")
    translucent_reference_path = write_stage("scene-gain-translucent-reference.usda", "0.3", "0.5")
    translucent_card_path = write_stage("scene-gain-translucent-card.usda", "0.15", "0.5")

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(reference_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 150)
    knobs = {}
    for name in ("enableGain", "gain", "enableGamma", "gamma"):
        knobs[name] = viewer.getParam(name)
        require(knobs[name] is not None, "the Viewer has a %s parameter" % name)

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        return bool(prop("natronSceneSynced")) and bool(prop("natronSceneShown")) and not prop("natronSceneError")

    def key(v, k, modifiers=Qt.NoModifier):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def painted_since(t0_ms):
        return lambda: (prop("natronSceneLastPaintMs", 0) or 0) >= t0_ms

    def repaint():
        """Asks for a paint and waits for it, then for the screen to settle."""
        t0 = time.time() * 1000.0
        v = view()
        if v is not None:
            v.update()
        ok = yield ("until", painted_since(t0), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        return ok

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

    def cube_centre(image, background):
        """The centroid of the pixels that differ from the background, and their count."""
        xs, ys = 0, 0
        n = 0
        for y in range(HUD_MARGIN, image.height() - HUD_MARGIN, 2):
            for x in range(0, image.width(), 2):
                if differs(rgb(image, x, y), background, CUBE_DIFFERENCE):
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

    def changed_pixels(a, b):
        n = 0
        for y in range(HUD_MARGIN, a.height() - HUD_MARGIN, 2):
            for x in range(0, a.width(), 2):
                if differs(rgb(a, x, y), rgb(b, x, y), BACKGROUND_SLACK + 1):
                    n += 1
        return n

    def check_corners(before, after, what):
        for x, y in corners(before):
            a, b = rgb(before, x, y), rgb(after, x, y)
            check(not differs(a, b, BACKGROUND_SLACK), "%s leaves the background at (%d, %d) as it was (%r -> %r)"
                  % (what, x, y, a, b))

    def set_grading(gain_stops, gamma):
        knobs["enableGain"].setValue(gain_stops != 0.0)
        knobs["gain"].setValue(gain_stops)
        knobs["enableGamma"].setValue(gamma != 1.0)
        knobs["gamma"].setValue(gamma)
        QApplication.processEvents()

    def steps():
        set_grading(0.0, 1.0)
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
        key(v, Qt.Key_6, CTRL)
        yield ("sleep", SETTLE_MS)
        require(prop("natronSceneDrawMode") == "unshaded", "Ctrl+6 draws unshaded (%r)" % (prop("natronSceneDrawMode"),))
        key(v, Qt.Key_A)
        yield from repaint()

        section("reference")
        reference = capture("scene-gain-reference", "the 0.3 grey cube at gain 1, gamma 1: what gain 2 must make of the 0.15 cube")
        require(reference is not None, "the scene view can be captured")
        background = rgb(reference, *corners(reference)[0])
        for x, y in corners(reference)[1:]:
            require(not differs(rgb(reference, x, y), background, BACKGROUND_SLACK),
                    "the corners show the background (%r at (%d, %d), %r at the first)" % (rgb(reference, x, y), x, y, background))
        centre, cube_pixels = cube_centre(reference, background)
        require(centre is not None and cube_pixels >= MIN_PIXELS, "the cube is drawn (%d pixels)" % cube_pixels)
        twice = mean_around(reference, centre)
        report("NOTE background %r, 0.3 cube %r at %r" % (background, twice, centre))

        t0 = time.time() * 1000.0
        usd.getParam("filename").setValue(card_path)
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) or 0) >= t0 and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the view syncs the 0.15 cube")
        yield from repaint()
        plain = capture("scene-gain-plain", "the 0.15 grey cube at gain 1, gamma 1")
        once = mean_around(plain, centre)
        require(differs(once, twice, COLOUR_SLACK * 2), "the 0.15 cube shows darker than the 0.3 one (%r vs %r)" % (once, twice))
        check_corners(reference, plain, "changing the cube's colour")

        section("gain")
        syncs = prop("natronSceneSyncRequests", 0)
        engines = prop("natronSceneEngineCount", 0)
        for stops in GAIN_STEPS:
            set_grading(stops, 1.0)
            yield from repaint()
        gained = capture("scene-gain-2", "gain 2 (1 f-stop): the cube matches the 0.3 reference, the background is unchanged")
        doubled = mean_around(gained, centre)
        check(not differs(doubled, twice, COLOUR_SLACK), "gain 2 shows the 0.15 cube as the 0.3 one at gain 1 (%r vs %r)"
              % (doubled, twice))
        check_corners(plain, gained, "gain 2")
        check(prop("natronSceneSyncRequests", 0) == syncs, "dragging gain asks for no scene sync (%r -> %r)"
              % (syncs, prop("natronSceneSyncRequests", 0)))
        check(prop("natronSceneEngineCount", 0) == engines, "dragging gain makes no engine (%r -> %r)"
              % (engines, prop("natronSceneEngineCount", 0)))

        section("gamma")
        set_grading(0.0, 2.0)
        yield from repaint()
        gamma = capture("scene-gain-gamma-2", "gamma 2: the cube brightens as the 2D viewer's gamma does, the background is unchanged")
        lifted = mean_around(gamma, centre)
        expected = tuple(255.0 * (value / 255.0) ** 0.5 for value in once)
        check(not differs(lifted, expected, COLOUR_SLACK), "gamma 2 takes the cube from %r to about %r (got %r)"
              % (once, expected, lifted))
        check_corners(plain, gamma, "gamma 2")

        section("grid")
        set_grading(0.0, 1.0)
        yield from repaint()
        no_grid_plain = crop()
        set_grading(1.0, 1.0)
        yield from repaint()
        no_grid_gained = crop()
        set_grading(0.0, 1.0)
        key(v, Qt.Key_G, CTRL)
        yield from repaint()
        require(prop("natronSceneShowGrid") is True, "Ctrl+G shows the grid")
        grid_plain = capture("scene-gain-grid-plain", "the grid at gain 1")
        set_grading(1.0, 1.0)
        yield from repaint()
        grid_gained = capture("scene-gain-grid-2", "the grid at gain 2: only the cube changes")
        grid_pixels = changed_pixels(no_grid_plain, grid_plain)
        check(grid_pixels >= MIN_PIXELS, "the grid shows (%d pixels differ from the view without it)" % grid_pixels)
        without = changed_pixels(no_grid_plain, no_grid_gained)
        with_grid = changed_pixels(grid_plain, grid_gained)
        report("NOTE pixels gain 2 changes: %d without the grid, %d with it" % (without, with_grid))
        check(with_grid <= without + MIN_PIXELS, "gain 2 leaves the grid as it was (%d pixels change with it, %d without)"
              % (with_grid, without))
        set_grading(0.0, 1.0)
        key(v, Qt.Key_G, CTRL)
        yield from repaint()
        require(prop("natronSceneShowGrid") is False, "Ctrl+G hides the grid again")

        section("translucent")
        t0 = time.time() * 1000.0
        usd.getParam("filename").setValue(translucent_reference_path)
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) or 0) >= t0 and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the view syncs the half-transparent 0.3 cube")
        yield from repaint()
        seen = capture("scene-gain-translucent-reference", "the half-transparent 0.3 cube at gain 1: the background shows through")
        through = mean_around(seen, centre)
        # The cube's grey over the background, by the alpha its faces add up to.
        alpha = (through[0] - background[0]) / float(twice[0] - background[0])
        report("NOTE half-transparent 0.3 cube %r, alpha %.3f" % (through, alpha))
        check(0.2 < alpha < 0.95, "the half-transparent cube lets the background through (alpha %.3f)" % alpha)

        t0 = time.time() * 1000.0
        usd.getParam("filename").setValue(translucent_card_path)
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) or 0) >= t0 and scene_drawn(), POLL_TIMEOUT_S)
        require(ok, "the view syncs the half-transparent 0.15 cube")
        set_grading(1.0, 1.0)
        yield from repaint()
        gained = capture("scene-gain-translucent-2", "the half-transparent 0.15 cube at gain 2: as the 0.3 one at gain 1")
        doubled = mean_around(gained, centre)
        check(not differs(doubled, through, COLOUR_SLACK), "gain 2 shows the half-transparent 0.15 cube as the 0.3 one at "
              "gain 1 (%r vs %r)" % (doubled, through))
        check_corners(seen, gained, "gain 2 on the half-transparent cube")

        set_grading(1.0, 2.0)
        yield from repaint()
        lifted_image = capture("scene-gain-translucent-gamma-2",
                               "gain 2, gamma 2 on the half-transparent 0.15 cube: the grey graded, then laid over the background")
        lifted = mean_around(lifted_image, centre)
        expected = tuple(alpha * 255.0 * (value / 255.0) ** 0.5 + (1.0 - alpha) * b for value, b in zip(twice, background))
        check(not differs(lifted, expected, COLOUR_SLACK), "gamma 2 grades the half-transparent cube's colour, not its "
              "colour times alpha (%r, expected about %r)" % (lifted, expected))
        check_corners(seen, lifted_image, "gamma 2 on the half-transparent cube")
        set_grading(0.0, 1.0)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
