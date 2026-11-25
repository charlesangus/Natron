"""The test-only Overlay node (NATRON_GUI_TEST_NODES=1, which run-gui-test.sh sets) with Interact
Primitives on: the screenshot shows every interact primitive the draw list offers, and a redraw
with nothing changed must not upload the overlay buffer again, read from the ViewerGL's
natronOverlayUploads counter. Changing Center must upload once more. The same holds for a Roto
with no strokes, a Tracker with one track and a Transform: steady redraws upload nothing, and
moving the track's centre or the Transform's centre uploads again.

Run in the natron-dev container:
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m434-cache/out GUI_TEST_TIMEOUT=360 \\
        Tests/gui/run-gui-test.sh Tests/gui/m434_overlay_cache.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
OVERLAY_ID = "test.natron.built-in.Overlay"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    held = {}

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def visible_viewer():
        for w in gt.widgets_of("ViewerGL"):
            try:
                if w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def mapping():
        value = held["gl"].property("natronCanonicalToWidget")
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def uploads():
        value = held["gl"].property("natronOverlayUploads")
        return None if value is None else int(value)

    def redraw():
        held["gl"].repaint()
        QApplication.processEvents()

    def park_cursor():
        gl = held["gl"]
        QApplication.sendEvent(gl, QMouseEvent(QEvent.MouseMove, QPointF(4.0, gl.height() - 4.0), Qt.NoButton,
                                               Qt.NoButton, Qt.NoModifier))
        QApplication.processEvents()

    def show_alone(plugin_id):
        """Creates plugin_id with its panel open, fed by a grey Constant, as the viewer's only input."""
        viewer = held["viewer_node"]
        gt.close_all_panels()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        for old in held.pop("scene_nodes", []):
            old.destroy(False)
        bg = app.createNode("Constant")
        gt.require(bg is not None, "create a Constant", gt.plugin_problem("Constant"))
        bg.getParam("color").set(0.5, 0.5, 0.5, 1.0)
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create %s" % plugin_id, gt.plugin_problem(plugin_id))
        held["scene_nodes"] = [node, bg]
        gt.require(node.connectInput(0, bg), "the Constant feeds the %s" % plugin_id)
        gt.require(viewer.connectInput(0, node), "the %s feeds the viewer" % plugin_id)
        return node

    def steady(label):
        """Checks that redraws with nothing changed upload nothing; returns the count."""
        park_cursor()
        redraw()
        first = uploads()
        redraw()
        redraw()
        second = uploads()
        check(second == first, "%s: redrawing with nothing changed uploads no overlay buffer (%r then %r)"
              % (label, first, second))
        return second

    def uploads_again(label, before):
        redraw()
        after = uploads()
        check(after is not None and after > before, "%s uploads the overlay buffer again (%r then %r)"
              % (label, before, after))

    def steps():
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        gt.require(OVERLAY_ID in gt._plugin_ids, "the test Overlay node is registered (NATRON_GUI_TEST_NODES=%r)"
                   % os.environ.get("NATRON_GUI_TEST_NODES"))
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        gt.require(viewer is not None, "the project has a viewer", gt.plugin_problem("Viewer"))
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(OVERLAY_ID, -1, None, props)
        gt.require(node is not None, "create the Overlay test node", gt.plugin_problem(OVERLAY_ID))
        held["node"], held["viewer_node"] = node, viewer
        node.setPosition(0, 0)
        viewer.setPosition(0, 200)
        gt.require(viewer.connectInput(0, node), "the Overlay node feeds the viewer")
        ok = yield ("until", lambda: visible_viewer() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "a viewer is up")
        held["gl"] = visible_viewer()
        ok = yield ("until", lambda: mapping() is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the viewer publishes natronCanonicalToWidget")
        gt.require(uploads() is not None, "the viewer publishes natronOverlayUploads")

        gl = held["gl"]
        ox, oy, sx, sy = mapping()
        # Left of the middle, so the interact primitives drawn to the right of Center stay in view.
        cx, cy = (gl.width() * 0.3 - ox) / sx, (gl.height() * 0.5 - oy) / sy
        node.getParam("center").set(float(round(cx)), float(round(cy)))
        primitives = node.getParam("interactPrimitives")
        gt.require(primitives is not None, "the Overlay node has an Interact Primitives parameter")
        primitives.setValue(True)
        QApplication.sendEvent(gl, QMouseEvent(QEvent.MouseMove, QPointF(4.0, gl.height() - 4.0), Qt.NoButton,
                                               Qt.NoButton, Qt.NoModifier))
        yield ("sleep", SETTLE_MS)

        gt.section("primitives")
        gt.shot("m434-interact-primitives.png")

        gt.section("cache")
        redraw()
        first = uploads()
        redraw()
        redraw()
        second = uploads()
        check(second == first, "redrawing with nothing changed uploads no overlay buffer (%r then %r)" % (first, second))
        node.getParam("center").set(float(round(cx)) + 10.0, float(round(cy)))
        yield ("sleep", SETTLE_MS)
        redraw()
        third = uploads()
        check(third is not None and third > second,
              "moving Center uploads the overlay buffer again (%r then %r)" % (second, third))
        report("NOTE overlay uploads %r, %r, %r" % (first, second, third))

        anchor = (float(round(cx)), float(round(cy)))

        gt.section("roto")
        show_alone("Roto")
        yield ("sleep", RENDER_MS)
        steady("Roto with no strokes")

        gt.section("tracker")
        tracker_node = show_alone("Tracker")
        track = tracker_node.getTrackerContext().createTrack()
        gt.require(track is not None, "created a track")
        centre = track.getParam("centerPoint")
        held["track"], held["centre"] = track, centre
        centre.set(*anchor)
        yield ("sleep", RENDER_MS)
        before = steady("Tracker")
        centre.set(anchor[0] + 10.0, anchor[1])
        yield ("sleep", SETTLE_MS)
        uploads_again("Tracker: moving the track's centre", before)

        gt.section("transform")
        transform = show_alone("Transform")
        transform.getParam("center").set(*anchor)
        yield ("sleep", RENDER_MS)
        before = steady("Transform")
        transform.getParam("center").set(anchor[0] + 10.0, anchor[1])
        yield ("sleep", SETTLE_MS)
        uploads_again("Transform: moving the centre", before)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
