"""The Scene Graph panel's bulk commands: the scope combo, the toolbar row, the scoped row menu and
the viewer's loadRows, unloadRows and setDrawStateOnRowsScoped.

Opens Tests/fixtures/sceneKindPayloads/world.usda, whose island assembly holds payloads of
components (two trees, a rock and a bush of the custom kind natronTestTree, which
scene_graph_bulk.pxrplugins registers), each holding a subcomponent payload, and checks:
  - the panel's three rows: the filter row holds the filter and Changed Here; the toolbar row the
    scope combo, the Load section (Load, Unload), the Display section (the draw states, Clear Below,
    Reset to Graph); the node row, between the tree and the prim inspector, Apply To Node and Bake to
    Node on the left and Cancel Loads on the right; none overflows the docked panel, and each is
    shot on its own;
  - the scope combo lists the built-in and custom kinds;
  - with no row selected every command is disabled; in Below scope Unload and Clear Below are;
  - Load To Component on the island loads its components and leaves their subcomponents unloaded;
  - Geometry To Component under a boxed island sets Geometry on each component and switches the island and
    the grove, which would block them, to Expand, and
    Geometry shows checked when every selected row is set Geometry;
  - Hide To natronTestTree sets the bush alone;
  - Expand and BBox on selected rows, and Clear Below;
  - Load Below loads everything, and Unload Below Component unloads what is below the components;
  - Load + Draw Below sets Load + Draw on the island and clears the states below it, as one change;
  - BBox To Component from the row menu, found by its object name;
  - the methods called by invokeMethod return what they did, and an empty string for a scope they
    do not take;
  - pinned to a node the viewer does not show, Load is enabled and Load To Component loads the
    panel's own stage.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
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
CRATE = "/World/Props/Crate"
COMPONENTS = [TREE_A, TREE_B, ROCK, BUSH]
SUBCOMPONENTS = [LEAVES_A, LEAVES_B, MOSS, BERRIES]
BUTTONS = ["load", "unload", "bbox", "expand", "geometry", "loadAndDraw", "hide", "clearBelow"]

STATE_NONE, STATE_UNLOADED, STATE_LOADING, STATE_LOADED, STATE_FAILED = range(5)
DRAW_BBOX, DRAW_EXPAND, DRAW_GEOMETRY, DRAW_LOAD_AND_DRAW, DRAW_HIDE = range(5)
ORIGIN_VIEWER = 0

try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, Q_RETURN_ARG, QItemSelectionModel, QMetaObject, QModelIndex, QPoint, QRect, Qt, QTimer
    from PySide6.QtGui import QAction
    from PySide6.QtWidgets import QApplication, QComboBox, QLabel, QLineEdit, QMenu, QTabWidget, QToolButton, QTreeView, QWidget

    from guitest import FIXTURES, OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3
    DRAW_STATE_ROLE = Qt.UserRole + 5
    ORIGIN_ROLE = Qt.UserRole + 7

    scene_path = os.path.join(FIXTURES, "sceneKindPayloads", "world.usda")
    require(os.path.isfile(scene_path), "the fixture %s exists" % scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    other = app.createNode("USDRead")
    require(other is not None, "create a second USDRead")
    other.getParam("initialDraw").setValue(3)
    other.getParam("filename").setValue(scene_path)
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

    def button(action):
        return child(QToolButton, "sceneGraphBulk_" + action)

    def scope_combo():
        return child(QComboBox, "sceneGraphBulkScope")

    def bulk_status():
        label = child(QLabel, "sceneGraphBulkStatus")
        return str(label.text()) if label is not None and label.isVisible() else ""

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

    def draw(path):
        """(state, origin) of path's row in the linked viewer, None when absent."""
        index = find(path)
        if not index.isValid() or index.data(DRAW_STATE_ROLE) is None:
            return None
        return (int(index.data(DRAW_STATE_ROLE)), int(index.data(ORIGIN_ROLE)))

    def set_here(path, wanted):
        return draw(path) == (wanted, ORIGIN_VIEWER)

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

    def press(action):
        """Clicks the toolbar button of action once it is enabled; returns the status it leaves."""
        ok = yield ("until", lambda: button(action).isEnabled(), POLL_TIMEOUT_S)
        check(ok, "the %s button is enabled in %s scope" % (action, scope_combo().currentData()))
        button(action).click()
        return bulk_status()

    def enabled_buttons():
        return [a for a in BUTTONS if button(a).isEnabled()]

    def invoke(method, paths, *args):
        """Calls one of the viewer's scoped methods; returns the summary it gives."""
        q_args = [Q_ARG("QStringList", paths)] + [Q_ARG("QString", a) for a in args]
        return QMetaObject.invokeMethod(view(), method, Qt.DirectConnection, Q_RETURN_ARG("QString"), *q_args)

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
        QTimer.singleShot(300, trigger)
        t.customContextMenuRequested.emit(t.visualRect(find(path)).center())
        QApplication.processEvents()
        return found == [True]

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

    def panel_rect(widget):
        """widget's rectangle in the panel's coordinates."""
        return QRect(widget.mapTo(panel(), QPoint(0, 0)), widget.size())

    def xywh(r):
        return (r.x(), r.y(), r.width(), r.height())

    def check_row(label, names):
        """Checks that the row of the panel's children named names, in that order left to right,
        shows each whole inside the panel; reports its extent and shoots it. Returns its rectangle."""
        widgets = [child(QWidget, n) for n in names]
        missing = [n for n, w in zip(names, widgets) if w is None]
        require(not missing, "the %s row has %r (missing %r)" % (label, names, missing))
        width = panel().width()
        rects = [panel_rect(w) for w in widgets]
        hidden = [n for n, w in zip(names, widgets) if not w.isVisible()]
        out = [n for n, r in zip(names, rects) if r.left() < 0 or r.right() >= width]
        squeezed = [n for n, w in zip(names, widgets) if w.width() < w.minimumSizeHint().width()]
        ordered = all(rects[i].right() < rects[i + 1].left() for i in range(len(rects) - 1))
        top = min(r.top() for r in rects)
        bottom = max(r.bottom() for r in rects)
        one_line = all(r.top() < rects[0].bottom() and r.bottom() > rects[0].top() for r in rects)
        natural = sum(w.sizeHint().width() for w in widgets)
        report("NOTE %s row: panel %d px wide, row from %d to %d, its widgets' natural widths add up to %d"
               % (label, width, rects[0].left(), rects[-1].right(), natural))
        check(not hidden and not out and not squeezed and ordered and one_line,
              "the %s row fits the panel on one line in order (hidden %r, past the edge %r, squeezed %r, ordered %r, one line %r)"
              % (label, hidden, out, squeezed, ordered, one_line))
        area = QRect(0, top, width, bottom - top + 1)
        path = os.path.join(OUT, "scene-graph-bulk-row-%s.png" % label)
        if panel().grab(area).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)
        return area

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
        ok = yield ("until", lambda: find(ISLAND).isValid() and find(CRATE).isValid(), POLL_TIMEOUT_S)
        require(ok, "the panel lists the island and the crate")
        tree().expand(find(WORLD))

        # The toolbar row.
        combo = scope_combo()
        require(combo is not None, "the panel has the scope combo")
        scopes = [combo.itemData(i) for i in range(combo.count()) if combo.itemData(i)]
        check(scopes[:5] == ["selected", "below", "assembly", "component", "subcomponent"],
              "the scope combo starts Selected, Below, To Assembly, To Component, To Subcomponent (got %r)" % (scopes,))
        check("natronTestTree" in scopes and "natronTestSet" in scopes, "the scope combo lists the registered custom kinds (got %r)" % (scopes,))
        missing = [a for a in BUTTONS if button(a) is None]
        require(not missing, "the toolbar has every command's button (missing %r)" % (missing,))
        check(child(QLineEdit, "sceneGraphFilter") is not None, "the panel has the filter field")
        filter_row = check_row("filter", ["sceneGraphFilter", "sceneGraphChangedHere"])
        toolbar_row = check_row("toolbar", ["sceneGraphBulkScope", "sceneGraphBulkLoadIcon", "sceneGraphBulk_load", "sceneGraphBulk_unload",
                                            "sceneGraphBulkDisplayIcon", "sceneGraphBulk_bbox", "sceneGraphBulk_expand",
                                            "sceneGraphBulk_geometry", "sceneGraphBulk_loadAndDraw", "sceneGraphBulk_hide",
                                            "sceneGraphBulk_clearBelow", "sceneGraphResetDrawStates"])
        node_row = check_row("node", ["sceneGraphApplyToNode", "sceneGraphBakeDrawStates", "sceneGraphCancelLoads"])
        labels = [str(button(a).text()) for a in BUTTONS]
        check(labels == ["Load", "Unload", "BBox", "Expand", "Geometry", "Load + Draw", "Hide", "Clear Below"],
              "the toolbar buttons read Load, Unload, BBox, Expand, Geometry, Load + Draw, Hide, Clear Below (got %r)" % (labels,))
        tree_rect = panel_rect(tree())
        tree_pane = child(QWidget, "sceneGraphTreePane")
        check(tree_pane is not None and tree_pane.isAncestorOf(child(QWidget, "sceneGraphApplyToNode")),
              "the tree and the node row share a pane")
        check(filter_row.bottom() < toolbar_row.top() and toolbar_row.bottom() < tree_rect.top() and tree_rect.bottom() < node_row.top(),
              "the filter row is above the toolbar row, the toolbar row above the tree, the node row below it (filter %r, toolbar %r, tree %r, node %r)"
              % (xywh(filter_row), xywh(toolbar_row), xywh(tree_rect), xywh(node_row)))
        tabs = child(QTabWidget, "sceneGraphInspectorTabs")
        check(tabs is not None and node_row.bottom() < panel_rect(tabs).top(),
              "the node row is above the prim inspector's tabs (tabs at %r)" % (xywh(panel_rect(tabs)) if tabs is not None else None,))
        cancel = panel_rect(child(QWidget, "sceneGraphCancelLoads"))
        bake = panel_rect(child(QWidget, "sceneGraphBakeDrawStates"))
        check(cancel.left() > bake.right() + 20 and panel().width() - cancel.right() < 20,
              "Cancel Loads sits at the node row's right end (bake ends %d, cancel %d to %d of %d)" % (bake.right(), cancel.left(), cancel.right(), panel().width()))
        check(child(QLabel, "sceneGraphBulkStatus") is not None, "the panel has the bulk status line")

        select_rows([])
        yield ("sleep", SETTLE_MS)
        check(enabled_buttons() == [], "with no row selected every command is disabled (enabled %r)" % (enabled_buttons(),))
        select_rows([ISLAND])
        set_scope("below")
        yield ("sleep", SETTLE_MS)
        check(button("load").isEnabled() and not button("unload").isEnabled() and not button("clearBelow").isEnabled(),
              "in Below scope Load is enabled, Unload and Clear Below are not (enabled %r)" % (enabled_buttons(),))
        check("Load Below" in button("load").toolTip(), "Load's tooltip names Load Below (got %r)" % (button("load").toolTip(),))
        set_scope("component")
        yield ("sleep", SETTLE_MS)
        check(enabled_buttons() == BUTTONS, "in a kind scope every command is enabled (enabled %r)" % (enabled_buttons(),))
        check("Unload Below Component" in button("unload").toolTip(),
              "Unload's tooltip reads Unload Below Component (got %r)" % (button("unload").toolTip(),))
        shot("scene-graph-bulk-toolbar.png")

        # The methods refuse what they do not take.
        check(invoke("unloadRows", [ISLAND], "below") == "", "unloadRows takes no Below scope")
        check(invoke("setDrawStateOnRowsScoped", [ISLAND], "clearBelow", "below") == "", "Clear Below takes no Below scope")
        check(invoke("loadRows", [ISLAND], "nonsense") == "", "loadRows takes no unknown scope")
        check(invoke("loadRows", ["not a path"], "selected") == "", "loadRows takes no invalid path")

        # Load To Component.
        check(state(ISLAND) == STATE_UNLOADED and state(CRATE) == STATE_UNLOADED, "nothing is loaded at first")
        summary = yield from press("load")
        check(summary == "Loading down to components under 1 row", "Load To Component says what it does (got %r)" % (summary,))
        ok = yield ("until", all_loaded([ISLAND] + COMPONENTS), POLL_TIMEOUT_S)
        check(ok, "Load To Component on the island loads it and its components (states %r)" % ([state(p) for p in [ISLAND] + COMPONENTS],))
        ok = yield ("until", lambda: all(state(p) is not None for p in SUBCOMPONENTS), POLL_TIMEOUT_S)
        check(ok, "the components' subcomponent rows are listed")
        yield ("sleep", SETTLE_MS)
        check(all(state(p) == STATE_UNLOADED for p in SUBCOMPONENTS),
              "the components' subcomponents stay unloaded (states %r)" % ([state(p) for p in SUBCOMPONENTS],))
        check(state(CRATE) == STATE_UNLOADED, "the crate, outside the selection, stays unloaded (state %r)" % (state(CRATE),))
        tree().expand(find(ISLAND))
        tree().expand(find(GROVE))
        shot("scene-graph-bulk-load-to-component.png")

        # Geometry To Component, under a boxed island the components' Geometry has to switch to Expand.
        check(invoke("setDrawStateOnRowsScoped", [ISLAND], "bbox", "selected") == "BBox set on 1 row", "box the island")
        ok = yield ("until", lambda: set_here(ISLAND, DRAW_BBOX), POLL_TIMEOUT_S)
        check(ok, "the island is boxed (got %r)" % (draw(ISLAND),))
        select_rows([ISLAND])
        summary = yield from press("geometry")
        check(summary == "Geometry set on 4 components under 1 row", "Geometry To Component says what it did (got %r)" % (summary,))
        ok = yield ("until", lambda: all(set_here(p, DRAW_GEOMETRY) for p in COMPONENTS), POLL_TIMEOUT_S)
        check(ok, "Geometry To Component sets Geometry on every component (got %r)" % ([draw(p) for p in COMPONENTS],))
        check(draw(ISLAND) is not None and draw(ISLAND)[0] == DRAW_EXPAND and draw(GROVE) is not None and draw(GROVE)[0] == DRAW_EXPAND,
              "the island and the grove read Expand (got %r, %r)" % (draw(ISLAND), draw(GROVE)))
        yield ("sleep", SETTLE_MS)
        check(not button("geometry").isChecked(), "Geometry is unchecked with the island selected")
        select_rows([TREE_A, TREE_B])
        ok = yield ("until", lambda: button("geometry").isChecked(), POLL_TIMEOUT_S)
        check(ok, "Geometry shows checked with two Geometry rows selected")
        check(not button("bbox").isChecked(), "BBox is unchecked with two Geometry rows selected")
        shot("scene-graph-bulk-geometry-to-component.png")

        # A custom kind matches itself alone.
        select_rows([ISLAND])
        set_scope("natronTestTree")
        summary = yield from press("hide")
        check(summary == "Hide set on 1 natronTestTree prim under 1 row", "Hide To natronTestTree says what it did (got %r)" % (summary,))
        ok = yield ("until", lambda: set_here(BUSH, DRAW_HIDE), POLL_TIMEOUT_S)
        check(ok, "Hide To natronTestTree hides the bush (got %r)" % (draw(BUSH),))
        check(all(set_here(p, DRAW_GEOMETRY) for p in (TREE_A, TREE_B, ROCK)), "the other components stay Geometry (got %r)" % ([draw(p) for p in COMPONENTS],))

        # Selected scope: Expand, BBox and Clear Below.
        set_scope("selected")
        select_rows([ROCK])
        summary = yield from press("bbox")
        check(summary == "BBox set on 1 row", "BBox on the selection says what it did (got %r)" % (summary,))
        ok = yield ("until", lambda: set_here(ROCK, DRAW_BBOX), POLL_TIMEOUT_S)
        check(ok, "BBox sets the rock (got %r)" % (draw(ROCK),))
        select_rows([GROVE])
        yield from press("expand")
        ok = yield ("until", lambda: set_here(GROVE, DRAW_EXPAND), POLL_TIMEOUT_S)
        check(ok, "Expand sets the grove (got %r)" % (draw(GROVE),))
        ok = yield ("until", lambda: button("expand").isChecked(), POLL_TIMEOUT_S)
        check(ok, "Expand shows checked with the grove selected")
        select_rows([ROCK])
        summary = yield from press("clearBelow")
        check(summary.startswith("States cleared below 1 row"), "Clear Below says what it did (got %r)" % (summary,))
        check(set_here(ROCK, DRAW_BBOX), "Clear Below keeps the row's own state (got %r)" % (draw(ROCK),))

        # Load Below, then Unload Below Component.
        select_rows([ISLAND])
        set_scope("below")
        summary = yield from press("load")
        check(summary == "Loading 1 row and every payload below them", "Load Below says what it does (got %r)" % (summary,))
        ok = yield ("until", all_loaded(SUBCOMPONENTS), POLL_TIMEOUT_S)
        check(ok, "Load Below loads every payload below the island (states %r)" % ([state(p) for p in SUBCOMPONENTS],))
        set_scope("component")
        summary = yield from press("unload")
        check(summary == "Unloading below components under 1 row", "Unload Below Component says what it does (got %r)" % (summary,))
        ok = yield ("until", lambda: all(state(p) == STATE_UNLOADED for p in SUBCOMPONENTS), POLL_TIMEOUT_S)
        check(ok, "Unload Below Component unloads the subcomponents (states %r)" % ([state(p) for p in SUBCOMPONENTS],))
        check(all_loaded([ISLAND] + COMPONENTS)(), "the components stay loaded (states %r)" % ([state(p) for p in [ISLAND] + COMPONENTS],))
        shot("scene-graph-bulk-unload-below-component.png")

        # BBox To Component from the row menu.
        check(shoot_menu(ISLAND, "sceneGraphDrawStateTo_component_bbox", "scene-graph-bulk-row-menu.png"),
              "the row menu opens down to Draw State, To Component, BBox")
        check(pick_in_menu(ISLAND, "sceneGraphDrawStateTo_component_bbox"), "the row menu offers BBox To Component, enabled")
        ok = yield ("until", lambda: all(set_here(p, DRAW_BBOX) for p in COMPONENTS), POLL_TIMEOUT_S)
        check(ok, "BBox To Component from the row menu boxes every component (got %r)" % ([draw(p) for p in COMPONENTS],))
        check(bulk_status() == "BBox set on 4 components under 1 row", "the menu entry writes the status line (got %r)" % (bulk_status(),))

        # Load + Draw Below, by invokeMethod.
        summary = invoke("setDrawStateOnRowsScoped", [ISLAND], "loadAndDraw", "below")
        check(summary == "Load + Draw set on 1 row, states below cleared", "setDrawStateOnRowsScoped returns what it did (got %r)" % (summary,))
        ok = yield ("until", lambda: set_here(ISLAND, DRAW_LOAD_AND_DRAW) and all(draw(p) is not None and draw(p)[1] != ORIGIN_VIEWER for p in COMPONENTS + [GROVE]),
                    POLL_TIMEOUT_S)
        check(ok, "Load + Draw Below sets the island and clears the states below it (got %r, %r)" % (draw(ISLAND), [draw(p) for p in COMPONENTS + [GROVE]]))
        ok = yield ("until", all_loaded(SUBCOMPONENTS), POLL_TIMEOUT_S)
        check(ok, "Load + Draw on the island loads the payloads below it (states %r)" % ([state(p) for p in SUBCOMPONENTS],))
        shot("scene-graph-bulk-load-and-draw-below.png")

        # Pinned to a node the viewer does not show, whose stage the panel reads itself, unloaded.
        pin = child(QToolButton, "sceneGraphPin")
        require(pin is not None, "the panel has a pin button")
        pin.click()
        ok = yield ("until", lambda: bool(panel().property("natronSceneGraphPinned")), POLL_TIMEOUT_S)
        check(ok, "the pin button pins the panel to the viewed USDRead")
        require(view_input(other), "switch the Viewer to the second USDRead")
        ok = yield ("until", lambda: scene_drawn() and find(ISLAND).isValid() and state(ISLAND) == STATE_UNLOADED, POLL_TIMEOUT_S)
        check(ok and str(panel().property("natronSceneGraphNode")) == usd.getScriptName(),
              "pinned, the panel reads the first USDRead on a stage of its own, the island unloaded (state %r)" % (state(ISLAND),))
        select_rows([ISLAND])
        set_scope("component")
        yield ("sleep", SETTLE_MS)
        check(button("load").isEnabled() and button("unload").isEnabled(), "pinned, Load and Unload are enabled (enabled %r)" % (enabled_buttons(),))
        set_scope("below")
        yield ("sleep", SETTLE_MS)
        check(not button("unload").isEnabled() and not button("clearBelow").isEnabled(),
              "pinned, Below scope still disables Unload and Clear Below (enabled %r)" % (enabled_buttons(),))
        set_scope("component")
        summary = yield from press("load")
        check(summary == "Loading down to components under 1 row", "pinned, Load To Component says what it does (got %r)" % (summary,))
        ok = yield ("until", all_loaded([ISLAND] + COMPONENTS), POLL_TIMEOUT_S)
        check(ok, "pinned, Load To Component loads the panel's own stage (states %r)" % ([state(p) for p in [ISLAND] + COMPONENTS],))
        ok = yield ("until", lambda: all(state(p) is not None for p in SUBCOMPONENTS), POLL_TIMEOUT_S)
        check(ok, "pinned, the subcomponent rows are listed")
        yield ("sleep", SETTLE_MS)
        check(all(state(p) == STATE_UNLOADED for p in SUBCOMPONENTS),
              "pinned, the subcomponents stay unloaded (states %r)" % ([state(p) for p in SUBCOMPONENTS],))
        shot("scene-graph-bulk-pinned.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
