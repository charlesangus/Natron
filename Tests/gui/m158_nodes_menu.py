"""Checks for the process-wide nodes menu (NatronGui.natron.nodesMenu()) that every window's left
toolbox and node-graph right-click menu show.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m158-shots/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m158_nodes_menu.py build/release/App/Natron

Sections:
  - api: nodesMenu() returns the QMenu named NodesMenu, the same object on every call;
  - tree: every user-creatable plug-in version (the tab menu's node items, keyed by ID and major)
    has exactly one plug-in-owned action, under its grouping path ("Misc" when it has none), with
    the ID, major, category colour and shortcut properties; top-level groupings keep the toolbox
    order; triggering an action creates the node;
  - python item: a QAction added under Merge/ shows in the main window's Merge toolbox popup;
  - second window: a window opened afterwards has the same toolbox buttons over the same menus,
    shows the Python item, and both windows' graph right-click menus hold every top-level entry
    of the nodes menu; a node created from the menu after a right-click in the second window lands
    in that window's project;
  - disable: disabling a Merge plug-in in Preferences removes only its action, keeps the Python
    item, and re-enabling it (or cancelling Preferences) brings the action back.

Plug-ins are enabled and disabled through the Preferences plug-in list's "Enabled" checkboxes,
which are AnimatedCheckBox frames toggled by a Space key press, and Preferences is cancelled at the
end, so the user's saved plug-in settings are never written.

Shots: m158-toolbox-window1.png, m158-toolbox-window2.png, m158-merge-popup.png.
"""
import json
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
SETTLE_MS = 500

PROP_OWNED = "natronPluginOwned"
PROP_ID = "natronPluginID"
PROP_MAJOR = "natronPluginMajor"
PROP_COLOR = "natronCategoryColor"
PROP_SHORTCUT = "natronShortcut"
PROP_GROUPING = "natronGrouping"
PROP_TOOLBUTTON_ICON = "natronToolButtonIcon"

NAMED_ORDER = ["Image", "Draw", "Time", "Channel", "Color", "Filter", "Keyer", "Merge", "Transform", "3D",
               "Deep", "Metadata", "Views", "ToolSets", "Other", "Misc"]
DEFAULT_GROUPING = "Misc"
MERGE_GROUPING = "Merge"
MERGE_ID = "net.sf.openfx.MergePlugin"
BLUR_ID = "net.sf.cimg.CImgBlur"
PYTHON_ITEM_TEXT = "M158 Python item"

# Columns of the Preferences plug-in list (PreferencesPanel.cpp COL_*).
PREFS_COL_ID = 1
PREFS_COL_VERSION = 2
PREFS_COL_ENABLED = 3

try:
    import NatronGui
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, QTimer
    from PySide6.QtGui import QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QGraphicsView, QMenu, QToolBar, QTreeWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def owned(action):
        return bool(action.property(PROP_OWNED))

    def nodes_menu():
        return NatronGui.natron.nodesMenu()

    # ---- the menu tree -------------------------------------------------------------------------

    def walk(menu, path, leaves):
        """Appends (plugin ID, major, grouping path, action) for every plug-in-owned leaf."""
        for a in menu.actions():
            if not owned(a):
                continue
            sub = a.menu()
            if sub is not None:
                walk(sub, path + [a.property(PROP_GROUPING)], leaves)
            else:
                leaves.append((a.property(PROP_ID), int(a.property(PROP_MAJOR)), path, a))
        return leaves

    def expected_items():
        """(ID, major) -> grouping path of every user-creatable plug-in version."""
        expected = {}
        for entry in json.loads(app.getTabMenuNodeItems()):
            key = (entry["id"], int(entry["major"]))
            expected.setdefault(key, list(entry["grouping"]) or [DEFAULT_GROUPING])
        return expected

    def check_tree(area, expected, absent=()):
        leaves = walk(nodes_menu(), [], [])
        paths = {}
        for pid, major, path, _ in leaves:
            paths.setdefault((pid, major), []).append(path)
        missing = sorted(k for k in expected if k not in paths and k not in absent)
        doubled = sorted(k for k, v in paths.items() if len(v) > 1)
        extra = sorted(k for k in paths if k not in expected)
        misplaced = sorted((k, paths[k][0], expected[k]) for k in expected
                           if k in paths and len(paths[k]) == 1 and paths[k][0] != expected[k])
        still_there = sorted(k for k in absent if k in paths)
        report("NOTE %s: %d plug-in actions for %d expected plug-in versions" % (area, len(leaves), len(expected)))
        check(not missing, "%s: every user-creatable plug-in has an action (missing %d: %r)"
              % (area, len(missing), missing[:10]))
        check(not doubled, "%s: no plug-in has two actions (%r)" % (area, doubled[:10]))
        check(not extra, "%s: no action for a plug-in that is not user-creatable (%r)" % (area, extra[:10]))
        check(not misplaced, "%s: each action sits under its plug-in's grouping path (%r)" % (area, misplaced[:5]))
        if absent:
            check(not still_there, "%s: the disabled plug-in has no action (%r)" % (area, still_there))
        return leaves

    def top_level_submenu(grouping):
        for a in nodes_menu().actions():
            if owned(a) and a.menu() is not None and a.property(PROP_GROUPING) == grouping:
                return a.menu()
        return None

    def find_leaf(pid, major=None):
        for leaf in walk(nodes_menu(), [], []):
            if leaf[0] == pid and (major is None or leaf[1] == major):
                return leaf
        return None

    # ---- windows, toolboxes and graphs ---------------------------------------------------------

    def gui_widgets():
        return [w for w in gt.widgets_of("Gui") if w.isVisible()]

    def toolbox_of(gui):
        for bar in gui.findChildren(QToolBar):
            if gt.class_name(bar) == "AutoHideToolBar":
                return bar
        return None

    def toolbox_buttons(gui):
        bar = toolbox_of(gui)
        if bar is None:
            return []
        buttons = []
        for a in bar.actions():
            w = bar.widgetForAction(a)
            if w is not None and hasattr(w, "menu") and w.menu() is not None:
                buttons.append(w)
        return buttons

    def graph_of(gui):
        for g in gui.findChildren(QGraphicsView):
            if gt.class_name(g) == "NodeGraph" and g.isVisible():
                return g
        return None

    def owned_top_level_menus():
        return [a.menu() for a in nodes_menu().actions() if owned(a) and a.menu() is not None]

    def check_toolbox(area, gui):
        buttons = toolbox_buttons(gui)
        menus = owned_top_level_menus()
        check([ptr(b.menu()) for b in buttons] == [ptr(m) for m in menus],
              "%s: one toolbox button per plug-in grouping, in the nodes menu's order (%d buttons, %d groupings)"
              % (area, len(buttons), len(menus)))
        groupings = [m.menuAction().property(PROP_GROUPING) for m in menus]
        tooltips_ok = all(g in (b.toolTip() or "") for b, g in zip(buttons, groupings))
        check(tooltips_ok, "%s: each button's tooltip names its grouping" % area)
        icons_ok = all(not b.icon().isNull() and
                       b.icon().cacheKey() == m.menuAction().property(PROP_TOOLBUTTON_ICON).cacheKey()
                       for b, m in zip(buttons, menus))
        check(icons_ok, "%s: each button draws its grouping's toolbox icon" % area)
        return buttons

    def open_menus():
        menus = []
        popup = QApplication.activePopupWidget()
        if isinstance(popup, QMenu):
            menus.append(popup)
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMenu) and w.isVisible() and all(ptr(w) != ptr(m) for m in menus):
                menus.append(w)
        return menus

    def close_popups():
        for _ in range(4):
            menus = open_menus()
            if not menus:
                return
            for m in reversed(menus):
                m.close()
            QApplication.processEvents()

    def with_open_menu(open_it, on_menu, settle_ms=500):
        """open_it runs a nested event loop (QMenu::exec or a tool button's popup), so on_menu is
        scheduled before it and runs inside that loop; it must close the menu when done. A
        watchdog keeps closing menus until open_it returns, so a missed menu cannot hang the run."""
        done = [False]

        def fire():
            menus = open_menus()
            try:
                on_menu(menus[0] if menus else None)
            except Exception:
                check(False, "menu handler error: " + traceback.format_exc())
                close_popups()

        def guard():
            if done[0]:
                return
            close_popups()
            QTimer.singleShot(2000, guard)
        QTimer.singleShot(settle_ms, fire)
        QTimer.singleShot(settle_ms + 4000, guard)
        open_it()
        done[0] = True
        QApplication.processEvents()

    def right_click(widget, on_menu):
        pos = QPointF(widget.width() // 2, widget.height() // 2)
        ev = QMouseEvent(QEvent.MouseButtonPress, pos, widget.mapToGlobal(pos),
                         Qt.RightButton, Qt.RightButton, Qt.NoModifier)
        with_open_menu(lambda: QApplication.sendEvent(widget, ev), on_menu)

    def check_graph_menu(area, gui):
        graph = graph_of(gui)
        if not check(graph is not None, "%s: found the node graph" % area):
            return
        seen = {}

        def on_menu(menu):
            seen["menu"] = menu is not None
            if menu is not None:
                seen["actions"] = set(ptr(a) for a in menu.actions())
            close_popups()

        right_click(graph.viewport(), on_menu)
        if check(seen.get("menu", False), "%s: the graph's right-click menu opened" % area):
            wanted = [ptr(a) for a in nodes_menu().actions()]
            check(all(p in seen["actions"] for p in wanted),
                  "%s: the right-click menu holds every top-level entry of the nodes menu (%d)" % (area, len(wanted)))

    def node_names(target_app):
        return set(n.getScriptName() for n in target_app.getChildren())

    def new_node(target_app, before, plugin_id):
        for n in target_app.getChildren():
            if n.getScriptName() not in before and n.getPluginID() == plugin_id:
                return n
        return None

    # ---- Preferences plug-in list --------------------------------------------------------------

    def open_prefs():
        for w in gt.widgets_of("PreferencesPanel"):
            if w.isVisible():
                return w
        return None

    def plugin_checkbox(prefs, pid, major):
        for tree in prefs.findChildren(QTreeWidget):
            items = tree.findItems(pid, Qt.MatchFlag.MatchExactly | Qt.MatchFlag.MatchRecursive, PREFS_COL_ID)
            for item in items:
                if item.text(PREFS_COL_VERSION).split(".")[0] == str(major):
                    return tree.itemWidget(item, PREFS_COL_ENABLED)
        return None

    def press_space(widget):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, Qt.Key_Space, Qt.NoModifier))
        QApplication.processEvents()

    def check_python_item(area, merge_menu, item):
        check(shiboken6.isValid(item), "%s: the Python item still exists" % area)
        if not shiboken6.isValid(item) or merge_menu is None:
            return
        actions = merge_menu.actions()
        ptrs = [ptr(a) for a in actions]
        check(ptr(item) in ptrs, "%s: the Python item is still in the Merge menu" % area)
        if ptr(item) in ptrs:
            index = ptrs.index(ptr(item))
            check(all(not owned(a) for a in actions[index:]),
                  "%s: plug-in actions are listed before the Python item" % area)

    # ---- steps ---------------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: len(gui_widgets()) > 0 and graph_of(gui_widgets()[0]) is not None,
                    STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        main_gui = gui_widgets()[0]
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("api")
        menu = nodes_menu()
        require(menu is not None, "natron.nodesMenu() returns a menu")
        check(isinstance(menu, QMenu), "nodesMenu() is a QMenu (got %s)" % type(menu).__name__)
        check(menu.objectName() == "NodesMenu", "the menu is named NodesMenu (got %r)" % menu.objectName())
        check(ptr(nodes_menu()) == ptr(menu), "nodesMenu() returns the same menu on every call")

        gt.section("tree")
        expected = expected_items()
        require(len(expected) > 0, "the plug-in list has user-creatable plug-ins")
        leaves = check_tree("tree", expected)
        colours_ok = all(str(a.property(PROP_COLOR) or "").startswith("#") for _, _, _, a in leaves)
        check(colours_ok, "every plug-in action carries a category colour name")
        check(all(isinstance(a.property(PROP_SHORTCUT), str) for _, _, _, a in leaves),
              "every plug-in action carries its shortcut text")
        merge_leaf = find_leaf(MERGE_ID)
        if check(merge_leaf is not None, "Merge has an action"):
            check(bool(merge_leaf[3].property(PROP_SHORTCUT)),
                  "Merge's action carries its default shortcut (got %r)" % merge_leaf[3].property(PROP_SHORTCUT))
        top = [a.property(PROP_GROUPING) for a in menu.actions() if owned(a) and a.menu() is not None]
        report("NOTE top-level groupings: %r" % top)
        named = [g for g in top if g in NAMED_ORDER]
        check(named == sorted(named, key=NAMED_ORDER.index), "named groupings keep the toolbox order (%r)" % named)
        check(top[:len(named)] == named, "named groupings come before the others (%r)" % top)
        check(all(owned(a) for a in menu.actions()), "the nodes menu holds only plug-in groupings so far")

        blur_leaf = find_leaf(BLUR_ID)
        if check(blur_leaf is not None, "Blur has an action"):
            before = node_names(app)
            blur_leaf[3].trigger()
            ok = yield ("until", lambda: new_node(app, before, BLUR_ID) is not None, POLL_TIMEOUT_S)
            node = new_node(app, before, BLUR_ID)
            if check(ok and node is not None, "triggering Blur's action creates a Blur in the project"):
                node.destroy()
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)

        main_buttons = check_toolbox("window 1", main_gui)
        bar = toolbox_of(main_gui)
        if bar is not None:
            path = os.path.join(gt.OUT, "m158-toolbox-window1.png")
            report(("SHOT " if bar.grab().save(path) else "NOTE could not save ") + path)

        gt.section("python item")
        merge_menu = top_level_submenu(MERGE_GROUPING)
        require(merge_menu is not None, "the nodes menu has a Merge submenu")
        item = merge_menu.addAction(PYTHON_ITEM_TEXT)
        fired = []
        item.triggered.connect(lambda: fired.append(True))
        check(not owned(item), "an action added from Python carries no plug-in marker")
        merge_button = next((b for b in main_buttons if ptr(b.menu()) == ptr(merge_menu)), None)
        if check(merge_button is not None, "window 1 has a Merge toolbox button"):
            seen = {}

            def on_popup(popup):
                seen["popup"] = popup is not None and ptr(popup) == ptr(merge_menu)
                if popup is not None:
                    seen["listed"] = ptr(item) in [ptr(a) for a in popup.actions()]
                    seen["drawn"] = popup.actionGeometry(item).isValid() and item.isVisible()
                    path = os.path.join(gt.OUT, "m158-merge-popup.png")
                    report(("SHOT " if popup.grab().save(path) else "NOTE could not save ") + path)
                close_popups()

            with_open_menu(merge_button.showMenu, on_popup)
            check(seen.get("popup", False), "the Merge button pops up the shared Merge menu")
            check(seen.get("listed", False) and seen.get("drawn", False),
                  "the Python item shows in the Merge popup")
        item.trigger()
        check(fired == [True], "the Python item's own slot runs when it is triggered")

        gt.section("second window")
        app2 = app.newProject()
        require(app2 is not None, "app.newProject() opened a second window")
        ok = yield ("until", lambda: len(gui_widgets()) >= 2 and all(graph_of(g) is not None for g in gui_widgets()),
                    STARTUP_TIMEOUT_S)
        require(ok, "the second window and its node graph are up")
        yield ("sleep", SETTLE_MS)
        second_gui = next(g for g in gui_widgets() if ptr(g) != ptr(main_gui))
        second_buttons = check_toolbox("window 2", second_gui)
        check([ptr(b.menu()) for b in second_buttons] == [ptr(b.menu()) for b in toolbox_buttons(main_gui)],
              "both windows' toolbox buttons show the same menus")
        second_merge = next((b for b in second_buttons if ptr(b.menu()) == ptr(merge_menu)), None)
        if check(second_merge is not None, "window 2 has a Merge toolbox button"):
            check(ptr(item) in [ptr(a) for a in second_merge.menu().actions()],
                  "window 2's Merge menu shows the Python item added before it opened")
        bar = toolbox_of(second_gui)
        if bar is not None:
            path = os.path.join(gt.OUT, "m158-toolbox-window2.png")
            report(("SHOT " if bar.grab().save(path) else "NOTE could not save ") + path)
        check_graph_menu("window 1", main_gui)
        check_graph_menu("window 2", second_gui)

        blur_leaf = find_leaf(BLUR_ID)
        if blur_leaf is not None:
            before1 = node_names(app)
            before2 = node_names(app2)
            blur_leaf[3].trigger()
            ok = yield ("until", lambda: new_node(app2, before2, BLUR_ID) is not None, POLL_TIMEOUT_S)
            check(ok, "after a right-click in window 2's graph, the menu creates nodes in window 2's project")
            check(new_node(app, before1, BLUR_ID) is None, "...and none in window 1's project")
            node = new_node(app2, before2, BLUR_ID)
            if node is not None:
                node.destroy()
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)

        gt.section("disable")
        target = None
        if merge_leaf is not None and merge_leaf[2] == [MERGE_GROUPING]:
            target = (merge_leaf[0], merge_leaf[1])
        else:
            target = next((k for k, p in sorted(expected.items()) if p == [MERGE_GROUPING]), None)
        require(target is not None, "found a plug-in directly under Merge to disable")
        report("NOTE disabling %s %d" % target)

        QMetaObject.invokeMethod(main_gui, "showSettings")
        ok = yield ("until", lambda: open_prefs() is not None, POLL_TIMEOUT_S)
        prefs = open_prefs()
        require(ok and prefs is not None, "the Preferences window opened")
        yield ("sleep", SETTLE_MS)
        checkbox = plugin_checkbox(prefs, target[0], target[1])
        require(checkbox is not None, "found the plug-in's Enabled checkbox in Preferences")

        press_space(checkbox)
        yield ("sleep", SETTLE_MS)
        check_tree("disabled", expected, absent=(target,))
        merge_menu = top_level_submenu(MERGE_GROUPING)
        check(merge_menu is not None, "disabled: the Merge submenu is still there")
        check_python_item("disabled", merge_menu, item)
        check_toolbox("disabled, window 1", main_gui)
        check_toolbox("disabled, window 2", second_gui)

        press_space(checkbox)
        yield ("sleep", SETTLE_MS)
        check_tree("re-enabled", expected)
        merge_menu = top_level_submenu(MERGE_GROUPING)
        check_python_item("re-enabled", merge_menu, item)

        press_space(checkbox)
        yield ("sleep", SETTLE_MS)
        check_tree("disabled again", expected, absent=(target,))
        QApplication.sendEvent(prefs, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
        ok = yield ("until", lambda: open_prefs() is None, POLL_TIMEOUT_S)
        check(ok, "Escape closes Preferences without saving")
        yield ("sleep", SETTLE_MS)
        check_tree("after cancelling Preferences", expected)
        merge_menu = top_level_submenu(MERGE_GROUPING)
        check_python_item("after cancelling Preferences", merge_menu, item)
        check_toolbox("after cancelling Preferences, window 1", main_gui)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
