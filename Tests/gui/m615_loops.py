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
    field drives its three dimensions, the driver row offers the block's variables (the solver's substep, dt and frame
    among them, the for-each's not), and the End's driven-knob table lists translate.x, .y and .z;
  - cache: with one substep, frames 1 to CACHE_FRAMES are shown; the timeline's cached line marks
    them, and seeking back to KEPT_FRAME shows the viewer's "cached sim frame" label;
  - progress: with a heartbeat running (a 5 ms Qt timer on the main thread, recording the
    wall-clock gap between ticks), the solver gets SLOW_SUBSTEPS substeps and the timeline jumps to
    JUMP_FRAME. The Progress panel must show a cancellable task within PROGRESS_TIMEOUT_S;
    clicking its cancel button must remove it within CANCEL_TIMEOUT_S and it must not come back,
    and the longest heartbeat gap must stay under MAX_GAP_MS.
Screenshots: m615_loops_hulls.png, m615_loops_drive_menu_foreach.png,
m615_loops_drive_menu_solver.png, m615_loops_table_foreach.png, m615_loops_table_solver.png,
m615_loops_cache.png, m615_loops_progress.png and m615_loops_cancelled.png in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m615_loops.py
"""
import os
import sys
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

try:
    from PySide6.QtCore import QMetaObject, QRect, QTimer
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QWidget

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

    gaps = {"last": None, "max_ms": 0.0, "ticks": 0}
    heartbeat = QTimer()
    heartbeat.setInterval(5)

    def tick():
        now = time.monotonic()
        if gaps["last"] is not None:
            gaps["max_ms"] = max(gaps["max_ms"], (now - gaps["last"]) * 1000.0)
        gaps["last"] = now
        gaps["ticks"] += 1

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
        heartbeat.start()
        param(solve_end, "substeps").setValue(SLOW_SUBSTEPS)
        py_viewer.seek(JUMP_FRAME)
        ok = yield ("until", lambda: cancel_button() is not None, PROGRESS_TIMEOUT_S)
        require(ok, "the Progress panel shows the solve with a cancel button within %g s" % PROGRESS_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        shot("m615_loops_progress.png")
        started = time.monotonic()
        cancel_button().click()
        ok = yield ("until", lambda: cancel_button() is None, CANCEL_TIMEOUT_S)
        took = time.monotonic() - started
        check(ok, "cancelling removes the task within %g s (%.2f s)" % (CANCEL_TIMEOUT_S, took))
        yield ("sleep", NO_RESTART_MS)
        check(cancel_button() is None, "the cancelled solve does not start again")
        heartbeat.stop()
        report("NOTE longest heartbeat gap %.1f ms over %d ticks" % (gaps["max_ms"], gaps["ticks"]))
        check(gaps["max_ms"] < MAX_GAP_MS,
              "the GUI thread never stalls for %g ms or more (%.1f ms)" % (MAX_GAP_MS, gaps["max_ms"]))
        shot("m615_loops_cancelled.png")
        param(solve_end, "substeps").setValue(1)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
