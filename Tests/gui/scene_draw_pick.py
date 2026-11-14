"""Boxes and markers of boxed rows are picked by depth with prims and guides, and guides go with a Hidden row.

Graph: USDRead of a small stage this script writes to $NATRON_GUI_TEST_OUT/scene_draw_pick.usda,
into Viewer1, seen from the front (Z, then A to frame). The front view looks down -Z, so of two
things at one pixel the one with the larger z is nearer. Each row is an Xform holding a Cube, but
for /World/Spot, which holds nothing and so draws a marker:
  /World/Near   at z = 3, 1 unit, boxed; /World/Far at z = -3, 3 units, boxed, around Near on screen
  /World/Shield at x = -4, z = 3, drawn;  /World/Behind at x = -4, z = -3, boxed, behind Shield
  /World/Crate  at x = 4, z = 3, boxed;   /World/Wall at x = 4, z = -3, drawn, behind Crate
  /World/Spot   at y = 3, boxed: a marker
  /World/Group  at x = -4, y = 3, boxed, its cube at /World/Group/Inner/Leaf
  /World/Lamps  at x = 4, y = 3: a sphere light Key and a camera Cam
natronSceneBoxes lists each drawn box as [path, kind, highlighted, x, y, left, top, right, bottom]
in widget pixels, and natronSceneGuides each drawn guide. The script checks:
  - a click on Near's box selects /World/Near, nearer than Far; a click on Far beside Near selects
    /World/Far; a click on the marker selects /World/Spot;
  - a drawn prim in front of a box wins (Shield over Behind), and a box in front of a drawn prim
    wins (Crate over Wall);
  - a picked box's row is selected in the Scene Graph tree and its box drawn highlighted;
  - selecting /World/Group/Inner/Leaf in the tree highlights Group's box, in the selection colour,
    and a click on that box selects /World/Group in the tree;
  - BBox on /World/Lamps keeps its guides; Hidden on it removes both and their pixels; clearing it
    brings them back;
  - in the Lobby, when present: Children on the base building draws boxes, and a click on the
    largest one selects a row and highlights its box when that box was nearest.

Files: $LOBBY_FILE (default .local/World_Lobby.usd); the Lobby section is skipped, and says so,
when the Lobby is absent.

Screenshots for sign-off: scene-pick-drawn, scene-pick-boxed, scene-pick-near-picked,
scene-pick-ancestor-highlighted, scene-pick-lamps-boxed, scene-pick-lamps-hidden,
scene-pick-lobby-before, scene-pick-lobby-boxed, scene-pick-lobby-picked.

Mouse and key events are sent to the SceneViewport. Run through Tests/gui/run-gui-test.sh.
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0
only if every check passed.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 1000
BOXED = ["/World/Near", "/World/Far", "/World/Behind", "/World/Crate", "/World/Spot"]
LEAF = "/World/Group/Inner/Leaf"
LAMPS = "/World/Lamps"
LAMP_GUIDES = ["/World/Lamps/Cam", "/World/Lamps/Key"]
BASE_BUILDING = "/World/assembly_BaseBuildingL0"
# The box overlay's selection colour, (1, 0.8, 0.1).
SELECTED_RGB = (255, 204, 25)

SCENE = """#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Xform "Near"
    {
        double3 xformOp:translate = (0, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
    def Xform "Far"
    {
        double3 xformOp:translate = (0, 0, -3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 3
        }
    }
    def Xform "Shield"
    {
        double3 xformOp:translate = (-4, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
    def Xform "Behind"
    {
        double3 xformOp:translate = (-4, 0, -3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 2
        }
    }
    def Xform "Crate"
    {
        double3 xformOp:translate = (4, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 1
        }
    }
    def Xform "Wall"
    {
        double3 xformOp:translate = (4, 0, -3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            double size = 2
        }
    }
    def Xform "Spot"
    {
        double3 xformOp:translate = (0, 3, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "Group"
    {
        double3 xformOp:translate = (-4, 3, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Xform "Inner"
        {
            def Cube "Leaf"
            {
                double size = 1
            }
        }
    }
    def Xform "Lamps"
    {
        double3 xformOp:translate = (4, 3, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def SphereLight "Key"
        {
            float inputs:radius = 0.3
        }
        def Camera "Cam"
        {
            double3 xformOp:translate = (1, 0, 0)
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }
    }
}
"""

try:
    from PySide6.QtCore import Q_ARG, QEvent, QItemSelectionModel, QMetaObject, QModelIndex, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(OUT, "scene_draw_pick.usda")
    with open(scene_path, "w") as f:
        f.write(SCENE)
    lobby_path = os.environ.get("LOBBY_FILE", os.path.join(ROOT, ".local", "World_Lobby.usd"))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if not views:
            return None
        v = hold(views[0])
        if not v.property("natronSceneRecordBoxes"):
            v.setProperty("natronSceneRecordBoxes", True)
            v.setProperty("natronSceneRecordGuides", True)
            v.update()
        return v

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def tree():
        p = panel()
        return hold(p.findChild(QTreeView, "sceneGraphTree")) if p is not None else None

    def model():
        t = tree()
        return hold(t.model()) if t is not None else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        while m.canFetchMore(parent):
            m.fetchMore(parent)
        return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]

    def find(path):
        """The index of path's row, reading the tree down to it; an invalid index when absent."""
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def tree_selection():
        t = tree()
        if t is None:
            return []
        return sorted(str(index.data(PATH_ROLE)) for index in t.selectionModel().selectedRows(0))

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def scene_error():
        v = view()
        return str(v.property("natronSceneError") or "") if v is not None else ""

    def draw_stats():
        v = view()
        stats = v.property("natronSceneDrawStats") if v is not None else None
        return dict(stats) if stats else {}

    def presented():
        v = view()
        return int(v.property("natronSceneDrawPresented") or 0) if v is not None else 0

    def selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

    def drawn_boxes():
        """Path to (kind, highlighted, centre QPoint, (left, top, right, bottom))."""
        v = view()
        value = v.property("natronSceneBoxes") if v is not None else None
        result = {}
        for entry in (value or []):
            path, kind, lit, x, y, left, top, right, bottom = list(entry)
            result[str(path)] = (str(kind), bool(lit), QPoint(int(x), int(y)), (left, top, right, bottom))
        return result

    def guide_paths():
        v = view()
        value = v.property("natronSceneGuides") if v is not None else None
        return sorted(set(str(list(entry)[0]) for entry in (value or [])))

    def activate():
        view().activateWindow()
        ok = yield ("until", lambda: view() is not None and view().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the scene view's window is active before input")
        view().setFocus()
        QApplication.processEvents()

    def key(k):
        yield from activate()
        QTest.keyClick(view(), k)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def click(pos):
        yield from activate()
        v = view()
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def pick(label, pos, expect):
        """Clicks at pos and waits for the selection that expect(selection) accepts."""
        before = selection()
        yield from click(pos)
        ok = yield ("until", lambda: expect(selection()), POLL_TIMEOUT_S)
        check(ok, "%s (selection %r, was %r)" % (label, selection(), before))
        return ok

    def grab():
        v = view()
        if v is None:
            return QImage()
        QApplication.processEvents()
        top_left = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), v.width(), v.height()).convertToFormat(QImage.Format_RGBX8888)

    def save(name):
        shot(name + ".png")
        image = grab()
        path = os.path.join(OUT, name + "_view.png")
        image.save(path)
        report("SHOT " + path)
        return image

    def changed_pixels(first, second):
        if first.isNull() or first.size() != second.size():
            return None
        a = bytes(first.constBits())[:first.sizeInBytes()]
        b = bytes(second.constBits())[:second.sizeInBytes()]
        if a == b:
            return 0
        return sum(1 for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3])

    def selected_pixels(image, rect):
        """Pixels in the selection colour within rect, grown by two pixels for the box's edges."""
        left, top, right, bottom = [int(round(c)) for c in rect]
        count = 0
        for y in range(max(0, top - 2), min(image.height(), bottom + 3)):
            for x in range(max(0, left - 2), min(image.width(), right + 3)):
                c = image.pixelColor(x, y)
                if all(abs(a - b) <= 40 for a, b in zip((c.red(), c.green(), c.blue()), SELECTED_RGB)):
                    count += 1
        return count

    def change(label, method, *args):
        """Calls a draw-state slot and waits until its frame is presented."""
        before = int(draw_stats().get("posted", 0))
        require(QMetaObject.invokeMethod(view(), method, *args), "%s: the scene view has %s" % (label, method))
        posted = int(draw_stats().get("posted", 0))
        if posted == before:
            check(False, "%s: the draw state changed something" % label)
            return
        ok = yield ("until", lambda: presented() >= posted, POLL_TIMEOUT_S)
        check(ok, "%s: reached the screen" % label)
        yield ("sleep", SETTLE_MS)

    def set_state(row, state):
        yield from change("%s %s" % (state, row), "setDrawState", Q_ARG("QString", row), Q_ARG("QString", state))

    def clear_states():
        yield from change("clear every row", "clearDrawStates")

    def wait_boxes(label, paths):
        """Waits until every path in paths has a box or marker on screen."""
        ok = yield ("until", lambda: set(paths) <= set(drawn_boxes()), POLL_TIMEOUT_S)
        check(ok, "%s: drawn (%r)" % (label, sorted(drawn_boxes())))
        return ok

    def select_in_tree(path):
        index = find(path)
        check(index.isValid(), "the panel lists %s" % path)
        if not index.isValid():
            return
        t = tree()
        parent = index.parent()
        while parent.isValid():
            t.expand(parent)
            parent = parent.parent()
        t.selectionModel().select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        t.scrollTo(index)
        QApplication.processEvents()

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the stage", scene_error())
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and len(rows()) == 1, POLL_TIMEOUT_S)
        require(ok, "the panel lists /World")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        yield from key(Qt.Key_Z)
        yield from key(Qt.Key_A)
        check(view().property("natronSceneViewMode") == "front", "the view looks from the front (%r)"
              % (view().property("natronSceneViewMode"),))
        save("scene-pick-drawn")

        section("pick boxes and markers")
        for row in BOXED:
            yield from set_state(row, "bbox")
        yield from wait_boxes("the boxed rows", BOXED)
        boxes = drawn_boxes()
        check(boxes.get("/World/Spot", ("",))[0] == "marker", "/World/Spot draws a marker (%r)" % (boxes.get("/World/Spot"),))
        save("scene-pick-boxed")
        if set(BOXED) <= set(boxes):
            yield from pick("a click on Near's box picks it, not Far behind it", boxes["/World/Near"][2],
                            lambda s: s == ["/World/Near"])
            ok = yield ("until", lambda: tree_selection() == ["/World/Near"], POLL_TIMEOUT_S)
            check(ok, "the tree selects /World/Near (%r)" % tree_selection())
            ok = yield ("until", lambda: drawn_boxes().get("/World/Near", ("", False))[1], POLL_TIMEOUT_S)
            check(ok, "Near's box is highlighted")
            check(not drawn_boxes().get("/World/Far", ("", True))[1], "Far's box is not")
            image = save("scene-pick-near-picked")
            check(selected_pixels(image, boxes["/World/Near"][3]) > 0, "Near's box is drawn in the selection colour")

            near_right = boxes["/World/Near"][3][2]
            far_right = boxes["/World/Far"][3][2]
            beside = QPoint(int((near_right + far_right) / 2), boxes["/World/Far"][2].y())
            yield from pick("a click on Far beside Near picks Far", beside, lambda s: s == ["/World/Far"])
            yield from pick("a click on the marker picks /World/Spot", boxes["/World/Spot"][2], lambda s: s == ["/World/Spot"])
            yield from pick("a drawn prim in front of a box wins (Shield over Behind)", boxes["/World/Behind"][2],
                            lambda s: len(s) == 1 and s[0].startswith("/World/Shield"))
            yield from pick("a box in front of a drawn prim wins (Crate over Wall)", boxes["/World/Crate"][2],
                            lambda s: s == ["/World/Crate"])

        section("ancestor highlight")
        yield from set_state("/World/Group", "bbox")
        yield from wait_boxes("Group's box", ["/World/Group"])
        group = drawn_boxes().get("/World/Group")
        before = grab()
        check(group is not None and not group[1], "Group's box is not highlighted before its leaf is selected")
        unlit = selected_pixels(before, group[3]) if group else None
        select_in_tree(LEAF)
        ok = yield ("until", lambda: selection() == [LEAF], POLL_TIMEOUT_S)
        check(ok, "selecting the leaf in the tree selects it in the viewer (%r)" % selection())
        ok = yield ("until", lambda: drawn_boxes().get("/World/Group", ("", False))[1], POLL_TIMEOUT_S)
        check(ok, "the leaf's nearest boxed ancestor, Group, is highlighted")
        yield ("sleep", SETTLE_MS)
        image = save("scene-pick-ancestor-highlighted")
        if group:
            lit = selected_pixels(image, group[3])
            check(lit > (unlit or 0), "Group's box turns the selection colour (%r pixels, %r before)" % (lit, unlit))
            yield from pick("a click on Group's box picks Group", group[2], lambda s: s == ["/World/Group"])
            ok = yield ("until", lambda: tree_selection() == ["/World/Group"], POLL_TIMEOUT_S)
            check(ok, "the tree selects /World/Group (%r)" % tree_selection())

        section("guides follow their row")
        yield from clear_states()
        ok = yield ("until", lambda: set(LAMP_GUIDES) <= set(guide_paths()), POLL_TIMEOUT_S)
        check(ok, "the camera and light guides are drawn (%r)" % guide_paths())
        shown = grab()
        yield from set_state(LAMPS, "bbox")
        ok = yield ("until", lambda: set(LAMP_GUIDES) <= set(guide_paths()), POLL_TIMEOUT_S)
        check(ok, "BBox on their row keeps the guides (%r)" % guide_paths())
        save("scene-pick-lamps-boxed")
        yield from set_state(LAMPS, "hidden")
        ok = yield ("until", lambda: not (set(LAMP_GUIDES) & set(guide_paths())), POLL_TIMEOUT_S)
        check(ok, "Hidden on their row removes both guides (%r)" % guide_paths())
        hidden = save("scene-pick-lamps-hidden")
        check(bool(changed_pixels(shown, hidden)), "the guides' pixels go")
        yield from clear_states()
        ok = yield ("until", lambda: set(LAMP_GUIDES) <= set(guide_paths()), POLL_TIMEOUT_S)
        check(ok, "clearing the row brings the guides back (%r)" % guide_paths())

        section("Lobby")
        if not os.path.isfile(lobby_path):
            report("SKIP the Lobby is not at %s (set LOBBY_FILE)" % lobby_path)
            return
        usd.getParam("filename").setValue(lobby_path)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the Lobby")
        yield from key(Qt.Key_V)
        yield from key(Qt.Key_A)
        save("scene-pick-lobby-before")
        yield from set_state(BASE_BUILDING, "children")
        ok = yield ("until", lambda: len(drawn_boxes()) > 0, POLL_TIMEOUT_S)
        check(ok, "Children on the base building draws boxes (%d)" % len(drawn_boxes()))
        yield ("sleep", SETTLE_MS)
        save("scene-pick-lobby-boxed")
        boxes = drawn_boxes()
        on_screen = [(path, entry) for path, entry in boxes.items()
                     if 0 <= entry[2].x() < view().width() and 0 <= entry[2].y() < view().height()]
        if on_screen:
            path, entry = max(on_screen, key=lambda item: (item[1][3][2] - item[1][3][0]) * (item[1][3][3] - item[1][3][1]))
            yield from pick("a click on the largest box selects one row", entry[2], lambda s: len(s) == 1)
            picked = selection()
            report("NOTE the click aimed at %s picked %r" % (path, picked))
            if picked and picked[0] in drawn_boxes():
                ok = yield ("until", lambda: drawn_boxes().get(picked[0], ("", False))[1], POLL_TIMEOUT_S)
                check(ok, "the picked box %s is highlighted" % picked[0])
                ok = yield ("until", lambda: tree_selection() == picked, POLL_TIMEOUT_S)
                check(ok, "the tree selects %s (%r)" % (picked[0], tree_selection()))
            yield ("sleep", SETTLE_MS)
            save("scene-pick-lobby-picked")
        else:
            check(False, "a box of the base building's children is on screen")
        yield from clear_states()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
