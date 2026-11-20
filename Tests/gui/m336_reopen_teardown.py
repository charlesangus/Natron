"""Scene viewport teardown off the main thread: the app stays responsive and draws again after each reset.

Graph: USDRead of a small heavy_city.usda (tools/bench/heavy_scene_make.py) into Viewer1, Full + Load,
with the Scene Graph panel docked beside the viewer. The script then runs CYCLES rounds of reset the
project, rebuild the graph. In the rounds listed in EARLY_CYCLES the reset comes right after the graph
is built, while the stage is still loading and drawing; in the others the first draw and the panel's
rows are waited for first. After every reset it checks:
  - a timer fires on the main thread within RESPONSIVE_MS;
  - the next graph's scene view draws and the Scene Graph panel lists /World.
Two more rounds destroy the scene's viewer while it draws: one deletes the Viewer node, one closes the
viewer's tab. Each is followed by a rebuilt graph that has to draw.

When Natron runs with NATRON_MAIN_HEARTBEAT=1 the longest main-thread gap around each reset is
reported as a NOTE and never fails a check: CI timings are noisy.

File: $NATRON_CITY_FILE, by default a city the script writes under $NATRON_GUI_TEST_OUT.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 600.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
RESPONSIVE_MS = 500
CYCLES = 6
EARLY_CYCLES = (1, 3)
# Time between creating the graph and resetting it in an early round: long enough for the viewer's
# stage to start, short enough that it has not finished loading.
EARLY_MS = 150
BLOCKS = 6
BUILDINGS_PER_BLOCK = 10
CITY_FLOORS = 16
CITY_PRIMS = BLOCKS * BUILDINGS_PER_BLOCK * 40 + BLOCKS + 1
INITIAL_DRAW_FULL_LOAD = 4

try:
    from PySide6.QtCore import QModelIndex, QObject, Qt, QTimer
    from PySide6.QtWidgets import QApplication, QMessageBox, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    city_path = os.environ.get("NATRON_CITY_FILE")
    if not city_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "m336_reopen_teardown"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, BLOCKS, BUILDINGS_PER_BLOCK, CITY_FLOORS)
        city_path = os.path.join(fixture_dir, "heavy_city.usda")
    require(os.path.isfile(city_path), "fixture exists: %s" % city_path)

    _held = []
    _toggle = [0]

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

    def row_count():
        m = model()
        if m is None:
            return 0
        # Rows arrive from the model's worker once fetchMore() has asked for them.
        if m.canFetchMore(QModelIndex()):
            m.fetchMore(QModelIndex())
        return m.rowCount(QModelIndex())

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def heartbeat():
        """Natron's main-thread heartbeat, there when NATRON_MAIN_HEARTBEAT=1."""
        instance = QApplication.instance()
        return instance.findChild(QObject, "natronMainHeartbeat", Qt.FindDirectChildrenOnly) if instance is not None else None

    def poke(name):
        # Qt reports a property change only when the value differs.
        _toggle[0] += 1
        hb = heartbeat()
        if hb is not None:
            hb.setProperty(name, _toggle[0])
        return hb

    def gap_report(label):
        hb = poke("natronMainGapSample")
        if hb is None:
            return
        over = hb.property("natronMainGapsOver100") or []
        report("NOTE %s: longest main-thread gap %.1f ms, %d gap(s) over 100 ms"
               % (label, float(hb.property("natronMainGapMaxMs") or 0.0), len(over)))

    def build_graph():
        """Makes the USDRead into a Viewer, which also makes the viewer when the reset left none."""
        usd = app.createNode("USDRead")
        require(usd is not None, "create USDRead", plugin_problem("USDRead"))
        usd.getParam("initialDraw").setValue(INITIAL_DRAW_FULL_LOAD)
        usd.setPosition(0, 0)
        usd.getParam("filename").setValue(city_path)
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        viewer.setPosition(0, 200)
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")

    def show_panel():
        """Puts the Scene Graph panel beside the viewer, unless it already shows."""
        p = panel()
        if p is not None and p.isVisible():
            return
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None

    def wait_for_scene(label):
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "%s: the viewer is up" % label)
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "%s: the scene view draws the city" % label)
        show_panel()
        ok = yield ("until", lambda: panel() is not None and panel().isVisible() and row_count() >= 1, POLL_TIMEOUT_S)
        check(ok, "%s: the Scene Graph panel lists its rows (%d)" % (label, row_count()))

    def answering_discard(call):
        """Runs call() while a timer discards the save question a layout edit makes the reset ask."""
        asked = []

        def answer():
            box = QApplication.activeModalWidget()
            if isinstance(box, QMessageBox):
                button = box.button(QMessageBox.StandardButton.Discard)
                if button is not None:
                    asked.append(box.text())
                    button.click()

        timer = QTimer()
        timer.timeout.connect(answer)
        timer.start(200)
        try:
            result = call()
        finally:
            timer.stop()
            timer = None
        return result, asked

    def reset(label):
        poke("natronMainGapReset")
        result, asked = answering_discard(app.resetProject)
        if asked:
            report("NOTE %s: discarded the layout edits (%r)" % (label, asked[0]))
        require(result, "%s: reset the project (close it, keep the window)" % label)
        forget_widgets()

    def responsive(label):
        """The main thread services a timer within RESPONSIVE_MS of the reset returning."""
        fired = []
        started = time.time()
        QTimer.singleShot(10, lambda: fired.append(time.time()))
        ok = yield ("until", lambda: bool(fired), RESPONSIVE_MS / 1000.0)
        check(ok, "%s: a timer fires within %d ms (%.0f ms)"
              % (label, RESPONSIVE_MS, ((fired[0] if fired else time.time()) - started) * 1000.0))
        gap_report(label)

    def steps():
        if heartbeat() is None:
            report("NOTE no natronMainHeartbeat; run with NATRON_MAIN_HEARTBEAT=1 to report main-thread gaps")

        for cycle in range(CYCLES):
            early = cycle in EARLY_CYCLES
            label = "cycle %d%s" % (cycle, " (early reset)" if early else "")
            section(label)
            build_graph()
            if early:
                yield ("sleep", EARLY_MS)
            else:
                yield from wait_for_scene(label)
                yield ("sleep", SETTLE_MS)
                if cycle == 0:
                    shot("reopen-teardown-first.png")
            reset(label)
            yield from responsive(label)
            yield ("sleep", SETTLE_MS)

        section("delete the viewer while it draws")
        build_graph()
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "delete: the viewer is up")
        yield ("sleep", EARLY_MS)
        poke("natronMainGapReset")
        doomed = app.getNode("Viewer1")
        require(doomed is not None, "delete: Viewer1 exists")
        doomed.destroy(False)
        doomed = None
        forget_widgets()
        yield from responsive("delete")
        yield ("sleep", SETTLE_MS)
        build_graph()
        yield from wait_for_scene("delete")

        section("close the viewer's tab while it draws")
        yield ("sleep", EARLY_MS)
        home = ancestor_of_class(view(), "TabWidget")
        index = -1
        if home is not None:
            tabs = app.getTabWidget(home.objectName())
            for i in range(tabs.count()):
                if str(tabs.getTabLabel(i)).startswith("Viewer"):
                    index = i
            if index >= 0:
                forget_widgets()
                poke("natronMainGapReset")
                tabs.closeTab(index)
            tabs = None
        home = None
        if index < 0:
            report("NOTE no Viewer tab found in the scene view's pane; the tab close is skipped")
        else:
            yield from responsive("tab close")
            yield ("sleep", SETTLE_MS)
        node = app.getNode("Viewer1")
        if node is not None:
            node.destroy(False)
        node = None
        forget_widgets()
        yield ("sleep", SETTLE_MS)
        build_graph()
        yield from wait_for_scene("after tab close")

        section("final")
        yield from responsive("final")
        shot("reopen-teardown-final.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
