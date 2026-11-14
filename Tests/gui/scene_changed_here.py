"""The Scene Graph panel's Changed Here marker and filter.

Graph: a USDRead of a scene the script writes to $NATRON_GUI_TEST_OUT, into a USDEditTransform on
one prim of it and on one prim deep in a Scope of FLAT_PRIMS children, into Viewer1.

Checks:
  - viewing the USDEditTransform marks its targets and their ancestors, and no other row, with the
    filter's own rows unread while the button has never been on;
  - the Changed Here button shows only the marked rows, down to the deep one, without the tree
    reading the flat Scope's rows, and shows the tree again when off with what was expanded still
    expanded;
  - toggling either way keeps the main thread's heartbeat within HEARTBEAT_BUDGET_MS of CPU and
    HEARTBEAT_BLOCKED_BUDGET_MS with blocking;
  - selecting a filtered row reaches the inspector, and the tree's selection after the button goes off;
  - viewing the USDRead, which authors nothing, marks no row, and the filter then shows none.

Rows are read as the panel left them: fetching them here would hide a panel that needs them fetched.
Only the tree's small branches are fetched, which a test of the marks on them needs.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off. Run through
Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 120.0
SETTLE_MS = 400
HEARTBEAT_MS = 10
HEARTBEAT_BUDGET_MS = 100.0
# Software GL holds the main thread 75-170 ms on any repaint, with no read at all, waiting for the
# rasteriser to flush.
HEARTBEAT_BLOCKED_BUDGET_MS = 400.0
FLAT_PRIMS = 50000

ROOT = "/Chg"
TARGET = ROOT + "/A/B"
FLAT = ROOT + "/Flat"
DEEP = FLAT + "/P%d" % (FLAT_PRIMS - 10)
MARKED = {ROOT, ROOT + "/A", TARGET, FLAT, DEEP}
UNMARKED = [ROOT + "/A/D", ROOT + "/C"]


def write_scene(path):
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Chg"\n)\n\n')
        f.write('def Xform "Chg"\n{\n')
        f.write('    def Xform "A"\n    {\n        def Xform "B"\n        {\n        }\n\n'
                '        def Xform "D"\n        {\n        }\n    }\n\n')
        f.write('    def Xform "C"\n    {\n    }\n\n')
        f.write('    def Scope "Flat"\n    {\n')
        for n in range(FLAT_PRIMS):
            f.write('        def Xform "P%d"\n        {\n        }\n' % n)
        f.write('    }\n}\n')


try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, Qt, QModelIndex, QTimer
    from PySide6.QtWidgets import QToolButton, QTreeView, QWidget

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    CHANGED_HERE_ROLE = Qt.UserRole + 2

    os.makedirs(OUT, exist_ok=True)
    scene_path = os.path.join(OUT, "scene-changed-here.usda")
    write_scene(scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    edit = app.createNode("USDEditTransform")
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    edit.setPosition(0, 100)
    viewer.setPosition(0, 200)
    require(edit.connectInput(0, usd), "connect the USDEditTransform to the USDRead")
    edit.getParam("primPattern").setValue(TARGET + " " + DEEP)
    edit.getParam("translate").set(1.0, 2.0, 3.0)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    # Looking a widget up wraps every widget in the application, which takes the main thread far
    # longer than the heartbeat budget. The polls run there too, so they must not repeat the walk.
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

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def filtered_tree():
        return child(QTreeView, "sceneGraphChangedHereTree")

    def inspector():
        return child(QWidget, "sceneGraphInspector")

    def model():
        t = tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def filtered_model():
        t = filtered_tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def tree_rows(parent=QModelIndex()):
        """The tree's rows under parent, fetched unless parent is the flat Scope."""
        m = model()
        if m is None:
            return []
        try:
            if not parent.isValid() or path_of(parent) != FLAT:
                while m.canFetchMore(parent):
                    m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def all_tree_rows(parent=QModelIndex()):
        result = []
        for index in tree_rows(parent):
            result.append(index)
            if path_of(index) != FLAT:
                result.extend(all_tree_rows(index))
        return result

    def find_tree_row(path):
        for index in all_tree_rows():
            if path_of(index) == path:
                return index
        return QModelIndex()

    def top_paths():
        return [path_of(i) for i in tree_rows()]

    def tree_marked_paths():
        return {path_of(i) for i in all_tree_rows() if i.data(CHANGED_HERE_ROLE)}

    def flat_rows_read():
        """How many of the flat Scope's rows the tree has read; 0 until it lists the Scope."""
        index = find_tree_row(FLAT)
        return model().rowCount(index) if index.isValid() else 0

    def filtered_rows(parent=QModelIndex()):
        """The filter's rows as it holds them; it has nothing to fetch."""
        m = filtered_model()
        result = []
        for r in range(m.rowCount(parent)):
            index = m.index(r, 0, parent)
            result.append(index)
            result.extend(filtered_rows(index))
        return result

    def find_filtered_row(path):
        for index in filtered_rows():
            if path_of(index) == path:
                return index
        return QModelIndex()

    def prop(name):
        return panel().property("natronSceneGraph" + name)

    def settled_on(node, count):
        return (prop("Node") == node.getScriptName() and not prop("ChangedHereBusy")
                and prop("ChangedHereCount") == count)

    def filter_on_with(count):
        return (prop("ChangedHereOnly") and prop("ChangedHereRows") == count and not prop("ChangedHereBusy")
                and filtered_tree().isVisible())

    def filter_off():
        return not prop("ChangedHereOnly") and tree().isVisible()

    def select_filtered(path):
        index = find_filtered_row(path)
        require(index.isValid(), "the filter has a row for %s" % (path,))
        selection = filtered_tree().selectionModel()
        selection.select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        selection.setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def inspector_path():
        return str(inspector().property("natronSceneGraphInspectorPath"))

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def view_input(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def main_thread_times_ms():
        """The calling thread's total time on a CPU, and total time ready to run without one."""
        with open("/proc/thread-self/schedstat") as f:
            fields = f.read().split()
        return int(fields[0]) / 1000000.0, int(fields[1]) / 1000000.0

    class Heartbeat:
        """The longest the main thread's event loop has gone without running a short timer.

        worst_cpu_ms is the most CPU one gap used, which is where a read on the main thread shows.
        worst_ms leaves out the time the thread was ready to run but had no CPU, of which a busy host
        adds hundreds of ms, so what it adds to worst_cpu_ms is time spent blocked.
        """

        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(HEARTBEAT_MS)
            self.timer.timeout.connect(self.beat)
            self.reset()

        def reset(self):
            self.last = time.monotonic()
            self.cpu, self.waited = main_thread_times_ms()
            self.worst_cpu_ms = 0.0
            self.worst_ms = 0.0
            self.worst_wall_ms = 0.0

        def beat(self):
            now = time.monotonic()
            cpu, waited = main_thread_times_ms()
            gap = (now - self.last) * 1000.0
            self.worst_wall_ms = max(self.worst_wall_ms, gap)
            self.worst_ms = max(self.worst_ms, gap - (waited - self.waited))
            self.worst_cpu_ms = max(self.worst_cpu_ms, cpu - self.cpu)
            self.last = now
            self.cpu = cpu
            self.waited = waited

        def start(self):
            self.reset()
            self.timer.start()

        def stop(self):
            self.timer.stop()
            return self.worst_ms

    def check_heartbeat(beat, worst, what):
        report("NOTE worst main-thread gap %s: %.1f ms on a CPU, %.1f ms with blocking, %.1f ms with waiting for a CPU"
               % (what, beat.worst_cpu_ms, worst, beat.worst_wall_ms))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "%s leaves the main thread idle (%.1f ms)" % (what, beat.worst_cpu_ms))
        check(worst <= HEARTBEAT_BLOCKED_BUDGET_MS, "%s does not block the main thread (%.1f ms)" % (what, worst))

    def steps():
        require(viewer.connectInput(0, edit), "connect the Viewer to the USDEditTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", lambda: view() is not None, POLL_TIMEOUT_S)
        require(ok, "the scene view is up")
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        button = child(QToolButton, "sceneGraphChangedHere")
        require(button is not None, "the panel has its Changed Here button")
        require(filtered_tree() is not None, "the panel has its Changed Here tree")
        require(inspector() is not None, "the panel has its inspector")

        ok = yield ("until", lambda: panel().isVisible() and ROOT in top_paths() and settled_on(edit, len(MARKED)), LOAD_TIMEOUT_S)
        require(ok, "the panel marks %d rows for the USDEditTransform (got %r, count %r)" % (len(MARKED), top_paths(), prop("ChangedHereCount")))
        marked = tree_marked_paths()
        check(marked == MARKED - {DEEP}, "the targets and their ancestors are marked, and no other row of the tree's small branches (got %r)" % (sorted(marked),))
        check(prop("ChangedHereRows") == 0, "the filter reads no row while the button has never been on (got %r)" % (prop("ChangedHereRows"),))
        check(tree().isVisible() and not filtered_tree().isVisible(), "the tree shows while the button is off")
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-marked.png")

        tree().expand(find_tree_row(ROOT))
        tree().expand(find_tree_row(ROOT + "/A"))
        tree().expand(find_tree_row(FLAT))
        yield ("sleep", SETTLE_MS)
        flat_before = flat_rows_read()
        check(flat_before < FLAT_PRIMS, "the tree has read part of the flat Scope (%d rows)" % (flat_before,))

        beat = Heartbeat()
        beat.start()
        button.click()
        ok = yield ("until", lambda: filter_on_with(len(MARKED)), LOAD_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        worst = beat.stop()
        require(ok, "the button shows the filter's rows (on %r, rows %r)" % (prop("ChangedHereOnly"), prop("ChangedHereRows")))
        check_heartbeat(beat, worst, "turning the filter on")
        shown = {path_of(i) for i in filtered_rows()}
        check(shown == MARKED, "the filter shows the marked rows, and no other (got %r)" % (sorted(shown),))
        check(not tree().isVisible(), "the tree is not showing under the filter")
        check(find_filtered_row(DEEP).isValid() and filtered_tree().isExpanded(find_filtered_row(FLAT)),
              "the deep marked prim shows through the filter")
        check(flat_rows_read() == flat_before, "the filter reads none of the tree's rows (%d, was %d)" % (flat_rows_read(), flat_before))
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-filtered.png")

        beat.start()
        button.click()
        ok = yield ("until", filter_off, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        worst = beat.stop()
        require(ok, "switching the button off shows the tree again")
        check_heartbeat(beat, worst, "turning the filter off")
        check(tree().isExpanded(find_tree_row(ROOT + "/A")) and tree().isExpanded(find_tree_row(FLAT)),
              "the tree keeps what was expanded")
        check(flat_rows_read() == flat_before, "the tree has read no more of the flat Scope (%d, was %d)" % (flat_rows_read(), flat_before))

        button.click()
        ok = yield ("until", lambda: filter_on_with(len(MARKED)), POLL_TIMEOUT_S)
        require(ok, "the button shows the filter's rows again")
        select_filtered(DEEP)
        ok = yield ("until", lambda: inspector_path() == DEEP, POLL_TIMEOUT_S)
        check(ok, "selecting the deep filtered row reaches the inspector (got %r)" % (inspector_path(),))
        select_filtered(TARGET)
        ok = yield ("until", lambda: inspector_path() == TARGET, POLL_TIMEOUT_S)
        check(ok, "selecting another filtered row reaches the inspector (got %r)" % (inspector_path(),))
        button.click()
        ok = yield ("until", lambda: filter_off() and [path_of(i) for i in tree().selectionModel().selectedRows()] == [TARGET], POLL_TIMEOUT_S)
        check(ok, "the filter's selection is the tree's when the button goes off (got %r)"
              % ([path_of(i) for i in tree().selectionModel().selectedRows()],))

        button.click()
        ok = yield ("until", lambda: filter_on_with(len(MARKED)), POLL_TIMEOUT_S)
        require(ok, "the button shows the filter's rows once more")
        require(view_input(usd), "switch the Viewer to the USDRead")
        ok = yield ("until", lambda: settled_on(usd, 0) and filter_on_with(0), LOAD_TIMEOUT_S)
        require(ok, "the panel follows the USDRead (node %r, count %r, rows %r)" % (prop("Node"), prop("ChangedHereCount"), prop("ChangedHereRows")))
        check(not filtered_rows(), "the filter shows no row of a node that authors nothing (got %r)" % ([path_of(i) for i in filtered_rows()],))
        yield ("sleep", SETTLE_MS)
        shot("scene-changed-here-none.png")

        button.click()
        ok = yield ("until", lambda: filter_off() and ROOT in top_paths(), LOAD_TIMEOUT_S)
        check(ok, "switching the button off shows the USDRead's scene")
        check(not tree_marked_paths(), "a node that authors nothing marks no row (got %r)" % (sorted(tree_marked_paths()),))
        for path in UNMARKED:
            check(find_tree_row(path).isValid(), "the tree lists %s" % (path,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
