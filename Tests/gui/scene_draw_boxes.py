"""Boxed rows of the payload city draw as tinted boxes, or as markers where no hint bounds them.

Graph: USDRead of a ten-block heavy_city.usda (tools/bench/heavy_scene_make.py) into Viewer1, every
payload unloaded. Nine in ten buildings author an extentsHint, so a boxed building draws a box from it; the
tenth, index % 10 == 0, has none and draws a marker at its transform. The script checks:
  - Children on /World/Block0 .. Block9 boxes their 120 buildings: 108 boxes and 12 markers
    (natronSceneDrawBoxes, natronSceneDrawMarkers), and the image changes;
  - BBox on a hinted building draws one box, on one without a hint one marker;
  - the Scene Graph panel's unknown-bounds role is set on exactly the buildings without a hint;
  - no payload file is opened at any point;
  - clearing every row gives back the first image, with nothing drawn.

File: $NATRON_CITY_FILE, by default a city of ten blocks the script writes under $NATRON_GUI_TEST_OUT.

Screenshots for sign-off: scene-boxes-none, scene-boxes-and-markers, scene-boxes-one-box,
scene-boxes-one-marker, scene-boxes-badges.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
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
BLOCKS = ["/World/Block%d" % b for b in range(10)]
BUILDINGS_PER_BLOCK = 12
CITY_FLOORS = 16
# Twenty prims to a building, the fewest that still fill sixteen floors.
CITY_PRIMS = len(BLOCKS) * BUILDINGS_PER_BLOCK * 20 + len(BLOCKS) + 1
HINTED = "/World/Block0/B1"
UNHINTED = "/World/Block0/B0"
PAYLOAD_MARK = "heavy_city_payloads"

try:
    from PySide6.QtCore import Q_ARG, QMetaObject, QModelIndex, Qt
    from PySide6.QtGui import QGuiApplication, QImage
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    UNKNOWN_BOUNDS_ROLE = Qt.UserRole + 4

    city_path = os.environ.get("NATRON_CITY_FILE")
    if not city_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "scene_draw_boxes"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, len(BLOCKS), BUILDINGS_PER_BLOCK, CITY_FLOORS)
        city_path = os.path.join(fixture_dir, "heavy_city.usda")
    require(os.path.isfile(city_path), "fixture exists: %s" % city_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.setPosition(0, 0)
    usd.getParam("filename").setValue(city_path)
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
        return hold(views[0]) if views else None

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

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def draw_stats():
        v = view()
        stats = v.property("natronSceneDrawStats") if v is not None else None
        return dict(stats) if stats else {}

    def presented():
        v = view()
        return int(v.property("natronSceneDrawPresented") or 0) if v is not None else 0

    def drawn():
        v = view()
        if v is None:
            return (0, 0)
        return (int(v.property("natronSceneDrawBoxes") or 0), int(v.property("natronSceneDrawMarkers") or 0))

    def opened_payloads():
        return sorted(layer.identifier for layer in Sdf.Layer.GetLoadedLayers() if PAYLOAD_MARK in layer.identifier)

    def frame_all():
        view().activateWindow()
        ok = yield ("until", lambda: view() is not None and view().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the scene view's window is active before the key press")
        v = view()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, Qt.Key_A)
        QApplication.processEvents()
        yield ("sleep", 2 * SETTLE_MS)

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

    def set_state(row, state):
        yield from change("%s %s" % (state, row), "setDrawState", Q_ARG("QString", row), Q_ARG("QString", state))

    def clear_state(row):
        yield from change("clear " + row, "clearDrawState", Q_ARG("QString", row))

    def expect_drawn(label, boxes, markers):
        """Waits for the box worker's list to reach the screen with these counts."""
        ok = yield ("until", lambda: drawn() == (boxes, markers), POLL_TIMEOUT_S)
        check(ok, "%s: %d boxes and %d markers drawn (%r)" % (label, boxes, markers, drawn()))
        yield ("sleep", SETTLE_MS)

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the city")
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
        yield from frame_all()
        check(opened_payloads() == [], "no payload file is open before any draw state (%r)" % opened_payloads()[:3])

        section("boxes and markers")
        base = save("scene-boxes-none")
        check(drawn() == (0, 0), "nothing is boxed at first (%r)" % (drawn(),))
        for block in BLOCKS:
            yield from set_state(block, "children")
        unhinted = sum(1 for b in range(len(BLOCKS)) for i in range(BUILDINGS_PER_BLOCK) if (b * BUILDINGS_PER_BLOCK + i) % 10 == 0)
        total = len(BLOCKS) * BUILDINGS_PER_BLOCK
        yield from expect_drawn("Children on %d blocks" % len(BLOCKS), total - unhinted, unhinted)
        image = save("scene-boxes-and-markers")
        check(bool(changed_pixels(base, image)), "the boxes and markers change the image")
        for block in BLOCKS:
            yield from clear_state(block)
        yield from expect_drawn("the blocks cleared", 0, 0)

        yield from set_state(HINTED, "bbox")
        yield from expect_drawn("BBox on the hinted %s" % HINTED, 1, 0)
        save("scene-boxes-one-box")
        yield from clear_state(HINTED)
        yield from set_state(UNHINTED, "bbox")
        yield from expect_drawn("BBox on %s, which has no hint" % UNHINTED, 0, 1)
        save("scene-boxes-one-marker")
        yield from clear_state(UNHINTED)
        yield from expect_drawn("every row cleared", 0, 0)
        restored = changed_pixels(base, grab())
        check(restored == 0, "clearing every row gives back the first image (%r pixels differ)" % (restored,))

        section("unknown-bounds badge")
        flagged = []
        for b, block in enumerate(BLOCKS):
            for i in range(BUILDINGS_PER_BLOCK):
                index = find("%s/B%d" % (block, i))
                check(index.isValid(), "the panel lists %s/B%d" % (block, i))
                if index.isValid() and bool(index.data(UNKNOWN_BOUNDS_ROLE)):
                    flagged.append(b * BUILDINGS_PER_BLOCK + i)
        check(flagged == [n for n in range(total) if n % 10 == 0],
              "the badge is on exactly the buildings without a hint (%r)" % flagged[:20])
        t = tree()
        t.expand(find("/World"))
        t.expand(find(BLOCKS[0]))
        t.scrollTo(find(UNHINTED))
        yield ("sleep", SETTLE_MS)
        save("scene-boxes-badges")
        check(opened_payloads() == [], "no payload file was opened (%r)" % opened_payloads()[:3])

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
