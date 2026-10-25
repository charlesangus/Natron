"""Checks for the process-wide nodes menu (NatronGui.natron.nodesMenu()) that every window's left
toolbox and node-graph right-click menu show.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m158-shots/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m158_nodes_menu.py build/release/App/Natron

Sections:
  - api: nodesMenu() returns the QMenu named NodesMenu, the same object on every call;
  - tree: the Preferences plug-in list, which shows every registered plug-in version except
    internal-only ones with its grouping and its Enabled checkbox state, is read as the registry;
    every enabled version in it except KNOWN_DEPRECATED (the list does not show deprecation) has
    an action and no other version has one; every action names an ID natron.getPluginIDs()
    returns and sits in a submenu named like the deepest grouping the list shows for it (the list
    merges same-named groups); no plug-in version has two actions, and every action carries the
    ID, major, category colour and shortcut properties; of the test PyPlug's two majors (from
    m158_nodes_menu.natronpath/), only the older one's label carries its major; the startup tree
    is the snapshot the later sections compare against; top-level groupings keep the toolbox
    order; triggering an action creates the node;
  - python item: a QAction added under Merge/ shows in the main window's Merge toolbox popup;
  - second window: a window opened afterwards has the same toolbox buttons over the same menus,
    shows the Python item, and both windows' graph right-click menus hold every top-level entry
    of the nodes menu; Blur chosen with key presses in the second window's right-click menu lands
    in that window's project; once that menu has closed, a trigger with the first window active
    lands in the first window's; with the second window active, Blur chosen from the first
    window's menu, opened as soon as the second's is dismissed, lands in the first window's;
  - disable: disabling a Merge plug-in in Preferences removes only its action, keeps the Python
    item, and re-enabling it (or cancelling Preferences) brings the action back; disabling the
    test PyPlug's major 2 leaves major 1's label as it was.

Plug-ins are enabled and disabled through the Preferences plug-in list's "Enabled" checkboxes,
which are AnimatedCheckBox frames toggled by a Space key press, and Preferences is cancelled at the
end, so the user's saved plug-in settings are never written.

Shots: m158-toolbox-window1.png, m158-toolbox-window2.png, m158-merge-popup.png.
"""
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
MERGE_ID = "Merge"
BLUR_ID = "net.sf.cimg.CImgBlur"
PYTHON_ITEM_TEXT = "M158 Python item"
TWO_MAJORS_ID = "fr.natron.m158.TwoMajors"
TWO_MAJORS_LABEL = "M158 Two Majors"
KNOWN_GROUPINGS = {MERGE_ID: [MERGE_GROUPING], BLUR_ID: ["Filter"], TWO_MAJORS_ID: ["Other"]}
# The Preferences list does not show which versions are deprecated. These are, in the bundled
# openfx-misc, openfx-io and openfx-arena (each calls setIsDeprecated(true) when describing them).
KNOWN_DEPRECATED = {
    ("fr.inria.openfx.OIIOResize", 1), ("fr.inria.openfx.OIIOText", 1), ("net.fxarena.openfx.MagickText", 5),
    ("net.sf.cimg.CImgBloom", 3), ("net.sf.cimg.CImgChromaBlur", 3), ("net.sf.cimg.CImgLaplacian", 3),
    ("net.sf.openfx.HueCorrect1", 1),
    ("net.sf.openfx.TrackerPM", 1), ("net.sf.openfx.joinViewsPlugin", 1), ("net.sf.openfx.oneViewPlugin", 2),
}

# Columns of the Preferences plug-in list (PreferencesPanel.cpp COL_*).
PREFS_COL_ID = 1
PREFS_COL_VERSION = 2
PREFS_COL_ENABLED = 3

try:
    import NatronGui
    import shiboken6
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, QTimer
    from PySide6.QtGui import QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
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

    def walk(menu, path, leaves):
        """Appends (plugin ID, major, grouping path, action) for every plug-in-owned leaf."""
        for a in menu.actions():
            if not owned(a):
                continue
            sub = gt.menu_of(a)
            if sub is not None:
                walk(sub, path + [a.property(PROP_GROUPING)], leaves)
            else:
                leaves.append((a.property(PROP_ID), int(a.property(PROP_MAJOR)), path, a))
        return leaves

    def expected_items(listed, enabled):
        """(ID, major) -> grouping path of every plug-in action at startup, once each has been
        checked against the plug-in registry: natron.getPluginIDs() and listed, the Preferences
        plug-in list, whose Enabled checkbox states are in enabled."""
        registered = set(NatronGui.natron.getPluginIDs())
        leaves = walk(nodes_menu(), [], [])
        expected = {}
        for pid, major, path, _ in leaves:
            expected.setdefault((pid, major), path)
        unknown = sorted(set(pid for pid, _, _, _ in leaves if pid not in registered))
        check(not unknown, "every plug-in action names a registered plug-in (%r)" % unknown[:10])
        unlisted = sorted(k for k in expected if k not in listed)
        check(not unlisted, "every plug-in action names a plug-in version the Preferences list shows, "
              "so none is internal-only or of a major that does not exist (%r)" % unlisted[:10])
        # The Preferences list files a plug-in under any existing group named like its deepest
        # grouping, so "Extra/Filter" can show as "Filter": only that deepest name is reliable.
        misplaced = sorted((k, p, listed[k] or [DEFAULT_GROUPING]) for k, p in expected.items()
                           if k in listed and p[-1:] != (listed[k] or [DEFAULT_GROUPING])[-1:])
        check(not misplaced, "every plug-in action sits in a submenu named like the deepest grouping the "
              "Preferences list shows for its plug-in (%r)" % misplaced[:5])
        for pid in sorted(KNOWN_GROUPINGS):
            majors = [m for k, m in listed if k == pid]
            newest = (pid, max(majors)) if majors else None
            check(newest in expected, "%s's newest listed major has an action (listed majors %r)" % (pid, majors))
        no_action = sorted(k for k in listed if k not in expected)
        report("NOTE %d plug-in IDs registered, %d plug-in versions in the Preferences list, %d with an "
               "action, %d without one (%r)"
               % (len(registered), len(listed), len(expected), len(no_action), no_action))
        unreadable = sorted(k for k, on in enabled.items() if on is None)
        require(not unreadable, "every Enabled checkbox in the Preferences list exposes its 'checked' state",
                "%d do not, e.g. %r" % (len(unreadable), unreadable[:5]))
        offered = set(k for k in listed if enabled.get(k) and k not in KNOWN_DEPRECATED)
        missing = sorted(k for k in offered if k not in expected)
        check(not missing, "every enabled plug-in version in the Preferences list, except the known "
              "deprecated ones, has an action (missing %d: %r)" % (len(missing), missing[:10]))
        unexpected = sorted(k for k in expected if k not in offered)
        check(not unexpected, "no disabled or known deprecated plug-in version has an action (%r)" % unexpected[:10])
        for pid, grouping in sorted(KNOWN_GROUPINGS.items()):
            paths = [p for (k, _), p in expected.items() if k == pid]
            check(grouping in paths, "%s has an action under %r (got %r)" % (pid, grouping, paths))
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
        check(not missing, "%s: every plug-in version with an action at startup still has one (missing %d: %r)"
              % (area, len(missing), missing[:10]))
        check(not doubled, "%s: no plug-in has two actions (%r)" % (area, doubled[:10]))
        check(not extra, "%s: no action for a plug-in version that had none at startup (%r)" % (area, extra[:10]))
        check(not misplaced, "%s: each action sits under its plug-in's grouping path (%r)" % (area, misplaced[:5]))
        if absent:
            check(not still_there, "%s: the disabled plug-in has no action (%r)" % (area, still_there))
        return leaves

    def top_level_submenu(grouping):
        for a in nodes_menu().actions():
            if owned(a) and a.property(PROP_GROUPING) == grouping and gt.menu_of(a) is not None:
                return gt.menu_of(a)
        return None

    def find_leaf(pid, major=None):
        for leaf in walk(nodes_menu(), [], []):
            if leaf[0] == pid and (major is None or leaf[1] == major):
                return leaf
        return None

    def gui_widgets():
        return [w for w in gt.widgets_of("Gui") if w.isVisible()]

    def toolbox_of(gui):
        # AutoHideToolBar has no Q_OBJECT, so its class name reads as QToolBar; it is the
        # window's only toolbar.
        bars = gui.findChildren(QToolBar)
        return bars[0] if bars else None

    def toolbox_buttons(gui):
        bar = toolbox_of(gui)
        if bar is None:
            return []
        buttons = []
        for a in bar.actions():
            w = bar.widgetForAction(a)
            if w is not None and hasattr(w, "menu") and gt.menu_of(w) is not None:
                buttons.append(w)
        return buttons

    def graph_of(gui):
        for g in gui.findChildren(QGraphicsView):
            if gt.class_name(g) == "NodeGraph" and g.isVisible():
                return g
        return None

    def owned_top_level_menus():
        return [gt.menu_of(a) for a in nodes_menu().actions() if owned(a) and gt.menu_of(a) is not None]

    def check_toolbox(area, gui):
        buttons = toolbox_buttons(gui)
        menus = owned_top_level_menus()
        check([ptr(gt.menu_of(b)) for b in buttons] == [ptr(m) for m in menus],
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

    def choose_with_keys(menu, leaf):
        """From an open menu holding the nodes menu's top-level entries, opens the submenus down to
        leaf's action with Right key presses and chooses it with Return, so the choice goes through
        QMenu's own hide-then-trigger sequence. Returns whether Return reached the action."""
        current = menu
        for grouping in leaf[2]:
            entry = next((a for a in (current.actions() if current is not None else [])
                          if owned(a) and a.property(PROP_GROUPING) == grouping and gt.menu_of(a) is not None), None)
            if entry is None:
                return False
            current.setActiveAction(entry)
            QTest.keyClick(current, Qt.Key_Right)
            current = gt.menu_of(entry)
            if not current.isVisible():
                return False
        if ptr(leaf[3]) not in [ptr(a) for a in current.actions()]:
            return False
        current.setActiveAction(leaf[3])
        QTest.keyClick(current, Qt.Key_Return)
        return True

    def activate(gui):
        gui.raise_()
        gui.activateWindow()
        ok = yield ("until", lambda: QApplication.activeWindow() is not None
                    and ptr(QApplication.activeWindow()) == ptr(gui), POLL_TIMEOUT_S)
        return ok

    def drop_new(target_app, before, plugin_id):
        node = new_node(target_app, before, plugin_id)
        if node is not None:
            node.destroy()
        gt.close_all_panels()

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

    def open_prefs():
        for w in gt.widgets_of("PreferencesPanel"):
            if w.isVisible():
                return w
        return None

    def plugin_list(prefs):
        for tree in prefs.findChildren(QTreeWidget):
            header = tree.headerItem()
            if header.text(PREFS_COL_ID) == "ID" and header.text(PREFS_COL_VERSION) == "Version":
                return tree
        return None

    def listed_plugin_versions(prefs):
        """(ID, major) -> grouping path of every row of the Preferences plug-in list."""
        tree = plugin_list(prefs)
        found = {}

        def visit(item, path):
            pid = item.text(PREFS_COL_ID)
            if pid:
                found.setdefault((pid, int(item.text(PREFS_COL_VERSION).split(".")[0])), path)
                return
            for i in range(item.childCount()):
                visit(item.child(i), path + [item.text(0)])

        if tree is not None:
            for i in range(tree.topLevelItemCount()):
                visit(tree.topLevelItem(i), [])
        return found

    def enabled_states(prefs):
        """(ID, major) -> the row's Enabled checkbox state, or None where it cannot be read."""
        tree = plugin_list(prefs)
        states = {}
        if tree is None:
            return states
        items = tree.findItems("*", Qt.MatchFlag.MatchWildcard | Qt.MatchFlag.MatchRecursive, PREFS_COL_ID)
        for item in items:
            pid = item.text(PREFS_COL_ID)
            if not pid:
                continue
            box = tree.itemWidget(item, PREFS_COL_ENABLED)
            state = box.property("checked") if box is not None else None
            states[(pid, int(item.text(PREFS_COL_VERSION).split(".")[0]))] = state if isinstance(state, bool) else None
        return states

    def read_plugin_list(gui):
        QMetaObject.invokeMethod(gui, "showSettings")
        ok = yield ("until", lambda: open_prefs() is not None, POLL_TIMEOUT_S)
        prefs = open_prefs()
        require(ok and prefs is not None, "the Preferences window opened")
        yield ("sleep", SETTLE_MS)
        listed = listed_plugin_versions(prefs)
        enabled = enabled_states(prefs)
        QApplication.sendEvent(prefs, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
        ok = yield ("until", lambda: open_prefs() is None, POLL_TIMEOUT_S)
        require(ok, "Escape closes Preferences")
        yield ("sleep", SETTLE_MS)
        return listed, enabled

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
        listed, enabled = yield from read_plugin_list(main_gui)
        require(len(listed) > 0, "the Preferences plug-in list has rows")
        expected = expected_items(listed, enabled)
        require(len(expected) > 0, "the plug-in list has user-creatable plug-ins")
        leaves = check_tree("tree", expected)
        several_majors = sorted(set(pid for pid, _ in listed if sum(1 for k, _ in listed if k == pid) > 1))
        report("NOTE plug-ins listed with more than one major: %r" % several_majors)
        labels = dict((major, a.text()) for pid, major, _, a in leaves if pid == TWO_MAJORS_ID)
        if check(sorted(labels) == [1, 2], "the test PyPlug has an action for each of its majors (got %r)"
                 % sorted(labels)):
            check(labels[2] == TWO_MAJORS_LABEL, "the newest major keeps the plain label (got %r)" % labels[2])
            check(labels[1] == TWO_MAJORS_LABEL + " 1", "the older major's label carries its major (got %r)"
                  % labels[1])
        colours_ok = all(str(a.property(PROP_COLOR) or "").startswith("#") for _, _, _, a in leaves)
        check(colours_ok, "every plug-in action carries a category colour name")
        check(all(isinstance(a.property(PROP_SHORTCUT), str) for _, _, _, a in leaves),
              "every plug-in action carries its shortcut text")
        merge_leaf = find_leaf(MERGE_ID)
        if check(merge_leaf is not None, "Merge has an action"):
            check(bool(merge_leaf[3].property(PROP_SHORTCUT)),
                  "Merge's action carries its default shortcut (got %r)" % merge_leaf[3].property(PROP_SHORTCUT))
        top = [a.property(PROP_GROUPING) for a in menu.actions() if owned(a) and gt.menu_of(a) is not None]
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
        merge_button = next((b for b in main_buttons if ptr(gt.menu_of(b)) == ptr(merge_menu)), None)
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
        check([ptr(gt.menu_of(b)) for b in second_buttons] == [ptr(gt.menu_of(b)) for b in toolbox_buttons(main_gui)],
              "both windows' toolbox buttons show the same menus")
        second_merge = next((b for b in second_buttons if ptr(gt.menu_of(b)) == ptr(merge_menu)), None)
        if check(second_merge is not None, "window 2 has a Merge toolbox button"):
            check(ptr(item) in [ptr(a) for a in gt.menu_of(second_merge).actions()],
                  "window 2's Merge menu shows the Python item added before it opened")
        bar = toolbox_of(second_gui)
        if bar is not None:
            path = os.path.join(gt.OUT, "m158-toolbox-window2.png")
            report(("SHOT " if bar.grab().save(path) else "NOTE could not save ") + path)
        check_graph_menu("window 1", main_gui)
        check_graph_menu("window 2", second_gui)

        blur_leaf = find_leaf(BLUR_ID)
        first_graph = graph_of(main_gui)
        second_graph = graph_of(second_gui)
        if blur_leaf is not None and first_graph is not None and second_graph is not None:
            chosen = []

            def choose_blur(menu):
                chosen.append(choose_with_keys(menu, blur_leaf))
                close_popups()

            before1, before2 = node_names(app), node_names(app2)
            right_click(second_graph.viewport(), choose_blur)
            check(chosen == [True], "Blur is chosen from window 2's graph menu with Right and Return key presses")
            ok = yield ("until", lambda: new_node(app2, before2, BLUR_ID) is not None, POLL_TIMEOUT_S)
            check(ok, "choosing Blur in window 2's graph menu creates it in window 2's project")
            check(new_node(app, before1, BLUR_ID) is None, "...and none in window 1's project")
            drop_new(app2, before2, BLUR_ID)
            yield ("sleep", SETTLE_MS)

            ok = yield from activate(main_gui)
            if not ok:
                report("SKIP second window: window 1 could not be made the active window, so a trigger "
                       "after window 2's graph menu closed is not checked")
            else:
                before1, before2 = node_names(app), node_names(app2)
                blur_leaf[3].trigger()
                ok = yield ("until", lambda: new_node(app, before1, BLUR_ID) is not None, POLL_TIMEOUT_S)
                check(ok, "once window 2's graph menu has closed, the menu creates nodes in the active "
                      "window 1's project")
                check(new_node(app2, before2, BLUR_ID) is None, "...and none in window 2's project")
                drop_new(app, before1, BLUR_ID)
                yield ("sleep", SETTLE_MS)

            ok = yield from activate(second_gui)
            if not ok:
                report("SKIP second window: window 2 could not be made the active window, so a popup opened "
                       "while a dismissed one's target is still being cleared is not checked")
            else:
                del chosen[:]
                before1, before2 = node_names(app), node_names(app2)

                def dismiss_then_open_first(menu):
                    if menu is not None:
                        QTest.keyClick(menu, Qt.Key_Escape)
                    # Window 1's menu opens before the event loop can run window 2's deferred clear.
                    right_click(first_graph.viewport(), choose_blur)

                right_click(second_graph.viewport(), dismiss_then_open_first)
                check(chosen == [True], "Blur is chosen from window 1's graph menu, opened as window 2's closed")
                ok = yield ("until", lambda: new_node(app, before1, BLUR_ID) is not None, POLL_TIMEOUT_S)
                check(ok, "with window 2 active, a node chosen from window 1's graph menu, opened straight after "
                      "window 2's was dismissed, goes into window 1's project")
                check(new_node(app2, before2, BLUR_ID) is None, "...and none into window 2's project")
                drop_new(app, before1, BLUR_ID)
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

        newest_two = plugin_checkbox(prefs, TWO_MAJORS_ID, 2)
        if check(newest_two is not None, "found the test PyPlug's major 2 Enabled checkbox"):
            press_space(newest_two)
            yield ("sleep", SETTLE_MS)
            labels = dict((major, a.text()) for pid, major, _, a in walk(nodes_menu(), [], []) if pid == TWO_MAJORS_ID)
            check(sorted(labels) == [1], "with major 2 disabled, only the test PyPlug's major 1 has an action (got %r)"
                  % sorted(labels))
            check(labels.get(1) == TWO_MAJORS_LABEL + " 1",
                  "with major 2 disabled, major 1 keeps the label carrying its major (got %r)" % labels.get(1))
            press_space(newest_two)
            yield ("sleep", SETTLE_MS)
            labels = dict((major, a.text()) for pid, major, _, a in walk(nodes_menu(), [], []) if pid == TWO_MAJORS_ID)
            check(labels.get(2) == TWO_MAJORS_LABEL, "re-enabling major 2 brings back its plain label (got %r)"
                  % labels.get(2))

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
