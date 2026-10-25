"""The viewer shows the same picture, colour-picker values and histogram whether the comp's root
reaches it from the CPU or from a GPU region's device root through the hand-off ladder.

Graph: Read(generated 512x384 half EXR) -> Grade -> Blur -> ColorCorrect -> Viewer input A, and
Read -> Grade (B) -> Viewer input B. Each run holds one configuration; its viewer crops are saved
as PNG under $NATRON_GUI_TEST_OUT/<label>/ with a captures.json, and every run but the CPU
reference compares its crops and picker values with $NATRON_GUI_TEST_OUT/cpu/ (override the
directory with M107_REFERENCE_DIR, the label with M107_LABEL).

Per viewer texture mode (8u, 32f) the cases are, in M107_CASES (comma list, default all):
  matrix     channels RGB/R/G/B/A/Y/Matte x (neutral, gain 2, gamma 2.2, gamma 0)
  aces       the same channels under an ACES view, then a mid-session switch between views
  wipe       the B Grade connected as viewer input B and the A/B wipe on, then off and B disconnected again
  pan        zoom to 200% and a middle-button drag that brings new image area into view
  picker     the info bar's RGBA values at three viewport points, and the viewed node's output picked
             (Ctrl+click) at the middle one
  sweep      Grade multiply stepped through 16 values; every step must change the picture, and the last one
             must be darker than the first
  cache      after the sweep, multiply back to 1: the frame shown at the sweep's first step again (from the
             viewer cache), with the picker, input and output, reading that frame's values rather than the
             last step's
  histogram  (once, at the end, on the frame the cache case left on screen) a new histogram pane draws
             what the CPU run's draws
With NATRON_GPU_FAULT set only the sweep runs: the log must show the hand-off dropping to the cpu rung on a
device loss during it, and the sweep frames after the loss must still change. Its grabs are not compared with
the CPU reference, whose sweep ran in a different viewer layout. Every GPU run without a fault must log the
rung NATRON_VIEWER_HANDOFF asks for, and the CPU reference must log no rung but cpu.
"Gain 2" is the multiplier 2, so the viewer's f-stop box gets 1.

Run the CPU reference first, then each GPU configuration, into the same output directory
(natron-dev container, repository root; use a build with the Vulkan runtime for the GPU runs):
    OUT=build/m107-out
    for cfg in "off:" "force:hostsync" "force:readback"; do
      docker exec -e NATRON_GUI_TEST_OUT=$OUT -e GUI_TEST_TIMEOUT=1800 \
          -e NATRON_GPU=${cfg%%:*} -e NATRON_VIEWER_HANDOFF=${cfg#*:} \
          -w /home/bosley/git/Natron natron-dev \
          Tests/gui/run-gui-test.sh Tests/gui/m107_viewer_handoff.py build/fast/App/Natron
    done
    docker exec -e NATRON_GUI_TEST_OUT=$OUT -e GUI_TEST_TIMEOUT=900 -e NATRON_GPU=force \
        -e NATRON_GPU_FAULT=lost@40 -w /home/bosley/git/Natron natron-dev-gpu \
        Tests/gui/run-gui-test.sh Tests/gui/m107_viewer_handoff.py build/fast/App/Natron
The fault run needs a hardware device (natron-dev-gpu, tools/ci/local/devshell.sh --gpu): on lavapipe the cost
model moves the sweep's regions to the CPU after the first GPU frame, so the device sees fewer than 40 operations
in the whole run and the loss never fires.
An empty NATRON_VIEWER_HANDOFF is ignored by the engine. Each run must end with
"SUMMARY 0 failure(s)"; results.txt is overwritten by every run, so read it after each one.
Set M107_TOLERANCE (code values, default 1) or M107_PICK_TOL (default 0.002) to relax the checks.
An 8u grab may also differ by one more code value on at most M107_DITHER_SHARE (default 0.005) of its
pixels: the CPU diffuses each row's quantisation error from a random column while the GPU dithers by pixel
position, so where the two float pictures sit either side of a rounding edge their dithers land two apart.
"""
import json
import os
import re
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 25.0
SETTLE_MS = 500
STABLE_S = 0.35
TOLERANCE = int(os.environ.get("M107_TOLERANCE", "1"))
PICK_TOL = float(os.environ.get("M107_PICK_TOL", "0.002"))
DITHER_SHARE = float(os.environ.get("M107_DITHER_SHARE", "0.005"))
SWEEP_STEPS = 16
HISTOGRAM_COLOURED_RATIO = 0.10
HISTOGRAM_DIFF_SHARE = float(os.environ.get("M107_HISTOGRAM_DIFF_SHARE", "0.02"))
CHANNELS = ["RGB", "Red", "Green", "Blue", "Alpha", "Luminance", "Matte"]
VARIANTS = [("neutral", 0.0, 1.0), ("gain2", 1.0, 1.0), ("gamma2.2", 0.0, 2.2), ("gamma0", 0.0, 0.0)]
ALL_CASES = ["matrix", "aces", "wipe", "pan", "picker", "sweep", "cache", "histogram"]
INTEROP_ERROR = re.compile(r"interop|ViewerHandoff|zerocopy|hostsync|VK_ERROR", re.IGNORECASE)
RUNG_LINE = re.compile(r"Viewer hand-off rung: (\w+)(?: \((.*)\))?")
POST_LOSS_SWEEP_STEPS = 4
SWEEP_DARKENING = 15

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel

    import guitest as gt
    from guitest import check, finish, report, require

    GPU_ENV = os.environ.get("NATRON_GPU", "")
    FAULT_ENV = os.environ.get("NATRON_GPU_FAULT", "")
    HANDOFF_ENV = os.environ.get("NATRON_VIEWER_HANDOFF", "")
    IS_REFERENCE = GPU_ENV == "off"
    LABEL = os.environ.get("M107_LABEL") or (
        "cpu" if IS_REFERENCE else ("fault-" if FAULT_ENV else "") + (HANDOFF_ENV or "auto"))
    CASES = [c for c in os.environ.get("M107_CASES", "sweep" if FAULT_ENV else ",".join(ALL_CASES)).split(",") if c]
    REF_DIR = os.environ.get("M107_REFERENCE_DIR") or os.path.join(gt.OUT, "cpu")
    RUN_DIR = os.path.join(gt.OUT, LABEL)
    os.makedirs(RUN_DIR, exist_ok=True)

    captures = {}
    pickers = {}
    reference_meta = {}

    def widget_with_tip(class_name, needle):
        for w in gt.widgets_of(class_name):
            try:
                if w.isVisible() and needle in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def combo_items(combo):
        return [str(i) for i in (combo.property("natronItems") or [])]

    def combo_current(combo):
        return str(combo.property("natronCurrentText") or "")

    def choose(combo, text, exact=True):
        """Selects the first item equal to (or, if not exact, containing) text; False if there is none."""
        items = combo_items(combo)
        for index, item in enumerate(items):
            if item == text or (not exact and text.lower() in item.lower()):
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
                return True
        return False

    def set_spin(box, value):
        QMetaObject.invokeMethod(box, "setValue", Q_ARG(float, float(value)))
        QApplication.processEvents()

    def spin_text(box):
        try:
            return float(box.text())
        except ValueError:
            return None

    def viewer_widget():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def grab():
        """The viewer's pixels as seen on screen; a widget grab of the GL viewer can come out blank."""
        QApplication.processEvents()
        w = viewer_widget()
        origin = w.mapToGlobal(QPoint(0, 0))
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        crop = screen.copy(QRect(origin.x(), origin.y(), w.width(), w.height()))
        return crop.toImage().convertToFormat(QImage.Format_RGBX8888)

    def image_bytes(img):
        return bytes(img.constBits())[:img.sizeInBytes()]

    def max_diff(a, b):
        if a.size() != b.size():
            return None
        da, db = image_bytes(a), image_bytes(b)
        if da == db:
            return 0
        try:
            import numpy
            x = numpy.frombuffer(da, numpy.uint8).astype(numpy.int16)
            y = numpy.frombuffer(db, numpy.uint8).astype(numpy.int16)
            return int(numpy.abs(x - y).max())
        except ImportError:
            return max(abs(p - q) for p, q in zip(da[::3], db[::3]))

    def diff_over(a, b, limit):
        """How many pixels of a and b differ by more than limit in some channel, of how many."""
        da, db = image_bytes(a), image_bytes(b)
        try:
            import numpy
            x = numpy.frombuffer(da, numpy.uint8).astype(numpy.int16).reshape(-1, 4)[:, :3]
            y = numpy.frombuffer(db, numpy.uint8).astype(numpy.int16).reshape(-1, 4)[:, :3]
            return int((numpy.abs(x - y).max(axis=1) > limit).sum()), x.shape[0]
        except ImportError:
            over = sum(1 for i in range(0, len(da), 4)
                       if max(abs(da[i + k] - db[i + k]) for k in range(3)) > limit)
            return over, len(da) // 4

    def looks_blank(img):
        core = img.copy(QRect(img.width() // 4, img.height() // 4, img.width() // 2, img.height() // 2))
        return len(set(image_bytes(core)[::97])) < 8

    def centre_mean(img):
        """The mean RGB code value over the middle fifth of an image."""
        total, count = 0, 0
        for y in range(img.height() * 2 // 5, img.height() * 3 // 5, 3):
            for x in range(img.width() * 2 // 5, img.width() * 3 // 5, 3):
                c = img.pixelColor(x, y)
                total += c.red() + c.green() + c.blue()
                count += 3
        return total / float(count) if count else 0.0

    def log_rungs():
        """The (rung, reason) pairs the hand-off has logged so far, in order."""
        path = os.path.join(gt.OUT, "natron.log")
        if not os.path.isfile(path):
            return []
        with open(path, errors="replace") as f:
            return [(m.group(1), m.group(2) or "") for m in (RUNG_LINE.search(l) for l in f) if m]

    def loss_seen():
        return any(r == "cpu" and "device lost" in why for r, why in log_rungs())

    def write_meta():
        with open(os.path.join(RUN_DIR, "captures.json"), "w") as f:
            json.dump({"captures": captures, "pickers": pickers}, f, indent=1)

    def settle_and_grab(name, previous=None):
        """Waits for the picture to stop changing (and, given previous, to differ from it)."""
        yield ("sleep", SETTLE_MS)
        state = {"data": None, "since": time.time()}

        def stable():
            now = time.time()
            data = image_bytes(grab())
            if data != state["data"]:
                state["data"] = data
                state["since"] = now
                return False
            if previous is not None and data == image_bytes(previous):
                return False
            return now - state["since"] >= STABLE_S

        ok = yield ("until", stable, POLL_TIMEOUT_S)
        check(ok, "%s: the viewer image settled%s" % (name, "" if previous is None else " on a new picture"))
        return grab()

    def capture(name, previous=None):
        img = yield from settle_and_grab(name, previous)
        path = os.path.join(RUN_DIR, name + ".png")
        img.save(path)
        report("SHOT " + path)
        captures[name] = name + ".png"
        write_meta()
        check(not looks_blank(img), "%s: the viewer shows an image" % name)
        if not IS_REFERENCE and not FAULT_ENV:
            ref_path = os.path.join(REF_DIR, name + ".png")
            ref = QImage(ref_path).convertToFormat(QImage.Format_RGBX8888) if os.path.isfile(ref_path) else None
            if ref is None or ref.isNull():
                check(False, "%s: the CPU reference %s exists" % (name, ref_path))
            else:
                diff = max_diff(img, ref)
                if name.startswith("8u-") and diff == TOLERANCE + 1:
                    over, total = diff_over(img, ref, TOLERANCE)
                    check(over <= DITHER_SHARE * total,
                          "%s: within %d code value(s) of the CPU grab but for dither (%d of %d pixels %d apart)" %
                          (name, TOLERANCE, over, total, diff))
                else:
                    check(diff is not None and diff <= TOLERANCE,
                          "%s: within %d code value(s) of the CPU grab (max difference %s)" % (name, TOLERANCE, diff))
        return img

    def make_source(path):
        import subprocess
        os.makedirs(os.path.dirname(path), exist_ok=True)
        corners = "fill:topleft=0.02,0.05,0.5,0.3:topright=1.4,0.3,0.05,1:bottomleft=0.1,0.8,0.1,0.6:bottomright=0.6,0.6,0.9,0.1"
        try:
            subprocess.run(["oiiotool", "--pattern", corners, "512x384", "4", "-d", "half", "-o", path],
                           check=True, capture_output=True)
            return True
        except (OSError, subprocess.CalledProcessError):
            pass
        try:
            import OpenImageIO as oiio
            import numpy
            ys, xs = numpy.mgrid[0:384, 0:512]
            u = (xs / 511.0)[..., None]
            v = (ys / 383.0)[..., None]
            tl, tr = numpy.array([0.02, 0.05, 0.5, 0.3]), numpy.array([1.4, 0.3, 0.05, 1.0])
            bl, br = numpy.array([0.1, 0.8, 0.1, 0.6]), numpy.array([0.6, 0.6, 0.9, 0.1])
            pixels = ((tl * (1 - u) + tr * u) * (1 - v) + (bl * (1 - u) + br * u) * v).astype(numpy.float32)
            out = oiio.ImageOutput.create(path)
            ok = out.open(path, oiio.ImageSpec(512, 384, 4, "half")) and out.write_image(pixels)
            out.close()
            return bool(ok)
        except ImportError:
            return False

    def create(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        return node

    def set_param(node, names, *values):
        for name in names:
            param = node.getParam(name)
            if param is not None:
                param.set(*values)
                return True
        return False

    def mouse(widget, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(kind, QPointF(pos), QPointF(widget.mapToGlobal(pos)), button,
                                                   buttons, modifiers))

    def find_aces(display_combo, view_combo):
        """Selects a view whose name mentions ACES, in the current display or else in an ACES display."""
        if choose(view_combo, "ACES", exact=False):
            return True
        if choose(display_combo, "ACES", exact=False):
            return True
        return False

    def info_picker_values():
        """The RGBA numbers the info bar shows for the first visible viewer, or None."""
        bars = [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]
        for bar in bars:
            labels = []
            for lab in bar.findChildren(QLabel):
                numbers = re.findall(r"-?\d+\.\d{5}", lab.text() or "")
                if len(numbers) >= 3 and lab.isVisible():
                    labels.append((lab.mapTo(bar, QPoint(0, 0)).x(), [float(n) for n in numbers]))
            if labels:
                return sorted(labels, key=lambda p: p[0])[0][1]
        return None

    def pick_at(w, fx, fy):
        """Moves the pointer onto a viewport point and returns the info bar's values there, or None."""
        target = QPoint(int(w.width() * fx), int(w.height() * fy))
        nudge = [0]

        def picked():
            nudge[0] ^= 1
            QTest.mouseMove(w, target + QPoint(nudge[0], 0))
            return info_picker_values() is not None

        yield ("until", picked, 5.0)
        yield ("sleep", 500)
        QTest.mouseMove(w, target)
        yield ("sleep", 300)
        return info_picker_values()

    def info_picker_source():
        """"in" or "out", whichever the info bar of the first visible viewer says its values were read from."""
        for bar in [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]:
            for lab in bar.findChildren(QLabel):
                found = re.search(r">(in|out)<", lab.text() or "")
                if found and lab.isVisible():
                    return found.group(1)
        return None

    def pick_output_at(w, fx, fy):
        """Ctrl+clicks a viewport point and returns the viewed node's output values the info bar then shows,
        or None, then clicks again to leave the picker. Ctrl+Alt picks the input when no eyedropper is on."""
        target = QPoint(int(w.width() * fx), int(w.height() * fy))
        QTest.mouseMove(w, target)
        yield ("sleep", 300)
        mods = Qt.ControlModifier
        mouse(w, QEvent.MouseButtonPress, target, Qt.LeftButton, Qt.LeftButton, mods)
        mouse(w, QEvent.MouseButtonRelease, target, Qt.LeftButton, Qt.NoButton, mods)
        yield ("sleep", 300)
        values = info_picker_values()
        source = info_picker_source()
        mouse(w, QEvent.MouseButtonPress, target, Qt.LeftButton, Qt.LeftButton)
        mouse(w, QEvent.MouseButtonRelease, target, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", 300)
        if source != "out":
            report("NOTE output pick read from %r" % source)
            return None
        return values

    def compare_pick(name, values):
        pickers[name] = values
        write_meta()
        if not IS_REFERENCE and not FAULT_ENV:
            expected = reference_meta.get("pickers", {}).get(name)
            match = (expected is not None and len(expected) == len(values)
                     and all(abs(a - b) <= PICK_TOL for a, b in zip(values, expected)))
            check(match, "%s: picker %r matches the CPU run's %r within %g" % (name, values, expected, PICK_TOL))

    def steps():
        check(IS_REFERENCE or GPU_ENV == "force", "NATRON_GPU is off or force (it is %r)" % GPU_ENV)
        report("NOTE label=%s NATRON_GPU=%r NATRON_VIEWER_HANDOFF=%r NATRON_GPU_FAULT=%r cases=%s" %
               (LABEL, GPU_ENV, HANDOFF_ENV, FAULT_ENV, ",".join(CASES)))
        if not IS_REFERENCE:
            meta_path = os.path.join(REF_DIR, "captures.json")
            require(os.path.isfile(meta_path), "the CPU reference exists at " + meta_path,
                    "run the NATRON_GPU=off configuration first")
            with open(meta_path) as f:
                reference_meta.update(json.load(f))

        source = os.path.join(gt.OUT, "fixtures", "m107-gradient.exr")
        require(make_source(source), "write the gradient EXR", "no oiiotool or OpenImageIO")
        read = app.createReader(source)
        require(read is not None, "create a Read for the gradient")
        grade = create("net.sf.openfx.GradePlugin")
        blur = create("net.sf.cimg.CImgBlur")
        colour = create("net.sf.openfx.ColorCorrectPlugin")
        grade_b = create("net.sf.openfx.GradePlugin")
        viewer = app.getNode("Viewer1") or create("fr.inria.built-in.Viewer")
        require(grade.connectInput(0, read) and blur.connectInput(0, grade) and colour.connectInput(0, blur)
                and viewer.connectInput(0, colour) and grade_b.connectInput(0, read), "wire the comp")
        require(set_param(grade, ["multiply"], 1.0, 1.0, 1.0, 1.0), "Grade has a multiply parameter")
        require(set_param(grade_b, ["multiply"], 0.5, 0.5, 0.5, 1.0), "the B Grade has a multiply parameter")
        check(set_param(blur, ["size"], 9.0, 9.0), "set the Blur size")
        check(set_param(colour, ["MasterSaturation"], 0.8, 0.8, 0.8, 0.8), "set the ColorCorrect saturation")
        check(set_param(colour, ["MasterGain"], 1.1, 1.1, 1.1, 1.1), "set the ColorCorrect gain")
        for n, node in enumerate([read, grade, blur, colour, viewer]):
            node.setPosition(100, 120 * n)
        grade_b.setPosition(300, 120)
        py_viewer = app.getViewer(viewer.getScriptName())

        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "a viewer is up")
        yield ("sleep", 1000)

        ok = yield ("until", lambda: (gt.viewer_centre_colour() or (0, 0, 0)) != (0, 0, 0), POLL_TIMEOUT_S)
        require(ok, "the viewer shows the comp")

        tab = next((w for w in gt.widgets_of("ViewerTab") if w.isVisible()), None)
        channels = widget_with_tip("ComboBox", "Display Channels:")
        display = widget_with_tip("ComboBox", "Display:")
        view = widget_with_tip("ComboBox", "View:")
        operator = widget_with_tip("ComboBox", "Operation applied between viewer inputs")
        input_b = widget_with_tip("ComboBox", "Viewer input B.")
        zoom = widget_with_tip("ComboBox", "Zoom:")
        gain_box = widget_with_tip("SpinBox", "Gain is shown as f-stops")
        gamma_box = widget_with_tip("SpinBox", "Viewer gamma correction level")
        require(None not in (tab, channels, display, view, operator, input_b, zoom, gain_box, gamma_box),
                "found the viewer tab's channel, display, view, operator, input, zoom, gain and gamma widgets",
                repr([tab, channels, display, view, operator, input_b, zoom, gain_box, gamma_box]))
        default_view = combo_current(view)
        default_display = combo_current(display)
        settings = NatronEngine.natron.getSettings().getParam("texturesBitDepth")
        require(settings is not None, "the settings have a texturesBitDepth parameter")

        def apply_variant(stops, gamma):
            set_spin(gain_box, stops)
            set_spin(gamma_box, gamma)
            check(spin_text(gain_box) == stops and spin_text(gamma_box) == gamma,
                  "gain box %r and gamma box %r hold %s and %s" % (gain_box.text(), gamma_box.text(), stops, gamma))

        def restore_display():
            choose(display, default_display)
            choose(view, default_view)
            apply_variant(0.0, 1.0)
            choose(channels, "RGB")

        def switch_mode(index, tag):
            settings.set(index)
            py_viewer.renderCurrentFrame(False)
            restore_display()
            check(settings.get() == index, "%s: the viewers' texture mode is set" % tag)

        post_loss = []
        for mode_index, mode in enumerate(["8u", "32f"]):
            gt.section(mode)
            switch_mode(mode_index, mode)

            if "matrix" in CASES:
                for variant, stops, gamma in VARIANTS:
                    apply_variant(stops, gamma)
                    for name in CHANNELS:
                        require(choose(channels, name, exact=False), "choose channels " + name,
                                repr(combo_items(channels)))
                        yield from capture("%s-%s-%s" % (mode, variant, name.lower()))
                restore_display()

            if "aces" in CASES:
                require(find_aces(display, view), "the OCIO config offers an ACES display or view",
                        "display items %r, view items %r" % (combo_items(display), combo_items(view)))
                yield ("sleep", SETTLE_MS)
                aces_display, aces_view = combo_current(display), combo_current(view)
                report("NOTE %s: ACES display %r view %r" % (mode, aces_display, aces_view))
                for name in CHANNELS:
                    require(choose(channels, name, exact=False), "choose channels " + name)
                    yield from capture("%s-aces-%s" % (mode, name.lower()))
                choose(channels, "RGB")
                alternatives = [v for v in combo_items(view) if v not in (aces_view, default_view)]
                before = yield from capture("%s-ocio-aces" % mode)
                for step, target in enumerate([default_view] + alternatives[:1] + [aces_view]):
                    if target == combo_current(view):
                        continue
                    check(choose(view, target), "%s: mid-session switch to view %r" % (mode, target))
                    before = yield from capture("%s-ocio-switch%d" % (mode, step), previous=before)
                restore_display()

            if "wipe" in CASES:
                shown = yield from settle_and_grab("%s-ab-before" % mode)
                require(viewer.getInput(1) is not None or viewer.connectInput(1, grade_b),
                        "connect the B Grade to viewer input 1")
                # Connecting a node to a viewer input also makes it the viewer's active A input.
                py_viewer.setAInput(0)
                yield ("sleep", SETTLE_MS)
                check(py_viewer.getAInput() == 0, "%s: viewer input A is back on the ColorCorrect (%d)" %
                      (mode, py_viewer.getAInput()))
                check(choose(input_b, grade_b.getScriptName(), exact=False), "%s: pick the B Grade as viewer input B" % mode)
                plain = yield from capture("%s-ab-a-only" % mode)
                check(abs(centre_mean(plain) - centre_mean(shown)) <= 2,
                      "%s: with B picked and no wipe the viewer still shows A (centre %.1f, before %.1f)" %
                      (mode, centre_mean(plain), centre_mean(shown)))
                check(choose(operator, "W-Under"), "%s: turn the A/B wipe on" % mode)
                yield from capture("%s-ab-wipe" % mode, previous=plain)
                choose(operator, " - ")
                off = yield from capture("%s-ab-wipe-off" % mode)
                check(abs(centre_mean(off) - centre_mean(plain)) <= 2,
                      "%s: the wipe turned off shows A again (centre %.1f, A %.1f)" %
                      (mode, centre_mean(off), centre_mean(plain)))
                choose(input_b, " - ")
                viewer.disconnectInput(1)
                yield ("sleep", SETTLE_MS)
                check(py_viewer.getAInput() == 0 and viewer.getInput(1) is None,
                      "%s: B disconnected, viewer input A stays on the ColorCorrect" % mode)

            if "pan" in CASES:
                w = viewer_widget()
                fit = yield from capture("%s-pan-fit" % mode)
                check(choose(zoom, "200%"), "%s: zoom to 200%%" % mode)
                yield ("sleep", 300)
                zoomed = yield from capture("%s-pan-zoomed" % mode, previous=fit)
                start = QPoint(w.width() // 2, w.height() // 2)
                mouse(w, QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton)
                for n in range(1, 9):
                    mouse(w, QEvent.MouseMove, start + QPoint(-18 * n, -12 * n), Qt.NoButton, Qt.MiddleButton)
                    yield ("sleep", 30)
                mouse(w, QEvent.MouseButtonRelease, start + QPoint(-144, -96), Qt.MiddleButton, Qt.NoButton)
                yield from capture("%s-pan-panned" % mode, previous=zoomed)
                QMetaObject.invokeMethod(tab, "centerViewer")
                choose(zoom, "100%")
                yield ("sleep", SETTLE_MS)

            if "picker" in CASES:
                w = viewer_widget()
                for n, (fx, fy) in enumerate([(0.4, 0.5), (0.5, 0.5), (0.6, 0.3)]):
                    name = "%s-picker%d" % (mode, n)
                    values = yield from pick_at(w, fx, fy)
                    check(values is not None, "%s: the info bar shows picked values (%r)" % (name, values))
                    if values is not None:
                        compare_pick(name, values)
                name = "%s-picker1-output" % mode
                values = yield from pick_output_at(w, 0.5, 0.5)
                check(values is not None, "%s: the info bar shows the viewed node's output (%r)" % (name, values))
                if values is not None:
                    compare_pick(name, values)
                QApplication.sendEvent(w, QEvent(QEvent.Leave))

            if "sweep" in CASES:
                first = previous = yield from capture("%s-sweep-00" % mode)
                for n in range(1, SWEEP_STEPS):
                    value = 1.0 - 0.04 * n
                    lost_before = FAULT_ENV and loss_seen()
                    set_param(grade, ["multiply"], value, value, value, 1.0)
                    img = yield from capture("%s-sweep-%02d" % (mode, n), previous=previous)
                    if lost_before:
                        post_loss.append(image_bytes(img) != image_bytes(previous))
                    previous = img
                check(centre_mean(previous) < centre_mean(first) - SWEEP_DARKENING,
                      "%s: the sweep darkens the picture (centre %.1f at multiply 1, %.1f at the last step)" %
                      (mode, centre_mean(first), centre_mean(previous)))
                set_param(grade, ["multiply"], 1.0, 1.0, 1.0, 1.0)
                if "cache" in CASES:
                    again = yield from capture("%s-cache-again" % mode, previous=previous)
                    diff = max_diff(again, first)
                    check(diff is not None and diff <= TOLERANCE,
                          "%s: multiply back to 1 shows the sweep's first frame again (max difference %s)" % (mode, diff))
                    w = viewer_widget()
                    values = yield from pick_at(w, 0.5, 0.5)
                    check(values is not None, "%s-cache-pick: the info bar shows picked values (%r)" % (mode, values))
                    if values is not None:
                        compare_pick("%s-cache-pick" % mode, values)
                        own = pickers.get("%s-picker1" % mode)
                        if own is not None:
                            check(len(own) == len(values) and all(abs(a - b) <= PICK_TOL for a, b in zip(values, own)),
                                  "%s-cache-pick: the picker reads the frame on screen, as at the picker case (%r, %r)" %
                                  (mode, values, own))
                    name = "%s-cache-pick-output" % mode
                    values = yield from pick_output_at(w, 0.5, 0.5)
                    check(values is not None, "%s: the info bar shows the viewed node's output (%r)" % (name, values))
                    if values is not None:
                        compare_pick(name, values)
                        own = pickers.get("%s-picker1-output" % mode)
                        if own is not None:
                            check(len(own) == len(values) and all(abs(a - b) <= PICK_TOL for a, b in zip(values, own)),
                                  "%s: the output picker reads the frame on screen, as at the picker case (%r, %r)" %
                                  (name, values, own))
                    QApplication.sendEvent(w, QEvent(QEvent.Leave))
                yield ("sleep", SETTLE_MS)

            gt.shot("m107-%s-%s.png" % (LABEL, mode))

        if "histogram" in CASES:
            gt.section("histogram")
            viewer_tab_widget = viewer_widget()
            # allWidgets() comes from a pointer-keyed set, so its order changes between runs. The histogram's size, and
            # with it the coloured pixel count, follows the pane it opens in, so every run must pick the same pane.
            def pane_key(p):
                origin = p.mapToGlobal(QPoint(0, 0))
                return (p.objectName(), origin.y(), origin.x())
            visible = sorted((p for p in gt.widgets_of("TabWidget") if p.isVisible()), key=pane_key)
            panes = [p for p in visible if not p.isAncestorOf(viewer_tab_widget)]
            pane = panes[0] if panes else visible[0]
            report("NOTE histogram pane: %s" % ("a pane without the viewer" if panes else "the viewer's own pane"))
            QMetaObject.invokeMethod(pane, "newHistogramHere")
            ok = yield ("until", lambda: any(h.isVisible() for h in gt.widgets_of("Histogram")), POLL_TIMEOUT_S)
            require(ok, "a histogram pane opens")
            yield ("sleep", 1500)
            histogram = next(h for h in gt.widgets_of("Histogram") if h.isVisible())
            origin = histogram.mapToGlobal(QPoint(0, 0))
            crop = QGuiApplication.primaryScreen().grabWindow(0).copy(
                QRect(origin.x(), origin.y(), histogram.width(), histogram.height())).toImage()
            path = os.path.join(RUN_DIR, "histogram.png")
            crop.save(path)
            report("SHOT " + path)
            coloured = 0
            for y in range(0, crop.height(), 2):
                for x in range(0, crop.width(), 2):
                    c = crop.pixelColor(x, y)
                    if max(c.red(), c.green(), c.blue()) - min(c.red(), c.green(), c.blue()) > 60:
                        coloured += 1
            pickers["histogram-coloured"] = [coloured]
            write_meta()
            check(coloured > 50, "the histogram draws coloured curves (%d sampled pixels)" % coloured)
            if not IS_REFERENCE:
                expected = reference_meta.get("pickers", {}).get("histogram-coloured", [0])[0]
                check(expected > 0 and abs(coloured - expected) <= HISTOGRAM_COLOURED_RATIO * expected,
                      "the histogram's coloured pixels (%d) are within %d%% of the CPU run's (%d)" %
                      (coloured, int(HISTOGRAM_COLOURED_RATIO * 100), expected))
                ref = QImage(os.path.join(REF_DIR, "histogram.png"))
                if ref.isNull() or ref.size() != crop.size():
                    report("NOTE no CPU histogram of the same size to compare (%s)" % ("missing" if ref.isNull() else "size"))
                elif not FAULT_ENV:
                    ref = ref.convertToFormat(crop.format())
                    sampled = differing = 0
                    for y in range(0, crop.height(), 2):
                        for x in range(0, crop.width(), 2):
                            a = crop.pixelColor(x, y)
                            b = ref.pixelColor(x, y)
                            sampled += 1
                            if max(abs(a.red() - b.red()), abs(a.green() - b.green()), abs(a.blue() - b.blue())) > 60:
                                differing += 1
                    share = differing / float(max(sampled, 1))
                    check(share <= HISTOGRAM_DIFF_SHARE,
                          "the histogram draws the CPU run's curves (%d of %d sampled pixels differ, at most %g)" %
                          (differing, sampled, HISTOGRAM_DIFF_SHARE))

        gt.section("log")
        log_path = os.path.join(gt.OUT, "natron.log")
        lines = []
        if os.path.isfile(log_path):
            with open(log_path, errors="replace") as f:
                lines = [l.rstrip() for l in f if INTEROP_ERROR.search(l)]
        for line in lines[:10]:
            report("NOTE log: " + line)
        rungs = log_rungs()
        for rung, why in rungs[:20]:
            report("NOTE rung: %s (%s)" % (rung, why))
        if IS_REFERENCE:
            check(not lines, "NATRON_GPU=off: the log has no interop or hand-off messages (%d)" % len(lines))
            check(all(r == "cpu" for r, _ in rungs), "NATRON_GPU=off: the hand-off logs no rung but cpu (%r)" % rungs)
        elif FAULT_ENV:
            report("NOTE %d hand-off or Vulkan lines in the log after %s" % (len(lines), FAULT_ENV))
            loss_at = next((i for i, (r, why) in enumerate(rungs) if r == "cpu" and "device lost" in why), None)
            check(loss_at is not None, "%s: the log shows the hand-off dropping to the cpu rung on a device loss%s" %
                  (FAULT_ENV, "" if loss_at is not None else
                   " (none: the fault fires only on that device operation, which a CPU-type device such as lavapipe,"
                   " whose regions the cost model moves to the CPU, may never reach)"))
            check(loss_at is not None and any(r != "cpu" for r, _ in rungs[:loss_at]),
                  "%s: a GPU rung was in use before the loss (%r)" % (FAULT_ENV, rungs[:loss_at or 0]))
            check(len(post_loss) >= POST_LOSS_SWEEP_STEPS and all(post_loss),
                  "%s: every sweep step after the loss changed the picture (%d of %d)" %
                  (FAULT_ENV, sum(post_loss), len(post_loss)))
        elif HANDOFF_ENV:
            check(any(r == HANDOFF_ENV for r, _ in rungs),
                  "the log shows the hand-off on the %s rung NATRON_VIEWER_HANDOFF asks for" % HANDOFF_ENV)

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
