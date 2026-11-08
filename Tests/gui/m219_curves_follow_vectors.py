"""Roto and RotoPaint curves following motion vectors, in the app, on a plate whose motion is known.

The plate is written at run time under build/m219-curves-follow-vectors/ with oiiotool: a noise
texture that pans (+PAN_U, +PAN_V) pixels a frame, frames 1..PLATE_FRAMES. A shape that follows it
from frame 1 is therefore at its start position plus (PAN_U, PAN_V) * (frame - 1) at every frame, and
VectorGenerator at Fine reads that motion to well under the 0.5 px tolerance. Medium drifts about
0.03 px a frame by design, so nothing here asserts accuracy on it.

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
  forward-only a source with only the forward layer is listed, follows forward, and a backward
               follow is refused with the source and the missing layer named and nothing keyed;
  abort        a second press mid-follow stops it, the baked frames stay and are right, the later
               frames are untouched, the GUI thread keeps ticking, and one undo removes the partial
               follow;
  undo         one undo restores the keys and positions of every followed shape for a per-point
               and for a fitted follow, and redo brings the follow back.

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup). The abort section
renders TV-L1 vectors, so give the run time:
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m219-curves-follow-vectors/out -e GUI_TEST_TIMEOUT=1800 \\
        -e NATRON_STALL_LOG=build/m219-curves-follow-vectors/out/stalls.jsonl \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m219_curves_follow_vectors.py <Natron AppImage>
Without NATRON_STALL_LOG the harness's 50 ms stall check is skipped and only the script's own tick
gap is checked.
"""
import json
import os
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 3000
SETTLE_MS = 500
SEEK_MS = 400
SHOT_MS = 900
POLL_TIMEOUT_S = 30.0
FOLLOW_TIMEOUT_S = 600.0
ABORT_TIMEOUT_S = 1200.0
TICK_MS = 20
PANEL_MENU_DELAY_MS = 400

WIDTH, HEIGHT = 320, 200
PLATE_FRAMES = 30
MAIN_LAST = 12
START = 1
PAN_U, PAN_V = 2, 1
BG_X0, BG_Y0 = 100, 40

SQ_X, SQ_Y, SQ_SIZE = 130.0, 130.0, 60.0
SQ2_X, SQ2_Y = 60.0, 170.0
BRIDGE_KEY = 9
BRIDGE_OFFSET = (8.0, 0.0)
POINTS = 4

STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
STALL_LIMIT_MS = 50.0
# The script's own tick also waits behind viewer repaints, so its limit is looser than the monitor's.
TICK_GAP_LIMIT_MS = 250.0
MIN_TICKS = 8

# Fine DIS reads a pure pan to a few hundredths of a pixel; TV-L1 is only held to a looser bound.
POS_TOL = 0.5
TVL1_TOL = 1.0
KEY_TOL = 1e-3
BRIDGE_TOL = 1.0
UNCHANGED_TOL = 1e-6
MATRIX_LINEAR_TOL = 0.02
MATRIX_PERSPECTIVE_TOL = 1e-3

ABORT_AFTER_KEYS = 4

HINTS = {
    "source": "The node whose motion vectors the shapes follow",
    "model": "How a shape follows the vectors",
    "backwardToStart": "back to the start of the range",
    "backwardOne": "Follows the selection one frame back",
    "forwardOne": "Follows the selection one frame forward",
    "forwardToEnd": "to the end of the range",
    "reduce": "removes the keys it wrote",
    "tolerance": "How far, in pixels, removing keys",
    "threshold": "A vector with less confidence than this",
}
MODELS = ["Per Point", "Translate", "Affine", "Perspective"]

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, QTimer, Qt
    from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QMouseEvent, QPainter
    from PySide6.QtWidgets import QApplication, QTreeWidget, QTreeWidgetItemIterator

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    BASE = os.path.join(gt.ROOT, "build", "m219-curves-follow-vectors")

    # Python wrappers die with their last reference and take the C++ side's script handles with them.
    _keep = []
    _viewer = []

    def keep(obj):
        _keep.append(obj)
        return obj

    def mono_ms():
        return time.monotonic() * 1000.0

    # ---- graph -------------------------------------------------------------------------------

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return keep(p)

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def seek(frame):
        app.getViewer(viewer_node().getScriptName()).seek(frame)

    def set_choice(node, name, label):
        p = param(node, name)
        options = [str(o) for o in p.getOptions()]
        require(label in options, "%s.%s offers %r (has %r)" % (node.getLabel(), name, label, options))
        p.setValue(options.index(label))

    def set_project_range(first, last):
        app.getProjectParam("frameRange").set(first, last)

    def run_tool(args):
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout

    def make_texture(directory, path, width, height, seed):
        """Noise at three scales, so the pyramid's coarse levels still see structure the way they do on a photograph."""
        layers = []
        for n, (divisor, weight) in enumerate(((12, 1.0), (4, 0.5), (2, 0.25))):
            layer = os.path.join(directory, "layer%d.exr" % n)
            run_tool(["oiiotool", "--pattern", "noise:type=uniform:min=0:max=1:seed=%d" % (seed + n),
                      "%dx%d" % (max(2, width // divisor), max(2, height // divisor)), "3",
                      "--resize:filter=blackman-harris", "%dx%d" % (width, height), "--mulc", str(weight), "-d", "float", "-o", layer])
            layers.append(layer)
        run_tool(["oiiotool", layers[0], layers[1], "--add", layers[2], "--add", "--mulc", "0.55", "-d", "float", "-o", path])

    def make_plate(directory):
        """Writes frames 1..PLATE_FRAMES as plate.NNNN.exr; returns the file pattern."""
        os.makedirs(directory, exist_ok=True)
        background = os.path.join(directory, "background.exr")
        make_texture(directory, background, WIDTH + 100, HEIGHT + 220, 7)
        for f in range(1, PLATE_FRAMES + 1):
            path = os.path.join(directory, "plate.%04d.exr" % f)
            # Cutting at a smaller x and a larger y moves the content +x and +y in Natron's y-up pixels.
            run_tool(["oiiotool", background, "--cut", "%dx%d+%d+%d" % (WIDTH, HEIGHT, BG_X0 - PAN_U * f, BG_Y0 + PAN_V * f),
                      "-d", "float", "-o", path])
        return os.path.join(directory, "plate.####.exr")

    def make_generator(read, x, y, outputs="Forward+Backward", method="DIS"):
        node = app.createNode("VectorGenerator")
        require(node is not None, "create a VectorGenerator", gt.plugin_problem("VectorGenerator"))
        node.setPosition(x, y)
        require(node.connectInput(0, read), "connect %s to %s" % (node.getLabel(), read.getLabel()))
        set_choice(node, "quality", "Fine")
        set_choice(node, "method", method)
        set_choice(node, "outputs", outputs)
        return keep(node)

    # ---- screenshots -------------------------------------------------------------------------

    def shot(name, caption):
        """The whole screen under a caption strip, so a reviewer reads the numbers beside the picture."""
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        bar = 36
        out = QImage(screen.width(), screen.height() + bar, QImage.Format_RGB32)
        out.fill(QColor(16, 16, 16))
        painter = QPainter(out)
        painter.drawImage(0, bar, screen)
        painter.setPen(QColor(255, 255, 255))
        font = QFont()
        font.setPixelSize(15)
        painter.setFont(font)
        painter.drawText(8, 0, out.width() - 16, bar, int(Qt.AlignVCenter | Qt.AlignLeft), caption)
        painter.end()
        path = os.path.join(gt.OUT, name)
        if out.save(path):
            report("SHOT " + path)
            report("NOTE caption of %s: %s" % (name, caption))
        else:
            report("NOTE could not save the screenshot " + path)

    # ---- shapes and selection ----------------------------------------------------------------

    def pos(bez, i, t):
        return tuple(bez.getControlPointPosition(i, t))

    def shifted(p, dx, dy):
        return (p[0] + dx, p[1] + dy, p[2] + dx, p[3] + dy, p[4] + dx, p[5] + dy)

    def deviation(a, b):
        return max(abs(a[k] - b[k]) for k in range(6))

    def known_shift(t):
        return (PAN_U * (t - START), PAN_V * (t - START))

    def snapshot(bez, frames):
        return {t: [pos(bez, i, t) for i in range(POINTS)] for t in frames}

    def max_deviation(bez, frames, expected_of):
        """Largest component error of the shape's points against expected_of(frame, index) over the frames."""
        worst = 0.0
        for t in frames:
            for i in range(POINTS):
                worst = max(worst, deviation(pos(bez, i, t), expected_of(t, i)))
        return worst

    def new_shape(ctx, x=SQ_X, y=SQ_Y):
        bez = ctx.createRectangle(x, y, SQ_SIZE, START)
        require(bez is not None, "create a Roto rectangle")
        require(bez.getNumControlPoints() == POINTS, "the rectangle has %d points" % POINTS)
        return keep(bez)

    def keys_of(bez):
        return sorted(float(k) for k in bez.getKeyframes())

    def shape_tree(label):
        for w in QApplication.allWidgets():
            try:
                if not isinstance(w, QTreeWidget) or not w.isVisible() or rg.ancestor_of_class(w, "NodeSettingsPanel") is None:
                    continue
                it = QTreeWidgetItemIterator(w)
                while it.value():
                    if it.value().text(0) == label:
                        return w
                    it += 1
            except RuntimeError:
                continue
        return None

    def tree_item(tree, label):
        it = QTreeWidgetItemIterator(tree)
        while it.value():
            if it.value().text(0) == label:
                return keep(it.value())
            it += 1
        return None

    def select_shapes(shapes):
        """Selects the shapes in the panel's tree, which is what a click on their rows does."""
        tree = shape_tree(shapes[0].getLabel())
        require(tree is not None, "the panel lists %s" % shapes[0].getLabel())
        keep(tree)
        tree.clearSelection()
        for bez in shapes:
            item = tree_item(tree, bez.getLabel())
            require(item is not None, "the panel lists %s" % bez.getLabel())
            item.setSelected(True)
        QApplication.processEvents()
        return tree

    def reset_shapes(ctx):
        rg.clear_shapes(ctx)
        QApplication.processEvents()

    # ---- the follow --------------------------------------------------------------------------

    def message(node):
        return str(node.getPersistentMessage() or "")

    def follow_finished(node):
        text = message(node)
        return text.startswith("Follow Vectors") and ("is done" in text or "aborted" in text or "reverted" in text or ": " in text[:20])

    def press(node, name):
        param(node, name).trigger()
        QApplication.processEvents()

    def ensure_source(node, source):
        """Chooses source in the picker. The list is only refreshed by a press or by focusing the viewer, so a press
        with nothing selected (which only warns) refreshes it first."""
        p = param(node, "followSource")
        if source.getScriptName() not in [str(o) for o in p.getOptions()]:
            press(node, "followForwardOne")
        options = [str(o) for o in p.getOptions()]
        require(source.getScriptName() in options, "the Follow Source picker lists %s (has %r)" % (source.getScriptName(), options))
        p.setValue(options.index(source.getScriptName()))
        QApplication.processEvents()

    def wait_follow(node, timeout, on_tick=None):
        """Ticks the GUI thread every TICK_MS until the follow's report arrives; returns (finished, stats)."""
        stats = {"ticks": 0, "max_gap": 0.0, "start": mono_ms(), "end": 0.0}
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline:
            yield ("sleep", TICK_MS)
            now = time.time()
            stats["max_gap"] = max(stats["max_gap"], (now - last) * 1000.0 - TICK_MS)
            stats["ticks"] += 1
            last = now
            if on_tick is not None:
                on_tick()
            if follow_finished(node):
                stats["end"] = mono_ms()
                return True, stats
        stats["end"] = mono_ms()
        return False, stats

    def run_follow(node, button, label, timeout=FOLLOW_TIMEOUT_S):
        press(node, button)
        ok, stats = yield from wait_follow(node, timeout)
        require(ok, "%s: the follow ended with a report (message %r)" % (label, message(node)))
        report("NOTE %s: the follow took %.0f ms, %d ticks, worst GUI tick gap %.0f ms; message %r"
               % (label, stats["end"] - stats["start"], stats["ticks"], stats["max_gap"], message(node)))
        return stats

    def read_stalls():
        if not STALL_LOG or not os.path.isfile(STALL_LOG):
            return []
        out = []
        with open(STALL_LOG) as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    s = json.loads(raw)
                    out.append((float(s["start_ms"]), float(s["duration_ms"])))
                except (KeyError, TypeError, ValueError):
                    continue
        return out

    def settle(ms=SHOT_MS):
        yield ("sleep", ms)

    def go_to(frame, ms=SEEK_MS):
        seek(frame)
        yield ("sleep", ms)

    # ---- viewer row --------------------------------------------------------------------------

    def row_widgets():
        found = {}
        for w in QApplication.allWidgets():
            try:
                tip = w.toolTip() or ""
                if not tip or not w.isVisible() or rg.ancestor_of_class(w, "ViewerTab") is None:
                    continue
                for key, hint in HINTS.items():
                    if hint in tip and key not in found:
                        found[key] = w
            except RuntimeError:
                continue
        return found

    def on_screen(w):
        top_left = w.mapToGlobal(w.rect().topLeft())
        centre = w.mapToGlobal(w.rect().center())
        screen = QGuiApplication.primaryScreen().geometry()
        return w.width() > 0 and w.height() > 0 and screen.contains(centre) and screen.contains(top_left)

    def check_row(host_label):
        ok = yield ("until", lambda: len(row_widgets()) == len(HINTS), POLL_TIMEOUT_S)
        found = row_widgets()
        missing = sorted(set(HINTS) - set(found))
        check(ok and not missing, "%s: the viewer shows the Follow Vectors row (missing: %r)" % (host_label, missing))
        hidden = sorted(k for k, w in found.items() if not on_screen(w))
        check(not hidden, "%s: every control of the row is on screen (off screen: %r)" % (host_label, hidden))
        return len(found)

    # ---- sections ----------------------------------------------------------------------------

    class Scene(object):
        pass

    def section_rows_roto(scene):
        gt.section("rows-roto")
        scene.roto, scene.ctx = rg.open_roto(app, "Roto")
        require(scene.roto is not None and scene.ctx is not None, "create a Roto with its panel open", gt.plugin_problem("Roto"))
        keep(scene.roto)
        keep(scene.ctx)
        require(scene.roto.connectInput(0, scene.read), "connect Roto to the plate")
        rg.activate()
        yield from settle()
        count = yield from check_row("Roto")
        models = [str(o) for o in param(scene.roto, "followModel").getOptions()]
        check(models == MODELS, "Roto: the model menu offers %r (has %r)" % (MODELS, models))
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
        set_choice(scene.roto, "followModel", "Per Point")
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        select_shapes([bez])
        yield from go_to(START)
        yield from run_follow(scene.roto, "followForwardToEnd", "viewer row, Per Point, forward to the end")
        check("is done" in message(scene.roto), "viewer row: the node message reports the follow is done (%r)" % message(scene.roto))
        worst = check_per_point_follow(bez, p0, "viewer row")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-viewer-row-follow.png", "Roto, Per Point, Forward To End pressed in the viewer row at frame %d: frame %d, largest error %.3f px (known pan %d,%d px a frame)"
             % (START, MAIN_LAST, worst, PAN_U, PAN_V))

    def right_click_item(tree, item):
        rect = tree.visualItemRect(item)
        point = QPointF(rect.center())
        for etype, buttons in ((QEvent.MouseButtonPress, Qt.RightButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
            QApplication.sendEvent(tree.viewport(), QMouseEvent(etype, point, tree.viewport().mapToGlobal(point), Qt.RightButton, buttons, Qt.NoModifier))
            QApplication.processEvents()

    def section_panel_entry(scene):
        gt.section("panel-entry")
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.gen)
        set_choice(scene.roto, "followModel", "Per Point")
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        tree = select_shapes([bez])
        item = tree_item(tree, bez.getLabel())
        yield from go_to(START)
        seen = {"labels": [], "enabled": False, "triggered": False}

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
            menu.close()

        QTimer.singleShot(PANEL_MENU_DELAY_MS, pick)
        right_click_item(tree, item)
        check(any(s.startswith("Follow Vectors") for s in seen["labels"]), "the panel menu has a Follow Vectors entry (entries %r)" % (seen["labels"],))
        check(seen["enabled"] and seen["triggered"], "the Follow Vectors entry is enabled for a shape and was triggered")
        ok, stats = yield from wait_follow(scene.roto, FOLLOW_TIMEOUT_S)
        require(ok, "panel entry: the follow ended with a report (message %r)" % message(scene.roto))
        report("NOTE panel entry: %d ticks, worst GUI tick gap %.0f ms; message %r" % (stats["ticks"], stats["max_gap"], message(scene.roto)))
        worst = check_per_point_follow(bez, p0, "panel entry")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-panel-follow.png", "Roto, Per Point, Follow Vectors... from the panel menu at frame %d: frame %d, largest error %.3f px" % (START, MAIN_LAST, worst))

    def section_models(scene):
        gt.section("models")
        for model in MODELS[1:]:
            reset_shapes(scene.ctx)
            ensure_source(scene.roto, scene.gen)
            set_choice(scene.roto, "followModel", model)
            bez = new_shape(scene.ctx)
            p0 = [pos(bez, i, START) for i in range(POINTS)]
            keys_before = keys_of(bez)
            select_shapes([bez])
            yield from go_to(START)
            yield from run_follow(scene.roto, "followForwardToEnd", model)
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
        set_choice(scene.roto, "followModel", "Per Point")
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
        yield from run_follow(scene.roto, "followForwardToEnd", "bridge, Forward at frame %d with a key at frame %d" % (START, BRIDGE_KEY))

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

    def section_forward_only(scene):
        gt.section("forward-only")
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.forward_gen)
        options = [str(o) for o in param(scene.roto, "followSource").getOptions()]
        check(scene.forward_gen.getScriptName() in options, "the forward-only source is listed (%r)" % (options,))
        check(scene.gen.getScriptName() in options, "the two-way source is listed (%r)" % (options,))
        check(scene.read.getScriptName() not in options, "a node with no motion layers is not listed (%r)" % (options,))
        check(scene.roto.getScriptName() not in options, "the host itself is not listed (%r)" % (options,))

        set_choice(scene.roto, "followModel", "Per Point")
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
        shot("m219-forward-only-refused.png", "Backward To Start on %s, which has no backward motion layer, is refused: %s" % (name, text.replace("\n", " ")))

        yield from go_to(START)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        yield from run_follow(scene.roto, "followForwardToEnd", "forward-only source, forward")
        check("is done" in message(scene.roto), "forward-only: a forward follow of the forward-only source completes (%r)" % message(scene.roto))
        worst = check_per_point_follow(bez, p0, "forward-only source")
        yield from go_to(MAIN_LAST)
        yield from settle()
        shot("m219-forward-only-follow.png", "Forward-only source %s followed forward to frame %d: largest error %.3f px" % (name, MAIN_LAST, worst))

    def section_abort(scene):
        gt.section("abort")
        set_project_range(START, PLATE_FRAMES)
        reset_shapes(scene.ctx)
        ensure_source(scene.roto, scene.slow_gen)
        set_choice(scene.roto, "followModel", "Per Point")
        bez = new_shape(scene.ctx)
        p0 = [pos(bez, i, START) for i in range(POINTS)]
        frames = list(range(START, PLATE_FRAMES + 1))
        before = snapshot(bez, frames)
        keys_before = keys_of(bez)
        select_shapes([bez])
        yield from go_to(START)

        state = {"aborted_at": None, "keys_at_abort": 0}

        def on_tick():
            if state["aborted_at"] is None and len(keys_of(bez)) >= ABORT_AFTER_KEYS:
                state["keys_at_abort"] = len(keys_of(bez))
                state["aborted_at"] = mono_ms()
                press(scene.roto, "followForwardToEnd")

        press(scene.roto, "followForwardToEnd")
        ok, stats = yield from wait_follow(scene.roto, ABORT_TIMEOUT_S, on_tick)
        require(ok, "abort: the follow ended with a report (message %r)" % message(scene.roto))
        require(state["aborted_at"] is not None, "abort: the follow was still running when %d keys had been baked; it finished first (message %r)"
                % (ABORT_AFTER_KEYS, message(scene.roto)))
        text = message(scene.roto)
        keys = keys_of(bez)
        last_key = max(keys)
        report("NOTE abort: pressed at %d keys, ended with keys %r, %d ticks, worst GUI tick gap %.0f ms; message %r"
               % (state["keys_at_abort"], keys, stats["ticks"], stats["max_gap"], text))
        check("aborted" in text, "abort: the report says the follow was aborted (%r)" % text)
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

    def section_undo(scene):
        gt.section("undo")
        frames = list(range(START, MAIN_LAST + 1))
        for model in ("Per Point", "Affine"):
            reset_shapes(scene.ctx)
            ensure_source(scene.roto, scene.gen)
            set_choice(scene.roto, "followModel", model)
            first = new_shape(scene.ctx)
            second = new_shape(scene.ctx, SQ2_X, SQ2_Y)
            shapes = [first, second]
            before = [snapshot(b, frames) for b in shapes]
            keys_before = [keys_of(b) for b in shapes]
            extra_before = [[param_of_shape(b, "extraMatrix").getValueAtTime(MAIN_LAST, d) for d in range(9)] for b in shapes]
            select_shapes(shapes)
            yield from go_to(START)
            yield from run_follow(scene.roto, "followForwardToEnd", "undo, %s on two shapes" % model)
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
        check(models == MODELS, "RotoPaint: the model menu offers %r (has %r)" % (MODELS, models))
        shot("m219-row-rotopaint.png", "RotoPaint: the Follow Vectors row in the viewer, %d of %d controls found" % (count, len(HINTS)))

    def steps():
        ok = yield ("until", lambda: gt.widgets_of("NodeGraph") != [], STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        rg.activate()

        pattern = make_plate(BASE)
        scene = Scene()
        scene.read = keep(app.createReader(pattern))
        require(scene.read is not None, "create a Read for " + pattern)
        scene.read.setPosition(0, 0)
        set_project_range(START, MAIN_LAST)
        scene.gen = make_generator(scene.read, 0, 150)
        scene.forward_gen = make_generator(scene.read, 150, 150, outputs="Forward")
        scene.slow_gen = make_generator(scene.read, 300, 150, method="TV-L1")
        yield ("sleep", SETTLE_MS)

        yield from section_rows_roto(scene)
        yield from section_viewer_row(scene)
        yield from section_panel_entry(scene)
        yield from section_models(scene)
        yield from section_bridge(scene)
        yield from section_forward_only(scene)
        yield from section_abort(scene)
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
