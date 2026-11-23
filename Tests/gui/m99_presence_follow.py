"""A two-process GUI test of live session presence and following a viewer. A hosts, B joins (see
Tests/gui/live_peers.py):

- A selects Tint and opens its panel; B's node graph outlines Tint with A's name, and B's own
  panel of Tint shows A's dot.
- B follows A from the Session Users panel; A moves to frame 20 and zooms, and B's viewer matches.
- B moves its own timeline, and following stops: the label goes, the Follow box clears and A's
  next frame change stays on A.

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
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication, QLabel, QTreeWidget

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

    def zoom():
        gl = lp.viewer_gl()
        value = gl.property("natronLiveSessionZoom") if gl is not None else None
        return float(value) if value is not None else None

    def frame():
        viewer = app.getViewer("Viewer1")
        return viewer.getCurrentFrame() if viewer is not None else None

    def client_command(words):
        verb = words[0]
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
            want_frame = int(words[1])
            want_zoom = float(words[2])
            ok = yield ("until", lambda: frame() == want_frame and zoom() is not None
                        and abs(zoom() - want_zoom) <= 1e-3 * max(1.0, want_zoom), 5.0)
            return "ok" if ok else "frame %r zoom %r" % (frame(), zoom())
        if verb == "scrub":
            app.getViewer("Viewer1").seek(int(words[1]))
            QApplication.processEvents()
            ok = yield ("until", lambda: follow_label() == "", 5.0)
            if not ok:
                return "still following after the scrub (%r)" % follow_label()
            tree, item = users_item(HOST_NAME)
            if item is not None and item.checkState(2) == Qt.Checked:
                return "the Follow box is still checked"
            if "Stopped following" not in lp.status_message():
                return "the status bar did not note it (%r)" % lp.status_message()
            return "ok"
        if verb == "frame":
            return str(frame())
        return "unknown command " + verb

    def host_steps():
        ok = yield ("until", lambda: lp.main_gui() is not None, lp.STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()
        src = app.createNode("Constant")
        tint = app.createNode("Grade")
        require(src is not None and tint is not None, "create the starting comp")
        src.setScriptName("Src")
        tint.setScriptName("Tint")
        src.setPosition(0, 0)
        tint.setPosition(0, 150)
        tint.connectInput(0, src)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.connectInput(0, tint)
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
        answer = yield from lp.client_do("follow " + HOST_NAME)
        require(answer == "ok", "B follows A", answer)
        app.getViewer("Viewer1").seek(20)
        gl = lp.viewer_gl()
        require(gl is not None, "A has a viewer")
        lp.wheel(gl, 2)
        yield ("sleep", 400)
        z = zoom()
        require(z is not None, "A's viewer reports its zoom")
        report("NOTE A's viewer: frame %r zoom %r" % (frame(), z))
        answer = yield from lp.client_do("matches 20 %r" % z)
        check(answer == "ok", "B's viewer matches A's frame and zoom: %s" % answer)

        gt.section("stop")
        answer = yield from lp.client_do("scrub 25")
        check(answer == "ok", "B's scrub stops following: %s" % answer)
        app.getViewer("Viewer1").seek(30)
        deadline = time.time() + 1.0
        while time.time() < deadline:
            yield ("sleep", 200)
        answer = yield from lp.client_do("frame")
        check(answer == "25", "A's next frame stays on A (B is at %s)" % answer)

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
