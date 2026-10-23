"""Interaction and screenshot checks for the native tab menu (tabtabtab, vendored under
Gui/Resources/TabTabTab with a Natron adapter) that replaces the old NodeCreationDialog.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m45_tab_menu.py

Conventions:
  - the popup is Gui/Resources/TabTabTab's TabTabTabWidget, found through gt.widgets_of(), with a
    QLineEdit (the filter/line-edit) and a QListView (the results) among its children;
  - a QEnterEvent is sent to a NodeGraph viewport before QTest.keyClick(view, Qt.Key_Tab), because
    Xvfb has no real pointer to hover the graph with;
  - events are sent straight to specific widgets with QApplication.sendEvent()/QTest.keyClick()
    rather than relying on real window-manager focus, so a check still drives the right widget
    even when another graph tab (e.g. a Group's) is the one currently on screen;
  - never call parentItem() on a node-graph item from Python: PySide deletes it and Natron
    crashes.

Shots: m45-open-at-cursor.png (the first open, unfiltered), m45-search-blr.png (the open menu
with coloured rows), m45-category-filter.png (a "[" category query), m45-command-pref.png (a
menu-command query), m45-insert-after-read.png (the graph after a Blur is inserted below the
selected Read), m45-python-merge-action.png (the group's graph after a Python action listed under
Merge created its node at the cursor) and m45-preferences-nodegraph.png (the "Tab menu" section of
Preferences > Nodegraph).
"""
import json
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280

READ_ID = "fr.inria.built-in.Read"
BLUR_ID = "net.sf.cimg.CImgBlur"
GROUP_ID = "fr.inria.built-in.Group"
REMAPPED_SEARCH_MODES = "consecutive,anchored_fuzzy,non_anchored_fuzzy"

try:
    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QEvent, QObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QColor, QCursor, QEnterEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import (QApplication, QLabel, QLineEdit, QListView, QMainWindow, QPushButton,
                                   QScrollArea, QTreeWidget)

    import guitest as gt
    from guitest import check, require

    import tabtabtab_natron
    import tabtabtab_nuke_core

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    STORAGE_DIR = os.path.join(gt.OUT, "tabtabtab")
    WEIGHTS_PATH = os.path.join(STORAGE_DIR, "tabtabtab_weights.json")

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create_node(plugin_id, x, y, group=None):
        node = app.createNode(plugin_id, -1, group, {})
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def input_connected_from(node, source):
        """The input index of node connected from source, comparing by script name since Effect
        wrapper objects returned by getInput() are not the same Python object as source."""
        if node is None or source is None:
            return None
        for i in range(node.getMaxInputCount()):
            inp = node.getInput(i)
            if inp is not None and inp.getScriptName() == source.getScriptName():
                return i
        return None

    def menu_actions(title_substr):
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow):
                continue
            mb = win.menuBar()
            if mb is None:
                continue
            for act in mb.actions():
                menu = gt.menu_of(act)
                if menu is not None and title_substr.lower() in act.text().lower():
                    return menu.actions()
        return []

    def nodes_menu_items(menu=None, parts=None):
        """(menupath, action) for every leaf of natron.nodesMenu(), the tab menu's node source."""
        menu = menu if menu is not None else NatronGui.natron.nodesMenu()
        parts = parts or []
        found = []
        if menu is None:
            return found
        for act in menu.actions():
            text = re.sub(r"&(&?)", r"\1", act.text())
            if not text:
                continue
            if gt.menu_of(act) is not None:
                found.extend(nodes_menu_items(gt.menu_of(act), parts + [text]))
            else:
                found.append(("/".join(parts + [text]), act))
        return found

    def nodes_menu_grouping(grouping):
        menu = NatronGui.natron.nodesMenu()
        for act in (menu.actions() if menu is not None else []):
            if act.property("natronGrouping") == grouping and gt.menu_of(act) is not None:
                return gt.menu_of(act)
        return None

    def find_action(menu_title_substr, text_substr):
        for a in menu_actions(menu_title_substr):
            if text_substr.lower() in a.text().lower():
                return a
        return None

    def enter_graph(view):
        centre = QPointF(view.rect().center())
        QApplication.sendEvent(view, QEnterEvent(centre, centre, QPointF(view.mapToGlobal(view.rect().center()))))

    def open_menu(view):
        enter_graph(view)
        QTest.keyClick(view, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        if not ok:
            return None, None, None
        popup = next(d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible())
        return popup, popup.findChildren(QLineEdit)[0], popup.findChildren(QListView)[0]

    def close_menu(popup):
        QTest.keyClick(popup, Qt.Key_Escape)
        QApplication.processEvents()

    def listed(line_edit, view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def row_name(entry):
        return entry.split("[")[0].strip()

    # Installed before the first Tab so the very first open's positioning is recorded too.
    class _PopupEventFilter(QObject):
        def __init__(self):
            super(_PopupEventFilter, self).__init__()
            self.events = []

        def eventFilter(self, obj, event):
            et = event.type()
            if et in (QEvent.Show, QEvent.Move):
                try:
                    if gt.class_name(obj) == "TabTabTabWidget":
                        self.events.append((et, obj.geometry()))
                except RuntimeError:
                    pass
            return False

    def check_position(main_graph, popup_filter, global_point, label):
        QCursor.setPos(global_point)
        popup_filter.events[:] = []
        popup, _line_edit, _view = yield from open_menu(main_graph)
        if not check(popup is not None, "%s: the tab menu opens" % label):
            return None
        shows = [g for t, g in popup_filter.events if t == QEvent.Show]
        seen_show = False
        move_after_show = False
        for t, g in popup_filter.events:
            if t == QEvent.Show:
                seen_show = True
            elif t == QEvent.Move and seen_show:
                move_after_show = True
        check(len(shows) >= 1, "%s: a Show event on the popup was recorded" % label)
        check(not move_after_show, "%s: no Move event follows the first Show" % label)
        if shows:
            check(shows[0].contains(global_point),
                 "%s: the popup's geometry at Show already contains the cursor (%r vs %r)"
                 % (label, shows[0], global_point))
        return popup

    def position_section(main_graph, popup_filter):
        gt.section("position")
        point1 = main_graph.mapToGlobal(main_graph.rect().center())
        popup1 = yield from check_position(main_graph, popup_filter, point1, "position (first open)")
        if popup1 is not None:
            yield ("sleep", 300)
            # Screen grabs under Xvfb do not draw the pointer, so record where it was.
            gt.report("NOTE m45-open-at-cursor.png: cursor at (%d, %d), popup at %r"
                      % (point1.x(), point1.y(), popup1.geometry()))
            gt.shot("m45-open-at-cursor.png")
            close_menu(popup1)

        point2 = main_graph.mapToGlobal(main_graph.rect().center() + QPoint(120, 60))
        popup2 = yield from check_position(main_graph, popup_filter, point2, "position (second open)")
        if popup2 is not None:
            close_menu(popup2)

    def search_section(main_graph):
        gt.section("search")
        popup, line_edit, view = yield from open_menu(main_graph)
        if not check(popup is not None, "search: the tab menu opens"):
            return None

        entries = listed(line_edit, view, "blr")
        check(bool(entries) and row_name(entries[0]) == "Blur",
             "search: 'blr' ranks Blur first (got %r)" % entries[:3])
        check(all(row_name(e)[:1].lower() == "b" for e in entries),
             "search: every 'blr' result's name starts with 'b' (got %r)" % entries)

        row_colour = view.model().index(0, 0).data(Qt.UserRole) if entries else None
        blur_item = next(((path, act) for path, act in nodes_menu_items()
                          if act.property("natronPluginID") == BLUR_ID and path.rsplit("/", 1)[-1] == "Blur"), None)
        blur_colour = blur_item[1].property("natronCategoryColor") if blur_item is not None else None
        if check(bool(blur_colour), "search: found Blur's nodes-menu action with a category colour"):
            want = QColor(blur_colour)
            check(row_colour is not None and row_colour.name() == want.name(),
                 "search: Blur's row colour (Qt.UserRole) matches its category colour (%r vs %r)"
                 % (row_colour.name() if row_colour else None, want.name()))

        gt.shot("m45-search-blr.png")

        entries_1sp = listed(line_edit, view, " blr")
        check(any(row_name(e) == "DirBlur" for e in entries_1sp),
             "search: ' blr' also lists DirBlur (got %r)" % entries_1sp)

        entries_2sp = listed(line_edit, view, "  blr")
        check(not any(row_name(e) in ("Blur", "DirBlur") for e in entries_2sp),
             "search: '  blr' lists neither Blur nor DirBlur (got %r)" % entries_2sp)

        close_menu(popup)
        return blur_item

    def category_section(main_graph):
        gt.section("category")
        popup, line_edit, view = yield from open_menu(main_graph)
        if not check(popup is not None, "category: the tab menu opens"):
            return

        entries = listed(line_edit, view, "gr[col")
        cats = set(e.rpartition("[")[2].split("]")[0] for e in entries)
        check(bool(entries), "category: 'gr[col' lists at least one row (got %r)" % entries)
        check(len(cats) == 1, "category: every 'gr[col' row shares one category (got %r)" % cats)
        if cats:
            check("col" in next(iter(cats)).lower(),
                 "category: the shared category looks like Color (got %r)" % cats)

        gt.shot("m45-category-filter.png")
        close_menu(popup)

    def keys_section(main_graph):
        gt.section("keys")
        before = len(app.getChildren())

        popup, line_edit, view = yield from open_menu(main_graph)
        if not check(popup is not None, "keys: the tab menu opens"):
            return
        # The popup refreshes its rows on the two event-loop ticks after it shows; let that
        # finish so the row count read below is the one Up wrapped to.
        yield ("sleep", 300)
        line_edit.setText("")
        QApplication.processEvents()
        model = view.model()
        check(view.currentIndex().row() == 0, "keys: a fresh filter selects row 0")
        QTest.keyClick(line_edit, Qt.Key_Up)
        QApplication.processEvents()
        check(view.currentIndex().row() == model.rowCount() - 1,
              "keys: Up from row 0 wraps to the last row (row %d of %d)"
              % (view.currentIndex().row(), model.rowCount()))

        QTest.keyClick(popup, Qt.Key_Escape)
        ok = yield ("until", lambda: not popup.isVisible(), 5.0)
        check(ok, "keys: Escape closes the menu")
        check(len(app.getChildren()) == before, "keys: Escape created no node")

        popup, _line_edit, _view = yield from open_menu(main_graph)
        if not check(popup is not None, "keys: the tab menu reopens"):
            return
        # TabTabTabWidget.event() closes on WindowDeactivate whatever caused it, so this one event
        # stands in for both an outside click and activating the main window.
        QApplication.sendEvent(popup, QEvent(QEvent.WindowDeactivate))
        ok = yield ("until", lambda: not popup.isVisible(), 5.0)
        check(ok, "keys: deactivating the popup's window closes it (outside click / main window activation)")
        check(len(app.getChildren()) == before, "keys: closing by deactivation created no node")

    def insert_after_selected_section(main_graph, read_node, global_point):
        gt.section("insert after selected")
        app.setSelection([read_node])
        QApplication.processEvents()
        before = set(n.getScriptName() for n in app.getChildren())

        QCursor.setPos(global_point)
        scene_pos = main_graph.mapToScene(main_graph.mapFromGlobal(global_point))
        popup, line_edit, view = yield from open_menu(main_graph)
        if not check(popup is not None, "insert: the tab menu opens"):
            return None

        entries = listed(line_edit, view, "Blur")
        row = next((r for r, e in enumerate(entries) if row_name(e) == "Blur"), -1)
        if not check(row >= 0, "insert: the menu offers Blur (got %r)" % entries):
            close_menu(popup)
            return None
        view.setCurrentIndex(view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)

        ok = yield ("until", lambda: len(app.getChildren()) > len(before), 5.0)
        if not check(ok, "insert: a new node was created"):
            return None
        new_node = next(n for n in app.getChildren() if n.getScriptName() not in before)
        check(new_node.getPluginID() == BLUR_ID, "insert: the new node is a Blur")
        check(input_connected_from(new_node, read_node) == 0, "insert: the Blur's input 0 is the selected Read")

        # With a node selected, GuiAppInstance::createNodeGui ignores the position hint and places
        # the new node below the selected one.
        nx, ny = new_node.getPosition()
        rx, ry = read_node.getPosition()
        check(ny > ry and abs(nx - rx) <= 150.0,
              "insert: the Blur is placed below the selected Read (Blur %r, Read %r)" % ((nx, ny), (rx, ry)))

        gt.shot("m45-insert-after-read.png")
        return new_node

    def tab_tab_section(main_graph, global_point):
        gt.section("tab-tab")
        # With nothing selected the node goes where the cursor was when the menu opened.
        app.clearSelection()
        QApplication.processEvents()
        QCursor.setPos(global_point)
        scene_pos = main_graph.mapToScene(main_graph.mapFromGlobal(global_point))
        before = set(n.getScriptName() for n in app.getChildren())

        popup, line_edit, _view = yield from open_menu(main_graph)
        if not check(popup is not None, "tab-tab: the tab menu reopens"):
            return None
        # The rows still on show were filtered with the pre-filled text before Blur's weight was
        # counted, so they rank alphabetically; the refresh on the two ticks after the popup
        # shows re-ranks them. A person's second Tab always comes after it.
        yield ("sleep", 300)
        text = line_edit.text()
        check(text.startswith(" ") and row_name(text) == "Blur",
             "tab-tab: reopening pre-fills the last choice (got %r)" % text)
        check(line_edit.selectedText() == text, "tab-tab: the pre-filled text is fully selected")

        QTest.keyClick(line_edit, Qt.Key_Tab)
        ok = yield ("until", lambda: len(app.getChildren()) > len(before), 5.0)
        if not check(ok, "tab-tab: Tab creates a second node"):
            return None
        new_node = next(n for n in app.getChildren() if n.getScriptName() not in before)
        check(new_node.getPluginID() == BLUR_ID, "tab-tab: the second node is also a Blur")
        nx, ny = new_node.getPosition()
        dist = ((nx - scene_pos.x()) ** 2 + (ny - scene_pos.y()) ** 2) ** 0.5
        check(dist <= 150.0,
              "tab-tab: the unconnected Blur is within 150px of the cursor's scene position (got %.1f)" % dist)
        return new_node

    def group_section(main_graph):
        gt.section("group")
        group_node = create_node(GROUP_ID, 900, 900)
        if group_node is None:
            return
        after_group = len(app.getChildren())

        ok = yield ("until", lambda: any(w.objectName() == group_node.getLabel() for w in gt.widgets_of("NodeGraph")),
                   5.0)
        if not check(ok, "group: the group's own node graph exists"):
            return
        subgraph = next(w for w in gt.widgets_of("NodeGraph") if w.objectName() == group_node.getLabel())

        before_children = len(group_node.getChildren())
        popup, line_edit, view = yield from open_menu(subgraph)
        if not check(popup is not None, "group: the tab menu opens over the group's graph"):
            return

        entries = listed(line_edit, view, "Blur")
        row = next((r for r, e in enumerate(entries) if row_name(e) == "Blur"), -1)
        if not check(row >= 0, "group: the menu offers Blur inside the group (got %r)" % entries):
            close_menu(popup)
            return
        view.setCurrentIndex(view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)

        ok = yield ("until", lambda: len(group_node.getChildren()) > before_children, 5.0)
        check(ok, "group: a node was created inside the group")
        check(len(app.getChildren()) == after_group, "group: the inner node did not appear at the root graph")

    def commands_section(main_graph):
        gt.section("commands")
        popup, line_edit, view = yield from open_menu(main_graph)
        if not check(popup is not None, "commands: the tab menu opens"):
            return

        entries = listed(line_edit, view, "pref")
        row = next((r for r, e in enumerate(entries) if row_name(e) == "Preferences...") , -1)
        check(row >= 0, "commands: 'pref' lists the Preferences action under Edit (got %r)" % entries)
        pref_colour = view.model().index(row, 0).data(Qt.UserRole) if row >= 0 else None
        check(pref_colour is None, "commands: a command row has no Qt.UserRole colour")
        gt.shot("m45-command-pref.png")

        if row >= 0:
            view.setCurrentIndex(view.model().index(row, 0))
            QTest.keyClick(line_edit, Qt.Key_Return)
            ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("PreferencesPanel")), POLL_TIMEOUT_S)
            check(ok, "commands: choosing Preferences opens the Preferences panel")
            for w in gt.widgets_of("PreferencesPanel"):
                if w.isVisible():
                    w.close()
            QApplication.processEvents()
        else:
            close_menu(popup)

        redo = find_action("edit", "redo")
        if check(redo is not None and not redo.isEnabled(),
                 "commands: found a disabled Redo action to use as the disabled-leaf case"):
            popup2, line_edit2, view2 = yield from open_menu(main_graph)
            if check(popup2 is not None, "commands: the tab menu reopens"):
                query = redo.text().replace("&", "").strip()
                entries2 = listed(line_edit2, view2, query)
                check(not any(row_name(e) == query for e in entries2),
                     "commands: a disabled leaf (%r) is absent from its own query" % query)
                close_menu(popup2)

        popup3, line_edit3, view3 = yield from open_menu(main_graph)
        if check(popup3 is not None, "commands: the tab menu reopens"):
            found_tab_menu = listed(line_edit3, view3, "tab menu")
            found_tabtabtab = listed(line_edit3, view3, "tabtabtab")
            check(not found_tab_menu and not found_tabtabtab,
                 "commands: nothing named 'Tab Menu' or 'Tabtabtab' is listed (got %r, %r)"
                 % (found_tab_menu, found_tabtabtab))
            close_menu(popup3)

    def weights_section(blur_item):
        gt.section("weights")
        if not check(blur_item is not None, "weights: resolved Blur's nodes-menu action"):
            return
        menupath = blur_item[0]
        ok = yield ("until", lambda: os.path.isfile(WEIGHTS_PATH), 5.0)
        if not check(ok, "weights: tabtabtab_weights.json was written to the scratch folder"):
            return
        with open(WEIGHTS_PATH) as f:
            weights = json.load(f)
        check(weights.get(menupath, 0) > 0,
             "weights: %r has a positive entry (got %r)" % (menupath, weights.get(menupath)))

    def python_merge_action_section(main_graph):
        gt.section("python action")
        merge_menu = nodes_menu_grouping("Merge")
        if not check(merge_menu is not None, "python action: found the nodes menu's Merge submenu"):
            return
        created = []

        def create_blur():
            created.append(app.createNode(BLUR_ID))

        action = merge_menu.addAction("M45 Python Blur")
        action.triggered.connect(create_blur)
        extra_actions = []
        try:
            group_node = create_node(GROUP_ID, 1400, 900)
            if group_node is None:
                return
            ok = yield ("until", lambda: any(w.objectName() == group_node.getLabel()
                                             for w in gt.widgets_of("NodeGraph")), 5.0)
            if not check(ok, "python action: the group's own node graph exists"):
                return
            subgraph = next(w for w in gt.widgets_of("NodeGraph") if w.objectName() == group_node.getLabel())
            app.clearSelection()
            QApplication.processEvents()

            global_point = subgraph.mapToGlobal(subgraph.rect().center() + QPoint(-80, 40))
            QCursor.setPos(global_point)
            # A node's position is in the coordinates of the graph's node layer, which a group's
            # graph offsets from its scene, so the check looks for the node's items just inside
            # its top-left corner, where the cursor was, rather than comparing positions.
            inside_corner = subgraph.mapFromGlobal(global_point) + QPoint(12, 8)
            items_before = len(subgraph.items(inside_corner))
            root_before = len(app.getChildren())

            popup, line_edit, view = yield from open_menu(subgraph)
            if not check(popup is not None, "python action: the tab menu opens over the group's graph"):
                return
            entries = listed(line_edit, view, "M45 Python Blur")
            row = next((r for r, e in enumerate(entries) if row_name(e) == "M45 Python Blur"), -1)
            check(row >= 0 and "[Merge]" in entries[row],
                 "python action: the action is listed under Merge (got %r)" % entries)
            if row < 0:
                close_menu(popup)
                return
            view.setCurrentIndex(view.model().index(row, 0))
            QTest.keyClick(line_edit, Qt.Key_Return)

            ok = yield ("until", lambda: bool(created), 5.0)
            if not check(ok and created[0] is not None, "python action: choosing it ran app.createNode()"):
                return
            node = created[0]
            inside = [n.getScriptName() for n in group_node.getChildren()]
            check(node.getScriptName() in inside, "python action: the node went into the group's graph")
            check(len(app.getChildren()) == root_before, "python action: the root graph got no node")
            items_after = len(subgraph.items(inside_corner))
            check(items_after > items_before,
                 "python action: the node is drawn where the cursor was (%d graph items there, %d before)"
                 % (items_after, items_before))
            gt.shot("m45-python-merge-action.png")

            # Triggered outside the tab menu, the same action gets no context: app.createNode()
            # with no group goes to the project root.
            del created[:]
            action.trigger()
            QApplication.processEvents()
            check(bool(created) and created[0] is not None, "python action: a direct trigger creates a node")
            if created and created[0] is not None:
                check(created[0].getScriptName() in [n.getScriptName() for n in app.getChildren()],
                     "python action: outside the tab menu the node goes to the root graph")
                check(created[0].getScriptName() not in [n.getScriptName() for n in group_node.getChildren()],
                     "python action: outside the tab menu the group gets no node")

            def create_inner():
                nested.append(app.createNode(BLUR_ID))

            def create_around_inner():
                nested.append(app.createNode(BLUR_ID))
                app.triggerTabMenuAction(inner_action)
                nested.append(app.createNode(BLUR_ID))

            nested = []
            inner_action = merge_menu.addAction("M45 Python Inner")
            inner_action.triggered.connect(create_inner)
            extra_actions.append(inner_action)
            outer_action = merge_menu.addAction("M45 Python Nested")
            outer_action.triggered.connect(create_around_inner)
            extra_actions.append(outer_action)
            root_before = len(app.getChildren())

            popup, line_edit, view = yield from open_menu(subgraph)
            if not check(popup is not None, "python action: the tab menu reopens over the group's graph"):
                return
            entries = listed(line_edit, view, "M45 Python Nested")
            row = next((r for r, e in enumerate(entries) if row_name(e) == "M45 Python Nested"), -1)
            if not check(row >= 0, "python action: the nesting action is listed (got %r)" % entries):
                close_menu(popup)
                return
            view.setCurrentIndex(view.model().index(row, 0))
            QTest.keyClick(line_edit, Qt.Key_Return)

            ok = yield ("until", lambda: len(nested) == 3, 5.0)
            if not check(ok and all(n is not None for n in nested),
                         "python action: an action triggered from inside a tab-menu action creates its node, "
                         "and the outer action creates one before and one after it (got %d)" % len(nested)):
                return
            inside = [n.getScriptName() for n in group_node.getChildren()]
            check(all(n.getScriptName() in inside for n in nested),
                 "python action: the nodes created before, inside and after the nested trigger all go into "
                 "the group's graph (%r in %r)" % ([n.getScriptName() for n in nested], inside))
            check(len(app.getChildren()) == root_before,
                 "python action: the nested trigger leaves no node in the root graph")
        finally:
            for extra in [action] + extra_actions:
                merge_menu.removeAction(extra)
                extra.deleteLater()

    def preferences_section(main_graph):
        gt.section("preferences")
        settings = NatronGui.natron.getSettings()
        search_param = settings.getParam("tabMenuSearchModes")
        scroll_param = settings.getParam("tabMenuScroll")
        if not check(search_param is not None and scroll_param is not None,
                     "preferences: found the tab menu preference params"):
            return

        original_search = search_param.getValue()
        original_scroll = scroll_param.getValue()
        try:
            remapped = next((i for i in range(search_param.getNumOptions())
                             if search_param.getOption(i) == REMAPPED_SEARCH_MODES), None)
            if check(remapped is not None, "preferences: found the remapped search-mode option"):
                search_param.setValue(remapped)
                popup, line_edit, view = yield from open_menu(main_graph)
                if check(popup is not None, "preferences: the menu opens after remapping search modes"):
                    entries = listed(line_edit, view, "blr")
                    check(not any(row_name(e) in ("Blur", "DirBlur") for e in entries),
                         "preferences: 'blr' lists neither Blur nor DirBlur under the remapped order "
                         "(got %r)" % entries)
                    entries_1sp = listed(line_edit, view, " blr")
                    check(any(row_name(e) == "Blur" for e in entries_1sp)
                         and not any(row_name(e) == "DirBlur" for e in entries_1sp),
                         "preferences: ' blr' lists Blur only under the remapped order (got %r)" % entries_1sp)
                    close_menu(popup)
                search_param.setValue(original_search)

            scroll_param.setValue(False)
            popup, line_edit, view = yield from open_menu(main_graph)
            if check(popup is not None, "preferences: the menu opens with scrolling off"):
                entries = listed(line_edit, view, "a")
                check(len(entries) <= 18, "preferences: at most 18 rows with scrolling off (got %d)" % len(entries))
                scrollbar = view.verticalScrollBar()
                check(scrollbar is None or not scrollbar.isVisible(),
                     "preferences: no visible scroll bar with scrolling off")
                close_menu(popup)
        finally:
            search_param.setValue(original_search)
            scroll_param.setValue(original_scroll)

    # There is no Python API to rebind a keybind, so the rebinding check drives the real
    # Shortcut Editor page of Preferences the way a person would.
    def _find_tree_item(tree, text):
        # Walking topLevelItem()/child() from Python fails on the Preferences trees: the second
        # item's wrapper already reports its C++ object deleted. findItems() does not.
        items = tree.findItems(text, Qt.MatchExactly | Qt.MatchRecursive, 0)
        return items[0] if items else None

    def _open_preferences():
        action = find_action("edit", "preferences")
        if not check(action is not None, "rebind: found the Preferences action in the Edit menu"):
            return None
        action.trigger()
        QApplication.processEvents()
        panels = [w for w in gt.widgets_of("PreferencesPanel") if w.isVisible()]
        if not check(len(panels) == 1, "rebind: the Preferences panel opened"):
            return None
        return panels[0]

    def _select_create_node_row(panel):
        page_trees = [w for w in panel.findChildren(QTreeWidget) if w.columnCount() == 1]
        if not check(len(page_trees) == 1, "rebind: found the preferences page tree"):
            return None
        page_item = _find_tree_item(page_trees[0], "Shortcut Editor")
        if not check(page_item is not None, "rebind: found the Shortcut Editor page"):
            return None
        page_trees[0].setCurrentItem(page_item)
        QApplication.processEvents()

        shortcut_trees = [w for w in panel.findChildren(QTreeWidget)
                         if w.columnCount() == 3 and w.headerItem() is not None
                         and w.headerItem().text(0) == "Command"]
        if not check(len(shortcut_trees) == 1, "rebind: found the shortcuts tree"):
            return None
        action_item = _find_tree_item(shortcut_trees[0], "Create Node")
        if not check(action_item is not None, "rebind: found the Create Node row"):
            return None
        shortcut_trees[0].setCurrentItem(action_item)
        QApplication.processEvents()
        return True

    def nodegraph_page_section():
        gt.section("preferences page")
        panel = _open_preferences()
        if panel is None:
            return
        page_trees = [w for w in panel.findChildren(QTreeWidget) if w.columnCount() == 1]
        page_item = _find_tree_item(page_trees[0], "Nodegraph") if len(page_trees) == 1 else None
        if check(page_item is not None, "preferences page: found the Nodegraph page"):
            page_trees[0].setCurrentItem(page_item)
            QApplication.processEvents()
            # A separator's label is bold, so its text is wrapped in <b>...</b>.
            headings = [w for w in panel.findChildren(QLabel) if re.sub(r"<[^>]*>", "", w.text()) == "Tab menu"]
            if check(len(headings) == 1, "preferences page: the Nodegraph page has a 'Tab menu' heading"):
                heading = headings[0]
                area = heading.parentWidget()
                while area is not None and not isinstance(area, QScrollArea):
                    area = area.parentWidget()
                if area is not None:
                    area.ensureWidgetVisible(heading, 50, 200)
                yield ("sleep", 300)
                check(heading.isVisible(), "preferences page: the 'Tab menu' heading is shown")
                gt.shot("m45-preferences-nodegraph.png")
        panel.close()
        QApplication.processEvents()

    def rebind_section(main_graph):
        gt.section("shortcut rebind")

        panel = _open_preferences()
        if panel is None:
            return
        if _select_create_node_row(panel) is None:
            panel.close()
            return

        editors = [w for w in panel.findChildren(QLineEdit) if w.placeholderText() == "Type to set shortcut"]
        validate_buttons = [w for w in panel.findChildren(QPushButton) if w.text() == "Validate" and w.isVisible()]
        if not check(len(editors) == 1 and len(validate_buttons) == 1,
                     "rebind: found the shortcut field and its Validate button"):
            panel.close()
            return
        editor, validate_btn = editors[0], validate_buttons[0]

        editor.clear()
        QTest.keyClick(editor, Qt.Key_F2)
        QApplication.processEvents()
        check("F2" in editor.text(), "rebind: the shortcut field records the new key (got %r)" % editor.text())
        validate_btn.click()
        QApplication.processEvents()
        panel.close()
        QApplication.processEvents()

        yield ("sleep", 300)
        check(not any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), "rebind: Tab no longer opens the menu")
        QTest.keyClick(main_graph, Qt.Key_Tab)
        QApplication.processEvents()
        yield ("sleep", 300)
        still_closed = not any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget"))
        check(still_closed, "rebind: Tab is no longer the create-node shortcut")

        enter_graph(main_graph)
        QTest.keyClick(main_graph, Qt.Key_F2)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        check(ok, "rebind: the rebound key (F2) opens the menu")
        for popup in gt.widgets_of("TabTabTabWidget"):
            if popup.isVisible():
                close_menu(popup)

        # Restore via Reset rather than retyping the original key: Reset applies the compiled-in
        # default (Tab) directly, with no risk of mistyping what "original" was.
        panel = _open_preferences()
        if panel is None:
            return
        if _select_create_node_row(panel) is None:
            panel.close()
            return
        reset_buttons = [w for w in panel.findChildren(QPushButton) if w.text() == "Reset" and w.isVisible()]
        if check(len(reset_buttons) == 1, "rebind: found the Reset button"):
            reset_buttons[0].click()
            QApplication.processEvents()
        panel.close()
        QApplication.processEvents()

        enter_graph(main_graph)
        QTest.keyClick(main_graph, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        check(ok, "rebind: Tab opens the menu again after Reset")
        for popup in gt.widgets_of("TabTabTabWidget"):
            if popup.isVisible():
                close_menu(popup)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        main_graph = root_graph()
        require(main_graph is not None, "found the node graph view")
        main_graph.setFocus()
        QApplication.processEvents()

        tabtabtab_natron.set_storage_dir(STORAGE_DIR)

        popup_filter = _PopupEventFilter()
        QApplication.instance().installEventFilter(popup_filter)

        yield from position_section(main_graph, popup_filter)
        blur_item = yield from search_section(main_graph)
        yield from category_section(main_graph)
        yield from keys_section(main_graph)

        read_node = app.createReader(os.path.join(gt.FIXTURES, "flat-rgba-only.exr"))
        require(read_node is not None, "create the Read used for the insert-after-selected check")
        require(read_node.getPluginID() == READ_ID, "the created reader is the native Read")
        read_node.setPosition(0, 0)

        cursor_point = main_graph.mapToGlobal(main_graph.rect().center())
        yield from insert_after_selected_section(main_graph, read_node, cursor_point)
        yield from tab_tab_section(main_graph, cursor_point)
        yield from group_section(main_graph)
        yield from commands_section(main_graph)
        yield from weights_section(blur_item)
        yield from python_merge_action_section(main_graph)
        yield from preferences_section(main_graph)
        yield from nodegraph_page_section()
        yield from rebind_section(main_graph)

        gt.section("preferences files")
        leftover_prefs = [os.path.join(d, "tabtabtab_prefs.json")
                         for d in (STORAGE_DIR, os.path.expanduser("~/.Natron"))
                         if os.path.isfile(os.path.join(d, "tabtabtab_prefs.json"))]
        check(not leftover_prefs, "no tabtabtab_prefs.json is ever created (found %r)" % leftover_prefs)

        menu_texts = []
        win = main_window()
        if win is not None and win.menuBar() is not None:
            for item in tabtabtab_nuke_core.find_qt_menu_items(win.menuBar()):
                menu_texts.append(item['menuobj'].text())
        check(not any("tab menu" in t.lower() or "tabtabtab" in t.lower() for t in menu_texts),
             "no Tab-menu preferences entry appears in the menus (got %r)" % menu_texts)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
