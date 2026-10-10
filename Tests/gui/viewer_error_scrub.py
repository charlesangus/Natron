"""The viewer shows a Shuffle's missing-layer error at every frame where it fails and at no
other, however fast the timeline moves, with every settings panel closed.

Graph: Read(flat-three-layers.exr) and Read(flat-rgba-only.exr) -> Switch (which keyed 0@1,
1@2) -> Shuffle (diffuse.g into rgba.r) -> Viewer. diffuse reaches the Shuffle at frame 1
only, so it fails at frame 2. Runs once with the Switch's channels on rgba and once on All.

After every seek it polls, with a timeout, both the ViewerGL widget's
"natronPersistentMessages" property, Effect.getPersistentMessage() and the colour on screen at
the centre of the viewer (the frame-1 image, or black where frame 2 failed), then checks the state
still holds a moment later, so a late render from a frame left behind cannot flip it.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
STARTUP_TIMEOUT_S = 60.0

try:
    from guitest import (FIXTURES, any_panel_open, check, close_all_panels, finish, plugin_problem, report,
                         require, run, shot, viewer_centre_colour, viewer_messages, viewer_up)

    def create(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        return node

    def create_reader(path):
        require(os.path.isfile(path), "fixture exists: " + path)
        node = app.createReader(path)
        require(node is not None, "create a Read for " + os.path.basename(path))
        return node

    reader_a = create_reader(os.path.join(FIXTURES, "flat-three-layers.exr"))
    reader_b = create_reader(os.path.join(FIXTURES, "flat-rgba-only.exr"))
    switch = create("net.sf.openfx.switchPlugin")
    shuffle = create("fr.natron.Shuffle")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = create("fr.inria.built-in.Viewer")

    require(switch.connectInput(0, reader_a), "connect Switch input 0 to the three-layer Read")
    require(switch.connectInput(1, reader_b), "connect Switch input 1 to the RGBA-only Read")
    which = switch.getParam("which")
    require(which is not None, "Switch has a which parameter")
    which.setValueAtTime(0, 1)
    which.setValueAtTime(1, 2)

    require(shuffle.connectInput(0, switch), "connect the Shuffle to the Switch")
    shuffle.getParam("in1").setLayer("diffuse")
    mapping = shuffle.getParam("mapping")
    mapping.connect("in1.#1", "out1.#0")
    # diffuse has no fourth channel, so the default alpha row would fail at frame 1 too.
    mapping.connect("0", "out1.#3")

    require(viewer.connectInput(0, shuffle), "connect the Viewer to the Shuffle")
    try:
        app.getProjectParam("frameRange").set(1, 10)
    except Exception:
        report("NOTE could not set the project frame range: " + traceback.format_exc().splitlines()[-1])

    reader_a.setPosition(0, 0)
    reader_b.setPosition(200, 0)
    switch.setPosition(100, 120)
    shuffle.setPosition(100, 240)
    viewer.setPosition(100, 360)

    channels = switch.getParam("channels")
    require(channels is not None, "Switch has a channels selector")
    shuffle_label = shuffle.getLabel()
    py_viewer = {}

    def seek(frame):
        if "v" not in py_viewer:
            py_viewer["v"] = app.getViewer(viewer.getScriptName())
        py_viewer["v"].seek(frame)

    def shuffle_error_in_viewer():
        return any(shuffle_label in m for m in viewer_messages())

    def image_matches(frame):
        c = viewer_centre_colour()
        if c is None:
            return False
        if frame == 2:
            return max(c) < 40
        return c[0] > 150 and c[1] > 150 and c[2] < 60

    def state_matches(frame):
        node_has = bool(shuffle.getPersistentMessage())
        viewer_has = shuffle_error_in_viewer()
        if frame == 2:
            return node_has and viewer_has and image_matches(2)
        return not node_has and not viewer_has and image_matches(1)

    def describe():
        return "node=%r viewer=%r image=%r" % (shuffle.getPersistentMessage(), viewer_messages(),
                                               viewer_centre_colour())

    def diagnose(tag, frame):
        """After a failed check, record what the viewer showed and whether a fresh render of the
        same frame, first through the cache and then bypassing it, puts the state right."""
        shot("fail-%s.png" % tag)
        report("NOTE %s: timeline at frame %d" % (tag, py_viewer["v"].getCurrentFrame()))
        for use_cache in (True, False):
            py_viewer["v"].renderCurrentFrame(use_cache)
            fixed = yield ("until", lambda: state_matches(frame), 5.0)
            report("NOTE %s: after renderCurrentFrame(useCache=%s) the state is %s (%s)" %
                   (tag, use_cache, "right" if fixed else "still wrong", describe()))
            if fixed:
                return

    def steps():
        ok = yield ("until", viewer_up, STARTUP_TIMEOUT_S)
        check(ok, "viewer widget is up")
        if not ok:
            return
        close_all_panels()
        yield ("sleep", 1000)
        close_all_panels()
        check(not any_panel_open(), "every settings panel is closed")

        for mode in ("rgba", "All"):
            if mode == "All":
                channels.setAll()
            else:
                channels.setLayer("rgba")
            close_all_panels()
            yield ("sleep", 500)

            frames = [1, 2] * 6
            for i, frame in enumerate(frames):
                seek(frame)
                ok = yield ("until", lambda f=frame: state_matches(f), POLL_TIMEOUT_S)
                if ok:
                    yield ("sleep", SETTLE_MS)
                    ok = state_matches(frame)
                check(ok, "%s: seek #%d to frame %d shows the error %s (%s)" %
                      (mode, i + 1, frame, "present" if frame == 2 else "absent", describe()))
                if i < 2:
                    shot("%s-frame%d.png" % (mode.lower(), frame))

            for n, landing in enumerate((2, 1, 2, 1)):
                for _ in range(8):
                    seek(1)
                    yield ("sleep", 15)
                    seek(2)
                    yield ("sleep", 15)
                seek(landing)
                ok = yield ("until", lambda f=landing: state_matches(f), POLL_TIMEOUT_S)
                if ok:
                    yield ("sleep", SETTLE_MS)
                    ok = state_matches(landing)
                check(ok, "%s: fast scrubbing landing #%d on frame %d shows the error %s (%s)" %
                      (mode, n + 1, landing, "present" if landing == 2 else "absent", describe()))
                if not ok:
                    yield from diagnose("%s-fast-scrub-landing%d-frame%d" % (mode.lower(), n + 1, landing), landing)
            shot("%s-after-fast-scrub-frame1.png" % mode.lower())

            # Seeks a millisecond apart, so many renders of both frames are in flight at once and
            # the landing request is usually served from the cache while they finish.
            for n, landing in enumerate((1, 2, 1, 1)):
                for _ in range(16):
                    seek(2)
                    yield ("sleep", 1)
                    seek(1)
                    yield ("sleep", 1)
                seek(2)
                seek(landing)
                ok = yield ("until", lambda f=landing: state_matches(f), POLL_TIMEOUT_S)
                if ok:
                    yield ("sleep", SETTLE_MS)
                    ok = state_matches(landing)
                check(ok, "%s: very fast scrubbing landing #%d on frame %d shows the error %s (%s)" %
                      (mode, n + 1, landing, "present" if landing == 2 else "absent", describe()))
                if not ok:
                    yield from diagnose("%s-very-fast-scrub-landing%d-frame%d" % (mode.lower(), n + 1, landing), landing)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
