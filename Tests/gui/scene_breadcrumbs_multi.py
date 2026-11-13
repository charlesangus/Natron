"""The breadcrumb bar's "+N" dropdown over a multiple selection.

Graph: USDRead(a generated hierarchy) -> USDPrune -> Viewer1. The hierarchy has /geo/group_1 with 14
children and /geo/group_2 with 6. The prune removes nothing.

Checks:
  - with the 20 children selected, a click on the "+19" badge opens a popup listing 20 rows, the
    selected paths in selection order, with Level off;
  - Level 2 groups them as "/geo/group_1 (14)" and "/geo/group_2 (6)", and the spinner shows 2;
  - a grouped row's drag carries its ancestor in the Scene Graph's mime format;
  - a click on group_2 in the second row selects only /geo/group_2 and closes the popup;
  - a second click on the badge closes an open popup rather than opening it again;
  - with 100000 paths selected (paths the stage does not have: the list never reads the stage), the
    badge opens a popup of 100000 rows and Level 1 groups them into "/many (100000)", with the main
    thread within the stall budget throughout.

Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
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
MIME = "application/x-natron-prim-paths"
GROUP_1 = "/geo/group_1"
GROUP_2 = "/geo/group_2"
GROUP_1_COUNT = 14
GROUP_2_COUNT = 6
SELECTED = (["%s/a_%02d" % (GROUP_1, i) for i in range(GROUP_1_COUNT)]
            + ["%s/b_%02d" % (GROUP_2, i) for i in range(GROUP_2_COUNT)])
MANY_COUNT = 100000
MANY = ["/many/p_%06d" % i for i in range(MANY_COUNT)]
POPUP_NAME = "sceneBreadcrumbListPopup"
HEARTBEAT_MS = 10
HEARTBEAT_BUDGET_MS = 100.0
HEARTBEAT_BLOCKED_BUDGET_MS = 400.0


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "breadcrumbs_multi.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "geo"\n{\n')
        for group, prefix, count in (("group_1", "a", GROUP_1_COUNT), ("group_2", "b", GROUP_2_COUNT)):
            f.write('    def Xform "%s"\n    {\n' % group)
            for i in range(count):
                f.write('        def Cube "%s_%02d"\n        {\n            double size = 0.2\n'
                        '            double3 xformOp:translate = (%d, 0, 0)\n'
                        '            uniform token[] xformOpOrder = ["xformOp:translate"]\n        }\n' % (prefix, i, i))
            f.write('    }\n')
        f.write('}\n')
    return path


try:
    from PySide6.QtCore import Q_ARG, QMetaObject, QMimeData, Qt, QTimer
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QFrame, QSpinBox, QToolButton

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_breadcrumbs_multi")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    prune = app.createNode("USDPrune")
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    prune.getParam("primPattern").setValue("/nothing")
    require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
    usd.setPosition(0, 0)
    prune.setPosition(0, 100)
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

    def bar():
        bars = [w for w in widgets_of("SceneBreadcrumbBar") if w.isVisible()]
        return hold(bars[0]) if bars else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def selection():
        v = view()
        return [str(p) for p in (v.property("natronSceneSelection") or [])] if v is not None else []

    def select(paths):
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, Q_ARG("QStringList", list(paths)))

    def badge(b):
        found = [w for w in b.findChildren(QToolButton, "breadcrumbMore") if w.isVisibleTo(b)]
        return hold(found[0]) if found else None

    def popup_of(window):
        """The open list popup, a direct child of the bar's window: no walk over every widget, which
        the heartbeat would otherwise measure."""
        found = [w for w in window.findChildren(QFrame, POPUP_NAME, Qt.FindDirectChildrenOnly) if w.isVisible()]
        return hold(found[0]) if found else None

    def rows(p):
        return [str(r) for r in (p.property("natronListRows") or [])]

    def row_count(p):
        return int(p.property("natronListRowCount") or 0)

    def set_level(p, level):
        QMetaObject.invokeMethod(p, "setLevel", Qt.DirectConnection, Q_ARG("int", level))

    def click_segment(p, row, segment):
        QMetaObject.invokeMethod(p, "clickSegment", Qt.DirectConnection, Q_ARG("int", row), Q_ARG("int", segment))

    def row_mime(p, row):
        QMetaObject.invokeMethod(p, "exposeMimeData", Qt.DirectConnection, Q_ARG("int", row))
        return hold(p.findChild(QMimeData, "breadcrumbListMime"))

    def main_thread_times_ms():
        with open("/proc/thread-self/schedstat") as f:
            fields = f.read().split()
        return int(fields[0]) / 1000000.0, int(fields[1]) / 1000000.0

    class Heartbeat:
        """The longest the main thread's event loop has gone without running a short timer: the most
        CPU one gap used, and the gap's length with the time spent ready to run without a CPU left out."""

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

        def beat(self):
            now = time.monotonic()
            cpu, waited = main_thread_times_ms()
            self.worst_ms = max(self.worst_ms, (now - self.last) * 1000.0 - (waited - self.waited))
            self.worst_cpu_ms = max(self.worst_cpu_ms, cpu - self.cpu)
            self.last, self.cpu, self.waited = now, cpu, waited

        def start(self):
            self.reset()
            self.timer.start()

        def stop(self):
            self.timer.stop()

    def check_heartbeat(beat, what):
        report("NOTE worst main-thread gap %s: %.1f ms on a CPU, %.1f ms with blocking" % (what, beat.worst_cpu_ms, beat.worst_ms))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "%s leaves the main thread idle (%.1f ms)" % (what, beat.worst_cpu_ms))
        check(beat.worst_ms <= HEARTBEAT_BLOCKED_BUDGET_MS, "%s does not block the main thread (%.1f ms)" % (what, beat.worst_ms))

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: bar() is not None, POLL_TIMEOUT_S)
        require(ok, "the breadcrumb bar is visible")
        b = bar()
        window = hold(b.window())

        select(SELECTED)
        ok = yield ("until", lambda: badge(b) is not None and int(b.property("natronBreadcrumbMore") or 0) == len(SELECTED) - 1,
                    POLL_TIMEOUT_S)
        require(ok, "the bar shows a +%d badge (got %r)" % (len(SELECTED) - 1, b.property("natronBreadcrumbMore")))
        QTest.mouseClick(badge(b), Qt.LeftButton)
        ok = yield ("until", lambda: popup_of(window) is not None, POLL_TIMEOUT_S)
        require(ok, "a click on the badge opens the list popup")
        p = popup_of(window)
        check(row_count(p) == len(SELECTED), "the popup lists %d rows (got %d)" % (len(SELECTED), row_count(p)))
        check(rows(p) == SELECTED, "the rows are the selected paths in order (got %r)" % (rows(p)[:4],))
        check(int(p.property("natronListLevel")) == 0, "Level starts off (got %r)" % (p.property("natronListLevel"),))
        shot("scene_breadcrumbs_multi_list.png")

        set_level(p, 2)
        expected = ["%s (%d)" % (GROUP_1, GROUP_1_COUNT), "%s (%d)" % (GROUP_2, GROUP_2_COUNT)]
        ok = yield ("until", lambda: rows(p) == expected, POLL_TIMEOUT_S)
        check(ok, "Level 2 groups the rows as %r (got %r)" % (expected, rows(p)))
        spin = hold(p.findChild(QSpinBox, "breadcrumbListLevel"))
        check(spin is not None and spin.value() == 2, "the Level spinner shows 2 (got %r)" % (spin.value() if spin else None,))
        shot("scene_breadcrumbs_multi_grouped.png")

        mime = row_mime(p, 1)
        require(mime is not None and mime.hasFormat(MIME), "a grouped row makes %s mime data" % MIME)
        carried = bytes(mime.data(MIME)).decode("utf-8").split("\n")
        check(carried == [GROUP_2], "the group_2 row's drag carries its ancestor (got %r)" % (carried,))
        check(mime.text() == GROUP_2, "the row's mime data has the path as text (got %r)" % (mime.text(),))

        click_segment(p, 1, 1)
        ok = yield ("until", lambda: selection() == [GROUP_2], POLL_TIMEOUT_S)
        check(ok, "a click on group_2 in the second row selects only %s (got %r)" % (GROUP_2, selection()[:4]))
        ok = yield ("until", lambda: popup_of(window) is None, POLL_TIMEOUT_S)
        check(ok, "the popup closes once a name is clicked")

        select(SELECTED)
        ok = yield ("until", lambda: badge(b) is not None and int(b.property("natronBreadcrumbMore") or 0) == len(SELECTED) - 1,
                    POLL_TIMEOUT_S)
        require(ok, "the badge is back with the 20 prims selected")
        QTest.mouseClick(badge(b), Qt.LeftButton)
        ok = yield ("until", lambda: popup_of(window) is not None, POLL_TIMEOUT_S)
        require(ok, "the badge opens the popup again")
        yield ("sleep", SETTLE_MS)
        QTest.mouseClick(badge(b), Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        check(popup_of(window) is None, "a second click on the badge closes the popup")
        p = popup_of(window)
        if p is not None:
            p.close()
            yield ("until", lambda: popup_of(window) is None, POLL_TIMEOUT_S)

        select(MANY)
        ok = yield ("until", lambda: int(b.property("natronBreadcrumbMore") or 0) == MANY_COUNT - 1 and badge(b) is not None,
                    LOAD_TIMEOUT_S)
        require(ok, "selectPaths takes %d paths the stage does not have (got +%r)" % (MANY_COUNT, b.property("natronBreadcrumbMore")))
        more = badge(b)
        yield ("sleep", SETTLE_MS)
        beat = Heartbeat()
        beat.start()
        QTest.mouseClick(more, Qt.LeftButton)
        ok = yield ("until", lambda: popup_of(window) is not None, POLL_TIMEOUT_S)
        require(ok, "the badge opens the popup over %d paths" % (MANY_COUNT,))
        p = popup_of(window)
        check(row_count(p) == MANY_COUNT, "the popup lists %d rows (got %d)" % (MANY_COUNT, row_count(p)))
        check(rows(p)[:2] == MANY[:2], "the first rows are the first paths (got %r)" % (rows(p)[:2],))
        yield ("sleep", SETTLE_MS)
        set_level(p, 1)
        ok = yield ("until", lambda: rows(p) == ["/many (%d)" % MANY_COUNT], POLL_TIMEOUT_S)
        check(ok, "Level 1 groups %d paths into one row (got %r)" % (MANY_COUNT, rows(p)[:2]))
        set_level(p, 2)
        ok = yield ("until", lambda: row_count(p) == MANY_COUNT, POLL_TIMEOUT_S)
        check(ok, "Level 2 makes one group per path (got %d rows)" % (row_count(p),))
        yield ("sleep", SETTLE_MS)
        beat.stop()
        check_heartbeat(beat, "listing and grouping %d selected paths" % (MANY_COUNT,))
        shot("scene_breadcrumbs_multi_many.png")
        p.close()
        yield ("until", lambda: popup_of(window) is None, POLL_TIMEOUT_S)
        select([])

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
