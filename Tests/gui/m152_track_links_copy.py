"""Curve track links and cross-node copy and paste.

A CheckerBoard feeds a SplineWarp and a Roto. A Tracker track is keyed directly through its centre
parameter (tracking a fixture is slow and flaky under Xvfb, and the link only reads the centre
curve). A SplineWarp destination control point and a Roto control point are linked to the track,
the timeline is scrubbed and both are checked to follow with their link-frame offset. Unlink and
Bake then keeps the keyed positions after the track is edited. Finally a Roto shape is copied into a
second SplineWarp through the curve list's Copy and Paste menu actions, and a linked source and
destination pair is copied into a third SplineWarp with its link intact. Saves screenshots of every
stage.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m152-shots -e GUI_TEST_TIMEOUT=300 \\
        -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m152_track_links_copy.py build/fast/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 1500
MENU_MS = 600
TOL = 1e-6
LINK_FRAME = 1
# Track centre keys: frame -> (x, y), in canonical coordinates.
TRACK_KEYS = {1: (400.0, 300.0), 5: (700.0, 360.0), 10: (900.0, 200.0)}
SCRUB_FRAMES = (3, 5, 8, 10)
RECT = (700.0, 350.0, 1200.0, 750.0)
ROTO_SQUARE = (200.0, 700.0, 400.0, 900.0)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Python wrappers die with their parent wrapper, so every one used later is held here.
    held = []

    def hold(obj):
        held.append(obj)
        return obj

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def curve_tree(panel):
        for tree in panel.findChildren(QTreeWidget):
            parent = tree.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return tree
                parent = parent.parentWidget()
        return None

    def fresh_tree(node):
        """The node's curve list, found again on every call: a tree wrapper held across a yield can
        be invalidated, and the panel wrapper is held so the tree's wrapper outlives this call."""
        panel = panel_of(node)
        if panel is None:
            return None
        hold(panel)
        return curve_tree(panel)

    def open_panel(view, node):
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")

    def visible_viewer():
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return viewers[0] if viewers else None

    def send_mouse(widget, kind, pos, button, buttons):
        pos = QPointF(pos)
        QApplication.sendEvent(widget, QMouseEvent(kind, pos, QPointF(widget.mapToGlobal(pos.toPoint())),
                                                   button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def tree_item(tree, curve):
        found = tree.findItems(curve.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
        return found[0] if found else None

    def select_in_tree(tree, curves):
        tree.clearSelection()
        items = []
        for curve in curves:
            item = tree_item(tree, curve)
            if item is not None:
                item.setSelected(True)
                items.append(item)
        QApplication.processEvents()
        return items

    def run_menu_action(tree, item, text, seen):
        """Right-clicks item in the curve list and triggers the menu action labelled text. The menu
        is modal, so the action is picked from a timer while it is up."""
        def pick():
            popup = QApplication.activePopupWidget()
            if popup is None:
                QTimer.singleShot(200, pick)
                return
            for act in popup.actions():
                if act.text() == text:
                    seen[text] = act.isEnabled()
                    if act.isEnabled():
                        act.trigger()
                    break
            popup.close()

        QTimer.singleShot(MENU_MS, pick)
        # A release on any other column of a selected row is taken as a click on that cell (activation,
        # lock, colour) and opens no menu, so the click lands on the name column.
        header = tree.header()
        x = header.sectionViewportPosition(0) + header.sectionSize(0) // 2
        pos = QPoint(x, tree.visualItemRect(item).center().y())
        send_mouse(tree.viewport(), QEvent.MouseButtonRelease, pos, Qt.RightButton, Qt.NoButton)

    def xy(value):
        if hasattr(value, "x") and hasattr(value, "y"):
            return float(value.x), float(value.y)
        return float(value[0]), float(value[1])

    def near(a, b):
        return abs(a[0] - b[0]) < TOL and abs(a[1] - b[1]) < TOL

    def point_at(curve, index, frame):
        return xy(curve.getControlPointPosition(index, float(frame))[:2])

    def make_square(roto, rect):
        x0, y0, x1, y1 = rect
        curve = hold(roto.createBezier(x0, y0, float(LINK_FRAME)))
        for x, y in ((x1, y0), (x1, y1), (x0, y1)):
            curve.addControlPoint(x, y)
        curve.setCurveFinished(True)
        return curve

    def show_in_viewer(viewer, node):
        # connectInput refuses an input that is already in use.
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connected %s to the viewer" % node.getScriptName())

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        view = node_graph_view()

        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 700)
        checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
        require(checker is not None, "created a CheckerBoard",
                gt.plugin_problem("net.sf.openfx.CheckerBoardPlugin"))
        checker.setPosition(0, 0)

        def new_warp(x):
            node = app.createNode("SplineWarp")
            require(node is not None, "created a SplineWarp", gt.plugin_problem("SplineWarp"))
            node.setPosition(x, 300)
            require(node.connectInput(0, checker), "connected the CheckerBoard to the SplineWarp")
            return node

        warp = new_warp(0)
        roto_node = app.createNode("fr.inria.built-in.Roto")
        require(roto_node is not None, "created a Roto", gt.plugin_problem("fr.inria.built-in.Roto"))
        roto_node.setPosition(200, 300)
        require(roto_node.connectInput(0, checker), "connected the CheckerBoard to the Roto")
        tracker_node = app.createNode("fr.inria.built-in.Tracker")
        require(tracker_node is not None, "created a Tracker", gt.plugin_problem("fr.inria.built-in.Tracker"))
        tracker_node.setPosition(400, 0)

        roto = hold(warp.getRotoContext())
        sw = hold(warp.getSplineWarp())
        roto_ctx = hold(roto_node.getRotoContext())
        tracker = hold(tracker_node.getTrackerContext())
        require(roto is not None and sw is not None, "the SplineWarp has curves and links")
        require(roto_ctx is not None, "the Roto has curves")
        require(tracker is not None, "the Tracker has a track context")

        gt.section("track-keys")
        track = hold(tracker.createTrack())
        require(track is not None, "created a track")
        centre = hold(track.getParam("centerPoint"))
        require(centre is not None, "the track has a centre parameter")
        for frame, (x, y) in sorted(TRACK_KEYS.items()):
            centre.set(x, y, float(frame))
        report("NOTE the track centre is keyed directly at frames %r; no tracking is run" %
               (sorted(TRACK_KEYS),))
        for frame, pos in sorted(TRACK_KEYS.items()):
            check(near(xy(centre.get(float(frame))), pos), "the track centre is keyed at frame %d" % frame)

        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        require(py_viewer is not None, "the viewer has a Python handle")

        open_panel(view, warp)
        ok = yield ("until", lambda: panel_of(warp) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the SplineWarp's settings panel")
        ok = yield ("until", lambda: visible_viewer() is not None, POLL_TIMEOUT_S)
        require(ok, "a viewer is on screen")
        require(fresh_tree(warp) is not None, "the SplineWarp's panel holds the curve list")
        show_in_viewer(viewer, warp)
        py_viewer.seek(LINK_FRAME)
        yield ("sleep", RENDER_MS)

        gt.section("link-splinewarp")
        closed = make_square(roto, RECT)
        yield ("sleep", SETTLE_MS)
        tree = fresh_tree(warp)
        items = select_in_tree(tree, [closed])
        require(len(items) == 1, "selected the closed curve in the curve list")
        seen = {}
        run_menu_action(tree, items[0], "Duplicate and Link", seen)
        ok = yield ("until", lambda: sw.getLink(closed) is not None, POLL_TIMEOUT_S)
        check(seen.get("Duplicate and Link", False), "the context menu offers Duplicate and Link")
        require(ok, "Duplicate and Link linked the closed curve")
        link = hold(sw.getLink(closed))
        dest = hold(link.getDestination())
        dest_index = 2
        dest_start = point_at(dest, dest_index, LINK_FRAME)
        gt.shot("m152-warp-before-link.png")
        dest.linkPointToTrack(dest_index, track)
        yield ("sleep", RENDER_MS)
        check(near(point_at(dest, dest_index, LINK_FRAME), dest_start),
              "linking keeps the destination point where it is at the link frame")
        gt.shot("m152-warp-linked.png")

        gt.section("link-roto")
        rsquare = make_square(roto_ctx, ROTO_SQUARE)
        roto_index = 0
        roto_start = point_at(rsquare, roto_index, LINK_FRAME)
        rsquare.linkPointToTrack(roto_index, track)
        check(near(point_at(rsquare, roto_index, LINK_FRAME), roto_start),
              "linking keeps the Roto point where it is at the link frame")

        gt.section("scrub")
        base = TRACK_KEYS[LINK_FRAME]
        for frame in SCRUB_FRAMES:
            py_viewer.seek(frame)
            yield ("sleep", RENDER_MS)
            t = xy(centre.get(float(frame)))
            delta = (t[0] - base[0], t[1] - base[1])
            want_warp = (dest_start[0] + delta[0], dest_start[1] + delta[1])
            want_roto = (roto_start[0] + delta[0], roto_start[1] + delta[1])
            check(near(point_at(dest, dest_index, frame), want_warp),
                  "the SplineWarp destination point follows the track at frame %d" % frame)
            check(near(point_at(rsquare, roto_index, frame), want_roto),
                  "the Roto point follows the track at frame %d" % frame)
            gt.shot("m152-warp-scrub-%02d.png" % frame)
        source_pos = point_at(closed, dest_index, SCRUB_FRAMES[-1])
        check(near(source_pos, (RECT[2], RECT[3])), "the SplineWarp source point stays put")

        open_panel(view, roto_node)
        ok = yield ("until", lambda: panel_of(roto_node) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the Roto's settings panel")
        show_in_viewer(viewer, roto_node)
        yield ("sleep", RENDER_MS)
        gt.shot("m152-roto-linked-scrubbed.png")
        show_in_viewer(viewer, warp)

        gt.section("unlink-and-bake")
        py_viewer.seek(LINK_FRAME)
        yield ("sleep", SETTLE_MS)
        frames = sorted(TRACK_KEYS)
        followed = dict((f, point_at(dest, dest_index, f)) for f in frames)
        followed_roto = dict((f, point_at(rsquare, roto_index, f)) for f in frames)
        dest.unlinkPoint(dest_index, True)
        rsquare.unlinkPoint(roto_index, True)
        yield ("sleep", RENDER_MS)
        for f in frames:
            check(near(point_at(dest, dest_index, f), followed[f]),
                  "the baked SplineWarp point keeps its value at frame %d" % f)
            check(near(point_at(rsquare, roto_index, f), followed_roto[f]),
                  "the baked Roto point keeps its value at frame %d" % f)
        keys = list(dest.getKeyframes())
        check(all(float(f) in [float(k) for k in keys] for f in frames),
              "the baked SplineWarp curve is keyed over the track's range (%r)" % (keys,))
        gt.shot("m152-warp-baked.png")
        centre.set(100.0, 100.0, float(frames[-1]))
        yield ("sleep", RENDER_MS)
        for f in frames:
            check(near(point_at(dest, dest_index, f), followed[f]),
                  "editing the track no longer moves the SplineWarp point at frame %d" % f)
            check(near(point_at(rsquare, roto_index, f), followed_roto[f]),
                  "editing the track no longer moves the Roto point at frame %d" % f)
        gt.shot("m152-warp-unlinked-track-edited.png")

        gt.section("copy-roto-into-splinewarp")
        warp2 = new_warp(300)
        roto2 = hold(warp2.getRotoContext())
        sw2 = hold(warp2.getSplineWarp())
        require(roto2 is not None and sw2 is not None, "the second SplineWarp has curves and links")
        open_panel(view, roto_node)
        ok = yield ("until", lambda: panel_of(roto_node) is not None, POLL_TIMEOUT_S)
        require(ok, "the Roto's panel is open")
        roto_tree = fresh_tree(roto_node)
        require(roto_tree is not None, "the Roto's panel holds the curve list")
        items = select_in_tree(roto_tree, [rsquare])
        require(len(items) == 1, "selected the Roto shape in its curve list")
        seen = {}
        run_menu_action(roto_tree, items[0], "Copy", seen)
        yield ("sleep", MENU_MS * 2)
        check(seen.get("Copy", False), "the curve list offers Copy")

        open_panel(view, warp2)
        ok = yield ("until", lambda: panel_of(warp2) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the second SplineWarp's settings panel")
        tree2 = fresh_tree(warp2)
        require(tree2 is not None, "the second SplineWarp's panel holds the curve list")
        show_in_viewer(viewer, warp2)
        target = tree2.topLevelItem(0)
        require(target is not None, "the second SplineWarp's curve list has a layer to paste into")
        seen = {}
        run_menu_action(tree2, target, "Paste", seen)
        name = rsquare.getScriptName()
        ok = yield ("until", lambda: roto2.getItemByName(name) is not None, POLL_TIMEOUT_S)
        check(seen.get("Paste", False), "the curve list offers Paste")
        require(ok, "the Roto shape arrived in the second SplineWarp")
        pasted = hold(roto2.getItemByName(name))
        check(sw2.getLink(pasted) is None, "the pasted Roto shape is unlinked in the SplineWarp")
        yield ("sleep", RENDER_MS)
        gt.shot("m152-roto-pasted-into-warp.png")

        gt.section("copy-linked-pair")
        warp3 = new_warp(600)
        roto3 = hold(warp3.getRotoContext())
        sw3 = hold(warp3.getSplineWarp())
        require(roto3 is not None and sw3 is not None, "the third SplineWarp has curves and links")
        open_panel(view, warp)
        ok = yield ("until", lambda: panel_of(warp) is not None, POLL_TIMEOUT_S)
        require(ok, "the first SplineWarp's panel is open")
        tree = fresh_tree(warp)
        require(tree is not None, "the first SplineWarp's panel holds the curve list")
        items = select_in_tree(tree, [closed, dest])
        require(len(items) == 2, "selected the linked source and destination")
        seen = {}
        run_menu_action(tree, items[0], "Copy", seen)
        yield ("sleep", MENU_MS * 2)
        check(seen.get("Copy", False), "the curve list offers Copy for the linked pair")

        open_panel(view, warp3)
        ok = yield ("until", lambda: panel_of(warp3) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the third SplineWarp's settings panel")
        tree3 = fresh_tree(warp3)
        require(tree3 is not None, "the third SplineWarp's panel holds the curve list")
        show_in_viewer(viewer, warp3)
        target = tree3.topLevelItem(0)
        require(target is not None, "the third SplineWarp's curve list has a layer to paste into")
        seen = {}
        run_menu_action(tree3, target, "Paste", seen)
        src_name = closed.getScriptName()
        dst_name = dest.getScriptName()
        ok = yield ("until", lambda: roto3.getItemByName(src_name) is not None
                    and roto3.getItemByName(dst_name) is not None, POLL_TIMEOUT_S)
        require(ok, "the linked pair arrived in the third SplineWarp")
        src3 = hold(roto3.getItemByName(src_name))
        dst3 = hold(roto3.getItemByName(dst_name))
        pair = sw3.getLink(src3)
        check(pair is not None, "the pasted source is still linked")
        if pair is not None:
            hold(pair)
            check(pair.getDestination().getScriptName() == dst3.getScriptName(),
                  "the pasted link joins the pasted source and destination")
        yield ("sleep", RENDER_MS)
        gt.shot("m152-linked-pair-pasted.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
