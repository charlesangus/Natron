"""Roto and RotoPaint curves following motion vectors, in the app, on a plate whose motion is known.

The plate is written at run time under build/m219-curves-follow-vectors/ with oiiotool: a noise
texture that pans (+PAN_U, +PAN_V) pixels a frame, frames 1..PLATE_FRAMES. A shape that follows it
from frame 1 is therefore at its start position plus (PAN_U, PAN_V) * (frame - 1) at every frame, and
VectorGenerator at Fine reads that motion to well under the 0.5 px tolerance. Medium drifts about
0.03 px a frame by design, so nothing here asserts accuracy on it.

This is the first of two scripts; m219_follow_abort.py runs the abort section, whose TV-L1 vectors
take most of the time the runner allows. They share the plate and scene setup in m219_follow_common.py.

Each section ends in a captioned screenshot (m219-*.png in the output directory; the caption strip
above the screen grab carries the measured numbers):
  rows         the Follow Vectors row is in the viewer, with its source, model, four direction
               buttons, reduce, tolerance and confidence threshold, for Roto and for RotoPaint;
  viewer-row   a Roto shape follows per point to the end of the range from the viewer row, and every
               frame is within tolerance of the known position;
  panel-entry  the right-click menu of the shape panel offers Follow Vectors... and runs the same
               follow;
  models       Translate, Affine and Perspective key the shape's transform and leave its points and
               their keys alone, and the transform reaches the known translation;
  bridge       a Forward press with a later key tracks forward from the current frame and backward
               from that key, blends the two, meets both keys exactly and follows one way after it;
               a Backward press on an unkeyed frame with an earlier key and a key after it bridges
               the same way down to the earlier key and keeps the current frame as it showed; a
               Forward press with only a later key tracks back from that key to the current frame;
  forward-only a source with only the forward layer is listed, follows forward, and a backward
               follow is refused with the source and the missing layer named and nothing keyed;
  undo         one undo restores the keys and positions of every followed shape for a per-point
               and for a fitted follow, and redo brings the follow back.

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m219-curves-follow-vectors/out -e GUI_TEST_TIMEOUT=900 \\
        -e NATRON_STALL_LOG=build/m219-curves-follow-vectors/out/stalls.jsonl \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m219_curves_follow_vectors.py <Natron AppImage>
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
    from PySide6.QtCore import QEvent, QPointF, QTimer, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require
    from m219_follow_common import *
    import m219_follow_common

    m219_follow_common.bind(app)

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def section_rows_roto(scene):
        gt.section("rows-roto")
        yield from open_scene_roto(scene)
        count = yield from check_row("Roto")
        models = [str(o) for o in param(scene.roto, "followModel").getOptions()]
        check(models == [MODEL_IDS[m] for m in MODELS], "Roto: the model menu offers %r (has %r)" % ([MODEL_IDS[m] for m in MODELS], models))
        shot("m219-row-roto.png", "Roto: the Follow Vectors row in the viewer, %d of %d controls found" % (count, len(HINTS)))

    def check_per_point_follow(bez, p0, label, tol=POS_TOL):
        frames = list(range(START, MAIN_LAST + 1))
        worst = max_deviation(bez, frames, lambda t, i: shifted(p0[i], *known_shift(t)))
        keys = keys_of(bez)
        report("NOTE %s: keys %r, largest error %.4f px over frames %d..%d" % (label, keys, worst, START, MAIN_LAST))
        check(float(MAIN_LAST) in keys, "%s: the shape is keyed at the last frame (keys %r)" % (label, keys))
        check(worst <= tol, "%s: every point, tangent and frame is within %.2f px of the known position (largest error %.4f)" % (label, tol, worst))
        return worst

    def section_viewer_row(scene):
        gt.section("viewer-row")
        set_project_range(START, MAIN_LAST)
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        select_shapes([bez])
        yield from go_to(START)
        yield from run_follow(scene, "followForwardToEnd", "viewer row, Per Point, forward to the end")
        worst = check_per_point_follow(bez, p0, "viewer row")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-viewer-row-follow.png", "Roto, Per Point, Forward To End pressed in the viewer row at frame %d: frame %d, largest error %.3f px (known pan %d,%d px a frame)"
             % (START, MAIN_LAST, worst, PAN_U, PAN_V))

    def right_click_item(tree, item):
        """Right-clicks the item's label. The panel's tree only opens its menu for the label column; a click on
        any other column of a selected row toggles that column instead, and visualItemRect() spans every column."""
        header = tree.header()
        x = header.sectionViewportPosition(0) + header.sectionSize(0) - 4
        y = tree.visualItemRect(item).center().y()
        point = QPointF(x, y)
        hit = tree.itemAt(point.toPoint())
        column = tree.indexAt(point.toPoint()).column()
        on_label = hit is not None and hit.text(0) == item.text(0) and column == 0
        check(on_label, "the right-click lands on the label of %s (item %r, column %d)" % (item.text(0), hit.text(0) if hit else None, column))
        if not on_label:
            return False
        for etype, buttons in ((QEvent.MouseButtonPress, Qt.RightButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
            QApplication.sendEvent(tree.viewport(), QMouseEvent(etype, point, tree.viewport().mapToGlobal(point), Qt.RightButton, buttons, Qt.NoModifier))
            QApplication.processEvents()
        return True

    def section_panel_entry(scene):
        gt.section("panel-entry")
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        tree = select_shapes([bez])
        item = tree_item(tree, bez.getLabel())
        yield from go_to(START)
        seen = {"labels": [], "enabled": False, "triggered": False, "fresh": False}

        def pick():
            menu = QApplication.activePopupWidget()
            if menu is None:
                return
            seen["labels"] = [a.text().replace("&", "") for a in menu.actions()]
            shot("m219-panel-menu.png", "The shape panel's right-click menu on %s offers Follow Vectors..." % bez.getLabel())
            for a in menu.actions():
                if a.text().replace("&", "").startswith("Follow Vectors"):
                    seen["enabled"] = a.isEnabled()
                    if a.isEnabled():
                        a.trigger()
                        seen["triggered"] = True
                        seen["fresh"] = scene.ctx.isFollowingVectors() or scene.ctx.getLastFollowReport() is None
            menu.close()

        previous = scene.ctx.getLastFollowReport()
        QTimer.singleShot(PANEL_MENU_DELAY_MS, pick)
        if not right_click_item(tree, item):
            return
        check(any(s.startswith("Follow Vectors") for s in seen["labels"]), "the panel menu has a Follow Vectors entry (entries %r)" % (seen["labels"],))
        check(seen["enabled"] and seen["triggered"], "the Follow Vectors entry is enabled for a shape and was triggered")
        check(seen["fresh"], "the entry started a follow, which drops the previous report (previous %r)"
              % (previous["message"] if previous else None,))
        if not seen["fresh"]:
            return
        outcome, stats = yield from wait_follow(scene, "panel entry", FOLLOW_TIMEOUT_S)
        report("NOTE panel entry: %d ticks, worst GUI tick gap %.0f ms; report %r" % (stats["ticks"], stats["max_gap"], outcome["message"]))
        check(outcome["status"] == "done", "panel entry: the follow is done (status %r, %r)" % (outcome["status"], outcome["message"]))
        worst = check_per_point_follow(bez, p0, "panel entry")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-panel-follow.png", "Roto, Per Point, Follow Vectors... from the panel menu at frame %d: frame %d, largest error %.3f px" % (START, MAIN_LAST, worst))

    def section_models(scene):
        gt.section("models")
        for model in MODELS[1:]:
            reset_shapes(scene.ctx)
            ensure_source(scene.roto, scene.gen)
            set_choice(scene.roto, "followModel", MODEL_IDS[model])
            bez = new_shape(scene.ctx)
            p0 = [pos(bez, i, START) for i in range(POINTS)]
            keys_before = keys_of(bez)
            select_shapes([bez])
            yield from go_to(START)
            yield from run_follow(scene, "followForwardToEnd", model)
            label = "%s fit" % model
            unmoved = max_deviation(bez, [START, MAIN_LAST], lambda t, i: p0[i])
            check(keys_of(bez) == keys_before, "%s: the points' keys are untouched (%r -> %r)" % (label, keys_before, keys_of(bez)))
            check(unmoved <= UNCHANGED_TOL, "%s: the control points stay where they were; the transform carries the motion (moved %.2e)" % (label, unmoved))
            want = known_shift(MAIN_LAST)
            if model == "Translate":
                translate = param_of_shape(bez, "translate")
                got = (translate.getValueAtTime(MAIN_LAST, 0), translate.getValueAtTime(MAIN_LAST, 1))
                start = (translate.getValueAtTime(START, 0), translate.getValueAtTime(START, 1))
                linear_ok = True
            else:
                extra = param_of_shape(bez, "extraMatrix")
                m = [extra.getValueAtTime(MAIN_LAST, d) for d in range(9)]
                got = (m[2], m[5])
                start = (extra.getValueAtTime(START, 2), extra.getValueAtTime(START, 5))
                linear_tol = MATRIX_LINEAR_TOL
                linear_ok = (abs(m[0] - 1) <= linear_tol and abs(m[4] - 1) <= linear_tol and abs(m[1]) <= linear_tol and abs(m[3]) <= linear_tol)
                check(linear_ok, "%s: the matrix's linear part is the identity for a pure pan (m0 %.4f m1 %.4f m3 %.4f m4 %.4f)" % (label, m[0], m[1], m[3], m[4]))
                check(abs(m[6]) <= MATRIX_PERSPECTIVE_TOL and abs(m[7]) <= MATRIX_PERSPECTIVE_TOL and abs(m[8] - 1) <= MATRIX_PERSPECTIVE_TOL,
                      "%s: the matrix has no perspective for a pure pan (m6 %.5f m7 %.5f m8 %.5f)" % (label, m[6], m[7], m[8]))
            err = max(abs(got[0] - want[0]), abs(got[1] - want[1]))
            check(max(abs(start[0]), abs(start[1])) <= POS_TOL, "%s: the transform is the identity at the start frame (%.3f, %.3f)" % (label, start[0], start[1]))
            check(err <= POS_TOL, "%s: the transform's translation at frame %d is (%d, %d) within %.2f px (got %.3f, %.3f)"
                  % (label, MAIN_LAST, want[0], want[1], POS_TOL, got[0], got[1]))
            yield from go_to(MAIN_LAST)
            yield from settle()
            shot("m219-model-%s.png" % model.lower(), "Roto, %s fit at frame %d: translation (%.3f, %.3f) against the known (%d, %d), error %.3f px; points and their keys unchanged"
                 % (model, MAIN_LAST, got[0], got[1], want[0], want[1], err))

    def param_of_shape(bez, name):
        p = bez.getParam(name)
        require(p is not None, "the shape has a parameter %r" % name)
        return keep(p)

    def set_key(bez, t, points):
        for i, p in enumerate(points):
            bez.setPointAtIndex(i, t, p[0], p[1], p[2], p[3], p[4], p[5])

    def smoothstep(s):
        return s * s * (3.0 - 2.0 * s)

    def section_bridge(scene):
        gt.section("bridge")
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        key_start = p0
        true_at_key = [shifted(p, *known_shift(BRIDGE_KEY)) for p in p0]
        key_end = [shifted(p, BRIDGE_OFFSET[0], BRIDGE_OFFSET[1]) for p in true_at_key]
        set_key(bez, START, key_start)
        set_key(bez, BRIDGE_KEY, key_end)
        keys = keys_of(bez)
        require(keys == [float(START), float(BRIDGE_KEY)], "the shape has keys at %d and %d (keys %r)" % (START, BRIDGE_KEY, keys))
        select_shapes([bez])
        yield from go_to(START)
        yield from run_follow(scene, "followForwardToEnd", "bridge, Forward at frame %d with a key at frame %d" % (START, BRIDGE_KEY))

        at_start = max_deviation(bez, [START], lambda t, i: key_start[i])
        at_key = max_deviation(bez, [BRIDGE_KEY], lambda t, i: key_end[i])
        check(at_start <= KEY_TOL, "bridge: the earlier key is met exactly (off by %.2e)" % at_start)
        check(at_key <= KEY_TOL, "bridge: the later key is met exactly (off by %.2e)" % at_key)

        gap = float(BRIDGE_KEY - START)

        def blended(t, i):
            w = smoothstep((t - START) / gap)
            dx, dy = known_shift(t)
            return shifted(p0[i], dx + w * BRIDGE_OFFSET[0], dy + w * BRIDGE_OFFSET[1])

        inside = list(range(START, BRIDGE_KEY + 1))
        worst = max_deviation(bez, inside, blended)
        check(worst <= BRIDGE_TOL, "bridge: frames %d..%d follow the forward pass blended into the backward pass by a smoothstep, within %.1f px (largest error %.3f)"
              % (START, BRIDGE_KEY, BRIDGE_TOL, worst))

        mid = START + (BRIDGE_KEY - START) // 2
        one_way = max_deviation(bez, [mid], lambda t, i: shifted(p0[i], *known_shift(t)))
        check(one_way >= BRIDGE_OFFSET[0] / 2.0 - BRIDGE_TOL, "bridge: frame %d is pulled %.2f px off the forward-only track towards the later key, so the backward pass counts (half the key's offset is %.1f)"
              % (mid, one_way, BRIDGE_OFFSET[0] / 2.0))

        after = list(range(BRIDGE_KEY + 1, MAIN_LAST + 1))
        beyond = max_deviation(bez, after, lambda t, i: shifted(key_end[i], PAN_U * (t - BRIDGE_KEY), PAN_V * (t - BRIDGE_KEY)))
        check(beyond <= POS_TOL, "bridge: after the later key the shape follows forward from that key within %.2f px (largest error %.3f)" % (POS_TOL, beyond))

        yield from go_to(mid)
        yield from settle()
        shot("m219-bridge-mid.png", "Bridge at frame %d, halfway between the keys at %d and %d: %.2f px off the forward-only track, blend error %.3f px"
             % (mid, START, BRIDGE_KEY, one_way, worst))
        yield from go_to(BRIDGE_KEY)
        yield from settle()
        shot("m219-bridge-key.png", "Bridge at the later key, frame %d: off the key by %.2e px, and the earlier key by %.2e px" % (BRIDGE_KEY, at_key, at_start))

        yield from check_backward_bridge(scene)
        yield from check_later_key_only(scene)

    def check_backward_bridge(scene):
        reset_shapes(scene.ctx)
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        current, after = BACK_CURRENT, BACK_LATER_KEY
        key_after = [shifted(shifted(p, *known_shift(after)), BRIDGE_OFFSET[0], BRIDGE_OFFSET[1]) for p in p0]
        set_key(bez, after, key_after)
        keys = keys_of(bez)
        require(keys == [float(START), float(after)], "backward bridge: the shape has keys at %d and %d (keys %r)" % (START, after, keys))
        shown = [pos(bez, i, current) for i in range(POINTS)]
        shown_off = max_deviation(bez, [current], lambda t, i: shifted(p0[i], *known_shift(t)))
        check(shown_off >= 2.0 * BRIDGE_TOL, "backward bridge: frame %d shows the shape %.2f px off the plate, so the two passes disagree" % (current, shown_off))

        select_shapes([bez])
        yield from go_to(current)
        yield from run_follow(scene, "followBackwardToStart", "backward bridge, Backward at frame %d with keys at %d and %d" % (current, START, after))

        at_start = max_deviation(bez, [START], lambda t, i: p0[i])
        at_current = max_deviation(bez, [current], lambda t, i: shown[i])
        at_after = max_deviation(bez, [after], lambda t, i: key_after[i])
        check(at_start <= KEY_TOL, "backward bridge: the earlier key is met exactly (off by %.2e)" % at_start)
        check(at_current <= KEY_TOL, "backward bridge: frame %d keeps what it showed (off by %.2e)" % (current, at_current))
        check(at_after <= KEY_TOL, "backward bridge: the key after the current frame is untouched (off by %.2e)" % at_after)
        keys = keys_of(bez)
        want_keys = [float(t) for t in range(START, current + 1)] + [float(after)]
        check(keys == want_keys, "backward bridge: frames %d..%d are keyed and the key at %d stays (keys %r)" % (START, current, after, keys))

        gap = float(current - START)

        def blended(t, i):
            w = smoothstep((t - START) / gap)
            forward = shifted(p0[i], *known_shift(t))
            backward = shifted(shown[i], -PAN_U * (current - t), -PAN_V * (current - t))
            return tuple(forward[k] * (1.0 - w) + backward[k] * w for k in range(6))

        worst = max_deviation(bez, list(range(START, current + 1)), blended)
        check(worst <= BRIDGE_TOL, "backward bridge: frames %d..%d follow the forward pass from %d blended into the backward pass from %d by a smoothstep, within %.1f px (largest error %.3f)"
              % (START, current, START, current, BRIDGE_TOL, worst))

        mid = START + (current - START) // 2
        pull = smoothstep((mid - START) / gap) * shown_off
        one_way = max_deviation(bez, [mid], lambda t, i: shifted(p0[i], *known_shift(t)))
        check(one_way >= pull - BRIDGE_TOL, "backward bridge: frame %d is pulled %.2f px off the forward-only track towards frame %d, so the backward pass counts (expected %.2f)"
              % (mid, one_way, current, pull))

        yield from go_to(mid)
        yield from settle()
        shot("m219-bridge-backward.png", "Backward bridge at frame %d, between the key at %d and the current frame %d: %.2f px off the forward-only track, blend error %.3f px; frame %d off by %.2e px"
             % (mid, START, current, one_way, worst, current, at_current))

    def check_later_key_only(scene):
        reset_shapes(scene.ctx)
        late = LATER_ONLY_KEY
        dx, dy = known_shift(late)
        bez = scene.ctx.createRectangle(SQ_X + dx, SQ_Y + dy, SQ_SIZE, late)
        require(bez is not None, "create a Roto rectangle keyed at frame %d" % late)
        keep(bez)
        keys = keys_of(bez)
        require(keys == [float(late)], "later key only: the shape's one key is at frame %d (keys %r)" % (late, keys))
        held = [pos(bez, i, late) for i in range(POINTS)]

        select_shapes([bez])
        yield from go_to(START)
        yield from run_follow(scene, "followForwardToEnd", "later key only, Forward at frame %d with the only key at frame %d" % (START, late))

        at_key = max_deviation(bez, [late], lambda t, i: held[i])
        check(at_key <= KEY_TOL, "later key only: the key is met exactly (off by %.2e)" % at_key)
        frames = list(range(START, MAIN_LAST + 1))
        worst = max_deviation(bez, frames, lambda t, i: shifted(held[i], PAN_U * (t - late), PAN_V * (t - late)))
        check(worst <= POS_TOL, "later key only: frames %d..%d track back and forward from the key at %d within %.2f px (largest error %.3f)"
              % (START, MAIN_LAST, late, POS_TOL, worst))
        off_held = max_deviation(bez, [START], lambda t, i: held[i])
        check(off_held >= PAN_U * (late - START) - POS_TOL, "later key only: frame %d is not given the key's held pose but where the vectors bring it (%.2f px from it)"
              % (START, off_held))
        keys = keys_of(bez)
        check(keys == [float(t) for t in frames], "later key only: frames %d..%d are keyed (keys %r)" % (START, MAIN_LAST, keys))

        yield from go_to(START)
        yield from settle()
        shot("m219-bridge-later-key-only.png", "Forward at frame %d with the only key at frame %d: frame %d is %.2f px from the key's pose, tracked back within %.3f px"
             % (START, late, START, off_held, worst))

    def section_forward_only(scene):
        gt.section("forward-only")
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.forward_gen)
        options = [str(o) for o in param(scene.roto, "followSource").getOptions()]
        check(scene.forward_gen.getScriptName() in options, "the forward-only source is listed (%r)" % (options,))
        check(scene.gen.getScriptName() in options, "the two-way source is listed (%r)" % (options,))
        check(scene.read.getScriptName() not in options, "a node with no motion layers is not listed (%r)" % (options,))
        check(scene.roto.getScriptName() not in options, "the host itself is not listed (%r)" % (options,))

        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        bez = new_shape(scene.ctx)
        frames = list(range(START, MAIN_LAST + 1))
        before = snapshot(bez, frames)
        keys_before = keys_of(bez)
        select_shapes([bez])
        yield from go_to(MAIN_LAST)
        press(scene.roto, "followBackwardToStart")
        yield from settle(SEEK_MS)
        text = message(scene.roto)
        name = scene.forward_gen.getScriptName()
        check(name in text and "backward motion layer" in text,
              "a backward follow of the forward-only source is refused naming %s and its missing backward motion layer (message %r)" % (name, text))
        check(keys_of(bez) == keys_before and all(snapshot(bez, frames)[t] == before[t] for t in frames),
              "the refused follow keyed nothing and moved nothing")
        check(not scene.ctx.isFollowingVectors() and scene.ctx.getLastFollowReport() is None,
              "the refused follow never ran (following %r, report %r)" % (scene.ctx.isFollowingVectors(), scene.ctx.getLastFollowReport()))
        shot("m219-forward-only-refused.png", "Backward To Start on %s, which has no backward motion layer, is refused: %s" % (name, text.replace("\n", " ")))

        yield from go_to(START)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        yield from run_follow(scene, "followForwardToEnd", "forward-only source, forward")
        worst = check_per_point_follow(bez, p0, "forward-only source")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-forward-only-follow.png", "Forward-only source %s followed forward to frame %d: largest error %.3f px" % (name, MAIN_LAST, worst))

    def section_undo(scene):
        gt.section("undo")
        frames = list(range(START, MAIN_LAST + 1))
        for model in ("Per Point", "Affine"):
            reset_shapes(scene.ctx)
            ensure_source(scene.roto, scene.gen)
            set_choice(scene.roto, "followModel", MODEL_IDS[model])
            first = new_shape(scene.ctx)
            second = new_shape(scene.ctx, SQ2_X, SQ2_Y)
            shapes = [first, second]
            before = [snapshot(b, frames) for b in shapes]
            keys_before = [keys_of(b) for b in shapes]
            extra_before = [[param_of_shape(b, "extraMatrix").getValueAtTime(MAIN_LAST, d) for d in range(9)] for b in shapes]
            select_shapes(shapes)
            yield from go_to(START)
            yield from run_follow(scene, "followForwardToEnd", "undo, %s on two shapes" % model)
            done = [snapshot(b, frames) for b in shapes]
            extra_done = [[param_of_shape(b, "extraMatrix").getValueAtTime(MAIN_LAST, d) for d in range(9)] for b in shapes]
            if model == "Per Point":
                changed = all(done[n][MAIN_LAST] != before[n][MAIN_LAST] for n in range(2))
                what = "points"
            else:
                changed = all(extra_done[n] != extra_before[n] for n in range(2))
                what = "extra matrix"
            check(changed, "undo: the %s follow moved both shapes' %s" % (model, what))
            done_keys = [keys_of(b) for b in shapes]

            check(rg.trigger("undo"), "Edit > Undo is available after the %s follow" % model)
            yield ("sleep", SEEK_MS)
            restored = []
            for n, b in enumerate(shapes):
                after = snapshot(b, frames)
                extra_after = [param_of_shape(b, "extraMatrix").getValueAtTime(MAIN_LAST, d) for d in range(9)]
                restored.append(keys_of(b) == keys_before[n] and after == before[n] and extra_after == extra_before[n])
            check(all(restored), "undo: one undo restores the keys, points and extra matrix of both shapes after a %s follow (restored %r)" % (model, restored))
            yield from go_to(MAIN_LAST)
            yield from settle()
            shot("m219-undo-%s.png" % model.lower().replace(" ", "-"),
                 "After one Undo of a %s follow of two shapes: both are back at their start positions (keys %r)" % (model, [keys_of(b) for b in shapes]))

            check(rg.trigger("redo"), "Edit > Redo is available after the undo")
            yield ("sleep", SEEK_MS)
            redone = [keys_of(b) == done_keys[n] and snapshot(b, frames) == done[n] for n, b in enumerate(shapes)]
            check(all(redone), "undo: redo brings the %s follow of both shapes back (%r)" % (model, redone))

    def section_rows_roto_paint(scene):
        gt.section("rows-rotopaint")
        gt.close_all_panels()
        scene.roto.destroy()
        QApplication.processEvents()
        paint, ctx = rg.open_roto(app, "RotoPaint")
        require(paint is not None and ctx is not None, "create a RotoPaint with its panel open", gt.plugin_problem("RotoPaint"))
        keep(paint)
        keep(ctx)
        rg.activate()
        yield from settle()
        count = yield from check_row("RotoPaint")
        models = [str(o) for o in param(paint, "followModel").getOptions()]
        check(models == [MODEL_IDS[m] for m in MODELS], "RotoPaint: the model menu offers %r (has %r)" % ([MODEL_IDS[m] for m in MODELS], models))
        shot("m219-row-rotopaint.png", "RotoPaint: the Follow Vectors row in the viewer, %d of %d controls found" % (count, len(HINTS)))

    def steps():
        scene = yield from start_scene()
        yield from section_rows_roto(scene)
        yield from section_viewer_row(scene)
        yield from section_panel_entry(scene)
        yield from section_models(scene)
        yield from section_bridge(scene)
        yield from section_forward_only(scene)
        yield from section_undo(scene)
        yield from section_rows_roto_paint(scene)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
