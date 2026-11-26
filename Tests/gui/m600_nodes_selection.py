"""Create Node from Selection on native geometry, from the 3D view, the Scene Graph and the Geometry
Spreadsheet, and the breadcrumb bar over a native geometry view.

Graph: GeoPrimitive (grid, 2 x 2 faces, 9 points) -> Viewer1, with the Scene Graph panel and the
Geometry Spreadsheet panel docked beside the viewer. The view is put in the top orthographic view
(C) and framed (A), so the grid is a square centred in it.

Checks:
  - the breadcrumb bar is hidden while the view shows the grid, and shown once a GeoToUSD after the
    grid is viewed instead (a USD scene);
  - view, Objects mode: a click on the grid selects its prim, which is made of the 4 faces
    (natronGeoPick); Tab on the view opens the tab menu filtered to geometry nodes (GeoTransform,
    no USDPrune), and GeoTransform is created between the GeoPrimitive and the Viewer with Group
    "0-3" (the prim's faces) and Group Type Primitives; one Undo removes it;
  - Scene Graph: selecting the grid's prim row and pressing Tab on the tree opens the same
    geometry-filtered menu; GeoTransform is created between the GeoPrimitive and the Viewer with
    Group "0-3" and Group Type Primitives; one Undo removes it;
  - Geometry Spreadsheet: selecting Points rows 0-3 makes them the view's point selection; Tab on
    the table opens the menu; GeoTransform is created between the GeoPrimitive and the Viewer with
    Group "0-3" and Group Type Points; one Undo removes it.

Shots: m600-breadcrumb-geometry.png, m600-breadcrumb-scene.png, m600-view-created.png,
m600-scene-graph-created.png and m600-spreadsheet-created.png.

Mouse and key events are sent to the widgets themselves (synthesized window-system events do not
reach Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and
results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
PICK_TIMEOUT_S = 3.0
SETTLE_MS = 700
STARTUP_TIMEOUT_S = 60.0
DEFAULT_NAVIGATION_PRESET = 0
TRANSFORM_ID = "GeoTransform"
SPREADSHEET = "geometrySpreadsheet"
FACES = "0-3"
# GeoTransform's Group Type options.
GROUP_TYPE_POINTS = 0
GROUP_TYPE_PRIMITIVES = 1

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelection, QItemSelectionModel, QModelIndex, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMainWindow, QTableView, QToolButton, QTreeView, QWidget

    from guitest import (check, class_name, fetch_rows, finish, menu_of, plugin_problem, report, require, run, section,
                         shot, viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    grid = app.createNode("GeoPrimitive")
    require(grid is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(2)
    grid.getParam("gridColumns").setValue(2)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    grid.setPosition(0, 0)
    viewer.setPosition(0, 220)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def key(k, modifiers=Qt.NoModifier):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def click(pos):
        v = view()
        for kind, button, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton, Qt.LeftButton),
                                      (QEvent.MouseButtonRelease, Qt.LeftButton, Qt.NoButton)):
            QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def picked_primitives():
        picks = prop("natronGeoPick", []) or []
        return [int(p) for pick in picks for p in pick[1]]

    def breadcrumb():
        bars = widgets_of("SceneBreadcrumbBar")
        return hold(bars[0]) if bars else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def panel(cls):
        panels = widgets_of(cls)
        return hold(panels[0]) if panels else None

    def child(cls_panel, cls, name):
        p = panel(cls_panel)
        try:
            return hold(p.findChild(cls, name)) if p is not None else None
        except RuntimeError:
            return None

    def tree():
        return child("SceneGraphPanel", QTreeView, "sceneGraphTree")

    def tree_rows(parent=QModelIndex()):
        t = tree()
        try:
            m = hold(t.model()) if t is not None else None
        except RuntimeError:
            m = None
        if m is None:
            return []
        try:
            fetch_rows(m, parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def tree_leaves(parent=QModelIndex(), depth=0):
        """The rows with no children, the prims the viewer made of the geometry."""
        leaves = []
        for index in tree_rows(parent):
            children = tree_leaves(index, depth + 1) if depth < 8 else []
            if children:
                leaves.extend(children)
            elif not tree_rows(index):
                leaves.append(index)
        return leaves

    def table():
        return child("GeoSpreadsheetPanel", QTableView, SPREADSHEET + "Table")

    def class_button(label):
        return child("GeoSpreadsheetPanel", QToolButton, SPREADSHEET + label)

    def sheet_class():
        sheet = child("GeoSpreadsheetPanel", QWidget, SPREADSHEET)
        return sheet.property("natronGeoSpreadsheetClass") if sheet is not None else None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui":
                return w
        return None

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for a in win.menuBar().actions():
            menu = menu_of(a)
            if menu is not None and a.text().lower().startswith("edit"):
                for item in menu.actions():
                    if item.text().lower().startswith(prefix):
                        return item
        return None

    def script_names():
        return set(n.getScriptName() for n in app.getChildren())

    def input_name(node, i=0):
        inp = node.getInput(i) if node is not None else None
        return inp.getScriptName() if inp is not None else None

    def open_menu(widget):
        widget.activateWindow()
        widget.setFocus()
        QApplication.processEvents()
        QTest.keyClick(widget, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        if not ok:
            return None, None, None
        popup = next(d for d in widgets_of("TabTabTabWidget") if d.isVisible())
        return popup, popup.findChildren(QLineEdit)[0], popup.findChildren(QListView)[0]

    def listed(line_edit, list_view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = list_view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def row_name(entry):
        return entry.split("[")[0].strip()

    def create_from_menu(widget, where):
        """Presses Tab on widget and chooses GeoTransform; the node created, or None."""
        names = script_names()
        popup, line_edit, list_view = yield from open_menu(widget)
        if not check(popup is not None, "Tab on the %s opens the tab menu" % (where,)):
            return None
        pruned = [row_name(e) for e in listed(line_edit, list_view, "USDPrune")]
        check("USDPrune" not in pruned, "the menu from the %s leaves out USDPrune, which takes no geometry (got %r)" % (where, pruned))
        entries = listed(line_edit, list_view, TRANSFORM_ID)
        row = next((r for r, e in enumerate(entries) if row_name(e) == TRANSFORM_ID), -1)
        if not check(row >= 0, "the menu from the %s offers GeoTransform (got %r)" % (where, entries)):
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()
            return None
        list_view.setCurrentIndex(list_view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)
        ok = yield ("until", lambda: len(script_names() - names) > 0, POLL_TIMEOUT_S)
        if not ok:
            return None
        return next(n for n in app.getChildren() if n.getScriptName() not in names)

    def check_created(node, where, group_type, shot_name):
        if not check(node is not None and node.getPluginID().endswith(TRANSFORM_ID),
                     "Tab and GeoTransform from the %s create a GeoTransform" % (where,)):
            return
        check(input_name(node) == grid.getScriptName(),
              "the GeoTransform from the %s reads the GeoPrimitive (got %r)" % (where, input_name(node)))
        ok = yield ("until", lambda: input_name(viewer) == node.getScriptName(), POLL_TIMEOUT_S)
        check(ok, "the Viewer shows the GeoTransform from the %s (its input is %r)" % (where, input_name(viewer)))
        group = node.getParam("group").getValue()
        check(group == FACES, "the GeoTransform from the %s has Group %r (got %r)" % (where, FACES, group))
        got_type = node.getParam("groupType").getValue()
        check(got_type == group_type, "the GeoTransform from the %s has Group Type option %d (got %d)" % (where, group_type, got_type))
        yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield settle()
        shot(shot_name)
        action = edit_menu_action("undo")
        require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
        name = node.getScriptName()
        action.trigger()
        QApplication.processEvents()
        ok = yield ("until", lambda: name not in script_names(), POLL_TIMEOUT_S)
        check(ok, "one Undo removes the GeoTransform from the %s" % (where,))
        ok = yield ("until", lambda: input_name(viewer) == grid.getScriptName(), POLL_TIMEOUT_S)
        check(ok, "and puts the Viewer back on the GeoPrimitive (its input is %r)" % (input_name(viewer),))
        yield ("until", scene_drawn, POLL_TIMEOUT_S)

    def view_section():
        section("view")
        check(prop("natronGeoPickMode") == "objects", "the view picks objects (%r)" % (prop("natronGeoPickMode"),))
        centre = QPoint(view().width() // 2, view().height() // 2)
        click(centre)
        ok = yield ("until", lambda: sorted(picked_primitives()) == [0, 1, 2, 3], POLL_TIMEOUT_S)
        require(ok, "a click on the grid selects its prim, made of the 4 faces (%r, %r)"
                % (prop("natronSceneSelection"), prop("natronGeoPick")))
        node = yield from create_from_menu(view(), "3D view")
        yield from check_created(node, "3D view", GROUP_TYPE_PRIMITIVES, "m600-view-created.png")

    def scene_graph_section():
        section("scene graph")
        ok = yield ("until", lambda: len(tree_leaves()) > 0, POLL_TIMEOUT_S)
        require(ok, "the Scene Graph lists the grid's prim")
        selection = tree().selectionModel()
        selection.clearSelection()
        QApplication.processEvents()
        leaves = tree_leaves()
        report("NOTE the Scene Graph's prims: %r" % ([str(i.data(PATH_ROLE)) for i in leaves],))
        for index in leaves:
            selection.select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            selection.setCurrentIndex(index, QItemSelectionModel.NoUpdate)
        QApplication.processEvents()
        ok = yield ("until", lambda: sorted(picked_primitives()) == [0, 1, 2, 3], POLL_TIMEOUT_S)
        require(ok, "the prim selected in the Scene Graph is the grid's 4 faces (%r)" % (prop("natronGeoPick"),))
        node = yield from create_from_menu(tree(), "Scene Graph")
        yield from check_created(node, "Scene Graph", GROUP_TYPE_PRIMITIVES, "m600-scene-graph-created.png")

    def spreadsheet_section():
        section("spreadsheet")
        ok = yield ("until", lambda: table() is not None and class_button("Points") is not None, POLL_TIMEOUT_S)
        require(ok, "the Geometry Spreadsheet has its table")
        class_button("Points").click()
        ok = yield ("until", lambda: sheet_class() == "Points" and table().model().rowCount() == 9, POLL_TIMEOUT_S)
        require(ok, "the spreadsheet lists the grid's 9 points (%r)" % (sheet_class(),))
        t = table()
        model = t.model()
        t.selectionModel().select(QItemSelection(model.index(0, 0), model.index(3, model.columnCount() - 1)),
                                  QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        QApplication.processEvents()
        ok = yield ("until", lambda: prop("natronGeoPickMode") == "points" and str(prop("natronGeoSelection", "")) == FACES,
                    POLL_TIMEOUT_S)
        require(ok, "Points rows 0-3 are the view's point selection (%r, %r)"
                % (prop("natronGeoPickMode"), prop("natronGeoSelection")))
        node = yield from create_from_menu(t, "Geometry Spreadsheet")
        yield from check_created(node, "Geometry Spreadsheet", GROUP_TYPE_POINTS, "m600-spreadsheet-created.png")

    def breadcrumb_section():
        section("breadcrumb")
        bar = breadcrumb()
        require(bar is not None, "the viewer has a breadcrumb bar")
        check(not bar.isVisible(), "the breadcrumb bar is hidden over the native geometry view")
        shot("m600-breadcrumb-geometry.png")
        to_usd = app.createNode("GeoToUSD")
        require(to_usd is not None, "create GeoToUSD", plugin_problem("GeoToUSD"))
        to_usd.setPosition(200, 110)
        require(to_usd.connectInput(0, grid), "connect the GeoToUSD to the GeoPrimitive")
        # connectInput refuses an input that is already connected.
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, to_usd), "view the GeoToUSD")
        ok = yield ("until", lambda: scene_drawn() and breadcrumb() is not None and breadcrumb().isVisible(), POLL_TIMEOUT_S)
        check(ok, "the breadcrumb bar shows over the GeoToUSD's scene")
        yield settle()
        shot("m600-breadcrumb-scene.png")
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, grid), "view the GeoPrimitive again")
        ok = yield ("until", lambda: scene_drawn() and breadcrumb() is not None and not breadcrumb().isVisible(), POLL_TIMEOUT_S)
        check(ok, "the breadcrumb bar hides again over the native geometry")

    def steps():
        require(viewer.connectInput(0, grid), "connect the Viewer to the GeoPrimitive")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the grid (%r)" % (prop("natronSceneError"),))

        # Hidden panels defer reading their rows, and the default layout docks neither.
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        right = app.getTabWidget(home.objectName()).splitHorizontally()
        require(right is not None, "split the viewer's pane")
        lower = right.splitVertically()
        require(lower is not None, "split the new pane")
        require(app.moveTab("sceneGraph", right), "move the Scene Graph panel beside the viewer")
        require(app.moveTab(SPREADSHEET, lower), "move the Geometry Spreadsheet panel beside the viewer")
        right = None
        lower = None
        ok = yield ("until", lambda: panel("SceneGraphPanel") is not None and panel("SceneGraphPanel").isVisible()
                    and panel("GeoSpreadsheetPanel") is not None and panel("GeoSpreadsheetPanel").isVisible(), POLL_TIMEOUT_S)
        require(ok, "both panels show")

        key(Qt.Key_C)
        yield ("until", lambda: prop("natronSceneViewMode") == "top", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()

        yield from breadcrumb_section()
        yield from view_section()
        yield from scene_graph_section()
        yield from spreadsheet_section()

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
