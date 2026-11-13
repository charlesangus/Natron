"""A viewer's draw states and payload loads are saved with the project and restored on reopening.

Graph: USDRead of a four-block heavy_city.usda (tools/bench/heavy_scene_make.py) into Viewer1, every
payload unloaded, with the file of /World/Block3/B3 deleted. The script sets Load All on Block0,
Children on Block1/B1, BBox on Block2, Full on Block2/B3 (which switches Block2 to Children), Hidden on
Block3/B0, and asks for the payloads of Block3/B2 and Block3/B3 as a twirl does. Then it checks:
  - the saved project holds the viewer's sceneView, with the missing payload's path in the load set;
  - reopening restores every draw state cell and loads the same buildings, with no request from
    the script;
  - the missing payload is badged as failed with USD's error in its tooltip, and the rest restores;
  - reopening again and cancelling the loads at once leaves the states restored and loads nothing.

File: $NATRON_CITY_FILE is not read: the fixture must lose a payload file, so the script always
writes its own city under $NATRON_GUI_TEST_OUT.

Screenshots for sign-off: scene-save-before, scene-save-restored, scene-save-cancelled.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback
import zipfile

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 1000
BLOCKS = ["/World/Block%d" % b for b in range(4)]
BUILDINGS_PER_BLOCK = 6
CITY_FLOORS = 16
# Enough prims to a building that a load is not over before a script can look.
CITY_PRIMS = len(BLOCKS) * BUILDINGS_PER_BLOCK * 400 + len(BLOCKS) + 1
BUILDINGS = ["%s/B%d" % (block, i) for block in BLOCKS for i in range(BUILDINGS_PER_BLOCK)]
STATES = [
    ("/World/Block0", "loadAll"),
    ("/World/Block1/B1", "children"),
    ("/World/Block2", "bbox"),
    ("/World/Block2/B3", "full"),
    ("/World/Block3/B0", "hidden"),
]
TWIRLED = "/World/Block3/B2"
BROKEN = "/World/Block3/B3"
CELLS = ["/World"] + BLOCKS + BUILDINGS
STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)

try:
    from PySide6.QtCore import Q_ARG, QMetaObject, QModelIndex, Qt, QTimer
    from PySide6.QtWidgets import QApplication, QMessageBox, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    PAYLOAD_STATE_ROLE = Qt.UserRole + 3
    INHERITED_ROLE = Qt.UserRole + 6

    sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
    import heavy_scene_make

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_draw_save"))
    os.makedirs(fixture_dir, exist_ok=True)
    heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, len(BLOCKS), BUILDINGS_PER_BLOCK, CITY_FLOORS)
    city_path = os.path.join(fixture_dir, "heavy_city.usda")
    broken_file = os.path.join(fixture_dir, "heavy_city_payloads", "b3_3.usdc")
    if os.path.exists(broken_file):
        os.remove(broken_file)
    require(os.path.isfile(city_path) and not os.path.exists(broken_file), "fixture written, %s missing" % broken_file)
    project_path = os.path.join(fixture_dir, "scene_draw_save.ntpz")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def forget_widgets():
        """Drops the wrappers of a closed project's widgets, whose C++ objects are gone."""
        del _held[:]

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

    def draw_column():
        m = model()
        if m is None:
            return -1
        for column in range(m.columnCount()):
            if str(m.headerData(column, Qt.Horizontal)) == "Draw State":
                return column
        return -1

    def cells():
        """Each row's draw state cell as (label, inherited)."""
        column = draw_column()
        shown = {}
        for path in CELLS:
            index = find(path)
            if not index.isValid() or column < 0:
                shown[path] = None
                continue
            cell = index.sibling(index.row(), column)
            shown[path] = (str(cell.data(Qt.DisplayRole)), bool(cell.data(INHERITED_ROLE)))
        return shown

    def payload_state(path):
        index = find(path)
        return index.data(PAYLOAD_STATE_ROLE) if index.isValid() else None

    def loaded():
        return [path for path in BUILDINGS if payload_state(path) == STATE_LOADED]

    def failures():
        v = view()
        value = v.property("natronSceneLoadFailures") if v is not None else None
        return dict((str(f[0]), str(f[1])) for f in (value or []))

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def loads_quiet():
        v = view()
        return v is not None and not v.property("natronSceneLoadsPending")

    def draw_stats():
        v = view()
        stats = v.property("natronSceneDrawStats") if v is not None else None
        return dict(stats) if stats else {}

    def presented():
        v = view()
        return int(v.property("natronSceneDrawPresented") or 0) if v is not None else 0

    def set_state(row, state):
        before = int(draw_stats().get("posted", 0))
        require(QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", row), Q_ARG("QString", state)),
                "%s %s: the scene view has setDrawState" % (state, row))
        posted = int(draw_stats().get("posted", 0))
        if state == "loadAll":
            # Load All under Full rows draws what Full draws, so with the reveal off it posts no draw
            # change: what it changes is the loads of the payloads below it.
            below = [p for p in BUILDINGS if p.startswith(row + "/")]
            ok = yield ("until", lambda: all(payload_state(p) in (STATE_LOADING, STATE_LOADED) for p in below), LOAD_TIMEOUT_S)
            check(bool(below) and ok, "%s %s: the draw state asks for the payloads below it" % (state, row))
        else:
            check(posted > before, "%s %s: the draw state changed something" % (state, row))
        ok = yield ("until", lambda: presented() >= posted, POLL_TIMEOUT_S)
        check(ok, "%s %s: reached the screen" % (state, row))

    def show_panel():
        """Puts the Scene Graph panel beside the viewer, unless a reopened layout already shows it."""
        p = panel()
        if p is not None and p.isVisible() and len(rows()) == 1:
            return
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel() is not None and panel().isVisible() and len(rows()) == 1, POLL_TIMEOUT_S)
        require(ok, "the panel lists /World")

    def wait_for_scene(label):
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "%s: the viewer is up" % label)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "%s: the scene view shows the city" % label)
        yield from show_panel()

    def wait_for_loads(label):
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: loads_quiet() and payload_state(BROKEN) in (STATE_FAILED, STATE_UNLOADED)
                    and not any(payload_state(p) == STATE_LOADING for p in BUILDINGS), LOAD_TIMEOUT_S)
        check(ok, "%s: the loads are over" % label)
        yield ("sleep", SETTLE_MS)

    def saved_project_yaml():
        if zipfile.is_zipfile(project_path):
            with zipfile.ZipFile(project_path) as archive:
                for name in archive.namelist():
                    if name.endswith("project.yaml"):
                        return archive.read(name).decode("utf-8")
            return ""
        with open(project_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def reopen(label):
        # Showing the Scene Graph panel splits a pane and moves a tab, which are project edits, so
        # the reset asks whether to save them. The saved project is the one under test: discard.
        asked = []

        def discard_save_question():
            box = QApplication.activeModalWidget()
            if isinstance(box, QMessageBox) and box.button(QMessageBox.StandardButton.Discard) is not None:
                asked.append(box.text())
                box.button(QMessageBox.StandardButton.Discard).click()

        answer = QTimer()
        answer.timeout.connect(discard_save_question)
        answer.start(200)
        try:
            reset = app.resetProject()
        finally:
            answer.stop()
            answer = None
        if asked:
            report("NOTE %s: discarded the layout edits (%r)" % (label, asked[0]))
        require(reset, "%s: reset the project (close it, keep the window)" % label)
        forget_widgets()
        yield ("sleep", SETTLE_MS)
        require(app.loadProject(project_path) is not None, "%s: reopen %s" % (label, project_path))

    def steps():
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
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        usd = None
        viewer = None
        yield from wait_for_scene("first open")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        check(loaded() == [], "no building is loaded at first (%r)" % loaded())

        section("set and save")
        for row, state in STATES:
            yield from set_state(row, state)
        require(QMetaObject.invokeMethod(view(), "requestLoad", Q_ARG("QStringList", [TWIRLED, BROKEN]), Q_ARG(bool, False)),
                "ask for the twirled payloads")
        yield from wait_for_loads("first open")
        want = sorted(["%s/B%d" % (BLOCKS[0], i) for i in range(BUILDINGS_PER_BLOCK)] + ["/World/Block1/B1", TWIRLED])
        saved_loaded = loaded()
        check(saved_loaded == want, "the states and the twirl load their buildings (%r)" % saved_loaded)
        check(payload_state(BROKEN) == STATE_FAILED, "the missing payload is badged before saving (%r)" % payload_state(BROKEN))
        saved_cells = cells()
        report("NOTE saved cells %r" % (saved_cells,))
        shot("scene-save-before.png")
        require(app.saveProjectAs(project_path), "save the project to " + project_path)
        text = saved_project_yaml()
        check("sceneView:" in text, "the project holds the viewer's sceneView")
        check(BROKEN in text.split("sceneView:", 1)[-1], "the missing payload stays in the saved load set")
        check("impliedChildren" in text, "Block2's switch to Children is saved as one made for a row below")

        section("reopen restores")
        yield from reopen("restore")
        yield from wait_for_scene("restore")
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        yield from wait_for_loads("restore")
        restored_cells = cells()
        check(restored_cells == saved_cells,
              "every draw state cell is restored (differ: %r)" % [p for p in CELLS if restored_cells.get(p) != saved_cells.get(p)])
        check(loaded() == saved_loaded, "the same buildings are loaded (%r)" % loaded())

        section("missing payload")
        check(payload_state(BROKEN) == STATE_FAILED, "the missing payload's row is badged as failed (%r)" % payload_state(BROKEN))
        tip = str(find(BROKEN).data(Qt.ToolTipRole))
        check("b3_3.usdc" in tip, "its tooltip gives USD's error (%r)" % tip)
        check(BROKEN in failures(), "the view reports the failed load (%r)" % list(failures()))
        shot("scene-save-restored.png")

        section("cancel stops the restore")
        yield from reopen("cancel")
        # In the same turn as the reopening, before any event can let a sync make the stage.
        views = [hold(w) for w in widgets_of("SceneViewport")]
        immediate = bool(views)
        if not immediate:
            ok = yield ("until", lambda: bool(widgets_of("SceneViewport")), STARTUP_TIMEOUT_S)
            require(ok, "cancel: the reopened viewer has a scene view")
            views = [hold(w) for w in widgets_of("SceneViewport")]
            report("NOTE the scene view was made after the reopening returned; loads may have started")
        for v in views:
            QMetaObject.invokeMethod(v, "cancelPendingLoads")
        views = None
        yield from wait_for_scene("cancel")
        yield ("sleep", 3 * SETTLE_MS)
        ok = yield ("until", loads_quiet, LOAD_TIMEOUT_S)
        check(ok, "cancel: nothing is left to load")
        cancelled_cells = cells()
        check(cancelled_cells == saved_cells, "the draw states are still restored after the cancel")
        if immediate:
            check(loaded() == [], "cancelling before the first sync loads nothing (%r)" % loaded())
        else:
            check(len(loaded()) < len(saved_loaded), "cancelling stops the restore's loads (%r)" % loaded())
        shot("scene-save-cancelled.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
