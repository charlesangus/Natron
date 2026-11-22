"""A selected half-transparent projected card and a selected opaque box both tint toward the
selection colour in the Storm 3D viewer.

Graph: USDRead1 (a backdrop quad, a card quad and a box) into USDAssignMaterial1's Scene input, with
USDProject3D1 (a half-transparent red Constant projected through USDCamera1) as its Material input,
assigned to the card only; USDAssignMaterial1 into USDMerge1, USDMerge1 into Viewer1. A second
USDRead, USDRead2 (an opaque slab), joins USDMerge1 for the last shot. The view is the front view,
framed to the whole scene, with the grid and guides hidden so only the geometry is tinted.

Screenshots, each a full window and a _view.png crop of the scene view:
  m278_nothing_selected      shaded, nothing selected: the baseline;
  m278_card_and_box_selected shaded, the card and the box selected: both tint, the backdrop does not;
  m278_wireframe_selected    Wireframe with the same selection: only the selected prims' lines tint;
  m278_selected_behind_opaque shaded, the box selected with the slab in front of its left half: the
                             slab stays untinted, the part of the box in the open tints.

The checks are lenient, since the gtests do the exact ones: a selected region's colour moves toward
the tint, an unselected region's does not.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh, at 1920x1080. Screenshots and
results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 800
STARTUP_TIMEOUT_S = 60.0
CTRL_LEFT_TUMBLE_NAVIGATION = 0
GRID_COLUMNS = 40
GRID_ROWS = 22
CARD = "/World/Card"
BOX = "/World/Box"
BACKDROP = "/World/Backdrop"
SLAB = "/World/Slab"
TINT = (255, 204, 26)
# The card tints at half strength, so it must move less than the opaque box.
MIN_BOX_MOVE = 25.0
MIN_CARD_MOVE = 8.0
MAX_UNSELECTED_MOVE = 12.0
MIN_WIRE_PIXELS = 40

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Backdrop"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-4, -2.4, -1), (4, -2.4, -1), (4, 2.4, -1), (-4, 2.4, -1)]
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
        color3f[] primvars:displayColor = [(0.15, 0.25, 0.6)]
    }

    def Mesh "Card"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2.2, -1.125, 0), (-0.2, -1.125, 0), (-0.2, 1.125, 0), (-2.2, 1.125, 0)]
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
    }

    def Cube "Box"
    {
        double size = 1.6
        double3 xformOp:translate = (1.4, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        color3f[] primvars:displayColor = [(0.7, 0.7, 0.7)]
    }
}
"""

SLAB_STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "Slab"
    {
        double size = 1
        double3 xformOp:translate = (1.0, 0, 1.6)
        float3 xformOp:scale = (1.2, 2.4, 0.4)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
        color3f[] primvars:displayColor = [(0.2, 0.7, 0.2)]
    }
}
"""

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    scene_path = os.path.join(OUT, "m278-selection.usda")
    with open(scene_path, "w") as f:
        f.write(STAGE)
    slab_path = os.path.join(OUT, "m278-slab.usda")
    with open(slab_path, "w") as f:
        f.write(SLAB_STAGE)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(CTRL_LEFT_TUMBLE_NAVIGATION)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    stage_read = make("USDRead", 0, 0)
    slab_read = make("USDRead", 300, 0)
    plate = make("Constant", 0, 100)
    camera = make("USDCamera", 150, 100)
    project = make("USDProject3D", 0, 200)
    assign = make("USDAssignMaterial", 150, 300)
    merge = make("USDMerge", 150, 400)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(150, 500)

    param(stage_read, "filename").setValue(scene_path)
    param(slab_read, "filename").setValue(slab_path)
    param(plate, "color").set(1.0, 0.0, 0.0, 0.5)
    param(camera, "translate").set(0.0, 0.0, 8.0)
    param(assign, "primPattern").set(CARD)

    CTRL, SHIFT = Qt.ControlModifier, Qt.ShiftModifier

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def mouse(kind, pos, button, buttons, modifiers=Qt.NoModifier):
        v = view()
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def click(pos, modifiers=Qt.NoModifier):
        mouse(QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        mouse(QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()
        return selection()

    def key(k, modifiers=Qt.NoModifier):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def crop():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height())

    def save_view(name):
        shot(name + ".png")
        image = crop()
        if image is not None:
            path = os.path.join(OUT, name + "_view.png")
            image.save(path)
            report("SHOT " + path)
        return image

    def patch(image, pos, half=1):
        """The mean colour of the pixels around pos."""
        total = [0, 0, 0]
        n = 0
        for dy in range(-half, half + 1):
            for dx in range(-half, half + 1):
                x, y = pos.x() + dx, pos.y() + dy
                if 0 <= x < image.width() and 0 <= y < image.height():
                    c = image.pixelColor(x, y)
                    total[0] += c.red()
                    total[1] += c.green()
                    total[2] += c.blue()
                    n += 1
        return tuple(t / float(max(1, n)) for t in total)

    def tint_distance(colour):
        return sum((a - b) ** 2 for a, b in zip(colour, TINT)) ** 0.5

    def move_toward_tint(before, after, pos):
        return tint_distance(patch(before, pos)) - tint_distance(patch(after, pos))

    def yellowish_pixels(image, step=2):
        n = 0
        for y in range(0, image.height(), step):
            for x in range(0, image.width(), step):
                c = image.pixelColor(x, y)
                if c.red() > 150 and c.green() > 100 and c.red() > c.blue() + 80 and c.green() > c.blue() + 40:
                    n += 1
        return n

    def nearest(points, target):
        return min(points, key=lambda p: (p - target).manhattanLength())

    def centre_of(points):
        x = sum(p.x() for p in points) / float(len(points))
        y = sum(p.y() for p in points) / float(len(points))
        return nearest(points, QPoint(int(x), int(y)))

    def probe():
        """Where each prim is drawn, by clicking a grid of points; leaves the selection empty."""
        v = view()
        sx = max(1, v.width() // GRID_COLUMNS)
        sy = max(1, v.height() // GRID_ROWS)
        hits = {}
        for row in range(GRID_ROWS):
            for column in range(GRID_COLUMNS):
                p = QPoint(sx // 2 + column * sx, sy // 2 + row * sy)
                picked = click(p)
                hits.setdefault(picked[0] if len(picked) == 1 else "", []).append(p)
            yield ("sleep", 0)
        if "" in hits:
            click(nearest(hits[""], QPoint(0, 0)))
        return hits

    def steps():
        require(project.connectInput(0, plate), "connect the Constant to USDProject3D1's Image input")
        require(project.connectInput(1, camera), "connect USDCamera1 to USDProject3D1's Camera input")
        require(assign.connectInput(0, stage_read), "connect USDRead1 to USDAssignMaterial1's Scene input")
        require(assign.connectInput(1, project), "connect USDProject3D1 to USDAssignMaterial1's Material input")
        require(merge.connectInput(0, assign), "connect USDAssignMaterial1 to USDMerge1")
        require(viewer.connectInput(0, merge), "connect the Viewer to USDMerge1")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))
        report("NOTE scene view %dx%d" % (v.width(), v.height()))
        key(Qt.Key_G, CTRL | SHIFT)
        yield settle()
        require(view().property("natronSceneShowGuides") is False, "Ctrl+Shift+G hides the guides")
        key(Qt.Key_G, CTRL)
        yield settle()
        require(view().property("natronSceneShowGrid") is False, "Ctrl+G hides the grid")
        key(Qt.Key_Z)
        key(Qt.Key_A)
        yield settle()

        section("nothing selected")
        hits = yield from probe()
        report("NOTE grid hits: %r" % dict((k, len(p)) for k, p in hits.items()))
        require(CARD in hits and BOX in hits and BACKDROP in hits and "" in hits,
                "the grid finds the card, the box, the backdrop and empty space")
        card = centre_of(hits[CARD])
        box = centre_of(hits[BOX])
        backdrop = min(hits[BACKDROP], key=lambda p: p.x())
        check(selection() == [], "the probe leaves nothing selected (%r)" % (selection(),))
        yield settle()
        plain = save_view("m278_nothing_selected")
        require(plain is not None, "the scene view can be grabbed")

        section("card and box selected")
        click(card)
        click(box, SHIFT)
        check(selection() == sorted([CARD, BOX]), "the card and the box are selected (%r)" % (selection(),))
        yield settle()
        selected = save_view("m278_card_and_box_selected")
        box_move = move_toward_tint(plain, selected, box)
        card_move = move_toward_tint(plain, selected, card)
        backdrop_move = abs(move_toward_tint(plain, selected, backdrop))
        report("NOTE moves toward the tint: box %.1f, card %.1f, backdrop %.1f" % (box_move, card_move, backdrop_move))
        check(box_move >= MIN_BOX_MOVE, "the selected opaque box moves toward the tint (%.1f)" % box_move)
        check(card_move >= MIN_CARD_MOVE, "the selected half-transparent card moves toward the tint (%.1f)" % card_move)
        check(backdrop_move <= MAX_UNSELECTED_MOVE, "the unselected backdrop does not move (%.1f)" % backdrop_move)

        section("wireframe")
        key(Qt.Key_4, CTRL)
        yield settle()
        check(view().property("natronSceneDrawMode") == "wireframe", "Ctrl+4 selects wireframe")
        wire = save_view("m278_wireframe_selected")
        click(nearest(hits[""], QPoint(0, 0)))
        yield settle()
        wire_plain = crop()
        selected_lines = yellowish_pixels(wire)
        plain_lines = yellowish_pixels(wire_plain)
        report("NOTE yellowish pixels in wireframe: %d selected, %d unselected" % (selected_lines, plain_lines))
        check(selected_lines >= plain_lines + MIN_WIRE_PIXELS,
              "the selected prims' wireframe lines are tinted (%d against %d)" % (selected_lines, plain_lines))
        key(Qt.Key_5, CTRL)
        yield settle()

        section("selected behind an opaque prim")
        require(merge.connectInput(1, slab_read), "connect USDRead2 to USDMerge1")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the slab")
        key(Qt.Key_A)
        yield settle()
        hits = yield from probe()
        report("NOTE grid hits: %r" % dict((k, len(p)) for k, p in hits.items()))
        require(SLAB in hits and BOX in hits and "" in hits, "the grid finds the slab and the part of the box in the open")
        slab = centre_of(hits[SLAB])
        open_box = centre_of(hits[BOX])
        yield settle()
        before = save_view("m278_slab_nothing_selected")
        click(open_box)
        check(selection() == [BOX], "the box is selected (%r)" % (selection(),))
        yield settle()
        behind = save_view("m278_selected_behind_opaque")
        open_move = move_toward_tint(before, behind, open_box)
        slab_move = abs(move_toward_tint(before, behind, slab))
        report("NOTE moves toward the tint: box in the open %.1f, slab %.1f" % (open_move, slab_move))
        check(open_move >= MIN_BOX_MOVE, "the part of the box in the open moves toward the tint (%.1f)" % open_move)
        check(slab_move <= MAX_UNSELECTED_MOVE, "the opaque slab in front of the selected box does not move (%.1f)" % slab_move)

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
