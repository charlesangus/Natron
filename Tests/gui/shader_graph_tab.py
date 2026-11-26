"""A GUI test of the tab menu inside a USD Material Builder: Tab lists only the shader nodes of the
builder's current target, and Backdrop; Shift+Tab makes the builder's next target current, which
the menu names; the main graph's Tab offers the builder and no shader. It is driven by
Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/tab GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_tab.py <AppImage or build/fast/App/Natron>'

Shots: shader_graph_tab_first.png (the menu for the first target), shader_graph_tab_next.png (after
Shift+Tab) and shader_graph_tab_main.png (the main graph's menu filtered to "material").
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
BACKDROP = "Backdrop"

try:
    import faulthandler

    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg
    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def check_target_list(ids, shaders, target, label):
        listed = set(ids)
        expected = set(pid for pid, targets in shaders.items() if target in targets)
        check(BACKDROP in listed, "%s lists Backdrop" % label)
        others = sorted(pid for pid in listed if pid != BACKDROP and pid not in shaders)
        check(not others, "%s lists only shader nodes and Backdrop (also %r)" % (label, others[:5]))
        wrong = sorted(pid for pid in listed if pid in shaders and target not in shaders[pid])
        check(not wrong, "%s lists no shader without the target %s (got %r)" % (label, target, wrong[:5]))
        missing = sorted(expected - listed)
        check(not missing, "%s lists every shader with the target %s (missing %r)" % (label, target, missing[:5]))

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        shaders = sg.shader_targets()
        require(len(shaders) > 100, "the manifests list the shader nodes (%d)" % len(shaders))

        gt.section("builder graph")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        graph = sg.graph_of(builder)

        popup, line_edit, _view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu in the builder's graph")
        context = tabtabtab_natron._plugin.context() or {}
        targets = tuple(context.get("networkTargets") or ())
        report("NOTE builder targets %r" % (targets,))
        require(len(targets) >= 2, "the builder's menu switches among its targets (%r)" % (targets,))
        first = tabtabtab_natron._network_target(context)
        check(first == targets[0], "the first target is current on a builder's first Tab (%r)" % first)
        label = sg.target_label(popup)
        first_label = tabtabtab_natron._network_target_label(context, first)
        check(first_label in label, "the menu names the current target %r (label %r)" % (first_label, label))
        check_target_list(sg.listed_plugin_ids(tabtabtab_natron), shaders, first, "the first target's menu")
        gt.shot("shader_graph_tab_first.png")

        QTest.keyClick(line_edit, Qt.Key_Backtab, Qt.ShiftModifier)
        QApplication.processEvents()
        check(popup.isVisible(), "Shift+Tab leaves the menu open")
        second = tabtabtab_natron._network_target(context)
        check(second == targets[1], "Shift+Tab makes the next target current (%r)" % second)
        label = sg.target_label(popup)
        second_label = tabtabtab_natron._network_target_label(context, second)
        check(second_label in label, "the menu names the new target %r (label %r)" % (second_label, label))
        view = popup.findChildren(sg.QListView)[0]
        rows = view.model().rowCount()
        check(rows > 0, "the menu shows rows after Shift+Tab (%d)" % rows)
        check_target_list(sg.listed_plugin_ids(tabtabtab_natron), shaders, second, "the next target's menu")
        gt.shot("shader_graph_tab_next.png")
        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)

        popup, _line_edit, _view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu again")
        again = tabtabtab_natron._network_target(tabtabtab_natron._plugin.context() or {})
        check(again == second, "the builder keeps the target last made current (%r)" % again)
        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)

        gt.section("main graph")
        root = sg.show_root_graph(app)
        require(root is not None, "the main graph is shown")
        popup, line_edit, _view = yield from sg.open_tab_menu(root)
        require(popup is not None, "Tab opens the menu in the main graph")
        check(not tabtabtab_natron._is_network_context(tabtabtab_natron._plugin.context()),
              "the main graph's menu is not filtered for a network")
        check(sg.target_label(popup) == "", "the main graph's menu names no target")
        ids = sg.listed_plugin_ids(tabtabtab_natron)
        check(sg.BUILDER in ids, "the main graph's menu offers %s" % sg.BUILDER)
        leaked = sorted(pid for pid in ids if pid in shaders)
        check(not leaked, "the main graph's menu offers no shader (got %r)" % (leaked[:5],))
        line_edit.setText("material")
        QApplication.processEvents()
        gt.shot("shader_graph_tab_main.png")
        sg.close_tab_menu(popup)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
