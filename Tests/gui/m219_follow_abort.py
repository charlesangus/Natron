"""Roto curves following TV-L1 motion vectors, aborted mid-follow, in the app, on a plate whose motion is known.

The second of two scripts; m219_curves_follow_vectors.py runs the other sections. Plate, scene and
follow helpers are in m219_follow_common.py, so the plate and its known pan are the ones described there.

  abort        a second press mid-follow stops it, the baked frames stay and are right, the later
               frames are untouched, the GUI thread keeps ticking, and one undo removes the partial
               follow. The vectors are TV-L1, which renders slowly, so this section alone takes most
               of the time the runner allows.

The section ends in a captioned screenshot (m219-abort.png in the output directory; the caption strip
above the screen grab carries the measured numbers).

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup). Give the run time for
the TV-L1 renders:
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m219-curves-follow-vectors/out-abort -e GUI_TEST_TIMEOUT=900 \\
        -e NATRON_STALL_LOG=build/m219-curves-follow-vectors/out-abort/stalls.jsonl \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m219_follow_abort.py <Natron AppImage>
Without NATRON_STALL_LOG the harness's 50 ms stall check is skipped and only the script's own tick
gap is checked.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 3000

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require
    from m219_follow_common import *
    import m219_follow_common

    m219_follow_common.bind(app)

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def section_abort(scene):
        set_project_range(START, PLATE_FRAMES)
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.slow_gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        frames = list(range(START, PLATE_FRAMES + 1))
        before = snapshot(bez, frames)
        keys_before = keys_of(bez)
        select_shapes([bez])
        yield from go_to(START)

        state = {"aborted_at": None, "keys_at_abort": 0, "keys_seen": len(keys_before), "started": mono_ms()}

        def on_tick():
            keys = len(keys_of(bez))
            if keys != state["keys_seen"]:
                state["keys_seen"] = keys
                report("NOTE abort: %d keys %.0f ms after the first press" % (keys, mono_ms() - state["started"]))
            # A press after the follow ended would start another one instead of stopping it.
            if state["aborted_at"] is None and keys >= ABORT_AFTER_KEYS and scene.ctx.isFollowingVectors():
                state["keys_at_abort"] = keys
                state["aborted_at"] = mono_ms()
                report("NOTE abort: second press at %d keys, %.0f ms after the first" % (keys, state["aborted_at"] - state["started"]))
                # No processEvents here, unlike press(): the abort can end the follow within it, and
                # the report would then be taken inside this tick.
                param(scene.roto, "followForwardToEnd").trigger()
                return (ABORT_END_S, "abort: the follow ends within %.0f s of the second press" % ABORT_END_S)
            return None

        press(scene.roto, "followForwardToEnd")
        outcome, stats = yield from wait_follow(scene, "abort", ABORT_TIMEOUT_S, on_tick)
        if state["aborted_at"] is not None:
            report("NOTE abort: the follow ended %.0f ms after the second press" % (stats["end"] - state["aborted_at"]))
        require(state["aborted_at"] is not None, "abort: the follow was still running when %d keys had been baked; it finished first (report %r)"
                % (ABORT_AFTER_KEYS, outcome["message"]))
        text = outcome["message"]
        keys = keys_of(bez)
        last_key = max(keys)
        report("NOTE abort: pressed at %d keys, ended with keys %r, %d ticks, worst GUI tick gap %.0f ms; report %r"
               % (state["keys_at_abort"], keys, stats["ticks"], stats["max_gap"], text))
        check(outcome["status"] == "aborted" and "aborted" in text, "abort: the report says the follow was aborted (status %r, %r)" % (outcome["status"], text))
        check(message(scene.roto) == text, "abort: the node keeps the report as its quiet message (message %r)" % (message(scene.roto),))
        check(report_dialog() is None, "abort: a follow the user stopped opens no dialog (dialog %r)" % (report_dialog(),))
        check(len(keys) >= ABORT_AFTER_KEYS and last_key < PLATE_FRAMES, "abort: the baked frames stay and the range is not finished (keys %r)" % (keys,))
        worst = max_deviation(bez, [int(k) for k in keys], lambda t, i: shifted(p0[i], *known_shift(t)))
        check(worst <= TVL1_TOL, "abort: every baked frame is within %.1f px of the known position (largest error %.3f)" % (TVL1_TOL, worst))
        held = max_deviation(bez, [PLATE_FRAMES], lambda t, i: pos(bez, i, last_key))
        check(held <= UNCHANGED_TOL, "abort: nothing was baked past frame %d (frame %d reads as that key, off by %.2e)" % (int(last_key), PLATE_FRAMES, held))
        check(stats["ticks"] >= MIN_TICKS, "abort: the GUI thread ran %d ticks during the follow (at least %d)" % (stats["ticks"], MIN_TICKS))
        check(stats["max_gap"] <= TICK_GAP_LIMIT_MS, "abort: the GUI thread never went more than %.0f ms between ticks during the follow (worst %.0f ms)"
              % (TICK_GAP_LIMIT_MS, stats["max_gap"]))
        if STALL_LOG:
            yield ("sleep", 300)
            stalls = [d for s, d in read_stalls() if stats["start"] <= s <= stats["end"] and d > STALL_LIMIT_MS]
            check(not stalls, "abort: no GUI-thread stall over %.0f ms during the follow (%r)" % (STALL_LIMIT_MS, stalls))
        else:
            report("NOTE abort: NATRON_STALL_LOG is not set, so the harness's stall monitor was not consulted")
        yield from go_to(int(last_key))
        yield from settle()
        shot("m219-abort.png", "Aborted mid-follow: %d keys baked (last at frame %d of %d), largest error %.3f px, worst GUI tick gap %.0f ms over %d ticks"
             % (len(keys), int(last_key), PLATE_FRAMES, worst, stats["max_gap"], stats["ticks"]))

        check(rg.trigger("undo"), "Edit > Undo is available after the aborted follow")
        yield ("sleep", SEEK_MS)
        after = snapshot(bez, frames)
        check(keys_of(bez) == keys_before and all(after[t] == before[t] for t in frames),
              "abort: one undo removes the partial follow (keys %r -> %r)" % (keys, keys_of(bez)))
        set_project_range(START, MAIN_LAST)


    def steps():
        gt.section("abort")
        scene = yield from start_scene()
        yield from open_scene_roto(scene)
        yield from section_abort(scene)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
