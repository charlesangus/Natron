"""Roto ignores viewer presses, double-clicks and key presses while a vector follow runs, in the app.

The follow writes the shape as it goes, so an edit made meanwhile would be overwritten by the frames
still to come, or would fight them. A ctest cannot send viewer events (there is no NodeGui headless),
so this runs the real viewer. Plate, scene and follow helpers are in m219_follow_common.py.

A TV-L1 follow of the whole plate is started with one control point selected by a real click. Once the
first frames have been baked, and with the follow still running, the script sends the events a user
would and reads the shape back:

  drag      a press and drag on the selected control point does not move it
  dblclick  a double-click on the shape's edge does not select its points (read after the follow: the
            positive control's drag then moves only the point it grabbed)
  key       Delete on the selected point leaves the shape's point count alone
  control   once the follow has been stopped the same drag moves the point, and only that point

A screenshot is taken mid-follow (m219-follow-guard.png in the output directory).

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m219-curves-follow-vectors/out-guards -e GUI_TEST_TIMEOUT=900 \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m219_follow_guards.py <Natron AppImage>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 3000
GUARD_AFTER_KEYS = 2
DRAG_PX = 40.0
MOVED_MIN = 5.0

try:
    import faulthandler

    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require
    from m219_follow_common import *
    import m219_follow_common

    m219_follow_common.bind(app)

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def corner_xy(bez, i, t):
        p = pos(bez, i, t)
        return (p[0], p[1])

    def widget_of(vmap, bez, i, t):
        x, y = corner_xy(bez, i, t)
        return vmap.to_widget(x, y)

    def moved(a, b):
        return max(abs(a[0] - b[0]), abs(a[1] - b[1]))

    def section_guards(scene):
        set_project_range(START, PLATE_FRAMES)
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.slow_gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        select_shapes([bez])
        yield from go_to(START)

        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        keep(gl)
        rg.activate()
        yield ("sleep", SETTLE_MS)
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")
        rg.select_tool(gl)

        # A real click selects point 0 alone, so a leaked select-all or a leaked Delete shows up later.
        grab = widget_of(vmap, bez, 0, START)
        rg.click(gl, grab)
        yield ("sleep", SEEK_MS)
        before = [corner_xy(bez, i, START) for i in range(POINTS)]
        scale = vmap.pixel_scale()
        target = QPointF(grab.x() + DRAG_PX, grab.y())
        edge = (widget_of(vmap, bez, 0, START) + widget_of(vmap, bez, 1, START)) / 2.0
        state = {"done": False, "running_after": False, "keys": 0}
        seen = {}

        def probe():
            return [corner_xy(bez, i, START) for i in range(POINTS)]

        def on_tick():
            keys = len(keys_of(bez))
            if state["done"] or keys < GUARD_AFTER_KEYS or not scene.ctx.isFollowingVectors():
                return None
            state["done"] = True
            state["keys"] = keys
            keys_in = keys_of(bez)

            rg.drag(gl, grab, target)
            seen["drag"] = probe()
            seen["drag_running"] = scene.ctx.isFollowingVectors()

            for press_pos in (edge, grab):
                rg.send_mouse(gl, QEvent.MouseButtonPress, press_pos, Qt.LeftButton, Qt.LeftButton)
                rg.send_mouse(gl, QEvent.MouseButtonRelease, press_pos, Qt.LeftButton, Qt.NoButton)
                rg.send_mouse(gl, QEvent.MouseButtonDblClick, press_pos, Qt.LeftButton, Qt.LeftButton)
                rg.send_mouse(gl, QEvent.MouseButtonRelease, press_pos, Qt.LeftButton, Qt.NoButton)
            seen["dbl"] = probe()
            seen["dbl_count"] = bez.getNumControlPoints()
            seen["dbl_shapes"] = len(rg.shapes(scene.ctx))
            seen["dbl_running"] = scene.ctx.isFollowingVectors()

            rg.send_key(gl, Qt.Key_Delete)
            rg.send_key(gl, Qt.Key_Backspace)
            seen["key_count"] = bez.getNumControlPoints()
            seen["key_shapes"] = len(rg.shapes(scene.ctx))
            seen["key"] = probe()
            seen["key_running"] = scene.ctx.isFollowingVectors()

            shot("m219-follow-guard.png", "Mid-follow (%d keys baked): a drag, a double-click and Delete/Backspace sent to the viewer; point 0 moved %.3f px, %d points, %d shape(s)"
                 % (keys, moved(seen["key"][0], before[0]), seen["key_count"], seen["key_shapes"]))
            seen["keys_in"] = keys_in
            return None

        press(scene.roto, "followForwardToEnd")
        stop = {"sent": False}

        def tick():
            r = on_tick()
            if state["done"] and not stop["sent"] and scene.ctx.isFollowingVectors():
                stop["sent"] = True
                param(scene.roto, "followForwardToEnd").trigger()
                return (ABORT_END_S, "guards: the follow ends within %.0f s of the user's stop" % ABORT_END_S)
            return r

        outcome, stats = yield from wait_follow(scene, "guards", ABORT_TIMEOUT_S, tick)
        require(state["done"], "guards: the follow was still running when %d keys had been baked; it finished first (report %r)"
                % (GUARD_AFTER_KEYS, outcome["message"]))
        report("NOTE guards: events sent after %d keys; report %r" % (state["keys"], outcome["message"]))

        check(seen["drag_running"] and seen["dbl_running"] and seen["key_running"],
              "guards: the follow was still running after the drag, the double-click and the keys (running: drag %r, double-click %r, keys %r)"
              % (seen["drag_running"], seen["dbl_running"], seen["key_running"]))
        drag_moved = max(moved(seen["drag"][i], before[i]) for i in range(POINTS))
        check(drag_moved <= UNCHANGED_TOL, "guards drag: a press and drag of %.0f px on the selected control point did not move any point (largest move %.3e)"
              % (DRAG_PX * abs(scale), drag_moved))
        dbl_moved = max(moved(seen["dbl"][i], before[i]) for i in range(POINTS))
        check(seen["dbl_count"] == POINTS and seen["dbl_shapes"] == 1 and dbl_moved <= UNCHANGED_TOL,
              "guards dblclick: a double-click on the shape's edge and on its point added no shape or point and moved nothing (%d points, %d shapes, largest move %.3e)"
              % (seen["dbl_count"], seen["dbl_shapes"], dbl_moved))
        check(seen["key_count"] == POINTS and seen["key_shapes"] == 1,
              "guards key: Delete and Backspace on the selected point removed neither the point nor the shape (%d points, %d shapes)"
              % (seen["key_count"], seen["key_shapes"]))
        key_moved = max(moved(seen["key"][i], before[i]) for i in range(POINTS))
        check(key_moved <= UNCHANGED_TOL, "guards key: the keys left every point where it was (largest move %.3e)" % key_moved)

        yield from go_to(START)
        yield from settle()
        # No click first: the drag grabs the point with whatever selection the follow's events left.
        grab_now = widget_of(vmap, bez, 0, START)
        rg.drag(gl, grab_now, QPointF(grab_now.x() + DRAG_PX, grab_now.y()))
        yield ("sleep", SEEK_MS)
        after = [corner_xy(bez, i, START) for i in range(POINTS)]
        point0 = moved(after[0], before[0])
        others = max(moved(after[i], before[i]) for i in range(1, POINTS))
        check(point0 >= MOVED_MIN and others <= KEY_TOL,
              "guards control: with no follow running the same press and drag moves the point and only that point (point 0 moved %.2f px, the others %.3e)"
              % (point0, others))
        set_project_range(START, MAIN_LAST)


    def steps():
        gt.section("guards")
        scene = yield from start_scene()
        yield from open_scene_roto(scene)
        yield from section_guards(scene)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
