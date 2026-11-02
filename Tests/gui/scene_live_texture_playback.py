"""During playback the 3D view shows an animated live texture changing with the playhead.

Graph: Constant1, its colour keyed red on odd frames and green on even ones over frames 1-24, into
USDEmissiveMaterial1's Emissive input (unlit, as the scene has no light), USDEmissiveMaterial1 into
USDCard1's Material input, USDCard1 into Viewer1. Playback runs forward for a few seconds while the
centre of the scene view is sampled every poll. The card must alternate between red and green many
times, never staying on one colour for long: a view that repaints only when its scene worker happens
to be idle changes colour every few seconds at best.

Screenshot: scene_live_texture_playback.png (full window) after pausing.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

FIRST = 1
LAST = 24
STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
PLAY_S = 6.0
SETTLE_MS = 700
MIN_CHANGES = 8
MAX_STALE_S = 1.0

try:
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    from guitest import check, finish, plugin_problem, report, require, run, shot, widgets_of

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    app.getProjectParam("frameRange").set(FIRST, LAST)
    constant = make("net.sf.openfx.ConstantPlugin", 0, 0)
    material = make("USDEmissiveMaterial", 0, 100)
    card = make("USDCard", 0, 200)
    color = constant.getParam("color")
    require(color is not None, "Constant1 has a color parameter")
    for frame in range(FIRST, LAST + 1):
        if frame % 2:
            color.set(1.0, 0.0, 0.0, 1.0, frame)
        else:
            color.set(0.0, 1.0, 0.0, 1.0, frame)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 300)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def centre_hue():
        """'r' or 'g' when a patch around the view centre is dominated by red or green, else None.
        The card sits at the origin, so a patch reads the card rather than the axes crossing it."""
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        centre = v.mapToGlobal(v.rect().center())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        votes = {"r": 0, "g": 0}
        total = 0
        for dy in range(-24, 25, 16):
            for dx in range(-24, 25, 16):
                c = image.pixelColor(centre.x() + dx, centre.y() + dy)
                total += 1
                if c.red() > 40 and c.red() > 2 * max(c.green(), c.blue()):
                    votes["r"] += 1
                elif c.green() > 40 and c.green() > 2 * max(c.red(), c.blue()):
                    votes["g"] += 1
        for hue, count in votes.items():
            if count * 4 >= total * 3:
                return hue
        return None

    def steps():
        require(material.connectInput(0, constant), "connect Constant1 to USDEmissiveMaterial1's Emissive input")
        require(card.connectInput(0, material), "connect USDEmissiveMaterial1 to USDCard1's Material input")
        require(viewer.connectInput(0, card), "connect the Viewer to USDCard1")
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        py_viewer.seek(FIRST)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the card")
        ok = yield ("until", lambda: centre_hue() == "r", POLL_TIMEOUT_S)
        require(ok, "the card is red at frame %d" % FIRST)
        py_viewer.seek(FIRST + 1)
        ok = yield ("until", lambda: centre_hue() == "g", POLL_TIMEOUT_S)
        require(ok, "seeking to frame %d turns the card green" % (FIRST + 1))

        samples = []
        started = time.time()

        def sample():
            now = time.time()
            samples.append((now - started, centre_hue()))
            return now - started >= PLAY_S

        py_viewer.startForward()
        yield ("until", sample, PLAY_S + 5.0)
        py_viewer.pause()

        changes = []
        last_hue = None
        last_change = 0.0
        longest = 0.0
        for t, hue in samples:
            if hue is None:
                continue
            if hue != last_hue:
                if last_hue is not None:
                    changes.append(t)
                longest = max(longest, t - last_change)
                last_hue = hue
                last_change = t
        if samples:
            longest = max(longest, samples[-1][0] - last_change)
        report("NOTE %d samples, %d colour changes, longest unchanged %.2fs" % (len(samples), len(changes), longest))
        check(len(changes) >= MIN_CHANGES,
              "the card changes colour with the playhead: %d changes in %.0fs of playback (want >= %d)"
              % (len(changes), PLAY_S, MIN_CHANGES))
        check(longest < MAX_STALE_S,
              "the card never keeps one colour for %.1fs or more during playback (longest %.2fs)" % (MAX_STALE_S, longest))
        v = view()
        error = v.property("natronSceneError") if v is not None else None
        check(not error, "no scene error after playback (%r)" % (error,))
        yield ("sleep", SETTLE_MS)
        shot("scene_live_texture_playback.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
