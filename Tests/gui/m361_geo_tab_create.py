"""Tab in the 3D view showing geometry: create a geometry node from the component selection.

Graph: GeoPrimitive (grid, 10 x 10 faces, 121 points) -> Viewer1. The view is put in the top
orthographic view (C) and framed (A), so the grid is a square centred in it and its centre is a
point.

Checks:
  - Points: a click at the centre selects one point and Grow (Ctrl+Up) gives it and its 4
    neighbours; Convert to Edges (Ctrl+F10) gives the 4 edges between them;
  - Tab on the view opens the tab menu, which lists GeoTransform and GeoSubdivide and leaves out
    Blur and USDPrune, which don't take geometry;
  - choosing GeoTransform creates it between the GeoPrimitive and the Viewer (its input is the
    GeoPrimitive, the Viewer's input is the GeoTransform), with Group holding the edge path the
    view shows (natronGeoSelection) and Group Type set to Edges;
  - one Undo removes the GeoTransform and puts the Viewer back on the GeoPrimitive;
  - Grow again (13 points), Convert to Primitives (F11) gives the 4 faces whose corners are all
    selected, recorded as their pattern, and Points mode (F9) turns them into their 9 corners;
  - Tab and GeoSubdivide from those 9 points creates it between the GeoPrimitive and the Viewer,
    with Group holding the 4 faces' pattern: points promote to the primitives they entirely cover;
  - one Undo removes the GeoSubdivide and puts the Viewer back on the GeoPrimitive.

Shots: m361-tab-menu.png (the menu over the view), m361-tab-transform.png,
m361-tab-transform-undone.png, m361-tab-subdivide.png and m361-tab-subdivide-undone.png.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
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
MODES = ("objects", "points", "edges", "primitives")
TRANSFORM_ID = "GeoTransform"
SUBDIVIDE_ID = "GeoSubdivide"
# The index of the Edges option of GeoTransform's Group Type.
TRANSFORM_EDGES = 2

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMainWindow

    from guitest import (check, class_name, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up,
                         widgets_of)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    grid = app.createNode("GeoPrimitive")
    require(grid is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(10)
    grid.getParam("gridColumns").setValue(10)

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    grid.setPosition(0, 0)
    viewer.setPosition(0, 220)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def count():
        return int(prop("natronGeoSelectionCount", 0))

    def pattern():
        return str(prop("natronGeoSelection", ""))

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def mouse(v, kind, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers))

    def click(pos, modifiers=Qt.NoModifier):
        v = view()
        mouse(v, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        mouse(v, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)
        QApplication.processEvents()

    def key(k, modifiers=Qt.NoModifier):
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k, modifiers)
        QApplication.processEvents()

    def pick_until(positions, want, modifiers=Qt.NoModifier):
        """Clicks each position in turn until want() holds after the click is served."""
        for pos in positions:
            click(pos, modifiers)
            ok = yield ("until", want, PICK_TIMEOUT_S)
            if ok:
                return pos
        return None

    def around(pos, radius=6, step=3):
        yield pos
        for r in range(step, radius + 1, step):
            for dx, dy in ((-r, r), (-r, 0), (0, r), (r, r), (r, 0), (0, -r), (-r, -r), (r, -r), (-r, r)):
                yield pos + QPoint(dx, dy)

    def set_mode(index):
        key((Qt.Key_F8, Qt.Key_F9, Qt.Key_F10, Qt.Key_F11)[index])
        return ("until", lambda: prop("natronGeoPickMode") == MODES[index], PICK_TIMEOUT_S)

    def selection_is(n, mode):
        return lambda: count() == n and prop("natronGeoPickMode") == mode

    def act(k, modifiers, want, label):
        key(k, modifiers)
        ok = yield ("until", want, PICK_TIMEOUT_S)
        check(ok, label)
        return ok

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

    def open_menu():
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, Qt.Key_Tab)
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

    def check_filtered(line_edit, list_view):
        geo = [row_name(e) for e in listed(line_edit, list_view, "Geo")]
        report("NOTE the menu lists for 'Geo': %r" % (geo,))
        check(TRANSFORM_ID in geo and SUBDIVIDE_ID in geo, "the menu offers GeoTransform and GeoSubdivide (got %r)" % (geo,))
        for text in ("Blur", "USDPrune"):
            got = [row_name(e) for e in listed(line_edit, list_view, text)]
            check(text not in got, "the menu leaves out %s, which takes no geometry (got %r)" % (text, got))

    def create_from_menu(plugin_id, menu_shot=None):
        """Presses Tab on the view and chooses plugin_id; the node created, or None."""
        names = script_names()
        popup, line_edit, list_view = yield from open_menu()
        if not check(popup is not None, "Tab on the 3D view opens the tab menu"):
            return None
        if menu_shot:
            check_filtered(line_edit, list_view)
        entries = listed(line_edit, list_view, plugin_id)
        row = next((r for r, e in enumerate(entries) if row_name(e) == plugin_id), -1)
        if menu_shot:
            shot(menu_shot)
        if not check(row >= 0, "the menu offers %s (got %r)" % (plugin_id, entries)):
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()
            return None
        list_view.setCurrentIndex(list_view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)
        ok = yield ("until", lambda: len(script_names() - names) > 0, POLL_TIMEOUT_S)
        if not ok:
            return None
        return next(n for n in app.getChildren() if n.getScriptName() not in names)

    def check_inserted(node, label):
        check(input_name(node) == grid.getScriptName(),
              "the %s reads the GeoPrimitive (got %r)" % (label, input_name(node)))
        ok = yield ("until", lambda: input_name(viewer) == node.getScriptName(), POLL_TIMEOUT_S)
        check(ok, "the Viewer now shows the %s (its input is %r)" % (label, input_name(viewer)))
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        check(ok, "the view draws the %s's output (%r)" % (label, prop("natronSceneError")))

    def undo_insertion(name, label):
        action = edit_menu_action("undo")
        require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
        action.trigger()
        QApplication.processEvents()
        ok = yield ("until", lambda: name not in script_names(), POLL_TIMEOUT_S)
        check(ok, "one Undo removes the %s" % (label,))
        ok = yield ("until", lambda: input_name(viewer) == grid.getScriptName(), POLL_TIMEOUT_S)
        check(ok, "and puts the Viewer back on the GeoPrimitive (its input is %r)" % (input_name(viewer),))
        yield ("until", scene_drawn, POLL_TIMEOUT_S)

    def steps():
        require(viewer.connectInput(0, grid), "connect the Viewer to the GeoPrimitive")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the grid (%r)" % (prop("natronSceneError"),))
        key(Qt.Key_C)
        yield ("until", lambda: prop("natronSceneViewMode") == "top", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()

        ok = yield set_mode(1)
        require(ok, "Points mode is on")
        yield settle()
        centre = QPoint(view().width() // 2, view().height() // 2)
        hit = yield from pick_until(around(centre), lambda: count() == 1)
        require(hit is not None, "a click at the grid's centre selects one point (%r)" % pattern())
        ok = yield from act(Qt.Key_Up, Qt.ControlModifier, selection_is(5, "points"), "Grow gives the point and its 4 neighbours")
        require(ok, "5 points are selected")
        ok = yield from act(Qt.Key_F10, Qt.ControlModifier, selection_is(4, "edges"),
                            "Convert to Edges gives the 4 edges between the 5 points")
        require(ok, "4 edges are selected")
        edge_path = pattern()
        report("NOTE edge selection pattern: %r" % (edge_path,))
        check(edge_path.startswith("p"), "the edge selection is a point path (%r)" % (edge_path,))

        transform = yield from create_from_menu(TRANSFORM_ID, "m361-tab-menu.png")
        require(transform is not None and transform.getPluginID().endswith(TRANSFORM_ID), "Tab and GeoTransform create a GeoTransform")
        yield from check_inserted(transform, "GeoTransform")
        group = transform.getParam("group").getValue()
        check(group == edge_path, "the GeoTransform's Group is the edge path (%r, selection %r)" % (group, edge_path))
        group_type = transform.getParam("groupType").getValue()
        check(group_type == TRANSFORM_EDGES, "the GeoTransform's Group Type is Edges (option %d)" % (group_type,))
        yield settle()
        shot("m361-tab-transform.png")
        yield from undo_insertion(transform.getScriptName(), "GeoTransform")
        yield settle()
        shot("m361-tab-transform-undone.png")

        ok = yield set_mode(1)
        require(ok, "Points mode is on again")
        ok = yield ("until", lambda: count() == 5, PICK_TIMEOUT_S)
        require(ok, "the 4 edges give their 5 points (%d)" % count())
        ok = yield from act(Qt.Key_Up, Qt.ControlModifier, selection_is(13, "points"), "Grow gives the 13-point diamond")
        require(ok, "13 points are selected")
        ok = yield set_mode(3)
        require(ok and count() == 4, "in Primitives mode the 4 fully covered faces remain (%d)" % count())
        faces = pattern()
        report("NOTE face selection pattern: %r" % (faces,))
        ok = yield set_mode(1)
        require(ok and count() == 9, "back in Points mode they give their 9 corners (%d)" % count())

        subdivide = yield from create_from_menu(SUBDIVIDE_ID)
        require(subdivide is not None and subdivide.getPluginID().endswith(SUBDIVIDE_ID), "Tab and GeoSubdivide create a GeoSubdivide")
        yield from check_inserted(subdivide, "GeoSubdivide")
        group = subdivide.getParam("group").getValue()
        check(group == faces, "the GeoSubdivide's Group holds the 4 faces the 9 points cover (%r, expected %r)" % (group, faces))
        check(prop("natronGeoPickMode") == "points", "the view stays in Points mode (%r)" % (prop("natronGeoPickMode"),))
        yield settle()
        shot("m361-tab-subdivide.png")
        yield from undo_insertion(subdivide.getScriptName(), "GeoSubdivide")
        yield settle()
        shot("m361-tab-subdivide-undone.png")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
