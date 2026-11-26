"""The Scene Graph panel's expand commands: Expand All Below, Expand To <kind> and Collapse All Below,
from the row menu, Shift-click on a branch arrow, *, the toolbar's Expand and Collapse buttons and a
key bound in the Shortcut Editor.

Opens Tests/fixtures/sceneKindPayloads/world.usda (its island assembly holds payloads of components,
the bush of the custom kind natronTestTree that scene_graph_expand.pxrplugins registers, each holding
a subcomponent payload) and a wide stage written at run time to $NATRON_GUI_TEST_OUT (100 groups of
50 rows, each row holding a cube, and 5 unloaded payloads), and checks:
  - the toolbar row, with Expand and Collapse after Reset to Graph, fits the docked panel; both are
    disabled with no row selected;
  - Expand To Component on the island, its components loaded, opens the island and the grove and
    leaves every component row shown and closed;
  - Expand To natronTestTree shows the bush closed and leaves the grove, where nothing matches, closed;
  - Expand All Below opens every row under the island and leaves the unloaded subcomponent payloads
    closed and unloaded, and says so;
  - Collapse All Below closes every row under the island;
  - * on the current row, Shift-click on its arrow, the toolbar buttons in Below and To Component
    scope, and Scene Graph: Expand All Below bound to K do the same and load nothing; Shift-click on
    an open row's arrow collapses all below it;
  - on the wide stage Expand All Below opens all 5,101 rows with children and no payload, and a timer
    posted as it starts fires while it is still running;
  - Esc stops a long expansion, leaving the rows it opened open, and the status line says Stopped;
  - switching the viewer to another node stops an expansion under way;
  - a plain click on an unloaded row's arrow still opens and loads it.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
WIDE_TIMEOUT_S = 240.0
SETTLE_MS = 400
WORLD = "/World"
ISLAND = "/World/Island"
GROVE = ISLAND + "/Grove"
TREE_A = GROVE + "/TreeA"
TREE_B = GROVE + "/TreeB"
ROCK = ISLAND + "/Rock"
BUSH = ISLAND + "/Bush"
LEAVES_A = TREE_A + "/Leaves"
LEAVES_B = TREE_B + "/Leaves"
MOSS = ROCK + "/Moss"
BERRIES = BUSH + "/Berries"
COMPONENTS = [TREE_A, TREE_B, ROCK, BUSH]
SUBCOMPONENTS = [LEAVES_A, LEAVES_B, MOSS, BERRIES]
WIDE = "/Wide"
WIDE_GROUPS = 100
WIDE_ROWS = 50
WIDE_PAYLOADS = 5
WIDE_OPENED = 1 + WIDE_GROUPS + WIDE_GROUPS * WIDE_ROWS

STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)

try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, QPoint, QRect, Qt, QTimer
    from PySide6.QtGui import QAction
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import (QApplication, QComboBox, QLabel, QLineEdit, QMainWindow, QMenu, QPushButton, QToolButton, QTreeView,
                                   QTreeWidget, QWidget)

    from guitest import FIXTURES, OUT, check, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3

    scene_path = os.path.join(FIXTURES, "sceneKindPayloads", "world.usda")
    require(os.path.isfile(scene_path), "the fixture %s exists" % scene_path)
    bush_path = os.path.join(FIXTURES, "sceneKindPayloads", "bush.usda")

    def write_wide(path):
        """A stage of WIDE_GROUPS groups of WIDE_ROWS rows, each row holding a cube, and WIDE_PAYLOADS
        unloaded payloads; written at run time so that no large fixture is committed."""
        lines = ["#usda 1.0", "(", '    defaultPrim = "Wide"', ")", "", 'def Xform "Wide"', "{"]
        for g in range(WIDE_GROUPS):
            lines.append('    def Xform "G%03d"' % g)
            lines.append("    {")
            if g < WIDE_PAYLOADS:
                lines.append('        def Xform "P" (')
                lines.append("            prepend payload = @%s@" % bush_path)
                lines.append("        )")
                lines.append("        {")
                lines.append("        }")
            for r in range(WIDE_ROWS):
                lines.append('        def Xform "I%02d"' % r)
                lines.append("        {")
                lines.append('            def Cube "Leaf"')
                lines.append("            {")
                lines.append("                double size = 0.01")
                lines.append("            }")
                lines.append("        }")
            lines.append("    }")
        lines.append("}")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")

    os.makedirs(OUT, exist_ok=True)
    wide_path = os.path.join(OUT, "scene-graph-expand-wide.usda")
    write_wide(wide_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    wide = app.createNode("USDRead")
    require(wide is not None, "create a second USDRead")
    wide.getParam("initialDraw").setValue(0)
    wide.getParam("filename").setValue(wide_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def child(kind, name):
        p = panel()
        return hold(p.findChild(kind, name)) if p is not None else None

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def model():
        t = tree()
        return hold(t.model()) if t is not None else None

    def scope_combo():
        return child(QComboBox, "sceneGraphBulkScope")

    def expand_button():
        return child(QToolButton, "sceneGraphExpand")

    def collapse_button():
        return child(QToolButton, "sceneGraphCollapse")

    def status():
        label = child(QLabel, "sceneGraphBulkStatus")
        return str(label.text()) if label is not None and label.isVisible() else ""

    def expanding():
        return bool(panel().property("natronSceneGraphExpanding"))

    def slices():
        return int(panel().property("natronSceneGraphExpandSlices") or 0)

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        while m.canFetchMore(parent):
            m.fetchMore(parent)
        return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def find(path):
        """The index of path's row, reading the tree down to it; an invalid index when absent."""
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if path_of(index) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def states(paths):
        return [state(p) for p in paths]

    def is_open(path):
        index = find(path)
        return index.isValid() and tree().isExpanded(index)

    def opened(paths):
        return [p for p in paths if is_open(p)]

    def shown(path):
        """Whether path's row is in the tree's laid-out rows: every ancestor is open."""
        parts = path.strip("/").split("/")
        return all(is_open("/" + "/".join(parts[:d])) for d in range(1, len(parts)))

    def all_loaded(paths):
        return lambda: all(state(p) == STATE_LOADED for p in paths)

    def select_rows(paths):
        t = tree()
        t.selectionModel().clearSelection()
        for path in paths:
            index = find(path)
            t.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            t.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def set_scope(scope):
        combo = scope_combo()
        index = combo.findData(scope)
        check(index >= 0, "the scope combo offers %s" % scope)
        combo.setCurrentIndex(index)

    def focus_tree():
        panel().activateWindow()
        QApplication.processEvents()
        tree().setFocus()
        QApplication.processEvents()

    def pick_in_menu(path, action_name):
        """Opens the row menu on path and triggers the action named action_name, wherever it is in it."""
        found = []

        def trigger():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            for action in popup.findChildren(QAction):
                if action.objectName() == action_name:
                    found.append(action.isEnabled())
                    if action.isEnabled():
                        action.trigger()
                    break
            popup.close()

        select_rows([path])
        t = tree()
        t.scrollTo(find(path))
        QTimer.singleShot(300, trigger)
        t.customContextMenuRequested.emit(t.visualRect(find(path)).center())
        QApplication.processEvents()
        return found == [True]

    def menu_texts(path):
        """{objectName: text} of the row menu on path's actions."""
        texts = {}

        def read():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            for action in popup.findChildren(QAction):
                if action.objectName():
                    texts[str(action.objectName())] = str(action.text())
            popup.close()

        select_rows([path])
        t = tree()
        QTimer.singleShot(300, read)
        t.customContextMenuRequested.emit(t.visualRect(find(path)).center())
        QApplication.processEvents()
        return texts

    def shoot_menu(path, action_name, name):
        """Opens the row menu on path down to the action named action_name and shoots it, triggering nothing."""
        found = []

        def close_popups():
            for _ in range(8):
                popup = QApplication.activePopupWidget()
                if popup is None:
                    return
                popup.close()

        def take():
            shot(name)
            close_popups()

        def expand():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            target = None
            for action in popup.findChildren(QAction):
                if action.objectName() == action_name:
                    target = action
                    break
            if target is None:
                close_popups()
                return
            menus = []
            holder = target.parent()
            while isinstance(holder, QMenu) and holder is not popup:
                menus.append(holder)
                holder = holder.parent()
            above = popup
            for menu in reversed(menus):
                above.setActiveAction(menu.menuAction())
                above = menu
            above.setActiveAction(target)
            found.append(True)
            QTimer.singleShot(SETTLE_MS, take)

        select_rows([path])
        t = tree()
        QTimer.singleShot(300, expand)
        t.customContextMenuRequested.emit(t.visualRect(find(path)).center())
        QApplication.processEvents()
        return found == [True]

    def arrow_point(path):
        """The centre of path's branch arrow in the tree's viewport."""
        t = tree()
        index = find(path)
        t.scrollTo(index)
        QApplication.processEvents()
        r = t.visualRect(index)
        return QPoint(r.left() - t.indentation() // 2, r.center().y())

    def shift_click_arrow(path):
        focus_tree()
        QTest.mouseClick(tree().viewport(), Qt.LeftButton, Qt.ShiftModifier, arrow_point(path))

    def finished(timeout=POLL_TIMEOUT_S):
        """Waits for the expander to finish; returns the status line it leaves."""
        ok = yield ("until", lambda: not expanding(), timeout)
        check(ok, "the expand command finishes within %ds (status %r)" % (timeout, status()))
        return status()

    def panel_rect(widget):
        return QRect(widget.mapTo(panel(), QPoint(0, 0)), widget.size())

    def check_row(label, names):
        """Checks that the row of the panel's children named names, in that order left to right,
        shows each whole inside the panel, and shoots it."""
        widgets = [child(QWidget, n) for n in names]
        missing = [n for n, w in zip(names, widgets) if w is None]
        require(not missing, "the %s row has %r (missing %r)" % (label, names, missing))
        width = panel().width()
        rects = [panel_rect(w) for w in widgets]
        hidden = [n for n, w in zip(names, widgets) if not w.isVisible()]
        out = [n for n, r in zip(names, rects) if r.left() < 0 or r.right() >= width]
        squeezed = [n for n, w in zip(names, widgets) if w.width() < w.minimumSizeHint().width()]
        ordered = all(rects[i].right() < rects[i + 1].left() for i in range(len(rects) - 1))
        one_line = all(r.top() < rects[0].bottom() and r.bottom() > rects[0].top() for r in rects)
        report("NOTE %s row: panel %d px wide, row from %d to %d" % (label, width, rects[0].left(), rects[-1].right()))
        check(not hidden and not out and not squeezed and ordered and one_line,
              "the %s row fits the panel on one line in order (hidden %r, past the edge %r, squeezed %r, ordered %r, one line %r)"
              % (label, hidden, out, squeezed, ordered, one_line))
        top = min(r.top() for r in rects)
        bottom = max(r.bottom() for r in rects)
        path = os.path.join(OUT, "scene-graph-expand-row-%s.png" % label)
        if panel().grab(QRect(0, top, width, bottom - top + 1)).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def view_input(node):
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def find_item(tree_widget, text, parent_text=None):
        # Model indexes, not QTreeWidgetItem wrappers: the Preferences trees' item wrappers
        # report their C++ object deleted by the time they are used (see m45_tab_menu.py).
        m = tree_widget.model()
        for idx in m.match(m.index(0, 0), Qt.DisplayRole, text, -1, Qt.MatchExactly | Qt.MatchRecursive):
            parent = idx.parent()
            if parent_text is None or (parent.isValid() and parent.data() == parent_text):
                return idx
        return None

    def open_preferences():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is None or "edit" not in act.text().lower():
                    continue
                for a in menu.actions():
                    if "preferences" in a.text().lower():
                        a.trigger()
                        QApplication.processEvents()
                        panels = [w for w in widgets_of("PreferencesPanel") if w.isVisible()]
                        return panels[0] if len(panels) == 1 else None
        return None

    def rebind(label, key, key_name):
        """Binds the 3DViewer group's command label to key in the Shortcut Editor."""
        prefs = open_preferences()
        require(prefs is not None, "the Preferences panel opens")
        page_trees = [w for w in prefs.findChildren(QTreeWidget) if w.columnCount() == 1]
        page = find_item(page_trees[0], "Shortcut Editor") if len(page_trees) == 1 else None
        require(page is not None, "found the Shortcut Editor page")
        page_trees[0].setCurrentIndex(page)
        QApplication.processEvents()
        shortcut_trees = [w for w in prefs.findChildren(QTreeWidget)
                          if w.columnCount() == 3 and w.headerItem() is not None and w.headerItem().text(0) == "Command"]
        require(len(shortcut_trees) == 1, "found the shortcuts tree")
        keys = shortcut_trees[0]
        group = find_item(keys, "3DViewer")
        require(group is not None, "the shortcuts tree has a 3DViewer group")
        keys.expand(group)
        for listed in ("Scene Graph: Expand All Below", "Scene Graph: Expand To Assembly", "Scene Graph: Expand To Component",
                       "Scene Graph: Expand To Subcomponent", "Scene Graph: Collapse All Below"):
            row = find_item(keys, listed, "3DViewer")
            check(row is not None, "the 3DViewer group lists %s" % listed)
            if row is not None:
                check(not str(row.siblingAtColumn(1).data() or ""), "%s is unbound by default (got %r)" % (listed, row.siblingAtColumn(1).data()))
        row = find_item(keys, label, "3DViewer")
        require(row is not None, "the 3DViewer group has a %s row" % label)
        keys.setCurrentIndex(row)
        keys.scrollTo(row)
        QApplication.processEvents()
        editors = [w for w in prefs.findChildren(QLineEdit) if w.placeholderText() == "Type to set shortcut" and w.isVisible()]
        validate = [w for w in prefs.findChildren(QPushButton) if w.text() == "Validate" and w.isVisible()]
        require(len(editors) == 1 and len(validate) == 1, "found the %s field and its Validate button" % label)
        editors[0].clear()
        QTest.keyClick(editors[0], key)
        QApplication.processEvents()
        check(key_name in editors[0].text(), "the field records %s (%r)" % (key_name, editors[0].text()))
        validate[0].click()
        QApplication.processEvents()
        row = find_item(keys, label, "3DViewer")
        check(row is not None and key_name in str(row.siblingAtColumn(1).data()), "the %s row shows %s (%r)"
              % (label, key_name, row.siblingAtColumn(1).data() if row is not None else None))
        prefs.close()
        QApplication.processEvents()

    BELOW_ISLAND = [ISLAND, GROVE] + COMPONENTS

    def check_all_below(how, summary):
        check(summary == "Expanded 6 rows; 4 unloaded payloads not expanded",
              "%s says what it did (got %r)" % (how, summary))
        check(opened(BELOW_ISLAND) == BELOW_ISLAND, "%s opens the island, the grove and every component (open %r)" % (how, opened(BELOW_ISLAND)))
        check(not opened(SUBCOMPONENTS), "%s leaves the unloaded subcomponents closed (open %r)" % (how, opened(SUBCOMPONENTS)))

    def check_nothing_loaded(how):
        yield ("sleep", SETTLE_MS * 2)
        check(states(SUBCOMPONENTS) == [STATE_UNLOADED] * len(SUBCOMPONENTS),
              "%s loads no subcomponent (states %r)" % (how, states(SUBCOMPONENTS)))

    def collapse_island(how):
        ok = pick_in_menu(ISLAND, "sceneGraphCollapseBelow")
        check(ok, "%s: the row menu has an enabled Collapse All Below" % how)
        summary = yield from finished()
        check(summary.startswith("Collapsed "), "%s: Collapse All Below says what it did (got %r)" % (how, summary))
        check(not opened(BELOW_ISLAND + SUBCOMPONENTS), "%s: Collapse All Below closes every row under the island (open %r)"
              % (how, opened(BELOW_ISLAND + SUBCOMPONENTS)))
        check(is_open(WORLD), "%s: Collapse All Below leaves the row above the island open" % how)

    def check_to_component(how, summary):
        check(summary.startswith("Expanded to ") and "Stopped" not in summary,
              "%s says what it did (got %r)" % (how, summary))
        check(is_open(ISLAND) and is_open(GROVE), "%s opens the island and the grove (open %r)" % (how, opened([ISLAND, GROVE])))
        check(all(shown(c) for c in COMPONENTS), "%s shows every component row (shown %r)" % (how, [shown(c) for c in COMPONENTS]))
        check(not opened(COMPONENTS), "%s opens no component (open %r)" % (how, opened(COMPONENTS)))

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(panel() is not None, "the Scene Graph panel exists")

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel().isVisible() and [path_of(i) for i in rows()] == [WORLD], POLL_TIMEOUT_S)
        require(ok, "the panel lists /World")
        ok = yield ("until", lambda: find(ISLAND).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists the island")
        tree().expand(find(WORLD))

        # The toolbar row and its buttons.
        require(expand_button() is not None and collapse_button() is not None, "the toolbar has Expand and Collapse buttons")
        check(str(expand_button().text()) == "Expand" and str(collapse_button().text()) == "Collapse",
              "the buttons read Expand and Collapse (got %r, %r)" % (expand_button().text(), collapse_button().text()))
        check_row("toolbar", ["sceneGraphBulkScope", "sceneGraphBulk_load", "sceneGraphBulk_clearBelow", "sceneGraphResetDrawStates",
                              "sceneGraphExpand", "sceneGraphCollapse"])
        tree().selectionModel().clearSelection()
        QApplication.processEvents()
        check(not expand_button().isEnabled() and not collapse_button().isEnabled(), "Expand and Collapse are disabled with no row selected")

        # The row menu's entries.
        texts = menu_texts(ISLAND)
        for name, text in (("sceneGraphExpandBelow", "Expand All Below"), ("sceneGraphCollapseBelow", "Collapse All Below"),
                           ("sceneGraphExpandTo_assembly", "Assembly"), ("sceneGraphExpandTo_component", "Component"),
                           ("sceneGraphExpandTo_subcomponent", "Subcomponent")):
            check(texts.get(name) == text, "the row menu has %s reading %r (got %r)" % (name, text, texts.get(name)))
        check("sceneGraphExpandTo_natronTestTree" in texts, "the row menu's Expand To lists the custom kind natronTestTree (got %r)" % sorted(texts))
        check(shoot_menu(ISLAND, "sceneGraphExpandTo_component", "scene-graph-expand-menu.png"), "shot the row menu's Expand To")
        yield ("sleep", SETTLE_MS)

        # Components loaded, subcomponents not.
        select_rows([ISLAND])
        set_scope("component")
        ok = yield ("until", lambda: child(QToolButton, "sceneGraphBulk_load").isEnabled(), POLL_TIMEOUT_S)
        require(ok, "Load is enabled in To Component scope")
        child(QToolButton, "sceneGraphBulk_load").click()
        ok = yield ("until", all_loaded([ISLAND] + COMPONENTS), POLL_TIMEOUT_S)
        require(ok, "Load To Component loads the island and its components (states %r)" % (states([ISLAND] + COMPONENTS),))
        ok = yield ("until", lambda: all(state(p) is not None for p in SUBCOMPONENTS), POLL_TIMEOUT_S)
        require(ok, "the subcomponent rows are listed")
        tree().collapse(find(ISLAND))
        tree().collapse(find(GROVE))
        yield ("sleep", SETTLE_MS)

        # Expand To Component from the row menu.
        check(pick_in_menu(ISLAND, "sceneGraphExpandTo_component"), "the row menu has an enabled Expand To Component")
        summary = yield from finished()
        check_to_component("Expand To Component", summary)
        yield from check_nothing_loaded("Expand To Component")
        shot("scene-graph-expand-to-component.png")
        yield from collapse_island("after Expand To Component")

        # Expand To a custom kind: the grove holds none, so stays closed.
        check(pick_in_menu(ISLAND, "sceneGraphExpandTo_natronTestTree"), "the row menu has an enabled Expand To natronTestTree")
        summary = yield from finished()
        check(is_open(ISLAND) and shown(BUSH) and not is_open(BUSH), "Expand To natronTestTree shows the bush closed (open %r)" % opened([ISLAND, BUSH]))
        check(not is_open(GROVE), "Expand To natronTestTree leaves the grove, which holds none, closed")
        yield from collapse_island("after Expand To natronTestTree")

        # Expand All Below from the row menu.
        check(pick_in_menu(ISLAND, "sceneGraphExpandBelow"), "the row menu has an enabled Expand All Below")
        summary = yield from finished()
        check_all_below("Expand All Below", summary)
        yield from check_nothing_loaded("Expand All Below")
        shot("scene-graph-expand-all-below.png")
        yield from collapse_island("after Expand All Below")

        # * on the current row.
        select_rows([ISLAND])
        focus_tree()
        QTest.keyClick(tree(), Qt.Key_Asterisk)
        summary = yield from finished()
        check_all_below("*", summary)
        yield from check_nothing_loaded("*")

        # Shift-click on an open row's arrow collapses all below; on a closed one it expands all below.
        shift_click_arrow(ISLAND)
        summary = yield from finished()
        check(summary.startswith("Collapsed "), "Shift-click on the open island's arrow collapses all below (got %r)" % summary)
        check(not opened(BELOW_ISLAND), "Shift-click on the open island's arrow closes every row under it (open %r)" % opened(BELOW_ISLAND))
        shift_click_arrow(ISLAND)
        summary = yield from finished()
        check_all_below("Shift-click on the island's arrow", summary)
        yield from check_nothing_loaded("Shift-click")

        # The toolbar buttons, driven by the scope combo.
        select_rows([ISLAND])
        set_scope("component")
        check(collapse_button().isEnabled(), "Collapse is enabled with a row selected")
        collapse_button().click()
        summary = yield from finished()
        check(not opened(BELOW_ISLAND), "Collapse in To Component scope collapses all below (open %r)" % opened(BELOW_ISLAND))
        select_rows([ISLAND])
        expand_button().click()
        summary = yield from finished()
        check_to_component("Expand in To Component scope", summary)
        collapse_button().click()
        yield from finished()
        select_rows([ISLAND])
        set_scope("below")
        expand_button().click()
        summary = yield from finished()
        check_all_below("Expand in Below scope", summary)
        yield from collapse_island("after Expand in Below scope")
        select_rows([ISLAND])
        set_scope("selected")
        expand_button().click()
        summary = yield from finished()
        check_all_below("Expand in Selected scope", summary)
        yield from collapse_island("after Expand in Selected scope")

        # An editable shortcut.
        rebind("Scene Graph: Expand All Below", Qt.Key_K, "K")
        select_rows([ISLAND])
        focus_tree()
        QTest.keyClick(tree(), Qt.Key_K)
        summary = yield from finished()
        check_all_below("K, bound to Scene Graph: Expand All Below,", summary)
        yield from check_nothing_loaded("K")

        # A plain click on an unloaded row's arrow opens and loads it, as without the expander.
        QTest.mouseClick(tree().viewport(), Qt.LeftButton, Qt.NoModifier, arrow_point(BERRIES))
        ok = yield ("until", lambda: is_open(BERRIES) and state(BERRIES) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "a plain click on the berries' arrow opens and loads them (open %r, state %r)" % (is_open(BERRIES), state(BERRIES)))

        # The wide stage: every row, no payload, and the event loop runs between slices.
        require(view_input(wide), "connect the Viewer to the wide stage")
        ok = yield ("until", lambda: [path_of(i) for i in rows()] == [WIDE], POLL_TIMEOUT_S)
        require(ok, "the panel lists /Wide")
        payloads = ["%s/G%03d/P" % (WIDE, g) for g in range(WIDE_PAYLOADS)]
        fired = []
        select_rows([WIDE])
        set_scope("below")
        ok = yield ("until", lambda: expand_button().isEnabled(), POLL_TIMEOUT_S)
        require(ok, "Expand is enabled once the wide stage's root is selected")
        expand_button().click()
        QTimer.singleShot(0, lambda: fired.append((expanding(), slices())))
        ok = yield ("until", lambda: bool(fired), POLL_TIMEOUT_S)
        check(ok and fired[0][0], "a timer posted as Expand All Below starts fires while it runs (got %r)" % (fired,))
        summary = yield from finished(WIDE_TIMEOUT_S)
        check(slices() > 1, "the wide expansion runs in more than one slice (%d)" % slices())
        check(summary == "Expanded 5,101 rows; 5 unloaded payloads not expanded", "Expand All Below on the wide stage says what it did (got %r)" % summary)
        check(states(payloads) == [STATE_UNLOADED] * WIDE_PAYLOADS, "Expand All Below loads none of the wide stage's payloads (states %r)" % states(payloads))
        check(not opened(payloads), "the wide stage's payload rows stay closed (open %r)" % opened(payloads))
        last = "%s/G%03d/I%02d" % (WIDE, WIDE_GROUPS - 1, WIDE_ROWS - 1)
        check(is_open(last), "the last row of the wide stage is open")
        shot("scene-graph-expand-wide.png")
        select_rows([WIDE])
        collapse_button().click()
        summary = yield from finished(WIDE_TIMEOUT_S)
        check(summary == "Collapsed 5,101 rows", "Collapse All Below on the wide stage says what it did (got %r)" % summary)
        check(not is_open(WIDE) and not is_open(last), "Collapse All Below closes the wide stage")

        # Esc stops a long expansion. The rows are read by now and a slice lays nothing out, so a
        # few slices open the whole stage: Esc goes in after the click's own slice, before the
        # event loop runs another, rather than after a sleep the run can outlast.
        select_rows([WIDE])
        focus_tree()
        expand_button().click()
        running = expanding()
        check(running, "the wide expansion is still running after its first slice (status %r)" % status())
        QTest.keyClick(tree(), Qt.Key_Escape)
        ok = yield ("until", lambda: not expanding(), POLL_TIMEOUT_S)
        check(ok, "Esc stops the expansion")
        if running:
            check(status().startswith("Stopped. Expanded "), "the status line says the expansion stopped (got %r)" % status())
            check(is_open(WIDE) and not is_open(last), "the rows opened before Esc stay open and the rest stay closed")
            count = int(panel().property("natronSceneGraphExpandedCount") or 0)
            check(0 < count < WIDE_OPENED, "the remembered open rows are the ones opened before Esc (%d)" % count)

        # Showing another node stops an expansion under way.
        select_rows([WIDE])
        expand_button().click()
        running = expanding()
        check(running, "the second wide expansion is running after its first slice (status %r)" % status())
        require(view_input(usd), "connect the Viewer back to the island's USDRead")
        ok = yield ("until", lambda: not expanding(), POLL_TIMEOUT_S)
        check(ok, "showing another node stops the expansion")
        ok = yield ("until", lambda: [path_of(i) for i in rows()] == [WORLD], POLL_TIMEOUT_S)
        check(ok, "the panel lists /World again (rows %r, status %r, scene drawn %r)"
              % ([path_of(i) for i in rows()], status(), scene_drawn()))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
