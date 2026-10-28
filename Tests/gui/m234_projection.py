"""M234 - Camera Projection Nodes: USDProject3D projects a CheckerBoard through a USDCamera onto a
USDCard in the 3D view, Geometry space with Frame Hold keeps the projection on the card as it moves,
and the USDProjectUV node exposes its knobs.

Graph: CheckerBoard1 (red/blue squares) into USDProject3D1's Image input, USDCamera1 into its Camera
input, USDProject3D1 into USDCard1's Material input, USDCard1 into Viewer1. The Viewer is switched to
the Un-tone-mapped view so screen colours are the image's. The checks:
  - the card shows saturated red and blue squares, not the fallback grey;
  - Projection Space = Geometry with Use Frame Hold on, then the card translated: the coloured squares
    move with the card (the card's colour pattern is unchanged, its position is not);
  - USDProjectUV1 (Scene = USDCard1, Camera = USDCamera1) has the Nuke UVProject knobs and its panel
    opens.

Screenshots: m234_world.png, m234_geometry_moved.png, m234_projectuv_panel.png (full window), each
scene shot with a _viewer.png crop.

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
UNTONED_VIEW = "Un-tone-mapped"
BOX_SIZE = 480.0
CARD_SHIFT = 1.5

PROJECTUV_KNOBS = ("camera", "primPattern", "projection", "plane", "projectOn", "frustumCulling", "invertU",
                   "invertV", "uScale", "vScale", "primvarName", "fitMode", "useFrameHold", "frameHold")
PROJECT3D_KNOBS = ("camera", "projectionSpace", "projectOn", "crop", "clipNearFar", "outside", "fitMode",
                   "useFrameHold", "frameHold")

try:
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    checker = make("net.sf.openfx.CheckerBoardPlugin", 0, 0)
    camera = make("USDCamera", 200, 0)
    project = make("USDProject3D", 0, 100)
    card = make("USDCard", 0, 200)
    project_uv = make("USDProjectUV", 200, 200)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)

    RED = (1.0, 0.0, 0.0, 1.0)
    BLUE = (0.0, 0.0, 1.0, 1.0)
    param(checker, "boxSize").set(BOX_SIZE, BOX_SIZE)
    for name, colour in (("color0", RED), ("color1", BLUE), ("color2", RED), ("color3", BLUE)):
        param(checker, name).set(*colour)
    param(checker, "lineWidth").set(0.0)
    param(camera, "translate").set(0.0, 0.0, 5.0)

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
            return None, None
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(origin.x(), origin.y(), v.width(), v.height()), v

    def hue(c):
        """'r', 'b', or None for a pixel that is neither clearly red nor clearly blue (grey, black)."""
        r, g, b = c.red(), c.green(), c.blue()
        if r > 60 and r > 2 * g and r > 2 * b:
            return "r"
        if b > 60 and b > 2 * g and b > 2 * r:
            return "b"
        return None

    def coloured_pixels(step=8):
        """(x, y, hue) for every coloured pixel on a step-px grid of the scene view."""
        image, _v = grab_view()
        if image is None:
            return []
        out = []
        for y in range(0, image.height(), step):
            for x in range(0, image.width(), step):
                h = hue(image.pixelColor(x, y))
                if h:
                    out.append((x, y, h))
        return out

    def patch_hues(half=60):
        image, v = grab_view()
        if image is None:
            return {}
        cx, cy = v.width() // 2, v.height() // 2
        counts = {"r": 0, "b": 0, "other": 0}
        for dy in range(-half, half + 1, 6):
            for dx in range(-half, half + 1, 6):
                counts[hue(image.pixelColor(cx + dx, cy + dy)) or "other"] += 1
        return counts

    def card_shows_checker():
        counts = patch_hues()
        total = sum(counts.values())
        return total > 0 and counts["r"] >= 0.1 * total and counts["b"] >= 0.1 * total

    def signature(points, grid=8):
        """The coloured pixels' bounding box, centroid and a grid x grid map of their hues inside it."""
        if len(points) < 20:
            return None
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        cells = {}
        for x, y, h in points:
            gx = min(grid - 1, (x - x0) * grid // max(1, x1 - x0 + 1))
            gy = min(grid - 1, (y - y0) * grid // max(1, y1 - y0 + 1))
            cells.setdefault((gx, gy), []).append(h)
        pattern = {k: max(set(v), key=v.count) for k, v in cells.items()}
        return {"box": (x0, y0, x1, y1), "centroid": (sum(xs) / len(xs), sum(ys) / len(ys)), "pattern": pattern}

    def pattern_agreement(a, b):
        keys = set(a["pattern"]) & set(b["pattern"])
        if not keys:
            return 0.0
        same = sum(1 for k in keys if a["pattern"][k] == b["pattern"][k])
        return same / float(len(keys))

    def save_shots(name):
        shot(name + ".png")
        image, _v = grab_view()
        if image is not None:
            path = os.path.join(OUT, name + "_viewer.png")
            image.save(path)
            report("SHOT " + path)

    def untoned_combo():
        for w in widgets_of("ComboBox"):
            try:
                if w.isVisible() and UNTONED_VIEW in [str(i) for i in (w.property("natronItems") or [])]:
                    return w
            except RuntimeError:
                continue
        return None

    def select_untoned():
        w = untoned_combo()
        if w is None:
            return False
        items = [str(i) for i in w.property("natronItems")]
        QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(UNTONED_VIEW)))
        QApplication.processEvents()
        return str(w.property("natronCurrentText")) == UNTONED_VIEW

    def node_graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                    if w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def open_panel(node):
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        app.selectNode(node, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())

    def steps():
        require(project.connectInput(0, checker), "connect CheckerBoard1 to USDProject3D1's Image input")
        require(project.connectInput(1, camera), "connect USDCamera1 to USDProject3D1's Camera input")
        require(card.connectInput(0, project), "connect USDProject3D1 to USDCard1's Material input")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, card), "connect the Viewer to USDCard1")
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the card")

        ok = yield ("until", select_untoned, POLL_TIMEOUT_S)
        check(ok, "the Viewer's View menu offers %s" % UNTONED_VIEW)
        for name in PROJECT3D_KNOBS:
            check(project.getParam(name) is not None, "USDProject3D1 has a %s parameter" % name)

        ok = yield ("until", card_shows_checker, POLL_TIMEOUT_S)
        check(ok, "the card shows the projected red and blue squares, not the fallback grey (%r)" % (patch_hues(),))
        yield ("sleep", SETTLE_MS)
        save_shots("m234_world")
        before = signature(coloured_pixels())
        check(before is not None, "the card is a region of red and blue pixels in the scene view")

        param(project, "projectionSpace").set(1)
        param(project, "useFrameHold").set(True)
        param(project, "frameHold").set(1.0)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: scene_drawn() and card_shows_checker(), POLL_TIMEOUT_S)
        check(ok, "Geometry space with Frame Hold still shows the projected squares (%r)" % (patch_hues(),))
        held = signature(coloured_pixels())

        param(card, "translate").set(CARD_SHIFT, 0.0, 0.0)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: scene_drawn() and card_shows_checker(), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        save_shots("m234_geometry_moved")
        moved = signature(coloured_pixels())
        require(held is not None and moved is not None, "the moved card still shows red and blue pixels")
        shift = moved["centroid"][0] - held["centroid"][0]
        agreement = pattern_agreement(held, moved)
        check(abs(shift) > 20, "the card moved on screen (centroid shifted %.0f px)" % shift)
        check(agreement >= 0.75,
              "the projected squares moved with the card: its colour pattern is unchanged (%.0f%% of cells agree)"
              % (100 * agreement))

        for name in PROJECTUV_KNOBS:
            check(project_uv.getParam(name) is not None, "USDProjectUV1 has a %s parameter" % name)
        require(project_uv.connectInput(0, card), "connect USDCard1 to USDProjectUV1's Scene input")
        require(project_uv.connectInput(1, camera), "connect USDCamera1 to USDProjectUV1's Camera input")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, project_uv), "connect the Viewer to USDProjectUV1")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the scene view shows the card through USDProjectUV1")
        yield from open_panel(project_uv)
        panel = panel_of(project_uv)
        yield ("sleep", SETTLE_MS)
        shot("m234_projectuv_panel.png")
        texts = [w.text() for w in panel.findChildren(QLabel)] if panel is not None else []
        for label in ("Projection", "Project On", "Frustum Culling", "Invert U", "Invert V", "U Scale", "V Scale"):
            check(any(label in t for t in texts) or project_uv.getParam(label.replace(" ", "")[0].lower() + label.replace(" ", "")[1:]) is not None,
                  "USDProjectUV1's panel lists %s" % label)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
