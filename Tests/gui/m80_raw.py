"""Develop RAW in the interface: a camera raw file dropped on the node graph becomes a Read with
RawPrepare, RawTemperature, RawHighlights, RawDemosaic and RawInputColor below it, connected in that
order inside a backdrop titled "Develop RAW". One undo removes the Read, the chain and the
backdrop together, and one redo brings them all back. Python's createReader(), a new filename on an
existing Read, the preference turned off and a project reload add no chain.

The Viewer shows raw directly: connected to the Read of the CR2 it shows the mosaic as grey with
"RAW mosaic" on its info line, and connected to a Read of the linear DNG it shows "RAW linear",
with no error either way.

The dropped file is Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2, or an empty file of that name
when the fixture is absent (a checkout without Git LFS has pointer files there); the Viewer
section is then skipped. The other raw files are empty files with a camera raw extension.

Shots: m80-develop-raw.png (the node graph after the drop), m80-viewer-mosaic.png and
m80-viewer-linear.png (the Viewer on each).

Usage: Tests/gui/run-gui-test.sh Tests/gui/m80_raw.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500

READ_ID = "fr.inria.built-in.Read"
BACKDROP_ID = "fr.inria.built-in.BackDrop"
CHAIN_IDS = ["fr.natron.RawPrepare", "fr.natron.RawTemperature", "fr.natron.RawHighlights",
             "fr.natron.RawDemosaic", "fr.natron.RawInputColor"]

try:
    import NatronEngine
    from PySide6.QtCore import QMimeData, QPointF, Qt, QUrl
    from PySide6.QtGui import QDropEvent
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow

    import guitest as gt
    from guitest import check, require

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures")

    def make_raw_file(name):
        """An empty file with a camera raw extension."""
        os.makedirs(FIXTURE_DIR, exist_ok=True)
        path = os.path.join(FIXTURE_DIR, name)
        open(path, "wb").close()
        return path

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible() and gt.class_name(w) == "Gui":
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def nodes_of(owner, plugin_id):
        return [n for n in owner.getChildren() if n.getPluginID() == plugin_id]

    def drop_file(path):
        """Drops path on the centre of the node graph, as a file dragged from a file browser."""
        win = main_window()
        graph = root_graph()
        require(win is not None and graph is not None, "the main window and its node graph are up")
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(path)])
        centre = win.mapFromGlobal(graph.mapToGlobal(graph.rect().center()))
        event = QDropEvent(QPointF(centre), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(win, event)

    def is_input(node, index, source):
        inp = node.getInput(index)
        return inp is not None and source is not None and inp.getScriptName() == source.getScriptName()

    def inside(node, backdrop):
        x, y = node.getPosition()
        w, h = node.getSize()
        bx, by = backdrop.getPosition()
        bw, bh = backdrop.getSize()
        return bx <= x and by <= y and x + w <= bx + bw and y + h <= by + bh

    def trigger_edit(name):
        """Undo and Redo are QActions on the Edit menu, matched by Qt's shortcut map before a key
        event sent to a widget would reach them, so the action itself is triggered."""
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = act.menu()
                if menu is None or not act.text().lower().startswith("edit"):
                    continue
                for a in menu.actions():
                    if a.text().lower().startswith(name) and a.isEnabled():
                        a.trigger()
                        QApplication.processEvents()
                        return True
        return check(False, "found an enabled %s action in the Edit menu" % name)

    def is_real_fixture(path):
        """A decodable fixture rather than a Git LFS pointer file or an empty stand-in."""
        return os.path.exists(path) and os.path.getsize(path) > (1 << 20)

    def info_line_has(text):
        for w in gt.widgets_of("InfoViewerWidget"):
            if w.isVisible() and any(text in label.text() for label in w.findChildren(QLabel)):
                return True
        return False

    def grey(colour):
        return colour is not None and max(colour) - min(colour) <= 2

    def show_in_viewer(node):
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer accepts the raw Read " + node.getScriptName())

    def settle_counts():
        return {pid: len(nodes_of(app, pid)) for pid in [READ_ID, BACKDROP_ID] + CHAIN_IDS}

    def steps():
        ok = yield ("until", lambda: root_graph() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        gt.close_all_panels()

        develop = NatronEngine.natron.getSettings().getParam("developRawOnRead")
        require(develop is not None, "the preferences have developRawOnRead")
        check(develop.getValue(), "Develop RAW files on read is on by default")

        gt.section("drop")
        dropped = os.path.join(gt.FIXTURES, "raw", "canon-eos-5d-mark-iii.cr2")
        if not os.path.exists(dropped):
            dropped = make_raw_file("canon-eos-5d-mark-iii.cr2")
        drop_file(dropped)
        ok = yield ("until", lambda: len(nodes_of(app, CHAIN_IDS[-1])) == 1, POLL_TIMEOUT_S)
        require(ok, "dropping a .CR2 file builds the Develop RAW chain")
        yield ("sleep", SETTLE_MS)
        reads = nodes_of(app, READ_ID)
        require(len(reads) == 1, "the drop made one Read")
        read = reads[0]
        check(read.getParam("filename").getValue().endswith("canon-eos-5d-mark-iii.cr2"), "the Read reads the dropped file")
        chain = [nodes_of(app, pid)[0] for pid in CHAIN_IDS]
        previous = read
        for node in chain:
            check(is_input(node, 0, previous), "%s is connected below %s" % (node.getPluginID(), previous.getPluginID()))
            previous = node
        backdrops = nodes_of(app, BACKDROP_ID)
        require(len(backdrops) == 1, "the chain has one backdrop")
        backdrop = backdrops[0]
        check(backdrop.getLabel() == "Develop RAW", "the backdrop is titled Develop RAW (got %r)" % backdrop.getLabel())
        for node in chain:
            check(inside(node, backdrop), "%s sits inside the backdrop" % node.getPluginID())
        check(not inside(read, backdrop), "the Read stays above the backdrop")
        rx, ry = read.getPosition()
        check(all(n.getPosition()[1] > ry for n in chain), "the chain is below the Read")
        ys = [n.getPosition()[1] for n in chain]
        check(ys == sorted(ys), "the chain runs top to bottom in order")
        gt.shot("m80-develop-raw.png")

        gt.section("undo")
        if trigger_edit("undo"):
            ok = yield ("until", lambda: not nodes_of(app, READ_ID) and not nodes_of(app, BACKDROP_ID), POLL_TIMEOUT_S)
            check(ok, "one undo removes the Read and the backdrop")
            check(all(not nodes_of(app, pid) for pid in CHAIN_IDS), "the same undo removes the whole chain")
            if trigger_edit("redo"):
                ok = yield ("until", lambda: len(nodes_of(app, CHAIN_IDS[-1])) == 1 and len(nodes_of(app, READ_ID)) == 1, POLL_TIMEOUT_S)
                check(ok, "one redo brings the Read and the chain back")
                check(len(nodes_of(app, BACKDROP_ID)) == 1, "the redo brings the backdrop back")
                check(is_input(nodes_of(app, CHAIN_IDS[0])[0], 0, nodes_of(app, READ_ID)[0]), "the chain is connected below the Read again")
                read = nodes_of(app, READ_ID)[0]

        gt.section("viewer")
        linear = os.path.join(gt.FIXTURES, "raw", "canon-eos-5d-mark-iii-linear-rgb.dng")
        if is_real_fixture(dropped) and is_real_fixture(linear):
            show_in_viewer(read)
            ok = yield ("until", lambda: info_line_has("RAW mosaic"), POLL_TIMEOUT_S)
            check(ok, "the info line reads RAW mosaic")
            yield ("sleep", SETTLE_MS)
            check(not gt.viewer_messages(), "the Viewer shows the mosaic without an error (%r)" % (gt.viewer_messages(),))
            check(grey(gt.viewer_centre_colour()), "the mosaic shows as grey (%r)" % (gt.viewer_centre_colour(),))
            gt.shot("m80-viewer-mosaic.png")

            linear_read = app.createReader(linear)
            require(linear_read is not None, "make a Read of the linear DNG")
            show_in_viewer(linear_read)
            ok = yield ("until", lambda: info_line_has("RAW linear"), POLL_TIMEOUT_S)
            check(ok, "the info line reads RAW linear")
            yield ("sleep", SETTLE_MS)
            check(not gt.viewer_messages(), "the Viewer shows the linear DNG without an error (%r)" % (gt.viewer_messages(),))
            gt.shot("m80-viewer-linear.png")
        else:
            gt.report("NOTE the raw fixtures are not checked out (git lfs pull); the Viewer section is skipped")
        counts = settle_counts()

        gt.section("no chain")
        scripted = app.createReader(make_raw_file("scripted.NEF"))
        require(scripted is not None, "Python's createReader() makes a Read")
        yield ("sleep", SETTLE_MS)
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "a Read made from Python gets no chain")

        plain = app.createReader(os.path.join(gt.FIXTURES, "flat-rgba-only.exr"))
        require(plain is not None, "make a Read of an EXR")
        plain.getParam("filename").setValue(make_raw_file("renamed.ARW"))
        yield ("sleep", SETTLE_MS)
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "a new raw filename on an existing Read gets no chain")

        develop.setValue(False)
        drop_file(make_raw_file("off.DNG"))
        yield ("until", lambda: len(nodes_of(app, READ_ID)) == counts[READ_ID] + 3, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        check(len(nodes_of(app, READ_ID)) == counts[READ_ID] + 3, "with the preference off the drop still makes a Read")
        check(settle_counts()[CHAIN_IDS[0]] == counts[CHAIN_IDS[0]], "with the preference off the drop gets no chain")
        develop.setValue(True)

        gt.section("reload")
        path = os.path.join(gt.OUT, "m80-develop.ntp")
        if check(app.saveProjectAs(path), "save the project to " + path):
            if check(app.resetProject(), "reset the project"):
                reloaded = app.loadProject(path)
                yield ("sleep", SETTLE_MS * 2)
                if check(reloaded is not None, "reload " + path):
                    for pid in CHAIN_IDS + [BACKDROP_ID]:
                        n = len(nodes_of(reloaded, pid))
                        check(n == 1, "the reloaded project has one %s (got %d)" % (pid, n))

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
