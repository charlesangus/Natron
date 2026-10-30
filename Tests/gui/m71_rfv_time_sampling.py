"""Reverse-Flow Variables sampled over time: a variable animated on a RFVSet is read at
the sample times of motion blur and of a TimeBlur node above the Set, not only at the output frame.

Graph: CheckerBoard -> Transform -> RFVSet -> Viewer. The Set declares a Double v keyed
0 at frame 1 and V_END at frame 10; the Transform's translate x is the expression rfv.v. All
comparisons are against the sharp render of the same frame, never against exact pixel values.

Cases (all at frame 5):
  sharp       Transform motionBlur 0: the reference render; it must also differ from frame 1, which
              shows that the expression moves the image at all;
  motion-blur Transform motionBlur 1, shutter 1: the render must differ from the sharp one;
  above       TimeBlur between Transform and Set, motionBlur 0: must differ from the sharp render;
  below       TimeBlur between Set and Viewer, motionBlur 0: must differ from the sharp render.

The viewer is compared through screen grabs, since a widget grab of the GL viewer can come out blank
under llvmpipe. Results and screenshots go where Tests/gui/run-gui-test.sh puts them.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m71-time/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m71_rfv_time_sampling.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 600
RENDER_MS = 2000
POLL_TIMEOUT_S = 25.0

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_TRANSFORM = "net.sf.openfx.TransformPlugin"
PLUGIN_TIMEBLUR = "net.sf.openfx.TimeBlur"
PLUGIN_SET = "RFVSet"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"

VARIABLE = "v"
V_END = 400.0
TEST_FRAME = 5
PIXEL_DIFF_MIN = 40
CHANGED_PIXELS_MIN = 100
SAMPLE_STRIDE = 4

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getPluginID(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(top_left.x(), top_left.y(), widget.width(), widget.height())

    def changed_pixels(a, b):
        """How many sampled pixels differ clearly between two same-sized viewer grabs."""
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), SAMPLE_STRIDE):
            for x in range(0, a.width(), SAMPLE_STRIDE):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def render_at(py_viewer, frame, shot_name, previous=None):
        """Seeks, then waits until the viewer has landed the render. With `previous` (the image the
        viewer showed before the change), first polls until a grab differs from it, so a slow render
        cannot be mistaken for the still-unchanged old frame, then waits until two consecutive grabs
        agree. If the viewer never differs from `previous`, the last crop is returned and the
        caller's check fails honestly. RENDER_MS is the minimum wait and the fallback if it never
        settles."""
        py_viewer.seek(frame)
        yield ("sleep", SETTLE_MS)
        timeout = RENDER_MS / 1000.0 + POLL_TIMEOUT_S
        last = [viewer_crop()]

        if previous is not None:
            def differs():
                last[0] = viewer_crop()
                return changed_pixels(previous, last[0]) != 0

            if not (yield ("until", differs, timeout)):
                report("NOTE viewer never differed from the previous image at frame %d" % frame)
                gt.shot(shot_name)
                return last[0]

        def settled():
            now = viewer_crop()
            same = changed_pixels(last[0], now) == 0
            last[0] = now
            return same

        ok = yield ("until", settled, timeout)
        if not ok:
            report("NOTE viewer did not settle at frame %d; falling back to a fixed wait" % frame)
            yield ("sleep", RENDER_MS)
            last[0] = viewer_crop()
        gt.shot(shot_name)
        return last[0]

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()

        # A short timeline, so that seeking to the test frame is not clamped.
        frame_range = app.getProjectParam("frameRange")
        if frame_range is not None:
            frame_range.setValue(1, 0)
            frame_range.setValue(20, 1)
        else:
            report("NOTE no frameRange project parameter; relying on the default timeline")

        gt.section("graph")
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = create(PLUGIN_VIEWER, 0, 600)
        checker = create(PLUGIN_CHECKER, 0, 0)
        transform = create(PLUGIN_TRANSFORM, 0, 150)
        setter = create(PLUGIN_SET, 0, 300)
        blur = create(PLUGIN_TIMEBLUR, 200, 225)

        variable = setter.createDoubleParam(VARIABLE, "V")
        require(variable is not None, "created a Double on the Set node")
        variable.setValue(0.0)
        setter.addRFVKnob(variable)
        variable = setter.getParam(VARIABLE)
        require(variable is not None and variable.getParent() is not None
                and variable.getParent().getScriptName() == "rfvVariables",
                "%s is a variable of the Set node" % VARIABLE)
        variable.setValueAtTime(0.0, 1)
        variable.setValueAtTime(V_END, 10)
        check(variable.getNumKeys() == 2, "the variable has two keyframes (got %d)" % variable.getNumKeys())

        translate = param(transform, "translate")
        require(translate.setExpression("rfv." + VARIABLE, False, 0), "set the expression rfv.v on translate x")
        param(transform, "motionBlur").setValue(0.0)
        param(transform, "shutter").setValue(1.0)
        shutter = blur.getParam("shutter")
        if shutter is not None:
            shutter.setValue(1.0)
        else:
            report("NOTE TimeBlur has no shutter parameter; using its default")

        def wire(chain):
            """Connects checker -> chain... -> viewer."""
            previous = checker
            for node in chain:
                connect(node, 0, previous)
                previous = node
            connect(viewer, 0, previous)

        wire([transform, setter])
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", gt.viewer_up, POLL_TIMEOUT_S)
        require(ok, "the viewer is up")
        yield ("sleep", SETTLE_MS)

        gt.section("sharp")
        frame_one = yield from render_at(py_viewer, 1, "m71-time-frame1.png")
        sharp = yield from render_at(py_viewer, TEST_FRAME, "m71-time-sharp.png", frame_one)
        require(sharp is not None and frame_one is not None, "grabbed the viewer")
        moved = changed_pixels(frame_one, sharp)
        check(moved >= CHANGED_PIXELS_MIN,
              "rfv.v moves the image between frames 1 and %d (%d sampled pixels differ)" % (TEST_FRAME, moved))

        gt.section("motion-blur")
        param(transform, "motionBlur").setValue(1.0)
        blurred = yield from render_at(py_viewer, TEST_FRAME, "m71-time-motion-blur.png", sharp)
        count = changed_pixels(sharp, blurred)
        check(count >= CHANGED_PIXELS_MIN,
              "motion blur on a Transform driven by rfv.v changes the frame-%d render (%d sampled pixels differ; identical means the variable was read at one time)"
              % (TEST_FRAME, count))
        param(transform, "motionBlur").setValue(0.0)

        gt.section("above")
        wire([transform, blur, setter])
        above = yield from render_at(py_viewer, TEST_FRAME, "m71-time-blur-above.png", blurred)
        count = changed_pixels(sharp, above)
        check(count >= CHANGED_PIXELS_MIN,
              "a TimeBlur above the Set changes the frame-%d render (%d sampled pixels differ)" % (TEST_FRAME, count))

        gt.section("below")
        wire([transform, setter, blur])
        below = yield from render_at(py_viewer, TEST_FRAME, "m71-time-blur-below.png", above)
        count = changed_pixels(sharp, below)
        check(count >= CHANGED_PIXELS_MIN,
              "a TimeBlur below the Set changes the frame-%d render (%d sampled pixels differ)" % (TEST_FRAME, count))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
