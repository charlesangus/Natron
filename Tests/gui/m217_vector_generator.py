"""The VectorGenerator in the app, on a small plate with known motion and an occluding object.

The plate is written at run time under build/m217-vectorgenerator/ with oiiotool: a noise
background that pans (+BG_U, +BG_V) pixels a frame and a 64x64 noise square, with its own
texture, that moves (+OBJ_U, 0) pixels a frame over it. Frames 1..5, evaluated at frame 3.
Forward is t -> t+1, so the background reads (+2, +1) and the square (+14, 0) in Natron's
y-up pixels. Ahead of the square the background is about to be covered (low Forward
confidence); behind it the background was just uncovered (low Backward confidence).

Checks, each with a screenshot of the viewer (m217-*.png in the output directory):
  the Forward layer, chosen from the viewer's layer menu, shows the square brighter than the
  background and both non-zero, and the vectors exported to EXR match the known motion;
  the vectorConfidence layer is high on the background and the square and low in the strips
  the square covers and uncovers, on the screen and in the exported values;
  switching Method between DIS and TV-L1 renders both and changes the picture;
  Show Low Confidence, on RGBA with the panel open, tints the strip the square covers red and the
  strip it uncovers blue and leaves the background alone, follows Confidence Threshold, keeps the
  render hash, and goes when it is turned off or the panel closes.
BENCH_VECTORGENERATOR=1 also times DIS and TV-L1 at Medium on a 2048x1080 plate and prints
the milliseconds per frame.

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD -e NATRON_GUI_TEST_OUT=build/m217-vectorgenerator/out \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m217_vector_generator.py <Natron AppImage>
"""
import os
import re
import statistics
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 3600
SETTLE_MS = 500
RENDER_MS = 4000
POLL_TIMEOUT_S = 30.0
RENDER_TIMEOUT_S = 1200.0
PLUGIN_VECTORGENERATOR = "VectorGenerator"
PLUGIN_SHUFFLE = "Shuffle"
VIEW_NAME = "Un-tone-mapped"
FORWARD_LAYER = "uk.co.thefoundry.OfxImagePlaneForwardMotionVector"
BACKWARD_LAYER = "uk.co.thefoundry.OfxImagePlaneBackMotionVector"
CONFIDENCE_LAYER = "vectorConfidence"
COLOUR_LAYER = "rgba"
ZOOM_TEXT = "100%"
ZOOM = 1

WIDTH, HEIGHT = 320, 200
CANVAS_W, CANVAS_H = 420, 420
FIRST_FRAME, LAST_FRAME, EVAL_FRAME = 1, 5, 3
BG_U, BG_V = 2, 1
OBJ_U = 14
OBJ_SIZE = 64
OBJ_X0, OBJ_Y = 60, 70
BG_X0, BG_Y0 = 100, 40
BENCH_WIDTH, BENCH_HEIGHT = 2048, 1080
BENCH_FRAMES = 5
BENCH_REPEATS = (2, 3, 4)

# Vector tolerances are in pixels; the confidence bounds are on the 0..1 scale.
BG_TOLERANCE = 0.5
OBJ_TOLERANCE = 1.5
OBJ_V_TOLERANCE = 1.0
STRIP_CONFIDENCE_MAX = 0.2
# TV-L1 reads higher in the strip the square uncovers: 0.283 at Medium on Backward.
TVL1_UNCOVERED_CONFIDENCE_MAX = 0.4
ELSEWHERE_CONFIDENCE_MIN = 0.9
CHANGED_PIXELS_MIN = 200
PIXEL_DIFF_MIN = 24
BLOCK = 2
SHOW_LOW_CONFIDENCE = "showLowConfidence"
CONFIDENCE_THRESHOLD = "confidenceThreshold"
DEFAULT_THRESHOLD = 0.5
# On the 0..255 screen scale; a half-opaque tint moves the difference of two channels by about 127.
TINT_GAIN_MIN = 40
UNTINTED_DIFF_MAX = 6
RESTORED_PIXELS_MAX = 20

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _viewer = []
    BASE = os.path.join(gt.ROOT, "build", "m217-vectorgenerator")

    def create(plugin_id, x=0, y=0, panel_open=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(panel_open)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def visible_viewer():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return views[0] if views else None

    def choose_view():
        for w in gt.widgets_of("ComboBox"):
            try:
                if not w.isVisible():
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
                if VIEW_NAME in items:
                    QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(VIEW_NAME)))
                    QApplication.processEvents()
                    return str(w.property("natronCurrentText")) == VIEW_NAME
            except RuntimeError:
                continue
        return False

    def set_zoom(text):
        for combo in gt.widgets_of("ComboBox"):
            try:
                if not combo.isVisible() or "Zoom:" not in (combo.toolTip() or ""):
                    continue
                items = [str(i).strip() for i in (combo.property("natronItems") or [])]
            except RuntimeError:
                continue
            if text in items:
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(text)))
                return True
            return check(False, "the zoom menu offers %s (%r)" % (text, items))
        return check(False, "found the viewer's zoom menu")

    def layer_combo():
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and "Layer:" in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def combo_items(combo):
        try:
            return [str(i) for i in (combo.property("natronItems") or [])]
        except RuntimeError:
            return []

    def choose_layer(text):
        """Picks the viewer layer-menu entry containing text; returns the entry or None."""
        combo = layer_combo()
        if combo is None:
            return None
        items = combo_items(combo)
        index = next((n for n, i in enumerate(items) if text in i), -1)
        if index < 0:
            return None
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
        QApplication.processEvents()
        return items[index]

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(
            top_left.x(), top_left.y(), widget.width(), widget.height())

    def save_viewer(name):
        image = viewer_crop()
        path = os.path.join(gt.OUT, name)
        if image is not None and image.save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save the viewer crop " + path)

    def changed_pixels(a, b, stride=2):
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def screen_colour(image, nx, ny):
        """Mean (r, g, b) of a block of viewer pixels at the plate pixel (nx, ny), Natron y-up,
        with the plate centred in the viewer at ZOOM."""
        cx = image.width() // 2 + int(round((nx - WIDTH / 2.0) * ZOOM))
        cy = image.height() // 2 - int(round((ny - HEIGHT / 2.0) * ZOOM))
        sums = [0, 0, 0]
        n = 0
        for y in range(cy - BLOCK, cy + BLOCK + 1):
            for x in range(cx - BLOCK, cx + BLOCK + 1):
                c = image.pixelColor(x, y)
                sums[0] += c.red()
                sums[1] += c.green()
                sums[2] += c.blue()
                n += 1
        return tuple(s / float(n) for s in sums)

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

    def make_plate(directory, width, height, frames, prefix="plate"):
        """Writes frames 1..frames of the moving plate as <prefix>.NNNN.exr; returns the file pattern."""
        os.makedirs(directory, exist_ok=True)
        canvas_w, canvas_h = width + 100, height + 220
        background = os.path.join(directory, "background.exr")
        square = os.path.join(directory, "square.exr")
        make_texture(directory, background, canvas_w, canvas_h, 7)
        make_texture(directory, square, OBJ_SIZE, OBJ_SIZE, 202)
        for f in range(1, frames + 1):
            path = os.path.join(directory, "%s.%04d.exr" % (prefix, f))
            run_tool(["oiiotool", square, background, "--cut", "%dx%d+%d+%d" % (width, height, BG_X0 - BG_U * f, BG_Y0 + BG_V * f),
                      "--paste", "+%d+%d" % (OBJ_X0 + OBJ_U * f, OBJ_Y), "-d", "float", "-o", path])
        return os.path.join(directory, prefix + ".####.exr")

    def region_mean(path, x, y, w, h):
        """Mean of each channel over the w x h window at (x, y) in file (y-down) coordinates."""
        out = run_tool(["oiiotool", path, "--crop", "%dx%d+%d+%d" % (w, h, x, y), "--printstats"])
        m = re.search(r"Stats Avg:\s+(.*?)\s+\(", out)
        require(m is not None, "oiiotool reports a mean for %s" % os.path.basename(path), out[-300:])
        return [float(v) for v in m.group(1).split()]

    def make_writer(source, x, y):
        write = app.createWriter(os.path.join(BASE, "export.####.exr"))
        require(write is not None, "create a Write")
        raw = [str(o) for o in param(write, "colorspace").getOptions()].index("Raw")
        param(write, "colorspace").setValue(raw)
        param(write, "bitDepth").setValue(2)
        write.setPosition(x, y)
        connect(write, 0, source)
        return write

    def make_shuffle(source, layer, x, y):
        """A Shuffle that copies the layer's two channels into rgba, with 0 and 1 in blue and alpha."""
        shuffle = create(PLUGIN_SHUFFLE, x, y)
        connect(shuffle, 0, source)
        in1 = param(shuffle, "in1")
        in1.setLayer(layer)
        out1 = param(shuffle, "out1")
        out1.setLayer(COLOUR_LAYER)
        require(in1.getLayer() == layer and out1.getLayer() == COLOUR_LAYER,
                "%s reads %s into rgba (read back %r, %r)" % (shuffle.getLabel(), layer, in1.getLayer(), out1.getLayer()))
        mapping = param(shuffle, "mapping")
        for c in range(2):
            mapping.connect("in1.#%d" % c, "out1.#%d" % c)
        mapping.connect("0", "out1.#2")
        mapping.connect("1", "out1.#3")
        return shuffle

    def make_exporter(source, layer, x, y):
        return make_writer(make_shuffle(source, layer, x, y), x, y + 120)

    def export_frame(write, name, frame):
        """Renders one frame to <name>.NNNN.exr and returns its path once the file is complete."""
        path = os.path.join(BASE, "%s.%04d.exr" % (name, frame))
        if os.path.exists(path):
            os.remove(path)
        param(write, "filename").setValue(os.path.join(BASE, name + ".####.exr"))
        param(write, "frameRange").setValue(2)
        param(write, "firstFrame").setValue(frame)
        param(write, "lastFrame").setValue(frame)
        app.render(write, frame, frame)
        state = {"size": -1}

        def stable():
            if not os.path.exists(path):
                return False
            size = os.path.getsize(path)
            same = size > 0 and size == state["size"]
            state["size"] = size
            return same

        ok = yield ("until", stable, RENDER_TIMEOUT_S)
        if not ok:
            report("NOTE Write filename %r, message %r, directory %r" % (
                param(write, "filename").get(), write.getPersistentMessage(), sorted(os.listdir(BASE))))
        require(ok, "the Write rendered " + os.path.basename(path))
        return path

    def seek(frame):
        app.getViewer(viewer_node().getScriptName()).seek(frame)

    def setup_graph(pattern):
        read = app.createReader(pattern)
        require(read is not None, "create a Read for " + pattern)
        read.setPosition(0, 0)
        node = create(PLUGIN_VECTORGENERATOR, 0, 150)
        connect(node, 0, read)
        return read, node

    def object_window(frame):
        left = OBJ_X0 + OBJ_U * frame
        return left, left + OBJ_SIZE

    def check_vectors(path, frame, label, direction):
        """Checks the exported vector against the known motion; direction is +1 for Forward, -1 for Backward."""
        left, right = object_window(frame)
        y0 = OBJ_Y + 8
        obj = region_mean(path, left + 10, y0, OBJ_SIZE - 20, OBJ_SIZE - 16)
        bg = region_mean(path, 200, 10, 100, 40)
        want_obj_u = direction * OBJ_U
        want_bg = (direction * BG_U, direction * BG_V)
        report("NOTE %s: background (U, V) = (%.2f, %.2f) expected (%d, %d); square (U, V) = (%.2f, %.2f) expected (%d, 0)"
               % (label, bg[0], bg[1], want_bg[0], want_bg[1], obj[0], obj[1], want_obj_u))
        check(abs(bg[0] - want_bg[0]) <= BG_TOLERANCE and abs(bg[1] - want_bg[1]) <= BG_TOLERANCE,
              "%s: the background vector is (%d, %d) within %.1f px (got %.2f, %.2f)"
              % (label, want_bg[0], want_bg[1], BG_TOLERANCE, bg[0], bg[1]))
        check(abs(obj[0] - want_obj_u) <= OBJ_TOLERANCE and abs(obj[1]) <= OBJ_V_TOLERANCE,
              "%s: the square's vector is (%d, 0) within %.1f px (got %.2f, %.2f)"
              % (label, want_obj_u, OBJ_TOLERANCE, obj[0], obj[1]))

    def check_confidence(path, frame, label, uncovered_max):
        left, right = object_window(frame)
        speed_gap = OBJ_U - BG_U
        y0, h = OBJ_Y + 6, OBJ_SIZE - 12
        covered = region_mean(path, right + 2, y0, speed_gap - 4, h)
        revealed = region_mean(path, left - speed_gap + 2, y0, speed_gap - 4, h)
        background = region_mean(path, 200, 10, 100, 40)
        inside = region_mean(path, left + 10, y0, OBJ_SIZE - 20, h)
        report("NOTE %s: Forward / Backward confidence: covered strip %.3f / %.3f, uncovered strip %.3f / %.3f, "
               "background %.3f / %.3f, square %.3f / %.3f" % (
                   label, covered[0], covered[1], revealed[0], revealed[1], background[0], background[1], inside[0], inside[1]))
        check(covered[0] < STRIP_CONFIDENCE_MAX,
              "%s: Forward confidence is below %.1f in the strip the square is about to cover (%.3f)" % (label, STRIP_CONFIDENCE_MAX, covered[0]))
        check(revealed[1] < uncovered_max,
              "%s: Backward confidence is below %.1f in the strip the square just uncovered (%.3f)" % (label, uncovered_max, revealed[1]))
        check(min(background[0], background[1], inside[0], inside[1]) > ELSEWHERE_CONFIDENCE_MIN,
              "%s: confidence is above %.1f on the background and inside the square (%.3f, %.3f, %.3f, %.3f)"
              % (label, ELSEWHERE_CONFIDENCE_MIN, background[0], background[1], inside[0], inside[1]))

    def screen_plate_points(frame):
        """Plate pixels (Natron y-up) of the square's centre, the strips beside it and the background."""
        left, right = object_window(frame)
        mid_y = HEIGHT - 1 - (OBJ_Y + OBJ_SIZE // 2)
        gap = OBJ_U - BG_U
        return {
            "square": ((left + right) // 2, mid_y),
            "covered": (right + gap // 2, mid_y),
            "uncovered": (left - gap // 2, mid_y),
            "background": (260, 40),
        }

    def wait_rendered():
        """Waits until two viewer grabs a second apart are identical: the viewer draws tiles as they finish."""
        yield ("sleep", RENDER_MS)
        deadline = time.time() + POLL_TIMEOUT_S
        previous = viewer_crop()
        while time.time() < deadline:
            yield ("sleep", 1000)
            current = viewer_crop()
            if changed_pixels(previous, current, 4) == 0:
                break
            previous = current
        ok = yield ("until", lambda: not gt.viewer_messages(), POLL_TIMEOUT_S)
        check(ok, "the viewer shows no error (%r)" % (gt.viewer_messages(),))

    def show_layer(node, text, shot_name):
        ok = yield ("until", lambda: layer_combo() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer's layer menu is on screen")
        listed = combo_items(layer_combo())
        report("NOTE viewer layer menu: %r" % (listed,))
        before = viewer_crop()
        entry = choose_layer(text)
        require(entry is not None, "the viewer's layer menu offers a %s entry" % text)
        report("NOTE chose the layer menu entry %r" % entry)
        ok = yield ("until", lambda: changed_pixels(before, viewer_crop(), 4) >= CHANGED_PIXELS_MIN, RENDER_TIMEOUT_S)
        check(ok, "the viewer redraws after choosing %s" % entry)
        yield from wait_rendered()
        save_viewer(shot_name)
        return viewer_crop()

    def section_states():
        gt.section("states")
        os.makedirs(BASE, exist_ok=True)
        pattern = make_plate(os.path.join(BASE, "plate"), WIDTH, HEIGHT, LAST_FRAME)
        read, node = setup_graph(pattern)
        forward_write = make_exporter(node, FORWARD_LAYER, 300, 300)
        backward_write = make_exporter(node, BACKWARD_LAYER, 600, 300)
        confidence_write = make_exporter(node, CONFIDENCE_LAYER, 900, 300)
        show(node)
        check(choose_view(), "the viewer's View menu is set to " + VIEW_NAME)
        seek(EVAL_FRAME)
        yield ("sleep", RENDER_MS)
        set_zoom(ZOOM_TEXT)
        check(param(node, "method").get() == 0, "Method defaults to DIS (%r)" % (param(node, "method").get(),))
        check(param(node, "quality").get() == 1, "Quality defaults to Medium (%r)" % (param(node, "quality").get(),))
        points = screen_plate_points(EVAL_FRAME)

        gt.section("plate")
        yield from wait_rendered()
        save_viewer("m217-plate.png")

        shots = {}
        for method_text, method_name in (("dis", "DIS"), ("tvl1", "TV-L1")):
            gt.section(method_name)
            method = param(node, "method")
            method.setValue(0 if method_text == "dis" else 1)
            check(method.get() == (0 if method_text == "dis" else 1), "Method is %s (%r)" % (method_name, method.get()))

            forward = yield from show_layer(node, "Forward", "m217-forward-%s.png" % method_text)
            shots[(method_text, "forward")] = forward
            sq = screen_colour(forward, *points["square"])
            bg = screen_colour(forward, *points["background"])
            report("NOTE %s Forward on screen: square %r background %r" % (method_name, sq, bg))
            check(bg[0] > 200 and bg[1] > 100 and sq[0] > 200 and sq[1] < 30,
                  "%s Forward layer: red (U) is lit on the background and the square, green (V) only on the background (%r, %r)"
                  % (method_name, tuple(round(v) for v in bg), tuple(round(v) for v in sq)))
            path = yield from export_frame(forward_write, "forward-" + method_text, EVAL_FRAME)
            check_vectors(path, EVAL_FRAME, "%s Forward" % method_name, 1)
            path = yield from export_frame(backward_write, "backward-" + method_text, EVAL_FRAME)
            check_vectors(path, EVAL_FRAME, "%s Backward" % method_name, -1)

            conf = yield from show_layer(node, CONFIDENCE_LAYER, "m217-confidence-%s.png" % method_text)
            shots[(method_text, "confidence")] = conf
            cov = screen_colour(conf, *points["covered"])
            unc = screen_colour(conf, *points["uncovered"])
            bgc = screen_colour(conf, *points["background"])
            sqc = screen_colour(conf, *points["square"])
            report("NOTE %s confidence on screen: covered %r uncovered %r background %r square %r" % (method_name, cov, unc, bgc, sqc))
            check(bgc[0] > 150 and bgc[1] > 150 and sqc[0] > 150 and sqc[1] > 150,
                  "%s confidence layer is bright (high) on the background and the square" % method_name)
            check(cov[0] < 0.5 * bgc[0],
                  "%s confidence layer: Forward (red) is dark in the strip ahead of the square (%.0f vs %.0f)" % (method_name, cov[0], bgc[0]))
            check(unc[1] < 0.5 * bgc[1],
                  "%s confidence layer: Backward (green) is dark in the strip behind the square (%.0f vs %.0f)" % (method_name, unc[1], bgc[1]))
            path = yield from export_frame(confidence_write, "confidence-" + method_text, EVAL_FRAME)
            check_confidence(path, EVAL_FRAME, method_name,
                             TVL1_UNCOVERED_CONFIDENCE_MAX if method_text == "tvl1" else STRIP_CONFIDENCE_MAX)

        gt.section("method switch")
        for kind in ("forward", "confidence"):
            n = changed_pixels(shots[("dis", kind)], shots[("tvl1", kind)])
            check(n >= CHANGED_PIXELS_MIN,
                  "switching DIS to TV-L1 changes the %s picture (%d sampled pixels differ, %d required)" % (kind, n, CHANGED_PIXELS_MIN))
        set_zoom("Fit")
        yield from section_overlay(read, points)

    def wait_restored(before, label):
        ok = yield ("until", lambda: 0 <= changed_pixels(before, viewer_crop()) <= RESTORED_PIXELS_MAX, RENDER_TIMEOUT_S)
        check(ok, "%s restores the picture (%d sampled pixels differ)" % (label, changed_pixels(before, viewer_crop())))

    def check_tint(before, after, points, label):
        """Forward's red tint in the strip ahead of the square, Backward's blue one behind it, nothing on the background."""
        cov0, cov1 = screen_colour(before, *points["covered"]), screen_colour(after, *points["covered"])
        unc0, unc1 = screen_colour(before, *points["uncovered"]), screen_colour(after, *points["uncovered"])
        bg0, bg1 = screen_colour(before, *points["background"]), screen_colour(after, *points["background"])
        report("NOTE %s tint: covered %r -> %r, uncovered %r -> %r, background %r -> %r" % (label, cov0, cov1, unc0, unc1, bg0, bg1))
        check((cov1[0] - cov1[2]) - (cov0[0] - cov0[2]) >= TINT_GAIN_MIN,
              "%s: the strip the square is about to cover is tinted red (%r -> %r)" % (label, cov0, cov1))
        check((unc1[2] - unc1[0]) - (unc0[2] - unc0[0]) >= TINT_GAIN_MIN,
              "%s: the strip the square just uncovered is tinted blue (%r -> %r)" % (label, unc0, unc1))
        check(max(abs(a - b) for a, b in zip(bg0, bg1)) <= UNTINTED_DIFF_MAX,
              "%s: the confident background is not tinted (%r -> %r)" % (label, bg0, bg1))

    def section_overlay(read, points):
        """Its own node, created with its panel open: the overlay draws only then."""
        gt.section("low confidence overlay")
        gt.close_all_panels()
        node = create(PLUGIN_VECTORGENERATOR, 1200, 150, panel_open=True)
        connect(node, 0, read)
        show(node)
        seek(EVAL_FRAME)
        yield ("sleep", RENDER_MS)
        # The opened panel can resize the viewer, and the plate points assume a centred plate.
        set_zoom("Fit")
        set_zoom(ZOOM_TEXT)
        before = yield from show_layer(node, COLOUR_LAYER, "m217-overlay-off.png")
        check(gt.any_panel_open(), "the node's panel is open")
        show_param = param(node, SHOW_LOW_CONFIDENCE)
        threshold = param(node, CONFIDENCE_THRESHOLD)
        check(show_param.get() is False, "Show Low Confidence defaults to off (%r)" % (show_param.get(),))
        check(threshold.get() == DEFAULT_THRESHOLD, "Confidence Threshold defaults to %.1f (%r)" % (DEFAULT_THRESHOLD, threshold.get()))

        for method_text, method_name, index in (("dis", "DIS", 0), ("tvl1", "TV-L1", 1)):
            gt.section("low confidence overlay " + method_name)
            param(node, "method").setValue(index)
            hash_before = node.getContentHash()
            yield from wait_rendered()
            show_param.setValue(True)
            ok = yield ("until", lambda: changed_pixels(before, viewer_crop()) >= CHANGED_PIXELS_MIN, RENDER_TIMEOUT_S)
            check(ok, "%s: Show Low Confidence draws over the viewer" % method_name)
            yield from wait_rendered()
            gt.shot("m217-overlay-%s.png" % method_text)
            tinted = viewer_crop()
            check_tint(before, tinted, points, method_name)
            check(node.getContentHash() == hash_before, "%s: the overlay leaves the render hash alone" % method_name)

            started = time.time()
            threshold.setValue(0.0)
            yield from wait_restored(before, "%s: a threshold of 0" % method_name)
            report("NOTE %s: threshold 0 redrew in %.0f ms" % (method_name, (time.time() - started) * 1000.0))

            threshold.setValue(1.0)
            ok = yield ("until", lambda: changed_pixels(before, viewer_crop()) > changed_pixels(before, tinted), POLL_TIMEOUT_S)
            check(ok, "%s: a threshold of 1 tints more than a threshold of %.1f (%d, %d sampled pixels)"
                  % (method_name, DEFAULT_THRESHOLD, changed_pixels(before, viewer_crop()), changed_pixels(before, tinted)))
            gt.shot("m217-overlay-threshold-1-%s.png" % method_text)
            check(node.getContentHash() == hash_before, "%s: the threshold leaves the render hash alone" % method_name)

            threshold.setValue(DEFAULT_THRESHOLD)
            show_param.setValue(False)
            yield from wait_restored(before, "%s: turning the overlay off" % method_name)

        show_param.setValue(True)
        ok = yield ("until", lambda: changed_pixels(before, viewer_crop()) >= CHANGED_PIXELS_MIN, RENDER_TIMEOUT_S)
        check(ok, "the overlay returns before the panel closes")
        gt.close_all_panels()
        yield from wait_restored(before, "closing the panel")
        set_zoom("Fit")

    def section_bench():
        gt.section("bench")
        directory = os.path.join(BASE, "bench-plate")
        pattern = make_plate(directory, BENCH_WIDTH, BENCH_HEIGHT, BENCH_FRAMES, "bench")
        read = app.createReader(pattern)
        require(read is not None, "create the bench Read")
        read.setPosition(0, 600)
        node = create(PLUGIN_VECTORGENERATOR, 0, 750)
        connect(node, 0, read)
        param(node, "quality").setValue(1)
        shuffle = make_shuffle(node, FORWARD_LAYER, 300, 750)

        def timed(source, name, frame):
            """Milliseconds to render one frame of source to a fresh Write, or None if it never finished."""
            write = make_writer(source, 600, 900)
            state = {"size": -1}
            path = os.path.join(BASE, "%s.%04d.exr" % (name, frame))
            if os.path.exists(path):
                os.remove(path)
            param(write, "filename").setValue(os.path.join(BASE, name + ".####.exr"))
            param(write, "frameRange").setValue(2)
            param(write, "firstFrame").setValue(frame)
            param(write, "lastFrame").setValue(frame)
            start = time.time()
            app.render(write, frame, frame)

            def stable():
                if not os.path.exists(path):
                    return False
                size = os.path.getsize(path)
                same = size > 0 and size == state["size"]
                state["size"] = size
                return same

            ok = yield ("until", stable, RENDER_TIMEOUT_S)
            elapsed = (time.time() - start) * 1000.0
            yield ("sleep", 500)
            write.destroy()
            return elapsed if ok else None

        warm = yield from timed(read, "bench-plain", 1)
        report("NOTE bench: first Read of the plate %.0f ms" % (warm or -1))
        baselines = []
        for frame in BENCH_REPEATS:
            ms = yield from timed(read, "bench-plain", frame)
            require(ms is not None, "the plain Write rendered frame %d" % frame)
            baselines.append(ms)
        baseline = statistics.median(baselines)
        report("BENCH Read + Write only, %dx%d: median %.0f ms per frame" % (BENCH_WIDTH, BENCH_HEIGHT, baseline))
        for method_text, method_name, index in (("dis", "DIS", 0), ("tvl1", "TV-L1", 1)):
            param(node, "method").setValue(index)
            times = []
            for frame in BENCH_REPEATS:
                ms = yield from timed(shuffle, "bench-" + method_text, frame)
                require(ms is not None, "%s rendered frame %d" % (method_name, frame))
                times.append(ms)
            report("BENCH %s, Medium, %dx%d: %s ms per frame (median %.0f ms; %.0f ms net of Read + Write)" % (
                method_name, BENCH_WIDTH, BENCH_HEIGHT, ", ".join("%.0f" % t for t in times),
                statistics.median(times), statistics.median(times) - baseline))

    def steps():
        ok = yield ("until", lambda: gt.widgets_of("NodeGraph") != [] , STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        yield from section_states()
        if os.environ.get("BENCH_VECTORGENERATOR") == "1":
            yield from section_bench()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
