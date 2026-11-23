"""Rows selected in the prim inspector's Spreadsheet tab become the 3D view's live selection.

Builds a GeoPrimitive grid (9 x 9 faces, 100 points) with the Viewer on it, and the Scene Graph panel
beside the viewer. Checks, on the viewport's natronGeoSelection, natronGeoSelectionCount and
natronGeoPickMode properties:
  - selecting Points rows 0-9 puts the view in points mode with the selection 0-9;
  - selecting other Points rows replaces that selection;
  - selecting Primitives rows 2-4 gives primitives mode with the selection 2-4;
  - selecting Vertices rows 0-3 selects the points those vertices use, in points mode;
  - selecting the Detail row clears nothing and selects nothing: the selection stays as it was;
  - selecting every row of a grid of about 4 million points (Ctrl+A) selects every point while the
    main thread's event loop never goes HEARTBEAT_BUDGET_MS without running a short timer.

One screenshot per check goes to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
BIG_TIMEOUT_S = 240.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
SPREADSHEET = "sceneGraphSpreadsheet"
SPREADSHEET_TITLE = "Spreadsheet"
GRID = 9
BIG_GRID = 2047
HEARTBEAT_MS = 10
HEARTBEAT_BUDGET_MS = 500.0

try:
    from PySide6.QtCore import QItemSelection, QItemSelectionModel, QTimer
    from PySide6.QtWidgets import QTabWidget, QTableView, QToolButton, QWidget

    from guitest import check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    grid = app.createNode("GeoPrimitive")
    require(grid is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(GRID)
    grid.getParam("gridColumns").setValue(GRID)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    grid.setPosition(0, 0)
    viewer.setPosition(0, 120)
    require(viewer.connectInput(0, grid), "view the grid")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    _found = {}

    def panel():
        if "panel" not in _found:
            panels = widgets_of("SceneGraphPanel")
            if panels:
                _found["panel"] = hold(panels[0])
        return _found.get("panel")

    def child(cls, name):
        if name not in _found:
            p = panel()
            try:
                c = hold(p.findChild(cls, name)) if p is not None else None
            except RuntimeError:
                return None
            if c is None:
                return None
            _found[name] = c
        return _found[name]

    def inspector():
        return child(QWidget, "sceneGraphInspector")

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

    def sheet():
        return child(QWidget, SPREADSHEET)

    def sheet_state(name):
        return sheet().property("natronGeoSpreadsheet" + name)

    def table():
        return child(QTableView, SPREADSHEET + "Table")

    def class_button(label):
        return child(QToolButton, SPREADSHEET + label)

    def points_count():
        """The exact count from the Points button's tooltip: the button itself shortens millions to 4.19M."""
        text = class_button("Points").toolTip()
        digits = "".join(c for c in text.split(":")[-1] if c.isdigit())
        return int(digits) if digits else -1

    def show_class(label):
        class_button(label).click()
        return sheet_state("Class") == label

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def picked():
        return str(prop("natronGeoSelection", "")), int(prop("natronGeoSelectionCount", 0)), str(prop("natronGeoPickMode", ""))

    def select_rows(first, last):
        t = table()
        model = t.model()
        selection = QItemSelection(model.index(first, 0), model.index(last, model.columnCount() - 1))
        t.selectionModel().select(selection, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)

    def show_spreadsheet_tab():
        tabs = child(QTabWidget, "sceneGraphInspectorTabs")
        titles = [tabs.tabText(i) for i in range(tabs.count())]
        if SPREADSHEET_TITLE not in titles:
            return False
        tabs.setCurrentIndex(titles.index(SPREADSHEET_TITLE))
        return True

    def main_thread_times_ms():
        """The calling thread's total time on a CPU, and total time ready to run without one."""
        with open("/proc/thread-self/schedstat") as f:
            fields = f.read().split()
        return int(fields[0]) / 1000000.0, int(fields[1]) / 1000000.0

    class Heartbeat:
        """The longest the main thread's event loop has gone without running a short timer, less the
        time it was ready to run but had no CPU, which a busy host adds."""

        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(HEARTBEAT_MS)
            self.timer.timeout.connect(self.beat)
            self.reset()

        def reset(self):
            self.last = time.monotonic()
            self.cpu, self.waited = main_thread_times_ms()
            self.worst_ms = 0.0
            self.worst_wall_ms = 0.0

        def beat(self):
            now = time.monotonic()
            cpu, waited = main_thread_times_ms()
            gap = (now - self.last) * 1000.0
            self.worst_wall_ms = max(self.worst_wall_ms, gap)
            self.worst_ms = max(self.worst_ms, gap - (waited - self.waited))
            self.last = now
            self.cpu = cpu
            self.waited = waited

        def start(self):
            self.reset()
            self.timer.start()

        def stop(self):
            self.timer.stop()
            return self.worst_ms

    def points_section():
        section("points")
        ok = yield ("until", lambda: points_count() == (GRID + 1) ** 2 and table().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the Points class counts the grid's points (got %r)" % (points_count(),))
        require(show_class("Points"), "the Points class is shown")
        select_rows(0, 9)
        ok = yield ("until", lambda: picked() == ("0-9", 10, "points"), POLL_TIMEOUT_S)
        check(ok, "Points rows 0-9 select points 0-9 in points mode (got %r)" % (picked(),))
        yield ("sleep", SETTLE_MS)
        shot("m337-selection-rows-points.png")
        select_rows(20, 22)
        ok = yield ("until", lambda: picked() == ("20-22", 3, "points"), POLL_TIMEOUT_S)
        check(ok, "other Points rows replace the selection (got %r)" % (picked(),))

    def primitives_section():
        section("primitives")
        require(show_class("Primitives"), "the Primitives class is shown")
        select_rows(2, 4)
        ok = yield ("until", lambda: picked() == ("2-4", 3, "primitives"), POLL_TIMEOUT_S)
        check(ok, "Primitives rows 2-4 select primitives 2-4 in primitives mode (got %r)" % (picked(),))
        yield ("sleep", SETTLE_MS)
        shot("m337-selection-rows-primitives.png")

    def vertices_section():
        section("vertices")
        require(show_class("Vertices"), "the Vertices class is shown")
        select_rows(0, 3)
        ok = yield ("until", lambda: picked()[2] == "points" and picked()[1] == 4, POLL_TIMEOUT_S)
        check(ok, "Vertices rows 0-3 select the 4 points of the first face in points mode (got %r)" % (picked(),))
        yield ("sleep", SETTLE_MS)
        shot("m337-selection-rows-vertices.png")
        before = picked()
        require(show_class("Detail"), "the Detail class is shown")
        select_rows(0, 0)
        yield ("sleep", SETTLE_MS)
        check(picked() == before, "the Detail row selects nothing and leaves the selection (got %r, was %r)" % (picked(), before))

    def big_section():
        section("big")
        resized_ms = int(time.time() * 1000)
        grid.getParam("gridRows").setValue(BIG_GRID)
        grid.getParam("gridColumns").setValue(BIG_GRID)
        total = (BIG_GRID + 1) ** 2
        ok = yield ("until", lambda: points_count() == total and table().isVisible(), BIG_TIMEOUT_S)
        require(ok, "the Points class counts the big grid's %d points (got %r)" % (total, points_count()))
        require(show_class("Points"), "the Points class is shown")
        # With software GL every window flush queues behind the scene view's frame, which takes
        # seconds for this grid: measured before that frame is done, the heartbeat would time the
        # grid's drawing instead of the selection.
        ok = yield ("until", lambda: (int(prop("natronSceneSyncedAtMs", 0) or 0) >= resized_ms) and scene_drawn() and not prop("natronSceneComposing", False)
                    and (int(prop("natronSceneLastPaintMs", 0) or 0) >= int(prop("natronSceneSyncedAtMs", 0) or 0)), BIG_TIMEOUT_S)
        require(ok, "the scene view draws the big grid")
        yield ("sleep", SETTLE_MS)
        beat = Heartbeat()
        beat.start()
        table().selectAll()
        ok = yield ("until", lambda: picked()[1] == total and picked()[2] == "points", BIG_TIMEOUT_S)
        worst = beat.stop()
        check(ok, "selecting all %d Points rows selects every point (got %r)" % (total, picked()[1:]))
        report("NOTE heartbeat worst gap %.1f ms (wall %.1f ms)" % (worst, beat.worst_wall_ms))
        check(worst <= HEARTBEAT_BUDGET_MS, "selecting all rows leaves the main thread responsive (%.1f ms)" % (worst,))
        yield ("sleep", SETTLE_MS)
        shot("m337-selection-rows-all.png")

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the grid")
        require(panel() is not None, "the Scene Graph panel exists")

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible(), POLL_TIMEOUT_S)
        require(ok, "the panel shows")
        require(inspector() is not None, "the panel has its inspector")
        require(show_spreadsheet_tab(), "the inspector has a Spreadsheet tab")
        require(sheet() is not None and table() is not None, "the Spreadsheet tab has its table")

        yield from points_section()
        yield from primitives_section()
        yield from vertices_section()
        yield from big_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
