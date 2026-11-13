"""Delegate viewer renders follow the viewer's draw states (M335 - Viewer Render Draw States).

Graph: USDRead of a four-building payload city (tools/bench/heavy_scene_make.py, one block of four
buildings, each its own payload) into Viewer1, every payload unloaded. B1..B3 author an extentsHint,
B0 does not. Under Storm the script sets Load All on /World/Block0/B1 and BBox on the other three
buildings, frames everything, then picks Cycles in the view's Renderer submenu, so the rules reach
the worker on the switch. It checks:
  - only B1's payload is opened, the other three stay unloaded;
  - the view keeps the rules and draws the boxed rows over the frame (natronSceneDrawBoxes and
    natronSceneDrawMarkers above 0);
  - clicks over a grid of the view (picked from the Cycles primId AOV and the boxes) find prims
    below B1 and none below another building;
  - BBox on B1 while Cycles renders reaches the rules (ruleCount) and the overlay (one more box),
    and B1's prims leave the frame: a later sweep picks no prim below any building.

If the Renderer submenu has no available Cycles entry the run is skipped (exit 0).

File: $NATRON_CITY_FILE, by default the city the script writes under $NATRON_GUI_TEST_OUT.

Screenshots: m335-cycles-one-asset, m335-cycles-all-boxed.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 300.0
REFINE_TIMEOUT_S = 180.0
STARTUP_TIMEOUT_S = 60.0
PICK_WAIT_S = 0.3
SETTLE_MS = 500
STORM_ID = "HdStormRendererPlugin"
CYCLES_ID = "HdCyclesPlugin"
MAX_SAMPLES = 4
GRID_COLUMNS = 12
GRID_ROWS = 8
BUILDINGS = 4
CITY_FLOORS = 4
# Twelve prims to a building, the fewest that fill four floors and leave room for cubes.
CITY_PRIMS = BUILDINGS * 12 + 2
BLOCK = "/World/Block0"
ROWS = ["%s/B%d" % (BLOCK, i) for i in range(BUILDINGS)]
LOADED = ROWS[1]
BOXED = [r for r in ROWS if r != LOADED]
PAYLOAD_MARK = "heavy_city_payloads"

try:
    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication
    from pxr import Sdf

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    city_path = os.environ.get("NATRON_CITY_FILE")
    if not city_path:
        sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
        import heavy_scene_make

        fixture_dir = os.path.realpath(os.path.join(OUT, "m335_render_draw_states"))
        os.makedirs(fixture_dir, exist_ok=True)
        heavy_scene_make.make_city(fixture_dir, CITY_PRIMS, 1, BUILDINGS, CITY_FLOORS)
        city_path = os.path.join(fixture_dir, "heavy_city.usda")
    require(os.path.isfile(city_path), "fixture exists: %s" % city_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
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

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def num(name, default=-1):
        value = prop(name)
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def draw_stats():
        value = prop("natronSceneDrawStats")
        return dict(value) if value else {}

    def drawn():
        return (int(num("natronSceneDrawBoxes", 0) or 0), int(num("natronSceneDrawMarkers", 0) or 0))

    def opened_payloads():
        return sorted(os.path.basename(layer.identifier) for layer in Sdf.Layer.GetLoadedLayers()
                      if PAYLOAD_MARK in layer.identifier)

    def payload_of(row):
        return "b0_%s.usdc" % row.rsplit("B", 1)[1]

    def selection():
        value = prop("natronSceneSelection")
        return sorted(str(p) for p in value) if value else []

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def activate_view():
        view().activateWindow()
        ok = yield ("until", lambda: view() is not None and view().isActiveWindow(), POLL_TIMEOUT_S)
        check(ok, "the scene view's window is active before input")
        view().setFocus()
        QApplication.processEvents()

    def frame_all():
        yield from activate_view()
        QTest.keyClick(view(), Qt.Key_A)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def popup():
        return QApplication.activePopupWidget()

    def post_context_menu(v, state):
        centre = v.rect().center()

        def post():
            press = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(v.mapToGlobal(centre)),
                                Qt.RightButton, Qt.RightButton, Qt.NoModifier)
            QApplication.sendEvent(v, press)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            state["closed"] = True

        QTimer.singleShot(0, post)

    def close_menus():
        for _ in range(4):
            p = popup()
            if p is None:
                break
            QTest.keyClick(p, Qt.Key_Escape)
            QApplication.processEvents()

    def open_renderer_submenu(state):
        """Steps that post the view's context menu and open its Renderer submenu; returns the
        submenu, or None."""
        activate_main_window()
        post_context_menu(view(), state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        if not ok:
            return None
        menu = hold(popup())
        actions = [a for a in menu.actions() if a.text().replace("&", "") == "Renderer" and a.menu() is not None]
        if not actions:
            return None
        # The submenu's wrapper lives only as long as the action's.
        action = hold(actions[0])
        sub = hold(action.menu())
        menu.setActiveAction(action)
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        return sub

    def finish_menu(state):
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)

    def sweep():
        """Steps that click a grid over the view; returns every path the clicks selected."""
        yield from activate_view()
        v = view()
        width, height = v.width(), v.height()
        found = set()
        previous = selection()
        for gy in range(GRID_ROWS):
            for gx in range(GRID_COLUMNS):
                pos = QPoint(int(width * (gx + 0.5) / GRID_COLUMNS), int(height * (gy + 0.5) / GRID_ROWS))
                mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
                mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
                QApplication.processEvents()
                # A click that picks what the last one did changes nothing to wait for.
                yield ("until", lambda: selection() != previous, PICK_WAIT_S)
                previous = selection()
                found.update(previous)
        return found

    def prims_below(paths, row):
        return sorted(p for p in paths if p.startswith(row + "/"))

    def prims_below_other_rows(paths, row):
        return sorted(p for p in paths for r in ROWS if r != row and p.startswith(r + "/"))

    def sweep_until(label, accept, timeout_sweeps):
        """Sweeps until accept(paths) holds, as the delegate restarts on a change; returns the last paths."""
        paths = set()
        for attempt in range(timeout_sweeps):
            paths = yield from sweep()
            if accept(paths):
                return paths
            view().update()
            yield ("sleep", SETTLE_MS)
        report("NOTE %s: last sweep picked %r" % (label, sorted(paths)[:20]))
        return paths

    def cycles_refined():
        return num("natronSceneSamples") >= 1 and num("natronSceneDrawnSamples") >= 1

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the scene view shows the city (%r)" % (prop("natronSceneError"),))
        check(prop("natronSceneRenderer") == STORM_ID, "the view starts on Storm (%r)" % (prop("natronSceneRenderer"),))
        QMetaObject.invokeMethod(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        check(opened_payloads() == [], "no payload file is open before any draw state (%r)" % (opened_payloads(),))

        section("one asset loaded, the rest boxed, under Storm")
        ok = QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", LOADED), Q_ARG("QString", "loadAll"))
        require(ok, "Load All on %s" % LOADED)
        ok = QMetaObject.invokeMethod(view(), "setDrawStateOnRows", Q_ARG("QStringList", BOXED), Q_ARG("QString", "bbox"))
        require(ok, "BBox on %r" % (BOXED,))
        ok = yield ("until", lambda: int(draw_stats().get("ruleCount", 0)) == len(ROWS), POLL_TIMEOUT_S)
        check(ok, "the view holds a rule for each building (%r)" % (draw_stats().get("ruleCount"),))
        ok = yield ("until", lambda: payload_of(LOADED) in opened_payloads() and not num("natronSceneLoadsPending", 0),
                    LOAD_TIMEOUT_S)
        check(ok, "Load All opens %s's payload (%r)" % (LOADED, opened_payloads()))
        yield from frame_all()

        section("Cycles")
        max_samples = viewer.getParam("maxSamples")
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the context menu has a Renderer submenu")
        labels = [a.text().replace("&", "") for a in sub.actions() if not a.isSeparator()]
        cycles = [a for a in sub.actions() if a.text().replace("&", "").startswith("Cycles")]
        if not cycles or not cycles[0].isEnabled() or "unavailable" in cycles[0].text():
            yield from finish_menu(state)
            report("SKIP Cycles is not available in this build (%r)" % (labels,))
            return
        cycles[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == CYCLES_ID, POLL_TIMEOUT_S)
        require(ok, "picking Cycles switches the view (%r)" % (prop("natronSceneRenderer"),))
        check(not prop("natronSceneRendererError"), "no renderer error on Cycles (%r)" % (prop("natronSceneRendererError"),))
        if max_samples is not None:
            max_samples.set(MAX_SAMPLES)
        ok = yield ("until", cycles_refined, REFINE_TIMEOUT_S)
        require(ok, "Cycles draws a frame (samples %r, drawn %r)" % (num("natronSceneSamples"), num("natronSceneDrawnSamples")))

        unrequested = [payload_of(r) for r in BOXED if payload_of(r) in opened_payloads()]
        check(len(unrequested) == 0, "unrequested payloads stay unloaded under Cycles (%d open: %r)" % (len(unrequested), unrequested))
        ok = yield ("until", lambda: drawn()[0] > 0 and sum(drawn()) == len(BOXED), POLL_TIMEOUT_S)
        check(ok, "the boxed buildings draw over the Cycles frame (boxes, markers %r)" % (drawn(),))
        check(int(draw_stats().get("ruleCount", 0)) == len(ROWS), "the rules survive the switch (%r)" % (draw_stats().get("ruleCount"),))

        paths = yield from sweep_until("one asset loaded",
                                       lambda p: bool(prims_below(p, LOADED)) and not prims_below_other_rows(p, LOADED), 5)
        check(bool(prims_below(paths, LOADED)), "clicks over the frame pick prims below %s (%r)" % (LOADED, prims_below(paths, LOADED)[:5]))
        check(not prims_below_other_rows(paths, LOADED),
              "no picked prim lies below another building (%r)" % (prims_below_other_rows(paths, LOADED)[:5],))
        shot("m335-cycles-one-asset.png")

        section("BBox the loaded asset while Cycles renders")
        restarts = num("natronSceneRestartCount")
        boxes_before = drawn()
        ok = QMetaObject.invokeMethod(view(), "setDrawState", Q_ARG("QString", LOADED), Q_ARG("QString", "bbox"))
        require(ok, "BBox on %s" % LOADED)
        ok = yield ("until", lambda: sum(drawn()) == len(ROWS), POLL_TIMEOUT_S)
        check(ok, "the overlay boxes %s too (boxes, markers %r, were %r)" % (LOADED, drawn(), boxes_before))
        ok = yield ("until", lambda: num("natronSceneRestartCount") > restarts, POLL_TIMEOUT_S)
        check(ok, "the change restarts the delegate (restarts %r -> %r)" % (restarts, num("natronSceneRestartCount")))
        ok = yield ("until", cycles_refined, REFINE_TIMEOUT_S)
        check(ok, "Cycles draws the new frame (samples %r)" % (num("natronSceneSamples"),))
        paths = yield from sweep_until("all boxed", lambda p: not any(prims_below(p, r) for r in ROWS), 5)
        check(not any(prims_below(paths, r) for r in ROWS),
              "%s's prims leave the frame (%r)" % (LOADED, sorted(p for r in ROWS for p in prims_below(paths, r))[:5]))
        check(payload_of(LOADED) in opened_payloads(), "boxing the row leaves its payload loaded (%r)" % (opened_payloads(),))
        unrequested = [payload_of(r) for r in BOXED if payload_of(r) in opened_payloads()]
        check(len(unrequested) == 0, "unrequested payloads stay unloaded (%d open: %r)" % (len(unrequested), unrequested))
        shot("m335-cycles-all-boxed.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
