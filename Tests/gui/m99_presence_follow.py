"""A two-process GUI test of live session presence and following a viewer. A hosts, B joins (see
Tests/gui/live_peers.py):

- A selects Tint and opens its panel; B's node graph outlines Tint with A's name, and B's own
  panel of Tint shows A's dot.
- B shows Viewer2 and follows A from the Session Users panel; B switches to A's Viewer1. A moves
  to frame 20, shows input 2 in R, zooms and pans, and B's viewer matches all of it.
- B moves its own timeline, and following stops: the label goes, the Follow box clears and A's
  next frame change stays on A.
- B follows A again and presses a key in its viewer, and following stops the same way.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m99-follow/out GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/m99_presence_follow.py <AppImage or build/release/App/Natron>
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
HOST_NAME = "Ann"
CLIENT_NAME = "Bea"

try:
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QLabel, QTreeWidget

    import NatronEngine

    import guitest as gt
    from guitest import check, report, require
    import live_peers as lp

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    ROLE, SYNC = lp.bind(app, "M99_FOLLOW_ROLE")

    def remote_marks(user):
        graph = lp.graph_view()
        if graph is None or graph.scene() is None:
            return 0
        return len([i for i in graph.scene().items() if i.toolTip() == user])

    def panel_dot(name, user):
        p = lp.panel_of(name)
        if p is None:
            return False
        return any(user in d.toolTip() and d.isVisible() for d in p.findChildren(QLabel, "liveSessionPresenceDot"))

    def follow_label():
        gl = lp.viewer_gl()
        label = gl.findChild(QLabel, "liveSessionFollowLabel") if gl is not None else None
        return label.text() if label is not None and label.isVisible() else ""

    def users_item(user):
        win = lp.main_gui()
        tree = win.findChild(QTreeWidget, "liveSessionUsers") if win is not None else None
        if tree is None:
            return None, None
        for i in range(tree.topLevelItemCount()):
            item = tree.topLevelItem(i)
            if item.text(0) == user:
                return tree, item
        return tree, None

    def viewer_property(name):
        gl = lp.viewer_gl()
        return gl.property(name) if gl is not None else None

    def zoom():
        value = viewer_property("natronLiveSessionZoom")
        return float(value) if value is not None else None

    def centre():
        x = viewer_property("natronLiveSessionCenterX")
        y = viewer_property("natronLiveSessionCenterY")
        return (float(x), float(y)) if x is not None and y is not None else None

    def shown_viewer():
        return viewer_property("natronLiveSessionViewer") or ""

    def viewer_param(viewer, name):
        node = app.getNode(viewer)
        param = node.getParam(name) if node is not None else None
        return param.get() if param is not None else None

    def show_viewer(label):
        """Makes label's tab current and clicks into it, which makes it the active viewer."""
        for w in gt.widgets_of("TabWidget"):
            try:
                name = w.objectName()
            except RuntimeError:
                continue
            pane = app.getTabWidget(name) if name else None
            if pane is None:
                continue
            for i in range(pane.count()):
                if pane.getTabLabel(i) == label:
                    pane.setCurrentIndex(i)
                    QApplication.processEvents()
                    gl = lp.viewer_gl()
                    if gl is None:
                        return False
                    lp.activate()
                    p = QPointF(gl.rect().center())
                    for kind, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
                        QApplication.sendEvent(gl, QMouseEvent(kind, p, Qt.LeftButton, buttons, Qt.NoModifier))
                        QApplication.processEvents()
                    return True
        return False

    def pan(widget, dx, dy, steps=6):
        start = QPointF(widget.rect().center())
        QApplication.sendEvent(widget, QMouseEvent(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton, Qt.NoModifier))
        QApplication.sendEvent(widget, QMouseEvent(QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton, Qt.NoModifier))
        for i in range(1, steps + 1):
            p = QPointF(start.x() + dx * i / steps, start.y() + dy * i / steps)
            QApplication.sendEvent(widget, QMouseEvent(QEvent.MouseMove, p, Qt.NoButton, Qt.MiddleButton, Qt.NoModifier))
            QApplication.processEvents()
        end = QPointF(start.x() + dx, start.y() + dy)
        QApplication.sendEvent(widget, QMouseEvent(QEvent.MouseButtonRelease, end, Qt.MiddleButton, Qt.NoButton, Qt.NoModifier))
        QApplication.processEvents()

    def frame():
        viewer = app.getViewer("Viewer1")
        return viewer.getCurrentFrame() if viewer is not None else None

    def stopped(what):
        ok = yield ("until", lambda: follow_label() == "", 5.0)
        if not ok:
            return "still following after the %s (%r)" % (what, follow_label())
        tree, item = users_item(HOST_NAME)
        if item is not None and item.checkState(2) == Qt.Checked:
            return "the Follow box is still checked after the %s" % what
        if "Stopped following" not in lp.status_message():
            return "the status bar did not note the %s (%r)" % (what, lp.status_message())
        return "ok"

    def client_command(words):
        verb = words[0]
        if verb == "show":
            return "ok" if show_viewer(words[1]) else "no viewer tab " + words[1]
        if verb == "shown":
            return shown_viewer()
        if verb == "marks":
            user = words[2]
            ok = yield ("until", lambda: remote_marks(user) > 0, 10.0)
            if not ok:
                return "no outline or tag for %s in the graph" % user
            opened = yield from lp.open_panel(words[1])
            if not opened:
                return "no panel for " + words[1]
            ok = yield ("until", lambda: panel_dot(words[1], user), 10.0)
            gt.close_all_panels()
            return "ok" if ok else "no dot for %s in the panel of %s" % (user, words[1])
        if verb == "follow":
            tree, item = users_item(words[1])
            if item is None:
                return "Session Users lists no %s" % words[1]
            item.setCheckState(2, Qt.Checked)
            QApplication.processEvents()
            ok = yield ("until", lambda: words[1] in follow_label(), 5.0)
            return "ok" if ok else "the viewer does not say it follows %s (%r)" % (words[1], follow_label())
        if verb == "matches":
            want_viewer = words[1]
            want_frame = int(words[2])
            want_zoom = float(words[3])
            want_centre = (float(words[4]), float(words[5]))
            want_input = int(words[6])
            want_channels = int(words[7])

            def near(a, b):
                return abs(a - b) <= 1e-3 * max(1.0, abs(b))

            def state():
                return (shown_viewer(), frame(), zoom(), centre(),
                        viewer_param(want_viewer, "aInput"), viewer_param(want_viewer, "displayChannels"))

            def matched():
                viewer, f, z, c, a, ch = state()
                return (viewer == want_viewer and f == want_frame and z is not None and near(z, want_zoom)
                        and c is not None and near(c[0], want_centre[0]) and near(c[1], want_centre[1])
                        and a == want_input and ch == want_channels)

            ok = yield ("until", matched, 5.0)
            return "ok" if ok else "viewer %r frame %r zoom %r centre %r input A %r channels %r" % state()
        if verb == "scrub":
            app.getViewer("Viewer1").seek(int(words[1]))
            QApplication.processEvents()
            answer = yield from stopped("scrub")
            return answer
        if verb == "key":
            gl = lp.viewer_gl()
            if gl is None:
                return "no viewer"
            lp.activate()
            gl.setFocus()
            lp.send_key(gl, Qt.Key_F)
            answer = yield from stopped("key press")
            return answer
        if verb == "frame":
            return str(frame())
        return "unknown command " + verb

    def host_steps():
        ok = yield ("until", lambda: lp.main_gui() is not None, lp.STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()
        src = app.createNode("Constant")
        tint = app.createNode("Grade")
        alt = app.createNode("Constant")
        require(src is not None and tint is not None and alt is not None, "create the starting comp")
        src.setScriptName("Src")
        tint.setScriptName("Tint")
        alt.setScriptName("Alt")
        src.setPosition(0, 0)
        tint.setPosition(0, 150)
        alt.setPosition(200, 0)
        tint.connectInput(0, src)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.connectInput(0, tint)
        viewer.connectInput(1, alt)
        viewer2 = app.createNode("Viewer")
        require(viewer2 is not None, "create a second Viewer node")
        viewer2.setScriptName("Viewer2")
        viewer2.connectInput(0, src)
        project_file = os.path.join(gt.OUT, "m99_follow_host.ntpz")
        require(app.saveProjectAs(project_file), "save the host project to " + project_file)

        gt.section("join")
        yield from lp.host_and_join("M99_FOLLOW_ROLE", HOST_NAME, CLIENT_NAME)

        gt.section("presence")
        opened = yield from lp.open_panel("Tint")
        require(opened, "A selects Tint and opens its panel")
        answer = yield from lp.client_do("marks Tint " + HOST_NAME)
        check(answer == "ok", "B sees A's outline on Tint and A's dot in its panel: %s" % answer)

        gt.section("follow")
        require(show_viewer("Viewer1"), "A shows Viewer1")
        answer = yield from lp.client_do("show Viewer2")
        require(answer == "ok", "B shows Viewer2", answer)
        answer = yield from lp.until_client("shown", "Viewer2")
        require(answer == "Viewer2", "B's active viewer is Viewer2 (%r)" % answer)
        answer = yield from lp.client_do("follow " + HOST_NAME)
        require(answer == "ok", "B follows A", answer)
        py_viewer = app.getViewer("Viewer1")
        py_viewer.seek(20)
        py_viewer.setAInput(1)
        py_viewer.setChannels(NatronEngine.Natron.DisplayChannelsEnum.eDisplayChannelsR)
        gl = lp.viewer_gl()
        require(gl is not None, "A has a viewer")
        lp.wheel(gl, 2)
        pan(gl, 60, -40)
        yield ("sleep", 400)
        z = zoom()
        c = centre()
        a_input = viewer_param("Viewer1", "aInput")
        a_channels = viewer_param("Viewer1", "displayChannels")
        require(z is not None and c is not None, "A's viewer reports its zoom and centre")
        require(shown_viewer() == "Viewer1", "A's viewer state is Viewer1's (%r)" % shown_viewer())
        require(a_input == 1 and a_channels == 2, "A shows input 2 in R (input A %r, channels %r)" % (a_input, a_channels))
        report("NOTE A's viewer: frame %r zoom %r centre %r" % (frame(), z, c))
        answer = yield from lp.client_do("matches Viewer1 20 %r %r %r 1 2" % (z, c[0], c[1]))
        check(answer == "ok", "B's viewer matches A's viewer node, frame, zoom, pan, input and channels: %s" % answer)

        gt.section("stop")
        answer = yield from lp.client_do("scrub 25")
        check(answer == "ok", "B's scrub stops following: %s" % answer)
        app.getViewer("Viewer1").seek(30)
        deadline = time.time() + 1.0
        while time.time() < deadline:
            yield ("sleep", 200)
        answer = yield from lp.client_do("frame")
        check(answer == "25", "A's next frame stays on A (B is at %s)" % answer)

        gt.section("key stop")
        answer = yield from lp.client_do("follow " + HOST_NAME)
        require(answer == "ok", "B follows A again", answer)
        answer = yield from lp.until_client("frame", "30")
        check(answer == "30", "B is back on A's frame 30 (%s)" % answer)
        answer = yield from lp.client_do("key")
        check(answer == "ok", "a key press in B's viewer stops following: %s" % answer)
        app.getViewer("Viewer1").seek(35)
        deadline = time.time() + 1.0
        while time.time() < deadline:
            yield ("sleep", 200)
        answer = yield from lp.client_do("frame")
        check(answer == "30", "A's next frame stays on A after the key press (B is at %s)" % answer)

        answer = yield from lp.client_do("quit")
        check(answer == "bye", "the client quits (%s)" % answer)

    lp.run(ROLE, host_steps, client_command)
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
