"""GUI check that the viewer's persistent-message banner is legible and routes its colour by message type.

Graph: Constant -> SlitScan1 -> SlitScan2 -> Merge (input B) -> Viewer. A SlitScan whose gain spans
more than 64 frames posts a clipped-window warning; a Read of a missing file, wired into the Merge's
input A, posts an error.

Steps, each polled until the viewer shows the expected messages:
  1. one warning (SlitScan1)   -> banner yellow, dark text on it;
  2. two warnings (SlitScan2 too, with a different offset so the text differs) -> still yellow;
  3. two warnings and an error -> red, light text on it.
A banner pixel is sampled from a screen grab of the viewer and the strip is scanned for dark
(warning) or light (error) text pixels. The grab is saved after each step.

Screenshots: m326_one_warning.png, m326_two_warnings.png, m326_warnings_and_error.png (full window)
and a _viewer.png crop of each.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
SETTLE_MS = 800
BANNER_PROBE_Y = 4
BANNER_PROBE_MARGIN_X = 30
YELLOW = (166, 166, 0)
RED = (128, 0, 0)
COLOUR_TOLERANCE = 12
MIN_TEXT_PIXELS = 40

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_messages, viewer_up, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def viewer_widget():
        visible = [w for w in widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def grab_viewer():
        """The viewer's area of a screen grab (a widget grab of the GL viewer can come out blank
        under llvmpipe), as a QImage."""
        w = viewer_widget()
        if w is None:
            return None
        QApplication.processEvents()
        origin = w.mapToGlobal(w.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(origin.x(), origin.y(), w.width(), w.height())

    def near(c, target):
        return all(abs(a - b) <= COLOUR_TOLERANCE for a, b in zip(c, target))

    def banner_colour():
        image = grab_viewer()
        if image is None:
            return None
        c = image.pixelColor(image.width() - BANNER_PROBE_MARGIN_X, BANNER_PROBE_Y)
        return (c.red(), c.green(), c.blue())

    def banner_text_pixels(dark):
        """How many pixels in the banner's top rows are text-coloured: near-black when dark,
        light when not. The strip is at least one line tall; the first 20 rows are all banner."""
        image = grab_viewer()
        if image is None:
            return 0
        count = 0
        for y in range(2, 20):
            for x in range(0, image.width() // 2):
                c = image.pixelColor(x, y)
                luma = (c.red() + c.green() + c.blue()) / 3.0
                if (dark and luma < 70) or (not dark and luma > 170):
                    count += 1
        return count

    def save(name):
        shot(name + ".png")
        image = grab_viewer()
        if image is not None:
            path = os.path.join(OUT, name + "_viewer.png")
            report(("SHOT " if image.save(path) else "NOTE could not save ") + path)

    def banner_step(label, name, nb_messages, colour, text_is_dark):
        ok = yield ("until", lambda: len(viewer_messages()) >= nb_messages and banner_colour() is not None and near(banner_colour(), colour), POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        messages = viewer_messages()
        report("NOTE %s: messages %r, banner colour %r" % (label, messages, banner_colour()))
        check(ok, "%s: the banner settled to %r within %gs" % (label, colour, POLL_TIMEOUT_S))
        check(len(messages) >= nb_messages, "%s: the viewer shows at least %d message(s) (got %d)" % (label, nb_messages, len(messages)))
        check(near(banner_colour() or (0, 0, 0), colour), "%s: the banner is %s (got %r)" % (label, "yellow" if colour == YELLOW else "red", banner_colour()))
        text = banner_text_pixels(text_is_dark)
        check(text >= MIN_TEXT_PIXELS, "%s: the banner text is %s on the strip (%d text pixels)" % (label, "dark" if text_is_dark else "light", text))
        save(name)

    def steps():
        ok = yield ("until", lambda: bool(widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")

        section("setup")
        constant = make("Constant", 0, 0)
        scan1 = make("SlitScan", 0, 100)
        scan2 = make("SlitScan", 0, 200)
        merge = make("Merge", 0, 300)
        read = make("Read", 200, 200)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        viewer.setPosition(0, 400)
        require(scan1.connectInput(0, constant), "connect SlitScan1 to the Constant")
        require(scan2.connectInput(0, scan1), "connect SlitScan2 to SlitScan1")
        require(merge.connectInput(0, scan2), "connect the Merge's B input to SlitScan2")
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, merge), "the Viewer shows the Merge")
        for node, name in ((scan1, "SlitScan1"), (scan2, "SlitScan2")):
            for knob in ("retimeGain", "retimeOffset"):
                require(node.getParam(knob) is not None, "%s has %s" % (name, knob))
        require(read.getParam("filename") is not None, "Read has a filename")
        read.getParam("filename").setValue(os.path.join(OUT, "m326_missing_file.exr"))
        app.clearSelection()
        ok = yield ("until", viewer_up, POLL_TIMEOUT_S)
        require(ok, "the viewer is visible")
        yield ("sleep", SETTLE_MS)

        section("one warning")
        scan1.getParam("retimeGain").setValue(100.0)
        yield from banner_step("one warning", "m326_one_warning", 1, YELLOW, True)

        section("two warnings")
        scan2.getParam("retimeGain").setValue(100.0)
        scan2.getParam("retimeOffset").setValue(5.0)
        yield from banner_step("two warnings", "m326_two_warnings", 2, YELLOW, True)

        section("warnings and an error")
        require(merge.connectInput(1, read), "connect the missing-file Read to the Merge's A input")
        yield from banner_step("warnings and an error", "m326_warnings_and_error", 3, RED, False)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
