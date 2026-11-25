"""The Scene Graph panel's filter as a path expression editor.

Graph: a USDRead of a scene the script writes to $NATRON_GUI_TEST_OUT, into Viewer1, and a USDPrune
reading it off to the side with its settings panel open, so that it is the current node. The scene
holds two components under a group, a plain Cube, an inactive Xform, a payload the viewer leaves
unloaded whose layer holds a component, and a Scope of FLAT_PRIMS children.

Checks:
  - //{kind:component} lists only the two components, selects them in the 3D view and their rows
    in the panel, and reports the unloaded payload as one branch not searched;
  - the walk over the flat Scope keeps the main thread's heartbeat within HEARTBEAT_BUDGET_MS of CPU
    and HEARTBEAT_BLOCKED_BUDGET_MS with blocking;
  - Apply To Node writes the expression, as typed, to the USDPrune's Prim Pattern, and one Undo
    restores the pattern before it;
  - Apply To Node clicked right after an edit, before the field's debounce, writes the edited text;
  - /World/Flat/* stops at the 1000-match cap with a More row and selects those matches, and More
    lists and selects the next 1000;
  - an expression that does not parse lists nothing, shows the parse error beside the field and
    disables Apply To Node;
  - {loaded:false} finds the unloaded payload and {active:false} the inactive prim;
  - text without /, { or % is still a name search.

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

WORLD = "/World"
CHAIR = "/World/Props/Chair"
TABLE = "/World/Props/Table"
COMPONENTS = sorted([CHAIR, TABLE])
OFF = "/World/Off"
LAZY = "/World/Lazy"
MARKER = "/World/Ground"
COMPONENT_EXPRESSION = "//{kind:component}"
INVALID_EXPRESSION = "//{kind:component"
FLAT_EXPRESSION = "/World/Flat/*"
# The Scene Graph panel lists this many matches before its More row.
FILTER_CAP = 1000


def write_scene(directory):
    """Writes the root layer and the payload's, and returns the root's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)
    with open(os.path.join(payloads, "lazy.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Lazy"\n)\n'
                'def Xform "Lazy" (\n    kind = "group"\n)\n{\n'
                '    def Xform "Inner" (\n        kind = "component"\n    )\n    {\n'
                '        def Cube "Geo"\n        {\n            double size = 0.5\n        }\n    }\n}\n')
    path = os.path.join(directory, "scene_filter_pattern.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n\n')
        f.write('def Xform "World" (\n    kind = "assembly"\n)\n{\n')
        f.write('    def Xform "Props" (\n        kind = "group"\n    )\n    {\n')
        for name, x in (("Chair", -2), ("Table", 2)):
            f.write('        def Xform "%s" (\n            kind = "component"\n        )\n        {\n'
                    '            double3 xformOp:translate = (%d, 0, 0)\n'
                    '            uniform token[] xformOpOrder = ["xformOp:translate"]\n'
                    '            def Cube "Geo"\n            {\n                double size = 1\n            }\n'
                    '        }\n' % (name, x))
        f.write('    }\n\n')
        f.write('    def Cube "Ground"\n    {\n        double size = 0.25\n    }\n\n')
        f.write('    def Xform "Off" (\n        active = false\n    )\n    {\n'
                '        def Cube "Geo"\n        {\n        }\n    }\n\n')
        f.write('    def Xform "Lazy" (\n        prepend payload = @./payloads/lazy.usda@\n    )\n    {\n    }\n\n')
        f.write('    def Scope "Flat"\n    {\n')
        for n in range(FLAT_PRIMS):
            f.write('        def Xform "P%d"\n        {\n        }\n' % n)
        f.write('    }\n}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QModelIndex, Qt, QTimer
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QListView, QMainWindow, QToolButton, QTreeView

    from guitest import OUT, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    fixture_dir = os.path.realpath(os.path.join(OUT, "scene_filter_pattern"))
    scene_path = write_scene(fixture_dir)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    prune = app.createNode("USDPrune", -1, None, props)
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    pattern = prune.getParam("primPattern")
    require(pattern is not None, "USDPrune has a primPattern parameter")
    pattern.setValue(MARKER)
    usd.setPosition(0, 0)
    viewer.setPosition(0, 200)
    prune.setPosition(200, 100)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def viewer_selection():
        v = view()
        value = v.property("natronSceneSelection") if v is not None else None
        return sorted(str(p) for p in value) if value else []

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

    def results():
        return child(QListView, "sceneGraphResults")

    def field():
        return child(QLineEdit, "sceneGraphFilter")

    def apply_button():
        return child(QToolButton, "sceneGraphApplyToNode")

    def message():
        return child(QLabel, "sceneGraphFilterMessage")

    def prop(name):
        return panel().property("natronSceneGraph" + name)

    def top_paths():
        t = tree()
        try:
            m = hold(t.model()) if t is not None else None
        except RuntimeError:
            return []
        if m is None:
            return []
        return [str(m.index(r, 0, QModelIndex()).data(PATH_ROLE)) for r in range(m.rowCount(QModelIndex()))]

    def result_paths():
        m = hold(results().model())
        return sorted(str(m.index(r, 0).data()) for r in range(m.rowCount()))

    def selected_results():
        return sorted(str(i.data()) for i in results().selectionModel().selectedRows())

    def filter_done():
        return not prop("FilterBusy")

    def set_filter(text):
        field().setText(text)
        QApplication.processEvents()

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui":
                return w
        return None

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def undo():
        action = edit_menu_action("undo")
        require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
        action.trigger()
        QApplication.processEvents()

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
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(panel() is not None, "the Scene Graph panel exists")
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        for name, w in (("filter field", field()), ("results list", results()), ("Apply To Node button", apply_button()),
                        ("filter message", message())):
            require(w is not None, "the panel has its " + name)
        ok = yield ("until", lambda: WORLD in top_paths(), LOAD_TIMEOUT_S)
        require(ok, "the panel lists the scene (got %r)" % (top_paths(),))
        check("/**" not in field().toolTip() and "//" in field().toolTip(),
              "the filter's help uses // for any depth, never /**")
        check(not apply_button().isEnabled(), "Apply To Node is disabled with no expression")

        beat = Heartbeat()
        beat.start()
        set_filter(COMPONENT_EXPRESSION)
        ok = yield ("until", lambda: filter_done() and prop("FilterExpression") and result_paths() == COMPONENTS, LOAD_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        worst = beat.stop()
        require(ok, "%s lists only the components (got %r, busy %r)" % (COMPONENT_EXPRESSION, result_paths(), prop("FilterBusy")))
        check_heartbeat(beat, worst, "matching %s over the flat Scope" % (COMPONENT_EXPRESSION,))
        ok = yield ("until", lambda: viewer_selection() == COMPONENTS, POLL_TIMEOUT_S)
        check(ok, "the components are selected in the 3D view (got %r)" % (viewer_selection(),))
        check(selected_results() == COMPONENTS, "the components' rows are selected in the panel (got %r)" % (selected_results(),))
        check(prop("FilterUnloaded") == 1, "the filter counts the unloaded payload as a branch not searched (got %r)" % (prop("FilterUnloaded"),))
        check(message().isVisible() and "1 unloaded branch" in message().text(),
              "the panel reports the unsearched branch beside the field (got %r)" % (message().text(),))
        check(not prop("FilterError"), "a valid expression has no error (got %r)" % (prop("FilterError"),))
        shot("scene-filter-pattern-components.png")

        require(apply_button().isEnabled(), "Apply To Node is enabled for a valid expression")
        apply_button().click()
        QApplication.processEvents()
        check(str(pattern.getValue()) == COMPONENT_EXPRESSION,
              "Apply To Node writes the expression to the USDPrune's pattern (got %r)" % (pattern.getValue(),))
        undo()
        check(str(pattern.getValue()) == MARKER, "one Undo restores the pattern before it (got %r)" % (pattern.getValue(),))

        # Clicked at once, before the field's edit is debounced into a new filter.
        set_filter("//{active:false}")
        require(apply_button().isEnabled(), "Apply To Node is still enabled from the last expression")
        apply_button().click()
        QApplication.processEvents()
        check(str(pattern.getValue()) == "//{active:false}",
              "Apply To Node right after an edit writes the text in the field (got %r)" % (pattern.getValue(),))
        undo()
        check(str(pattern.getValue()) == MARKER, "one Undo restores the pattern again (got %r)" % (pattern.getValue(),))

        def result_rows():
            return hold(results().model()).rowCount()

        set_filter(FLAT_EXPRESSION)
        ok = yield ("until", lambda: filter_done() and result_rows() == FILTER_CAP + 1, LOAD_TIMEOUT_S)
        require(ok, "%s stops at the result cap with a More row (got %d rows)" % (FLAT_EXPRESSION, result_rows()))
        ok = yield ("until", lambda: len(viewer_selection()) == FILTER_CAP, POLL_TIMEOUT_S)
        check(ok, "the capped matches are selected (got %d)" % (len(viewer_selection()),))
        model = hold(results().model())
        results().clicked.emit(model.index(model.rowCount() - 1, 0))
        QApplication.processEvents()
        ok = yield ("until", lambda: filter_done() and result_rows() == 2 * FILTER_CAP + 1, LOAD_TIMEOUT_S)
        require(ok, "More lists the next matches (got %d rows)" % (result_rows(),))
        ok = yield ("until", lambda: len(viewer_selection()) == 2 * FILTER_CAP, POLL_TIMEOUT_S)
        check(ok, "the matches More lists are selected too (got %d)" % (len(viewer_selection()),))

        set_filter(INVALID_EXPRESSION)
        ok = yield ("until", lambda: filter_done() and bool(prop("FilterError")), POLL_TIMEOUT_S)
        require(ok, "%s shows an error" % (INVALID_EXPRESSION,))
        check(message().isVisible() and message().text() == str(prop("FilterError")),
              "the parse error shows beside the field (got %r)" % (message().text(),))
        check(INVALID_EXPRESSION in message().text(), "the error names the expression (got %r)" % (message().text(),))
        check(result_paths() == [], "an invalid expression lists nothing (got %r)" % (result_paths(),))
        check(not apply_button().isEnabled(), "Apply To Node is disabled for an invalid expression")
        yield ("sleep", SETTLE_MS)
        shot("scene-filter-pattern-error.png")
        apply_button().click()
        QApplication.processEvents()
        check(str(pattern.getValue()) == MARKER, "nothing is written for an invalid expression (got %r)" % (pattern.getValue(),))

        set_filter("//{loaded:false}")
        ok = yield ("until", lambda: filter_done() and not prop("FilterError") and result_paths() == [LAZY], LOAD_TIMEOUT_S)
        check(ok, "{loaded:false} finds the unloaded payload (got %r, error %r)" % (result_paths(), prop("FilterError")))
        set_filter("//{active:false}")
        ok = yield ("until", lambda: filter_done() and not prop("FilterError") and result_paths() == [OFF], LOAD_TIMEOUT_S)
        check(ok, "{active:false} finds the inactive prim (got %r, error %r)" % (result_paths(), prop("FilterError")))

        set_filter("Chair")
        ok = yield ("until", lambda: filter_done() and not prop("FilterExpression") and result_paths() == [CHAIR], LOAD_TIMEOUT_S)
        check(ok, "text without /, { or %% is a name search (got %r, expression %r)" % (result_paths(), prop("FilterExpression")))
        check(not apply_button().isEnabled(), "Apply To Node is disabled for a name search")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
