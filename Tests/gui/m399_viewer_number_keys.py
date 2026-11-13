"""A GUI test that the viewer number keys reuse the Viewer already in the selection's group instead
of creating another one. It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m399-gui/viewer-keys GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m399_viewer_number_keys.py <AppImage or build/fast/App/Natron>'

Each case selects a non-Viewer node, presses "1" on the node graph through QTest (which runs the
shortcut step a plain sendEvent skips), and checks that the group's Viewer count is unchanged and
that the expected Viewer's input 0 is the selected node:
  a  a Viewer made by script and never selected, after the graph's remembered viewer went away
     with another Viewer's deletion;
  b  a Viewer whose tab was taken out of its pane: it is reused and its tab is shown again;
  d  inside a Group that has its own Viewer while the root graph has one: the Group's is used;
  e  inside a Group with no Viewer: exactly one is made there, and a second press reuses it;
  f  the only Viewer was deleted with the Delete key (it sits in the undo trash): one is made;
  g  inside a Group with three Viewers, the newest deleted after the middle one and then the root
     one were focused: the middle one is used, not the first created;
  c  a project saved and reloaded with one Viewer (last, since it resets the project).

Widget wrappers are looked up again after every yield: a wrapper kept across one can outlive its
C++ object, and children found through a collected parent's wrapper die with it.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 400
ACTIVATE_TIMEOUT_S = 10.0
VIEWER = "Viewer"
SOURCE = "Blur"
MAX_PANES = 40

try:
    import NatronEngine
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QCursor
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x, y, group=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def viewers_in(group=None):
        owner = group if group is not None else app
        return [n for n in owner.getChildren() if n.getPluginID() == VIEWER]

    def destroy_viewers(group=None):
        for v in viewers_in(group):
            v.destroy()
        QApplication.processEvents()

    def input_name(viewer):
        src = viewer.getInput(0) if viewer is not None else None
        return src.getScriptName() if src is not None else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def graph_of(group=None):
        """The NodeGraph of group (the root graph when None). createGroupGui() names a group's graph
        after the group's label; the root graph's objectName is left empty."""
        name = group.getLabel() if group is not None else ""
        found = [g for g in gt.widgets_of("NodeGraph") if g.objectName() == name]
        return found[-1] if found else None

    def panes():
        found = []
        for i in range(1, MAX_PANES + 1):
            pane = app.getTabWidget("pane%d" % i)
            if pane is not None:
                found.append(pane)
        return found

    def find_tab(label):
        for pane in panes():
            for i in range(pane.count()):
                if pane.getTabLabel(i) == label:
                    return pane, i
        return None, -1

    def show_graph_tab(label):
        pane, i = find_tab(label)
        if pane is None:
            return False
        pane.setCurrentIndex(i)
        QApplication.processEvents()
        return True

    def viewer_tab_label(viewer, group=None):
        return viewer.getLabel() if group is None else "%s.%s" % (group.getLabel(), viewer.getLabel())

    def activate_main_window():
        win = main_window()
        if win is None:
            return False
        win.activateWindow()
        win.raise_()
        deadline = time.time() + ACTIVATE_TIMEOUT_S
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)
        return win.isActiveWindow()

    def focus_graph(group=None):
        graph = graph_of(group)
        if graph is None or not graph.isVisible():
            return False
        if not activate_main_window():
            return False
        graph.setFocus(Qt.OtherFocusReason)
        vp = graph.viewport()
        QCursor.setPos(vp.mapToGlobal(vp.rect().center()))
        QApplication.processEvents()
        return True

    def press(key, group=None):
        gt.require(focus_graph(group), "the %s graph is shown and focused in an active window"
                   % (group.getLabel() if group is not None else "root"))
        QTest.keyClick(graph_of(group), key)
        QApplication.processEvents()

    click_problem = [""]

    def click_viewer(viewer, group=None):
        """Makes viewer's tab current in its pane and clicks its image, which focuses that Viewer."""
        label = viewer_tab_label(viewer, group)
        pane, i = find_tab(label)
        if pane is None:
            tabs = [pane_.getTabLabel(k) for pane_ in panes() for k in range(pane_.count())]
            click_problem[0] = "no tab %r among %r" % (label, tabs)
            return False
        pane.setCurrentIndex(i)
        shown = []
        deadline = time.time() + ACTIVATE_TIMEOUT_S
        while time.time() < deadline:
            QApplication.processEvents()
            shown = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
            if len(shown) == 1:
                break
            time.sleep(0.02)
        if len(shown) != 1:
            click_problem[0] = "%d visible ViewerGL widgets after selecting tab %r" % (len(shown), label)
            return False
        QTest.mouseClick(shown[0], Qt.LeftButton, Qt.NoModifier, shown[0].rect().center())
        QApplication.processEvents()
        return True

    def select_only(node, group=None):
        app.clearSelection()
        if group is not None:
            app.clearSelection(group)
        app.selectNode(node, True)
        QApplication.processEvents()

    def check_reused(case, viewer, source, group=None, count=1):
        viewers = viewers_in(group)
        where = group.getLabel() if group is not None else "the root graph"
        check(len(viewers) == count, "%s: %s still holds %d Viewer(s) (got %d)" % (case, where, count, len(viewers)))
        check(input_name(viewer) == source.getScriptName(),
              "%s: %s's input 0 is %s (got %r)" % (case, viewer.getScriptName(), source.getScriptName(), input_name(viewer)))

    def steps():
        ok = yield ("until", lambda: graph_of() is not None and graph_of().isVisible(), STARTUP_TIMEOUT_S)
        gt.require(ok, "the root node graph is up")
        gt.require(QTest is not None, "PySide6.QtTest is importable")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("a-scripted-viewer")
        destroy_viewers()
        source = create(SOURCE, 0, 0)
        kept = create(VIEWER, 0, 300)
        other = create(VIEWER, 300, 300)
        yield ("sleep", SETTLE_MS)
        other.destroy()
        yield ("sleep", SETTLE_MS)
        gt.require(len(viewers_in()) == 1, "a: one Viewer left after deleting the other")
        select_only(source)
        press(Qt.Key_1)
        yield ("sleep", SETTLE_MS)
        check_reused("a", kept, source)

        gt.section("b-closed-tab")
        destroy_viewers()
        kept = create(VIEWER, 0, 300)
        yield ("sleep", SETTLE_MS)
        label = viewer_tab_label(kept)
        pane, index = find_tab(label)
        gt.require(pane is not None, "b: found the tab of " + label)
        pane.removeTab(index)
        QApplication.processEvents()
        gt.require(find_tab(label)[0] is None, "b: %s's tab is out of every pane" % label)
        gt.require(len(viewers_in()) == 1, "b: the Viewer node outlives its closed tab")
        yield ("sleep", SETTLE_MS)
        select_only(source)
        press(Qt.Key_1)
        yield ("sleep", SETTLE_MS)
        check_reused("b", kept, source)
        check(find_tab(label)[0] is not None, "b: %s's tab is shown again" % label)

        gt.section("d-group-with-viewer")
        destroy_viewers()
        root_viewer = create(VIEWER, 0, 300)
        gt.require(show_graph_tab("Node Graph") and focus_graph(), "d: the root graph is focused before the Group is made")
        group = create("Group", 600, 0)
        yield ("sleep", SETTLE_MS)
        gt.require(show_graph_tab(group.getLabel()), "d: %s's graph has a tab" % group.getLabel())
        inner_viewer = create(VIEWER, 0, 300, group)
        inner_source = create(SOURCE, 0, 0, group)
        yield ("sleep", SETTLE_MS)
        gt.require(show_graph_tab(group.getLabel()), "d: %s's graph is the current tab" % group.getLabel())
        select_only(inner_source, group)
        press(Qt.Key_1, group)
        yield ("sleep", SETTLE_MS)
        check_reused("d", inner_viewer, inner_source, group)
        check(len(viewers_in()) == 1, "d: the root graph still holds one Viewer (got %d)" % len(viewers_in()))
        check(input_name(root_viewer) != inner_source.getScriptName(), "d: the root Viewer was left alone")

        gt.section("e-group-without-viewer")
        gt.require(show_graph_tab("Node Graph") and focus_graph(), "e: the root graph is focused before the Group is made")
        bare = create("Group", 900, 0)
        yield ("sleep", SETTLE_MS)
        bare_source = create(SOURCE, 0, 0, bare)
        yield ("sleep", SETTLE_MS)
        gt.require(show_graph_tab(bare.getLabel()), "e: %s's graph is the current tab" % bare.getLabel())
        gt.require(len(viewers_in(bare)) == 0, "e: %s starts with no Viewer" % bare.getLabel())
        select_only(bare_source, bare)
        press(Qt.Key_1, bare)
        yield ("sleep", SETTLE_MS)
        made = viewers_in(bare)
        check(len(made) == 1, "e: the first press made exactly one Viewer in %s (got %d)" % (bare.getLabel(), len(made)))
        check(len(viewers_in()) == 1, "e: the root graph still holds one Viewer (got %d)" % len(viewers_in()))
        if made:
            check(input_name(made[0]) == bare_source.getScriptName(),
                  "e: the new Viewer's input 0 is %s (got %r)" % (bare_source.getScriptName(), input_name(made[0])))
            gt.require(show_graph_tab(bare.getLabel()), "e: %s's graph is the current tab again" % bare.getLabel())
            select_only(bare_source, bare)
            press(Qt.Key_1, bare)
            yield ("sleep", SETTLE_MS)
            check_reused("e second press", made[0], bare_source, bare)

        gt.section("f-deleted-viewer")
        gt.require(show_graph_tab("Node Graph"), "f: the root graph is the current tab")
        gt.require(len(viewers_in()) == 1, "f: the root graph holds one Viewer")
        doomed = viewers_in()[0]
        select_only(doomed)
        press(Qt.Key_Backspace)
        yield ("sleep", SETTLE_MS)
        gt.require(len(viewers_in()) == 0, "f: the Delete key put the only Viewer in the trash")
        select_only(source)
        press(Qt.Key_1)
        yield ("sleep", SETTLE_MS)
        made = viewers_in()
        check(len(made) == 1, "f: one Viewer was made (got %d)" % len(made))
        if made:
            check(input_name(made[0]) == source.getScriptName(),
                  "f: the new Viewer's input 0 is %s (got %r)" % (source.getScriptName(), input_name(made[0])))

        gt.section("g-recently-focused-viewer")
        gt.require(show_graph_tab("Node Graph") and focus_graph(), "g: the root graph is focused before the Group is made")
        gt.require(len(viewers_in()) == 1, "g: the root graph holds one Viewer")
        root_viewer = viewers_in()[0]
        multi = create("Group", 1200, 0)
        yield ("sleep", SETTLE_MS)
        first = create(VIEWER, 0, 300, multi)
        focused = create(VIEWER, 300, 300, multi)
        newest = create(VIEWER, 600, 300, multi)
        multi_source = create(SOURCE, 0, 0, multi)
        yield ("sleep", SETTLE_MS)
        gt.require(click_viewer(focused, multi), "g: clicked the image of %s, the only Viewer shown"
                   % viewer_tab_label(focused, multi), click_problem[0])
        yield ("sleep", SETTLE_MS)
        gt.require(click_viewer(root_viewer), "g: clicked the image of the root Viewer, the only Viewer shown",
                   click_problem[0])
        yield ("sleep", SETTLE_MS)
        newest.destroy()
        yield ("sleep", SETTLE_MS)
        gt.require(len(viewers_in(multi)) == 2, "g: %s holds two Viewers after deleting the newest"
                   % multi.getLabel())
        gt.require(show_graph_tab(multi.getLabel()), "g: %s's graph is the current tab" % multi.getLabel())
        select_only(multi_source, multi)
        press(Qt.Key_1, multi)
        yield ("sleep", SETTLE_MS)
        check_reused("g", focused, multi_source, multi, count=2)
        check(input_name(first) != multi_source.getScriptName(), "g: the first-created Viewer was left alone")

        gt.section("c-reloaded-project")
        destroy_viewers()
        reload_viewer = create(VIEWER, 0, 300)
        reload_source = create(SOURCE, 300, 0)
        yield ("sleep", SETTLE_MS)
        viewer_name = reload_viewer.getScriptName()
        source_name = reload_source.getScriptName()
        # Saving appends .ntpz to any other suffix, and loading a path that does not exist
        # blocks on a modal error dialog.
        path = os.path.join(gt.OUT, "m399-viewer-reload.ntpz")
        gt.require(app.saveProjectAs(path), "c: saved the project to " + path)
        gt.require(app.resetProject(), "c: reset the project")
        yield ("sleep", SETTLE_MS)
        host = app.loadProject(path)
        yield ("sleep", 800)
        gt.require(host is not None, "c: reloaded " + path)
        ok = yield ("until", lambda: host.getNode(viewer_name) is not None and graph_of() is not None
                    and graph_of().isVisible(), 20.0)
        gt.require(ok, "c: the reloaded Viewer and the root graph are up")
        yield ("sleep", SETTLE_MS)
        gt.require(len(viewers_in()) == 1, "c: the reloaded project holds one Viewer (got %d)" % len(viewers_in()))
        select_only(host.getNode(source_name))
        press(Qt.Key_1)
        yield ("sleep", SETTLE_MS)
        check_reused("c", host.getNode(viewer_name), host.getNode(source_name))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
