"""Before/after parity of the viewer interacts as they move onto overlay draw lists: one scene per
interact, each a 256 x 256 crop of the viewer at 100% around a fixed canonical anchor, compared
with a reference crop under Tests/gui/references/m434/.

Scenes (M434_PARITY_SCENES picks a comma-separated subset):
    roto              a feathered Roto Bezier with one point selected, so its tangents draw
    rotodrag          the same Bezier with a point held mid-drag
    rotopaint         a RotoPaint brush stroke, selected
    tracker           a Tracker with two tracks
    trackerselected   the same Tracker with one track clicked, so its pattern and search boxes draw
    splinewarp        a SplineWarp source curve linked to a destination curve
    transform         a Transform's centre, rotation and scale handles
    transformhover    the same Transform with the cursor over its centre
    transformdrag     the same Transform with its centre held mid-drag
    cornerpin         a CornerPin's four "to" corners
    position          a Position's translate handle
    rectangle         a Rectangle's box and its handles
    ramp              a Ramp's two points over its own gradient

Every scene but the ramp sits on a Constant of 0.18 (mid-grey when displayed), so the black shadows under the interacts
show. With M434_PARITY_RECORD=1 the crops are written as the references instead of compared. A
pixel passes when some reference pixel within PARITY_RADIUS of it (0 by default: the same pixel)
is within PARITY_TOLERANCE on every channel. The overlay's own pixels are those that change when
the node's panel closes, and a scene passes when at most PARITY_MAX_BAD of that count fail, so a
thin interact gets no more slack than a large one. A failing scene leaves its crop and a diff
image in the output.

Run in the natron-dev container:
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m434-parity/out GUI_TEST_TIMEOUT=600 \\
        Tests/gui/run-gui-test.sh Tests/gui/m434_interact_parity.py <AppImage or build/fast/App/Natron>'
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 540
SETTLE_MS = 500
RENDER_MS = 1500
CROP = 256
PARITY_TOLERANCE = int(os.environ.get("M434_PARITY_TOLERANCE", "40"))
PARITY_RADIUS = int(os.environ.get("M434_PARITY_RADIUS", "0"))
PARITY_MAX_BAD = float(os.environ.get("M434_PARITY_MAX_BAD", "0.03"))
RECORD = os.environ.get("M434_PARITY_RECORD", "") == "1"
# The draw list cuts dashes as geometry where the old renderer used GL line stipple: same period, phase off by under a
# pixel and a different shade. That is an accepted renderer difference, so dashed scenes search one pixel wider.
DASHED_SCENES = ("roto", "rotodrag", "ramp")
DASH_RADIUS = 1
DASH_MASK_PX = 2.5
ALL_SCENES = ("roto", "rotodrag", "rotopaint", "tracker", "trackerselected", "splinewarp", "transform",
              "transformhover", "transformdrag", "cornerpin", "position", "rectangle", "ramp")
SCENES = [s.strip() for s in os.environ.get("M434_PARITY_SCENES", ",".join(ALL_SCENES)).split(",") if s.strip()]

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, Q_ARG
    from PySide6.QtGui import QColor, QGuiApplication, QImage, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    REFERENCES = os.path.join(gt.ROOT, "Tests", "gui", "references", "m434")

    # PySide wrappers used across steps; a wrapper that dies takes its children's with it.
    held = {}

    # ---- plumbing --------------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def choose(combo, text):
        items = [str(i) for i in (combo.property("natronItems") or [])]
        for index, item in enumerate(items):
            if text.lower() in item.lower():
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
                QApplication.processEvents()
                return True
        return False

    def visible_viewer():
        for w in gt.widgets_of("ViewerGL"):
            try:
                if w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        gl = held.get("gl")
        value = gl.property("natronCanonicalToWidget") if gl is not None else None
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def to_widget(x, y):
        ox, oy, sx, sy = mapping()
        return ox + sx * x, oy + sy * y

    def to_canonical(wx, wy):
        ox, oy, sx, sy = mapping()
        return (wx - ox) / sx, (wy - oy) / sy

    def anchored(dx, dy):
        ax, ay = held["anchor"]
        return ax + dx, ay + dy

    def mouse(kind, x, y, button, buttons):
        QApplication.sendEvent(held["gl"], QMouseEvent(kind, QPointF(x, y), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def click_canonical(x, y):
        wx, wy = to_widget(x, y)
        mouse(QEvent.MouseMove, wx, wy, Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseButtonPress, wx, wy, Qt.LeftButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, wx, wy, Qt.LeftButton, Qt.NoButton)

    def hover_canonical(x, y):
        wx, wy = to_widget(x, y)
        mouse(QEvent.MouseMove, wx, wy, Qt.NoButton, Qt.NoButton)
        held["keep_cursor"] = True

    def drag_canonical(x, y, dx, dy):
        """Presses at (x, y) and moves by (dx, dy) without releasing; the release runs after the crop."""
        wx, wy = to_widget(x, y)
        ex, ey = to_widget(x + dx, y + dy)
        mouse(QEvent.MouseMove, wx, wy, Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseButtonPress, wx, wy, Qt.LeftButton, Qt.LeftButton)
        moves = 8
        for i in range(1, moves + 1):
            f = i / float(moves)
            mouse(QEvent.MouseMove, wx + (ex - wx) * f, wy + (ey - wy) * f, Qt.NoButton, Qt.LeftButton)
        held["keep_cursor"] = True
        held["after_crop"] = lambda: mouse(QEvent.MouseButtonRelease, ex, ey, Qt.LeftButton, Qt.NoButton)

    def park_cursor():
        """Off the crop, so no handle is hovered."""
        gl = held["gl"]
        mouse(QEvent.MouseMove, 4.0, gl.height() - 4.0, Qt.NoButton, Qt.NoButton)

    def create(plugin_id, x, y, panel=True):
        if panel:
            props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
            node = app.createNode(plugin_id, -1, None, props)
        else:
            node = app.createNode(plugin_id)
        gt.require(node is not None, "create %s" % plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        held.setdefault("scene_nodes", []).append(node)
        return node

    def param(node, name):
        p = node.getParam(name)
        owner = node.getLabel() if hasattr(node, "getLabel") else node.getScriptName()
        gt.require(p is not None, "%s has a parameter %r" % (owner, name))
        return p

    GREY = 0.18

    def background():
        constant = create("Constant", 0, -200, panel=False)
        param(constant, "color").set(GREY, GREY, GREY, 1.0)
        return constant

    def show(node):
        viewer = held["viewer_node"]
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        gt.require(viewer.connectInput(0, node), "the viewer shows %s" % node.getLabel())
        QApplication.processEvents()

    def clear_scene():
        viewer = held["viewer_node"]
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        gt.close_all_panels()
        for node in reversed(held.pop("scene_nodes", [])):
            node.destroy(False)
        held.pop("scene_items", None)
        QApplication.processEvents()

    def circle_points(cx, cy, r, count):
        """Clockwise from the top: (x, y, lx, ly, rx, ry) with circular tangents."""
        tangent = r * 4.0 / 3.0 * math.tan(math.pi / (2 * count))
        pts = []
        for i in range(count):
            a = math.pi / 2 - 2 * math.pi * i / count
            x, y = cx + r * math.cos(a), cy + r * math.sin(a)
            tx, ty = math.sin(a), -math.cos(a)
            pts.append((x, y, x - tx * tangent, y - ty * tangent, x + tx * tangent, y + ty * tangent))
        return pts

    def make_curve(roto, pts, feather, t):
        bez = roto.createBezier(pts[0][0], pts[0][1], t)
        gt.require(bez is not None, "createBezier made a curve")
        for p in pts[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        for i, p in enumerate(pts):
            bez.setPointAtIndex(i, t, *p)
            bez.setFeatherPointAtIndex(i, t, *(feather[i] if feather else p))
        held.setdefault("scene_items", []).append(bez)
        return bez

    # ---- crops -----------------------------------------------------------------------------

    def crop():
        """The CROP x CROP screen pixels centred on the anchor, as RGB32."""
        QApplication.processEvents()
        gl = held["gl"]
        wx, wy = to_widget(*held["anchor"])
        g = gl.mapToGlobal(QPoint(int(round(wx)) - CROP // 2, int(round(wy)) - CROP // 2))
        img = QGuiApplication.primaryScreen().grabWindow(0, g.x(), g.y(), CROP, CROP).toImage()
        return img.convertToFormat(QImage.Format_RGB32)

    def pixels(img):
        """[(r, g, b)] row-major, from the RGB32 bytes."""
        stride = img.bytesPerLine()
        raw = bytes(img.constBits())
        out = []
        for y in range(img.height()):
            row = y * stride
            for x in range(img.width()):
                i = row + 4 * x
                out.append((raw[i + 2], raw[i + 1], raw[i]))
        return out

    def close(p, q, tol):
        return abs(p[0] - q[0]) <= tol and abs(p[1] - q[1]) <= tol and abs(p[2] - q[2]) <= tol

    def dash_mask(scene):
        """Crop pixels within DASH_MASK_PX of the ramp's two dashed lines, whose shade differs between renderers."""
        if scene != "ramp":
            return None
        ax, ay = held["anchor"]
        wx, wy = to_widget(ax, ay)
        x0, y0 = int(round(wx)) - CROP // 2, int(round(wy)) - CROP // 2
        (px, py), (qx, qy) = [to_widget(*anchored(dx, dy)) for dx, dy in ((-80.0, -50.0), (80.0, 50.0))]
        px, py, qx, qy = px - x0, py - y0, qx - x0, qy - y0
        length = math.hypot(qx - px, qy - py)
        if length == 0.0:
            return None
        ux, uy = (qx - px) / length, (qy - py) / length
        masked = set()
        for y in range(CROP):
            for x in range(CROP):
                # Each line crosses one point at a right angle to the line between the two points.
                along0 = (x + 0.5 - px) * ux + (y + 0.5 - py) * uy
                along1 = (x + 0.5 - qx) * ux + (y + 0.5 - qy) * uy
                if min(abs(along0), abs(along1)) <= DASH_MASK_PX:
                    masked.add(y * CROP + x)
        return masked

    def compare(actual, reference, radius=None, skip=None):
        """(bad pixel count, diff image): red where a pixel has no close reference pixel nearby."""
        radius = PARITY_RADIUS if radius is None else radius
        w, h = actual.width(), actual.height()
        a, r = pixels(actual), pixels(reference)
        diff = QImage(w, h, QImage.Format_RGB32)
        diff.fill(QColor(0, 0, 0))
        bad = 0
        tol = PARITY_TOLERANCE
        for y in range(h):
            for x in range(w):
                p = a[y * w + x]
                ok = skip is not None and y * w + x in skip
                for ny in range(max(0, y - radius), min(h, y + radius + 1)):
                    if ok:
                        break
                    for nx in range(max(0, x - radius), min(w, x + radius + 1)):
                        if close(p, r[ny * w + nx], tol):
                            ok = True
                            break
                    if ok:
                        break
                if not ok:
                    bad += 1
                    diff.setPixelColor(x, y, QColor(255, 0, 0))
                else:
                    level = (p[0] + p[1] + p[2]) // 6
                    diff.setPixelColor(x, y, QColor(level, level, level))
        return bad, diff

    def overlay_pixels(with_overlay, without_overlay):
        a, b = pixels(with_overlay), pixels(without_overlay)
        return sum(1 for p, q in zip(a, b) if not close(p, q, PARITY_TOLERANCE))

    def judge(scene):
        img = crop()
        gt.require(not img.isNull() and img.width() == CROP and img.height() == CROP,
                   "%s: read a %d px crop from the screen" % (scene, CROP))
        after_crop = held.pop("after_crop", None)
        if after_crop is not None:
            after_crop()
            yield ("sleep", SETTLE_MS)
        # The overlays draw only for nodes with an open panel, so this crop is the image alone.
        gt.close_all_panels()
        park_cursor()
        yield ("sleep", SETTLE_MS)
        base = crop()
        drawn = overlay_pixels(img, base)
        check(drawn > 0, "%s: the overlay draws over the image (%d pixels)" % (scene, drawn))
        path = os.path.join(REFERENCES, scene + ".png")
        if RECORD:
            if not os.path.isdir(REFERENCES):
                os.makedirs(REFERENCES)
            check(img.save(path, "PNG", 0), "%s: recorded the reference %s" % (scene, path))
            return
        reference = QImage(path)
        if reference.isNull():
            check(False, "%s: a reference crop exists at %s (record with M434_PARITY_RECORD=1)" % (scene, path))
            img.save(os.path.join(gt.OUT, "m434-%s-actual.png" % scene))
            return
        reference = reference.convertToFormat(QImage.Format_RGB32)
        if reference.width() != CROP or reference.height() != CROP:
            check(False, "%s: the reference is %d x %d, not %d px" % (scene, reference.width(), reference.height(), CROP))
            return
        radius = max(PARITY_RADIUS, DASH_RADIUS) if scene in DASHED_SCENES else PARITY_RADIUS
        bad, diff = compare(img, reference, radius, dash_mask(scene))
        allowed = int(PARITY_MAX_BAD * drawn)
        ok = bad <= allowed
        if not ok:
            img.save(os.path.join(gt.OUT, "m434-%s-actual.png" % scene))
            diff.save(os.path.join(gt.OUT, "m434-%s-diff.png" % scene))
        check(ok, "%s: %d pixels differ from the reference, at most %d allowed (%g of the %d overlay pixels;"
              " tolerance %d within %d px)" % (scene, bad, allowed, PARITY_MAX_BAD, drawn, PARITY_TOLERANCE, radius))

    # ---- scenes ----------------------------------------------------------------------------

    def roto_curve(t):
        bg = background()
        node = create("Roto", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the Roto")
        roto = node.getRotoContext()
        gt.require(roto is not None, "the Roto node has a roto context")
        ax, ay = held["anchor"]
        pts = circle_points(ax, ay, 70.0, 6)
        make_curve(roto, pts, circle_points(ax, ay, 85.0, 6), t)
        show(node)
        yield ("sleep", RENDER_MS)
        # The Roto viewer starts on the Bezier drawing tool; Q switches to selection.
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyPress, Qt.Key_Q, Qt.NoModifier))
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyRelease, Qt.Key_Q, Qt.NoModifier))
        click_canonical(pts[1][0], pts[1][1])
        held["roto_points"] = pts
        yield ("sleep", SETTLE_MS)

    def scene_roto(t):
        for cmd in roto_curve(t):
            yield cmd

    def scene_rotodrag(t):
        for cmd in roto_curve(t):
            yield cmd
        pts = held.pop("roto_points")
        drag_canonical(pts[1][0], pts[1][1], 18.0, 12.0)
        yield ("sleep", RENDER_MS)

    def scene_rotopaint(t):
        bg = background()
        node = create("RotoPaint", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the RotoPaint")
        gt.require(node.getRotoContext() is not None, "the RotoPaint node has a roto context")
        param(node, "sizeSpinbox").setValue(4.0)
        show(node)
        yield ("sleep", RENDER_MS)
        events = 24
        path = []
        for i in range(events + 1):
            f = i / float(events)
            path.append(to_widget(*anchored(-90.0 + 180.0 * f, 40.0 * math.sin(2 * math.pi * f))))
        mouse(QEvent.MouseMove, path[0][0], path[0][1], Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseButtonPress, path[0][0], path[0][1], Qt.LeftButton, Qt.LeftButton)
        for x, y in path[1:]:
            mouse(QEvent.MouseMove, x, y, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", 8)
        mouse(QEvent.MouseButtonRelease, path[-1][0], path[-1][1], Qt.LeftButton, Qt.NoButton)
        yield ("sleep", RENDER_MS)
        # A finished stroke is image content; the overlay only draws once the stroke is selected.
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyPress, Qt.Key_Q, Qt.NoModifier))
        QApplication.sendEvent(held["gl"], QKeyEvent(QEvent.KeyRelease, Qt.Key_Q, Qt.NoModifier))
        # The viewer's select tool picks Beziers only, so the stroke is selected in the panel's item
        # tree, by model index (the tree's item wrappers die with their parents).
        selected = False
        for tree in QApplication.allWidgets():
            if tree.metaObject().className() != "QTreeWidget" or not tree.isVisible():
                continue
            model = tree.model()
            for row in range(model.rowCount()):
                layer = model.index(row, 0)
                for child in range(model.rowCount(layer)):
                    index = model.index(child, 0, layer)
                    if str(model.data(index)).startswith("Brush"):
                        tree.setCurrentIndex(index)
                        selected = True
        gt.require(selected, "the RotoPaint item tree lists the brush stroke")
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def scene_tracker(t):
        bg = background()
        node = create("Tracker", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the Tracker")
        tracker = node.getTrackerContext()
        gt.require(tracker is not None, "the Tracker has a tracker context")
        items = held.setdefault("scene_items", [])
        for dx in (-60.0, 60.0):
            track = tracker.createTrack()
            gt.require(track is not None, "created a track")
            centre = param(track, "centerPoint")
            centre.set(*anchored(dx, 0.0))
            items.extend((track, centre))
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_trackerselected(t):
        for cmd in scene_tracker(t):
            yield cmd
        click_canonical(*anchored(-60.0, 0.0))
        yield ("sleep", SETTLE_MS)

    def scene_splinewarp(t):
        bg = background()
        node = create("SplineWarp", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the SplineWarp")
        roto = node.getRotoContext()
        sw = node.getSplineWarp()
        gt.require(roto is not None and sw is not None, "the SplineWarp has curves and links")
        held.setdefault("scene_items", []).extend((roto, sw))
        ax, ay = held["anchor"]
        src = make_curve(roto, circle_points(ax - 30.0, ay, 50.0, 5), None, t)
        dst = make_curve(roto, circle_points(ax + 35.0, ay + 15.0, 60.0, 5), None, t)
        link = sw.link(src, dst)
        gt.require(link is not None, "linked the source curve to the destination")
        held["scene_items"].append(link)
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_transform(t):
        bg = background()
        node = create("Transform", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the Transform")
        param(node, "center").set(*held["anchor"])
        param(node, "rotate").setValue(20.0)
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_transformhover(t):
        for cmd in scene_transform(t):
            yield cmd
        hover_canonical(*held["anchor"])
        yield ("sleep", SETTLE_MS)

    def scene_transformdrag(t):
        for cmd in scene_transform(t):
            yield cmd
        drag_canonical(held["anchor"][0], held["anchor"][1], 25.0, 15.0)
        yield ("sleep", RENDER_MS)

    def scene_cornerpin(t):
        bg = background()
        node = create("CornerPin", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the CornerPin")
        corners = ((-80.0, -70.0), (90.0, -60.0), (70.0, 80.0), (-90.0, 60.0))
        for i, (dx, dy) in enumerate(corners):
            param(node, "to%d" % (i + 1)).set(*anchored(dx, dy))
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_position(t):
        bg = background()
        node = create("Position", 0, 0)
        gt.require(node.connectInput(0, bg), "the Constant feeds the Position")
        param(node, "translate").set(*held["anchor"])
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_rectangle(t):
        node = create("Rectangle", 0, 0)
        param(node, "color0").set(GREY, GREY, GREY, 1.0)
        param(node, "color1").set(GREY, GREY, GREY, 1.0)
        extent = param(node, "extent")
        options = [str(o).lower().replace(" ", "") for o in extent.getOptions()]
        index = next((i for i, o in enumerate(options) if o == "size"), -1)
        gt.require(index >= 0, "the Rectangle's extent menu has a Size entry (%r)" % options)
        extent.setValue(index)
        param(node, "bottomLeft").set(*anchored(-80.0, -60.0))
        param(node, "size").set(160.0, 120.0)
        show(node)
        yield ("sleep", RENDER_MS)

    def scene_ramp(t):
        node = create("Ramp", 0, 0)
        param(node, "point0").set(*anchored(-80.0, -50.0))
        param(node, "point1").set(*anchored(80.0, 50.0))
        show(node)
        yield ("sleep", RENDER_MS)

    SCENE_STEPS = {
        "roto": scene_roto, "rotodrag": scene_rotodrag, "rotopaint": scene_rotopaint,
        "tracker": scene_tracker, "trackerselected": scene_trackerselected, "splinewarp": scene_splinewarp,
        "transform": scene_transform, "transformhover": scene_transformhover,
        "transformdrag": scene_transformdrag, "cornerpin": scene_cornerpin, "position": scene_position,
        "rectangle": scene_rectangle, "ramp": scene_ramp,
    }

    # ---- steps -----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window is up")
        win = main_window()
        held["win"] = win
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        unknown = [s for s in SCENES if s not in SCENE_STEPS]
        gt.require(not unknown, "every scene in M434_PARITY_SCENES is known (%r)" % unknown)
        report("NOTE %s the references in %s" % ("recording" if RECORD else "comparing with", REFERENCES))

        gt.section("setup")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        gt.require(viewer is not None, "the project has a viewer", gt.plugin_problem("Viewer"))
        viewer.setPosition(0, 200)
        held["viewer_node"] = viewer
        probe = create("Constant", 0, -200, panel=False)
        show(probe)
        ok = yield ("until", lambda: visible_viewer() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "a viewer is up")
        held["gl"] = visible_viewer()
        view_combo = widget_with_tip("ComboBox", "View:")
        zoom_combo = widget_with_tip("ComboBox", "Zoom:")
        held["view_combo"], held["zoom_combo"] = view_combo, zoom_combo
        gt.require(view_combo is not None and zoom_combo is not None, "found the viewer's View and Zoom menus")
        check(choose(view_combo, "Un-tone-mapped") or choose(view_combo, "Raw"),
              "the viewer shows the Un-tone-mapped (or Raw) view")
        check(choose(zoom_combo, "100%"), "the viewer zooms to 100%")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the viewer publishes natronCanonicalToWidget")
        gl = held["gl"]
        gt.require(gl.width() >= CROP and gl.height() >= CROP, "the viewer is at least %d px each way" % CROP)
        cx, cy = to_canonical(gl.width() / 2.0, gl.height() / 2.0)
        held["anchor"] = (float(round(cx)), float(round(cy)))
        report("NOTE canonical to widget mapping %r, anchor %r" % (mapping(), held["anchor"]))
        clear_scene()
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        check(ok, "the main window is active")

        t = app.timelineGetTime()
        for scene in SCENES:
            gt.section(scene)
            park_cursor()
            for cmd in SCENE_STEPS[scene](t):
                yield cmd
            if not held.pop("keep_cursor", False):
                park_cursor()
            yield ("sleep", SETTLE_MS)
            gt.shot("m434-parity-%s.png" % scene)
            for cmd in judge(scene):
                yield cmd
            held.pop("roto_points", None)
            clear_scene()
            yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
