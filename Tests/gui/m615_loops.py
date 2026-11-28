"""Geometry loop blocks in the GUI: hulls, default names, loop drivers, solver progress and the
simulation cache line.

Graph: a GeoForEach block, GeoPrimitive -> GeoForEachBegin -> GeoTransform -> GeoForEachEnd, and
beside it a GeoSolver block, GeoPrimitive -> GeoSolverBegin -> GeoTransform -> GeoSolverEnd ->
GeoToUSD -> Viewer1.

Sections:
  - names: the GeoForEachEnd's Loop Name is loop1 and the GeoSolverEnd's loop2, and a third block
    end created after them takes loop3;
  - hulls: the graph's loop block item covers both blocks' begin, body and end nodes;
  - drive: in each block, Drive from loop (all dimensions) on the body GeoTransform's Translate x
    field drives its three dimensions, the driver row offers the block's variables (both offer
    iteration and frame; the solver's substep, numsubsteps and dt, the for-each's not), and the
    End's driven-knob table lists translate.x, .y and .z;
  - cache: with one substep, frames 1 to CACHE_FRAMES are shown; the timeline's cached line marks
    them, and seeking back to KEPT_FRAME shows the viewer's "cached sim frame" label;
  - progress: with a heartbeat running (a 5 ms Qt timer on the main thread, recording the
    wall-clock gap between ticks), the solver gets SLOW_SUBSTEPS substeps and the timeline jumps to
    JUMP_FRAME. The Progress panel must show a cancellable task within PROGRESS_TIMEOUT_S;
    clicking its cancel button must remove it within CANCEL_TIMEOUT_S and it must not come back,
    and the longest heartbeat gap must stay under MAX_GAP_MS. The screenshot taken meanwhile is the
    script's own work on the GUI thread, so the gap across it is left out;
  - pairs: the Loop menu's GeoForEach, GeoSolver and Loop entries each create their begin and end
    (fed by the selected GeoPrimitive for the geometry kinds), the begin feeding the end placed
    below it, both selected; one Ctrl+Z removes both and one Ctrl+Shift+Z restores them wired;
    the single begin and end entries stay.
    app.createLoopPair() of each kind returns [begin, end] wired the same way, and of an unknown
    kind creates nothing.
Screenshots: m615_loops_hulls.png, m615_loops_drive_menu_foreach.png,
m615_loops_drive_menu_solver.png, m615_loops_table_foreach.png, m615_loops_table_solver.png,
m615_loops_cache.png, m615_loops_progress.png and m615_loops_cancelled.png in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m615_loops.py
"""
import os
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 700
PROGRESS_TIMEOUT_S = float(os.environ.get("NATRON_TEST_PROGRESS_TIMEOUT_S", "20"))
CANCEL_TIMEOUT_S = float(os.environ.get("NATRON_TEST_CANCEL_TIMEOUT_S", "10"))
MAX_GAP_MS = float(os.environ.get("NATRON_TEST_MAX_GAP_MS", "100"))
NO_RESTART_MS = 2000
CACHE_FRAMES = 5
KEPT_FRAME = 3
JUMP_FRAME = 500
SLOW_SUBSTEPS = 20000
DRIVE_ITEM = "Drive from loop (all dimensions)"
CANCEL_TOOLTIP = "Cancel and remove the task from the list"
TRANSLATE_ROWS = ["translate.x", "translate.y", "translate.z"]
SOLVER_ONLY = ("loop.substep", "loop.numsubsteps", "loop.dt")
SHARED = ("loop.iteration", "loop.frame")
# GeoPrimitive's Type, GeoWrangle's Run Over, GeoSolverBegin's Method.
SHAPE_CUBE = 2
RUN_OVER_POINTS = 0
METHOD_PREV_FRAME = 0
BODY = "@P += {0, 1, 0} * @TimeInc;"
PROP_GROUPING = "natronGrouping"
PROP_PLUGIN_ID = "natronPluginID"
PROP_LOOP_PAIR = "natronLoopPair"
PAIRS = [
    ("GeoForEach", "GeoForEachBegin", "GeoForEachEnd"),
    ("GeoSolver", "GeoSolverBegin", "GeoSolverEnd"),
    ("Loop", "LoopBegin", "LoopEnd"),
]

try:
    import NatronGui
    from PySide6.QtCore import QMetaObject, QRect, Qt, QTimer
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QWidget

    import guitest as gt
    from guitest import OUT, check, plugin_problem, report, require, run, section, shot, widgets_of

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def root_names():
        return set(n.getScriptName() for n in app.getChildren())

    def new_nodes(before):
        return [n for n in app.getChildren() if n.getScriptName() not in before]

    def input_name(node, i):
        src = node.getInput(i)
        return src.getScriptName() if src is not None else None

    def loop_group_menu():
        for a in NatronGui.natron.nodesMenu().actions():
            if a.property(PROP_GROUPING) == "Loop" and gt.menu_of(a) is not None:
                return hold(gt.menu_of(a))
        return None

    def key_in_graph(key, mods):
        """Sends key to the node graph; undo and redo are window shortcuts, which only an active
        window receives."""
        win = next((w for w in QApplication.topLevelWidgets() if isinstance(w, QMainWindow) and w.isVisible()), None)
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active for the undo shortcuts")
        view = node_graph_view()
        if view is None:
            check(False, "the node graph is on screen for the undo shortcuts")
            return
        view.setFocus()
        QTest.keyClick(view, key, mods)
        QApplication.processEvents()

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getScriptName(), name))
        return hold(p)

    def node_graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def block_item():
        view = node_graph_view()
        if view is None:
            return None
        # NodeGraph.scene() raises "already deleted" in this binary; the view lists its items itself.
        try:
            for item in view.items():
                obj = item.toGraphicsObject()
                if obj is not None and obj.objectName() == "LoopBlockItem":
                    return hold(obj)
        except RuntimeError:
            return None
        return None

    def block_rect():
        item = block_item()
        if item is None:
            return None
        try:
            return item.sceneBoundingRect()
        except RuntimeError:
            return None

    def panel_of(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
        return None

    def open_panel(node):
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            app.selectNode(node, True)
            QApplication.processEvents()
            view = node_graph_view()
            if view is None or not QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel"):
                report("NOTE showSelectedNodeSettingsPanel was not invoked for %s (attempt %d)"
                       % (node.getScriptName(), attempt + 1))
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def child(panel, name):
        if panel is None:
            return None
        for w in panel.findChildren(QWidget, name):
            try:
                if w.isVisible():
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def knob_field(panel, script_name):
        """The first-dimension field of the knob named script_name: its tool tip opens with the script
        name in bold. A bare substring test also matches other knobs whose help text mentions it."""
        tag = "<b>%s</b>" % script_name
        if panel is None:
            return None
        for w in panel.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and tag in w.toolTip()[:60]:
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def row_variables(panel):
        """The items of the driver row's variable list on panel, None without a row."""
        combo = child(child(panel, "KnobDriverRow"), "driverVariable")
        return [str(v) for v in combo.property("natronItems")] if combo is not None else None

    def table_rows(node):
        """The parameter column of the driven-knob table on node's open panel, None without one."""
        table = child(panel_of(node), "LoopDriverTable")
        if table is None:
            return None
        return sorted(table.item(r, 1).text() for r in range(table.rowCount()) if table.item(r, 1) is not None)

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def right_click(field, trigger, shot_name):
        """Opens field's right-click menu and triggers the item named trigger; returns
        {"entries": [...], "item": (enabled, tooltip) or None}."""
        seen = {"entries": None, "item": None}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                check(False, "the right-click menu is open")
                seen["entries"] = []
                return
            hold(popup)
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            action = next((a for a in popup.actions() if a.text() == trigger), None)
            if action is not None:
                seen["item"] = (action.isEnabled(), action.toolTip())
            shot(shot_name)
            popup.close()
            if action is not None and action.isEnabled():
                action.trigger()

        QTimer.singleShot(800, inspect)
        field.customContextMenuRequested.emit(field.rect().center())
        return seen

    def timeline():
        visible = [w for w in widgets_of("TimeLineGui") if w.isVisible()]
        return hold(visible[0]) if visible else None

    def cached_frames():
        w = timeline()
        value = w.property("natronCachedFrames") if w is not None else None
        return sorted(int(t) for t in value) if value is not None else []

    def kept_label_shown():
        for w in QApplication.allWidgets():
            try:
                if w.objectName() == "keptSimulationFrame" and w.isVisible():
                    return True
            except RuntimeError:
                continue
        return False

    def cancel_button():
        """The enabled Progress panel cancel button. Button has no Q_OBJECT, so it reports its base
        class, QPushButton."""
        for panel in widgets_of("ProgressPanel"):
            hold(panel)
            for w in panel.findChildren(QWidget):
                try:
                    if gt.class_name(w) in ("Button", "QPushButton") and w.isEnabled() and CANCEL_TOOLTIP in w.toolTip():
                        return hold(w)
                except RuntimeError:
                    continue
        return None

    def scene_view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def scene_drawn():
        v = scene_view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def synced_at_ms():
        v = scene_view()
        return (v.property("natronSceneSyncedAtMs") or 0) if v is not None else 0

    def now_ms():
        return int(time.time() * 1000)

    gaps = {"last": None, "max_ms": 0.0, "ticks": 0, "sched": None, "started": None, "phase": "", "worst": ""}
    heartbeat = QTimer()
    heartbeat.setInterval(5)
    # Run time against run-queue wait of the GUI thread (/proc schedstat): a stall that is mostly
    # wait is the thread starved of a CPU, one that is mostly run is work done on it.
    schedstat = "/proc/self/task/%d/schedstat" % threading.get_native_id()

    def sched():
        try:
            with open(schedstat) as f:
                run_ns, wait_ns = f.read().split()[:2]
            return int(run_ns) / 1e6, int(wait_ns) / 1e6
        except (OSError, ValueError):
            return None

    def tick():
        now = time.monotonic()
        stat = sched()
        if gaps["last"] is not None:
            gap = (now - gaps["last"]) * 1000.0
            if gap > gaps["max_ms"]:
                gaps["max_ms"] = gap
                split = ""
                if stat is not None and gaps["sched"] is not None:
                    split = ", GUI thread ran %.1f ms and waited for a CPU %.1f ms" % (
                        stat[0] - gaps["sched"][0], stat[1] - gaps["sched"][1])
                gaps["worst"] = "%.1f ms at +%.2f s, %s%s" % (gap, now - gaps["started"], gaps["phase"], split)
        if gaps["started"] is None:
            gaps["started"] = now
        gaps["last"] = now
        gaps["sched"] = stat
        gaps["ticks"] += 1

    def unmeasured_shot(name):
        started = time.monotonic()
        shot(name)
        report("NOTE screenshot %s took %.1f ms, left out of the heartbeat" % (name, (time.monotonic() - started) * 1000.0))
        gaps["last"] = None
        gaps["sched"] = None

    heartbeat.timeout.connect(tick)

    app.getProjectParam("frameRange").set(1, JUMP_FRAME)

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        gt.close_all_panels()

        section("names")
        cube = make("GeoPrimitive", 0, 0)
        param(cube, "type").setValue(SHAPE_CUBE)
        each_begin = make("GeoForEachBegin", 0, 110)
        require(each_begin.connectInput(0, cube), "connect the GeoPrimitive into the GeoForEachBegin")
        each_move = make("GeoTransform", 0, 220)
        require(each_move.connectInput(0, each_begin), "connect the GeoForEachBegin into its GeoTransform")
        each_end = make("GeoForEachEnd", 0, 330)
        require(each_end.connectInput(0, each_move), "connect the GeoTransform into the GeoForEachEnd")

        sim_cube = make("GeoPrimitive", 600, 0)
        param(sim_cube, "type").setValue(SHAPE_CUBE)
        solve_begin = make("GeoSolverBegin", 600, 110)
        require(solve_begin.connectInput(0, sim_cube), "connect the GeoPrimitive into the GeoSolverBegin")
        param(solve_begin, "method").setValue(METHOD_PREV_FRAME)
        solve_body = make("GeoWrangle", 600, 220)
        require(solve_body.connectInput(0, solve_begin), "connect the GeoSolverBegin into the GeoWrangle")
        param(solve_body, "runOver").setValue(RUN_OVER_POINTS)
        param(solve_body, "snippet").setValue(BODY)
        solve_move = make("GeoTransform", 600, 330)
        require(solve_move.connectInput(0, solve_body), "connect the GeoWrangle into the solver's GeoTransform")
        solve_end = make("GeoSolverEnd", 600, 440)
        require(solve_end.connectInput(0, solve_move), "connect the GeoTransform into the GeoSolverEnd")
        to_usd = make("GeoToUSD", 600, 550)
        require(to_usd.connectInput(0, solve_end), "connect the GeoSolverEnd into the GeoToUSD")

        each_name = param(each_end, "loopName").getValue()
        solve_name = param(solve_end, "loopName").getValue()
        check(each_name == "loop1", "the first block end is named loop1 (got %r)" % each_name)
        check(solve_name == "loop2", "the solver after it is named loop2 (got %r)" % solve_name)
        extra = make("GeoForEachEnd", 1200, 0)
        extra_name = param(extra, "loopName").getValue()
        check(extra_name == "loop3", "a third block end is named loop3 (got %r)" % extra_name)
        extra.destroy()

        section("hulls")
        ok = yield ("until", lambda: block_rect() is not None and block_rect().width() > 0., POLL_TIMEOUT_S)
        require(ok, "the graph draws the loop blocks")
        view = node_graph_view()
        view.centerOn(300, 250)
        yield ("sleep", SETTLE_MS)
        rect = block_rect()
        for node in (each_begin, each_move, each_end, solve_begin, solve_body, solve_move, solve_end):
            x, y = node.getPosition()
            check(rect.contains(x, y), "the hulls cover %s" % node.getScriptName())
        for node in (cube, sim_cube):
            x, y = node.getPosition()
            check(not rect.contains(x - 60, y - 60), "the hulls stop short of %s" % node.getScriptName())
        shot("m615_loops_hulls.png")

        section("drive")
        for kind, move, end, is_solver in (("foreach", each_move, each_end, False),
                                           ("solver", solve_move, solve_end, True)):
            translate = param(move, "translate")
            ok = yield from open_panel(move)
            require(ok, "open %s's settings panel" % move.getScriptName())
            yield ("sleep", 600)
            field = knob_field(panel_of(move), "translate")
            require(field is not None, "found %s's translate field" % move.getScriptName())
            seen = right_click(field, DRIVE_ITEM, "m615_loops_drive_menu_%s.png" % kind)
            ok = yield ("until", lambda: seen["entries"] is not None, POLL_TIMEOUT_S)
            require(ok, "the translate menu opened")
            check(seen["item"] is not None and seen["item"][0],
                  "the %s body's menu has an enabled \"%s\" (got %r)" % (kind, DRIVE_ITEM, seen["entries"]))
            ok = yield ("until", lambda: all(translate.hasDriver(d) for d in range(3)), POLL_TIMEOUT_S)
            require(ok, "Drive from loop drives translate's three dimensions in the %s" % kind)
            ok = yield ("until", lambda: row_variables(panel_of(move)) is not None, POLL_TIMEOUT_S)
            require(ok, "the %s body's driver row shows" % kind)
            items = row_variables(panel_of(move))
            report("NOTE %s driver variables: %r" % (kind, items))
            for name in SHARED:
                check(name in items, "the %s's driver row offers %s" % (kind, name))
            name = end.getParam("loopName").getValue()
            check("loops.%s.iteration" % name in items, "the %s's driver row offers loops.%s.iteration" % (kind, name))
            for name in SOLVER_ONLY:
                if is_solver:
                    check(name in items, "the solver's driver row offers %s" % name)
                else:
                    check(name not in items, "the for-each's driver row does not offer %s" % name)

            ok = yield from open_panel(end)
            require(ok, "open %s's settings panel" % end.getScriptName())
            ok = yield ("until", lambda: table_rows(end) == TRANSLATE_ROWS, POLL_TIMEOUT_S)
            check(ok, "the %s End's table lists translate's dimensions (got %r)" % (kind, table_rows(end)))
            yield ("sleep", 300)
            screen_crop("m615_loops_table_%s.png" % kind, panel_of(end))
        gt.close_all_panels()

        section("cache")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        viewer.setPosition(600, 660)
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the viewer has a Python handle")
        require(viewer.connectInput(0, to_usd), "connect the Viewer to the GeoToUSD")
        ok = yield ("until", lambda: scene_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")

        def show(frame):
            asked_ms = now_ms()
            py_viewer.seek(frame)
            ok = yield ("until", lambda: app.timelineGetTime() == frame and synced_at_ms() >= asked_ms and scene_drawn(),
                        POLL_TIMEOUT_S)
            v = scene_view()
            check(ok, "the scene view shows frame %d (%r)" % (frame, v.property("natronSceneError") if v else None))

        for frame in range(1, CACHE_FRAMES + 1):
            yield from show(frame)
        wanted = list(range(1, CACHE_FRAMES + 1))
        ok = yield ("until", lambda: all(f in cached_frames() for f in wanted), POLL_TIMEOUT_S)
        check(ok, "the cached line marks the solved frames %r (got %r)" % (wanted, cached_frames()))
        yield from show(KEPT_FRAME)
        ok = yield ("until", kept_label_shown, POLL_TIMEOUT_S)
        check(ok, "the viewer says it shows a cached sim frame at frame %d" % KEPT_FRAME)
        yield ("sleep", SETTLE_MS)
        shot("m615_loops_cache.png")

        section("progress")
        gaps["last"] = None
        gaps["max_ms"] = 0.0
        gaps["ticks"] = 0
        gaps["started"] = None
        gaps["worst"] = ""
        gaps["phase"] = "while setting the substeps and jumping ahead"
        heartbeat.start()
        param(solve_end, "substeps").setValue(SLOW_SUBSTEPS)
        py_viewer.seek(JUMP_FRAME)
        gaps["phase"] = "while waiting for the progress task"
        ok = yield ("until", lambda: cancel_button() is not None, PROGRESS_TIMEOUT_S)
        require(ok, "the Progress panel shows the solve with a cancel button within %g s" % PROGRESS_TIMEOUT_S)
        gaps["phase"] = "while the solve runs"
        yield ("sleep", SETTLE_MS)
        unmeasured_shot("m615_loops_progress.png")
        started = time.monotonic()
        gaps["phase"] = "while cancelling"
        cancel_button().click()
        ok = yield ("until", lambda: cancel_button() is None, CANCEL_TIMEOUT_S)
        took = time.monotonic() - started
        check(ok, "cancelling removes the task within %g s (%.2f s)" % (CANCEL_TIMEOUT_S, took))
        gaps["phase"] = "after the cancel"
        yield ("sleep", NO_RESTART_MS)
        check(cancel_button() is None, "the cancelled solve does not start again")
        heartbeat.stop()
        report("NOTE longest heartbeat gap %.1f ms over %d ticks" % (gaps["max_ms"], gaps["ticks"]))
        if gaps["worst"]:
            report("NOTE longest heartbeat gap " + gaps["worst"])
        check(gaps["max_ms"] < MAX_GAP_MS,
              "the GUI thread never stalls for %g ms or more (%.1f ms)" % (MAX_GAP_MS, gaps["max_ms"]))
        shot("m615_loops_cancelled.png")
        param(solve_end, "substeps").setValue(1)

        section("pairs")
        loop_menu = loop_group_menu()
        require(loop_menu is not None, "the nodes menu has a Loop submenu")
        leaves = set(a.property(PROP_PLUGIN_ID) for a in loop_menu.actions() if a.property(PROP_PLUGIN_ID))
        for index, (kind, begin_id, end_id) in enumerate(PAIRS):
            check(begin_id in leaves and end_id in leaves,
                  "%s: the Loop menu still offers %s and %s alone" % (kind, begin_id, end_id))
            action = next((a for a in loop_menu.actions() if a.property(PROP_LOOP_PAIR) == kind), None)
            require(action is not None, "%s: the Loop menu has a %s entry" % (kind, kind))
            check(action.text() == kind, "%s: the entry reads %r" % (kind, action.text()))

            source = None
            app.clearSelection()
            if begin_id.startswith("Geo"):
                source = make("GeoPrimitive", 1200 + 300 * index, 0)
                app.selectNode(source, False)
            QApplication.processEvents()
            before = root_names()
            action.trigger()
            ok = yield ("until", lambda: len(new_nodes(before)) >= 2, POLL_TIMEOUT_S)
            created = new_nodes(before)
            require(ok and len(created) == 2, "%s: the entry creates two nodes (%r)"
                    % (kind, [n.getScriptName() for n in created]))
            begin = next((n for n in created if n.getPluginID() == begin_id), None)
            end = next((n for n in created if n.getPluginID() == end_id), None)
            require(begin is not None and end is not None,
                    "%s: the entry creates a %s and a %s" % (kind, begin_id, end_id))
            check(input_name(end, 0) == begin.getScriptName(),
                  "%s: the %s feeds the %s (%r)" % (kind, begin_id, end_id, input_name(end, 0)))
            if source is not None:
                check(input_name(begin, 0) == source.getScriptName(),
                      "%s: the selected node feeds the %s (%r)" % (kind, begin_id, input_name(begin, 0)))
            selected = sorted(n.getScriptName() for n in app.getSelectedNodes())
            check(selected == sorted([begin.getScriptName(), end.getScriptName()]),
                  "%s: both new nodes are selected (%r)" % (kind, selected))
            check(end.getPosition()[1] > begin.getPosition()[1] + begin.getSize()[1],
                  "%s: the %s is below the %s" % (kind, end_id, begin_id))
            names = (begin.getScriptName(), end.getScriptName())
            yield from key_in_graph(Qt.Key_Z, Qt.ControlModifier)
            ok = yield ("until", lambda: not (set(names) & root_names()), 5.0)
            check(ok, "%s: one undo removes both nodes (left %r)" % (kind, sorted(set(names) & root_names())))
            yield from key_in_graph(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier)
            ok = yield ("until", lambda: set(names) <= root_names(), 5.0)
            check(ok, "%s: one redo restores both nodes (have %r)" % (kind, sorted(set(names) & root_names())))
            if ok:
                check(input_name(end, 0) == begin.getScriptName(),
                      "%s: after the redo the %s feeds the %s again (%r)" % (kind, begin_id, end_id, input_name(end, 0)))
                end.destroy(False)
                begin.destroy(False)
            if source is not None:
                source.destroy(False)

            before = root_names()
            pair = app.createLoopPair(kind)
            require(len(pair) == 2, "%s: app.createLoopPair returns two nodes (%d)" % (kind, len(pair)))
            check(pair[0].getPluginID() == begin_id and pair[1].getPluginID() == end_id,
                  "%s: app.createLoopPair returns [%s, %s] (%s, %s)"
                  % (kind, begin_id, end_id, pair[0].getPluginID(), pair[1].getPluginID()))
            check(input_name(pair[1], 0) == pair[0].getScriptName(),
                  "%s: app.createLoopPair's begin feeds its end" % kind)
            check(pair[1].getPosition()[1] > pair[0].getPosition()[1] + pair[0].getSize()[1],
                  "%s: app.createLoopPair puts the end below the begin" % kind)
            check(len(new_nodes(before)) == 2, "%s: app.createLoopPair creates two nodes" % kind)
            pair[1].destroy(False)
            pair[0].destroy(False)
        check(app.createLoopPair("GeoSolverBegin") == [], "app.createLoopPair of an unknown kind creates nothing")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
