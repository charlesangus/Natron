"""Progressive viewer render (M254 - Progressive Viewer Render): with Cycles picked in the scene
view's Renderer submenu the view refines in the background, its samples counter rises, Stop halts
it where it is, Resume continues from that count, and Max Samples caps it.

Graph: USDCube into Viewer1. Once Storm has drawn the cube the script picks Cycles through the
view's context menu (posted, as the menu is modal). If the submenu has no available Cycles entry
the run is skipped (exit 0). Then it:
  - waits for natronSceneSamples to rise, and checks natronSceneDrawnSamples, the count of the
    frame on screen, follows the engine's natronSceneSamples;
  - triggers Stop: natronSceneRefinementStopped is set and the count stays put; Resume: the count
    rises from where it stopped, without a restart (natronSceneRestartCount unchanged);
  - types a Max Samples into the submenu's "maxSamples" spin box: the render restarts and
    converges to that count;
  - with a large cap, times the Qt event loop with a 0 ms timer while the view refines at its
    normal size and then at 3840x2160, recording natronSceneUploadMs and natronSceneUploadPixels.
    The longest gap must stay under 33 ms.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
REFINE_TIMEOUT_S = 120.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
STORM_ID = "HdStormRendererPlugin"
CYCLES_ID = "HdCyclesPlugin"
CAPPED_SAMPLES = 24
LONG_SAMPLES = 1 << 20
PROBE_MS = 2000
LOOP_LIMIT_MS = 33.0

try:
    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QSpinBox

    from guitest import check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    cube = app.createNode("USDCube")
    require(cube is not None, "create USDCube", plugin_problem("USDCube"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    cube.setPosition(0, 0)
    viewer.setPosition(0, 150)

    _held = []

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

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

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def popup():
        return QApplication.activePopupWidget()

    def submenu_of(menu, title):
        for action in menu.actions():
            if action.text().replace("&", "") == title and action.menu() is not None:
                return action
        return None

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
        v = view()
        post_context_menu(v, state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        if not ok:
            return None
        menu = popup()
        _held.append(menu)
        action = submenu_of(menu, "Renderer")
        if action is None:
            return None
        sub = action.menu()
        _held.append(sub)
        menu.setActiveAction(action)
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        return sub

    def finish_menu(state):
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def entry(sub, text):
        found = [a for a in sub.actions() if a.text().replace("&", "") == text]
        return found[0] if found else None

    def trigger(text):
        """Steps that pick the Renderer submenu entry text; returns whether it was there and enabled."""
        state = {}
        sub = yield from open_renderer_submenu(state)
        action = entry(sub, text) if sub is not None else None
        enabled = action is not None and action.isEnabled()
        if enabled:
            action.trigger()
        yield from finish_menu(state)
        return enabled

    def set_max_samples(count):
        """Steps that type count into the submenu's Max Samples box; returns whether the box was there."""
        state = {}
        sub = yield from open_renderer_submenu(state)
        spin = sub.findChild(QSpinBox, "maxSamples") if sub is not None else None
        if spin is not None:
            _held.append(spin)
            spin.setFocus()
            spin.selectAll()
            QTest.keyClicks(spin, str(count))
            QTest.keyClick(spin, Qt.Key_Return)
            QApplication.processEvents()
        yield from finish_menu(state)
        return spin is not None

    def probe_event_loop(label):
        """Steps that time the event loop for PROBE_MS while the view refines; returns the longest gap in ms."""
        gaps = []
        last = [time.perf_counter()]
        timer = QTimer()
        timer.setInterval(0)

        def tick():
            now = time.perf_counter()
            gaps.append((now - last[0]) * 1000.)
            last[0] = now

        timer.timeout.connect(tick)
        timer.start()
        samples_before = num("natronSceneSamples")
        yield ("sleep", PROBE_MS)
        timer.stop()
        longest = max(gaps) if gaps else float("inf")
        mean = (sum(gaps) / len(gaps)) if gaps else float("inf")
        v = view()
        report("NOTE %s: view %dx%d, %d loop iterations, longest %.1f ms, mean %.2f ms, upload %.2f ms for %d pixels, samples %r -> %r"
               % (label, v.width(), v.height(), len(gaps), longest, mean, float(num("natronSceneUploadMs", 0.)),
                  int(num("natronSceneUploadPixels", 0)), samples_before, num("natronSceneSamples")))
        return longest

    def steps():
        require(viewer.connectInput(0, cube), "connect the Viewer to the USDCube")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the cube (%r)" % (prop("natronSceneError"),))
        yield ("sleep", SETTLE_MS)
        check(prop("natronSceneRenderer") == STORM_ID, "the view starts on Storm (%r)" % (prop("natronSceneRenderer"),))

        # Cycles, when this build has it.
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the context menu has a Renderer submenu")
        labels = [a.text().replace("&", "") for a in sub.actions() if not a.isSeparator()]
        cycles = [a for a in sub.actions() if a.text().replace("&", "").startswith("Cycles")]
        report("NOTE Renderer submenu: %r" % (labels,))
        if not cycles or not cycles[0].isEnabled() or "unavailable" in cycles[0].text():
            yield from finish_menu(state)
            report("SKIP Cycles is not available in this build (%r)" % (labels,))
            return
        check(entry(sub, "Stop") is None, "Storm's submenu has no refinement controls")
        cycles[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == CYCLES_ID, POLL_TIMEOUT_S)
        require(ok, "picking Cycles switches the view (%r)" % (prop("natronSceneRenderer"),))
        check(not prop("natronSceneRendererError"), "no renderer error on Cycles (%r)" % (prop("natronSceneRendererError"),))

        # The counter rises, and the frame on screen follows the engine.
        ok = yield ("until", lambda: num("natronSceneSamples") >= 1, REFINE_TIMEOUT_S)
        require(ok, "Cycles reports samples (%r)" % (num("natronSceneSamples"),))
        report("NOTE default Max Samples %r" % (num("natronSceneMaxSamples"),))
        check(num("natronSceneMaxSamples") > 0, "Max Samples defaults to the delegate's count (%r)" % (num("natronSceneMaxSamples"),))
        ok = yield from set_max_samples(LONG_SAMPLES)
        require(ok, "the Renderer submenu has a Max Samples box on Cycles")
        ok = yield ("until", lambda: num("natronSceneMaxSamples") == LONG_SAMPLES, POLL_TIMEOUT_S)
        check(ok, "Max Samples takes the typed count (%r)" % (num("natronSceneMaxSamples"),))
        first = num("natronSceneSamples")
        ok = yield ("until", lambda: num("natronSceneSamples") >= max(first, 1) + 2, REFINE_TIMEOUT_S)
        check(ok, "the samples counter rises (%r -> %r)" % (first, num("natronSceneSamples")))
        ok = yield ("until", lambda: num("natronSceneDrawnSamples") >= 1, POLL_TIMEOUT_S)
        check(ok, "the view draws refined frames (drawn samples %r)" % (num("natronSceneDrawnSamples"),))
        shot("m254-cycles-refining.png")

        # Stop holds the count; Resume continues from it.
        restarts = num("natronSceneRestartCount")
        enabled = yield from trigger("Stop")
        check(enabled, "Stop is enabled while the view refines")
        ok = yield ("until", lambda: bool(prop("natronSceneRefinementStopped")), POLL_TIMEOUT_S)
        check(ok, "Stop sets natronSceneRefinementStopped")
        yield ("sleep", SETTLE_MS)
        stopped = num("natronSceneSamples")
        yield ("sleep", 1500)
        check(num("natronSceneSamples") == stopped, "the count holds while stopped (%r -> %r)" % (stopped, num("natronSceneSamples")))
        view().update()
        ok = yield ("until", lambda: num("natronSceneDrawnSamples") == stopped, POLL_TIMEOUT_S)
        check(ok, "the counter on screen matches the engine's (drawn %r, engine %r)" % (num("natronSceneDrawnSamples"), stopped))
        shot("m254-cycles-stopped.png")
        state = {}
        sub = yield from open_renderer_submenu(state)
        if sub is not None:
            check(not entry(sub, "Stop").isEnabled() and entry(sub, "Resume").isEnabled(),
                  "while stopped, Stop is disabled and Resume enabled")
            yield ("sleep", SETTLE_MS)
            shot("m254-renderer-submenu.png")
        yield from finish_menu(state)
        enabled = yield from trigger("Resume")
        check(enabled, "Resume is enabled while stopped")
        ok = yield ("until", lambda: not prop("natronSceneRefinementStopped") and num("natronSceneSamples") > stopped, REFINE_TIMEOUT_S)
        check(ok, "Resume continues past the stopped count (%r -> %r)" % (stopped, num("natronSceneSamples")))
        check(num("natronSceneSamples") >= stopped and num("natronSceneRestartCount") == restarts,
              "Resume does not restart the render (restarts %r -> %r)" % (restarts, num("natronSceneRestartCount")))

        # Responsiveness while refining, at the view's size and at 4K.
        activate_main_window()
        longest = yield from probe_event_loop("normal size")
        check(longest < LOOP_LIMIT_MS, "the event loop stays under %d ms while refining (longest %.1f ms)" % (LOOP_LIMIT_MS, longest))
        v = view()
        old_min = v.minimumSize()
        v.setMinimumSize(3840, 2160)
        main_gui().resize(3840 + 400, 2160 + 400)
        ok = yield ("until", lambda: view().width() >= 3840 and int(num("natronSceneUploadPixels", 0)) >= 3840 * 2160, REFINE_TIMEOUT_S)
        check(ok, "the view refines at 3840x2160 (view %dx%d, upload %r pixels)"
              % (view().width(), view().height(), num("natronSceneUploadPixels")))
        longest = yield from probe_event_loop("3840x2160")
        check(longest < LOOP_LIMIT_MS, "the event loop stays under %d ms while refining at 4K (longest %.1f ms)" % (LOOP_LIMIT_MS, longest))
        v.setMinimumSize(old_min)
        main_gui().resize(1920, 1080)
        yield ("sleep", SETTLE_MS)

        # A Max Samples cap: the render restarts and converges to it.
        restarts = num("natronSceneRestartCount")
        ok = yield from set_max_samples(CAPPED_SAMPLES)
        require(ok, "the Max Samples box is there again")
        ok = yield ("until", lambda: num("natronSceneRestartCount") > restarts, POLL_TIMEOUT_S)
        check(ok, "a new Max Samples restarts the render (restarts %r -> %r)" % (restarts, num("natronSceneRestartCount")))
        ok = yield ("until", lambda: bool(prop("natronSceneRefineConverged")) and num("natronSceneSamples") == CAPPED_SAMPLES, REFINE_TIMEOUT_S)
        check(ok, "the render converges at Max Samples (samples %r, converged %r)"
              % (num("natronSceneSamples"), prop("natronSceneRefineConverged")))
        ok = yield ("until", lambda: num("natronSceneDrawnSamples") == CAPPED_SAMPLES, POLL_TIMEOUT_S)
        check(ok, "the view shows the capped frame (drawn %r)" % (num("natronSceneDrawnSamples"),))
        check(not prop("natronSceneRefining"), "refinement stops at the cap")
        frames = num("natronSceneRefineFrames")
        yield ("sleep", 1000)
        check(num("natronSceneRefineFrames") == frames, "no frame is published after convergence (%r -> %r)"
              % (frames, num("natronSceneRefineFrames")))
        shot("m254-cycles-converged.png")

        # Back to Storm.
        state = {}
        sub = yield from open_renderer_submenu(state)
        storm = [a for a in sub.actions() if a.text().replace("&", "").startswith("Storm")] if sub is not None else []
        if storm:
            storm[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == STORM_ID and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "switching back to Storm draws the scene (%r)" % (prop("natronSceneRenderer"),))
        check(num("natronSceneSamples") == -1, "Storm shows no samples counter (%r)" % (num("natronSceneSamples"),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
