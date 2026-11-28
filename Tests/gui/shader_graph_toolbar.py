"""A GUI test that the left toolbar follows the active graph: inside a USD Material Builder it has
one button per target of the builder, each listing that target's shader nodes in one submenu per
manifest kind, alphabetically, and a Backdrop button, and none of the main graph's groupings;
Shift+Tab in the tab menu changes the tab menu's target only, not the toolbar; back on the main
graph's tab it has one button per nodes-menu grouping again. It is driven by
Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/toolbar GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_toolbar.py <AppImage or build/fast/App/Natron>'

Shots: shader_graph_toolbar_builder.png (the builder's graph), shader_graph_toolbar_next.png
(after Shift+Tab) and shader_graph_toolbar_main.png (the main graph).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
BACKDROP = "Backdrop"
PROP_ID = "natronPluginID"
PROP_OWNED = "natronPluginOwned"
PROP_GROUPING = "natronGrouping"
PROP_TARGETS = "natronNetworkTargets"

try:
    import faulthandler

    import NatronGui
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg
    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    toolbox_buttons = sg.toolbox_buttons

    def menu_plugin_ids(menu, found):
        for a in menu.actions():
            sub = gt.menu_of(a)
            if sub is not None:
                menu_plugin_ids(sub, found)
                continue
            value = a.property(PROP_ID)
            if isinstance(value, str) and value:
                found.append(value)
        return found

    def toolbox_plugin_ids():
        found = []
        for button in toolbox_buttons():
            menu_plugin_ids(gt.menu_of(button), found)
        return found

    def main_groupings():
        """The groupings of the nodes menu's top-level plug-in submenus, in order."""
        found = []
        for a in NatronGui.natron.nodesMenu().actions():
            if a.property(PROP_OWNED) and gt.menu_of(a) is not None:
                found.append(a.property(PROP_GROUPING))
        return found

    def button_groupings():
        return [gt.menu_of(b).menuAction().property(PROP_GROUPING) for b in toolbox_buttons()]

    def kind_label(kind):
        """The category the toolbar lists a manifest kind under, as shaderLabelFromScriptName()
        gives it for the kinds the manifests carry."""
        return " ".join(word.capitalize() for word in kind.split("_") if word)

    def shader_kinds():
        found = {}
        for renderer in sg.RENDERERS:
            for node in sg.manifest_nodes(renderer):
                found[node["id"]] = kind_label(node.get("kind", ""))
        return found

    def target_menus():
        """{target: menu} of the toolbar's target buttons."""
        found = {}
        for button in toolbox_buttons():
            menu = gt.menu_of(button)
            target = menu.menuAction().property(PROP_TARGETS)
            if isinstance(target, str) and target:
                found[target] = menu
        return found

    def target_layout(menu):
        """[(category, [(text, plugin ID)])] of a target button's menu, in menu order; the nodes
        listed outside any submenu come under the category None."""
        layout = []
        loose = []
        for a in menu.actions():
            sub = gt.menu_of(a)
            if sub is None:
                value = a.property(PROP_ID)
                if isinstance(value, str) and value:
                    loose.append((a.text(), value))
                continue
            entries = []
            for b in sub.actions():
                value = b.property(PROP_ID)
                if gt.menu_of(b) is None and isinstance(value, str) and value:
                    entries.append((b.text(), value))
            layout.append((a.property(PROP_GROUPING), entries))
        if loose:
            layout.append((None, loose))
        return layout

    def toolbox_snapshot():
        return sorted((target, target_layout(menu)) for target, menu in target_menus().items())

    def check_target_menu(shaders, kinds, target, menu, label):
        layout = target_layout(menu)
        categories = [c for c, _entries in layout if c is not None]
        report("NOTE %s, %s: categories %r" % (label, target, [(c, len(e)) for c, e in layout]))
        check(categories, "%s groups the %s nodes into submenus" % (label, target))
        check(categories == sorted(categories, key=str.lower),
              "%s lists the %s submenus alphabetically (%r)" % (label, target, categories))
        ids = []
        for category, entries in layout:
            texts = [t for t, _pid in entries]
            check(texts == sorted(texts, key=str.lower),
                  "%s sorts %s/%s alphabetically (%r)" % (label, target, category, texts[:6]))
            misfiled = [pid for _t, pid in entries if category is not None and kinds.get(pid) != category]
            check(not misfiled, "%s files %s/%s by manifest kind (misfiled %r)" % (label, target, category, misfiled[:5]))
            ids.extend(pid for _t, pid in entries)
        expected = set(pid for pid, targets in shaders.items() if target in targets)
        missing = sorted(expected - set(ids))
        check(not missing, "%s offers every %s shader (missing %r)" % (label, target, missing[:5]))
        extra = sorted(set(ids) - expected)
        check(not extra, "%s offers only %s shaders under %s (also %r)" % (label, target, target, extra[:5]))
        check(len(ids) == len(expected),
              "%s offers each %s shader once (%d entries, %d creatable)" % (label, target, len(ids), len(expected)))

    def check_builder_toolbox(shaders, kinds, targets, label):
        menus = target_menus()
        report("NOTE %s: %d buttons, targets %r" % (label, len(toolbox_buttons()), sorted(menus)))
        check(sorted(menus) == sorted(targets), "%s has one button per builder target (%r, expected %r)"
              % (label, sorted(menus), sorted(targets)))
        for target, menu in menus.items():
            check_target_menu(shaders, kinds, target, menu, label)
        ids = toolbox_plugin_ids()
        check(ids.count(BACKDROP) == 1, "%s offers Backdrop once (%d)" % (label, ids.count(BACKDROP)))
        others = sorted(set(pid for pid in ids if pid != BACKDROP and pid not in shaders))
        check(not others, "%s offers only shader nodes and Backdrop (also %r)" % (label, others[:5]))
        check(len(toolbox_buttons()) == len(menus) + 1, "%s has the target buttons and Backdrop only (%d buttons)"
              % (label, len(toolbox_buttons())))
        main = set(g for g in main_groupings() if g)
        leaked = sorted(g for g in button_groupings() if g in main)
        check(not leaked, "%s has none of the main graph's grouping buttons (got %r)" % (label, leaked))

    def check_main_toolbox(shaders, label):
        expected = main_groupings()
        got = button_groupings()
        check(got == expected, "%s has one button per nodes-menu grouping, in order (%r, expected %r)"
              % (label, got, expected))
        shaders_offered = [pid for pid in toolbox_plugin_ids() if pid in shaders]
        check(not shaders_offered, "%s offers no shader node (got %r)" % (label, shaders_offered[:5]))

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        sg.park_viewer(app)
        require(ok, "the main window is up")
        shaders = sg.shader_targets()
        require(len(shaders) > 100, "the manifests list the shader nodes (%d)" % len(shaders))
        kinds = shader_kinds()

        gt.section("main graph at startup")
        expected = len(main_groupings())
        ok = yield ("until", sg.toolbox_settled(lambda n: n == expected), 20.0)
        check(ok, "the startup toolbar settles on %d buttons (%d)" % (expected, len(toolbox_buttons())))
        check_main_toolbox(shaders, "the startup toolbar")

        gt.section("builder graph")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        yield ("sleep", SETTLE_MS)
        graph = sg.graph_of(builder)

        popup, line_edit, _view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu in the builder's graph")
        context = tabtabtab_natron._plugin.context() or {}
        targets = tuple(context.get("networkTargets") or ())
        report("NOTE builder targets %r" % (targets,))
        require(len(targets) >= 2, "the builder has several targets (%r)" % (targets,))
        first = tabtabtab_natron._network_target(context)
        sg.close_tab_menu(popup)
        yield ("until", sg.toolbox_settled(lambda n: n == len(targets) + 1), 20.0)
        check_builder_toolbox(shaders, kinds, targets, "the builder's toolbar")
        before = toolbox_snapshot()
        gt.shot("shader_graph_toolbar_builder.png")

        popup, line_edit, _view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu again")
        sg.key_click(line_edit, Qt.Key_Backtab, Qt.ShiftModifier)
        QApplication.processEvents()
        second = tabtabtab_natron._network_target(context)
        require(second != first, "Shift+Tab makes another target current in the tab menu (%r)" % second)
        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)
        check(toolbox_snapshot() == before, "Shift+Tab leaves the toolbar as it was")
        gt.shot("shader_graph_toolbar_next.png")

        gt.section("back on the main graph")
        root = sg.show_root_graph(app)
        require(root is not None, "the main graph is shown")
        yield ("until", sg.toolbox_settled(lambda n: n == expected), 20.0)
        check_main_toolbox(shaders, "the main graph's toolbar")
        gt.shot("shader_graph_toolbar_main.png")

        gt.section("builder graph again")
        require(sg.open_graph(app, builder), "the builder's graph tab is shown again")
        yield ("until", sg.toolbox_settled(lambda n: n == len(targets) + 1), 20.0)
        check_builder_toolbox(shaders, kinds, targets, "the builder's toolbar on coming back")
        check(toolbox_snapshot() == before, "the builder's toolbar on coming back is the one it had")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
