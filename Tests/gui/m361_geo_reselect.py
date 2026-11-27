"""Backtick in the 3D view reselects for the selected node, Enter writes the selection back.

Graph: GeoPrimitive (grid, 4 x 4 faces) -> GeoDelete (primitive group 0-3) -> Viewer1, plus a GeoMerge
reading the GeoPrimitive. The view is put in the top orthographic view (C) and framed (A).

Checks:
  - with only the GeoDelete selected in the node graph, backtick shows its group: the view is in
    Primitives mode with the selection pattern equal to the group, and the Viewer meanwhile shows the
    GeoDelete's input (the GeoPrimitive) so the numbers are those on screen;
  - two more faces added with Shift+click and Enter write the extended pattern and the Primitives
    group type into the GeoDelete, leave reselect mode and put the Viewer back on the GeoDelete;
  - one Undo puts the old group back;
  - backtick again and Escape leave the group as it was and put the Viewer back;
  - with only the GeoMerge selected, which has no group, backtick does nothing and the status bar says
    the node has no group.

Shots: m361-reselect-loaded.png, m361-reselect-extended.png, m361-reselect-written.png,
m361-reselect-undone.png, m361-reselect-cancelled.png and m361-reselect-no-group.png.

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
OLD_GROUP = "0-3"
PRIMITIVES_OPTION = 1

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QMainWindow

    from guitest import check, class_name, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()
    navigation.setValue(DEFAULT_NAVIGATION_PRESET)

    grid = app.createNode("GeoPrimitive")
    require(grid is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    grid.getParam("type").setValue(1)
    grid.getParam("gridRows").setValue(4)
    grid.getParam("gridColumns").setValue(4)
    delete = app.createNode("GeoDelete")
    require(delete is not None, "create GeoDelete", plugin_problem("GeoDelete"))
    merge = app.createNode("GeoMerge")
    require(merge is not None, "create GeoMerge", plugin_problem("GeoMerge"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    grid.setPosition(0, 0)
    delete.setPosition(0, 160)
    merge.setPosition(240, 160)
    viewer.setPosition(0, 320)
    require(delete.connectInput(0, grid), "connect the GeoDelete to the GeoPrimitive")
    require(merge.connectInput(0, grid), "connect the GeoMerge to the GeoPrimitive")
    delete.getParam("groupType").setValue(PRIMITIVES_OPTION)
    delete.getParam("group").setValue(OLD_GROUP)

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

    def reselecting():
        return bool(prop("natronGeoReselecting", False))

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

    def add_face(want, modifiers=Qt.ShiftModifier):
        """Shift+clicks over the grid until the selection has want faces."""
        v = view()
        w, h = v.width(), v.height()
        for fy in (0.4, 0.5, 0.6, 0.35, 0.65, 0.45, 0.55):
            for fx in (0.4, 0.5, 0.6, 0.35, 0.65, 0.45, 0.55):
                click(QPoint(int(w * fx), int(h * fy)), modifiers)
                ok = yield ("until", lambda: count() == want, PICK_TIMEOUT_S)
                if ok:
                    return True
        return False

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui":
                return w
        return None

    def status_text():
        win = main_window()
        return win.statusBar().currentMessage() if win is not None and win.statusBar() is not None else ""

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

    def input_name(node, i=0):
        inp = node.getInput(i) if node is not None else None
        return inp.getScriptName() if inp is not None else None

    def select_only(node):
        app.selectNode(node, True)
        QApplication.processEvents()

    def viewer_shows(node):
        return lambda: input_name(viewer) == node.getScriptName()

    def steps():
        require(viewer.connectInput(0, delete), "connect the Viewer to the GeoDelete")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the GeoDelete output (%r)" % (prop("natronSceneError"),))
        key(Qt.Key_C)
        yield ("until", lambda: prop("natronSceneViewMode") == "top", PICK_TIMEOUT_S)
        key(Qt.Key_A)
        yield settle()

        select_only(delete)
        key(Qt.Key_QuoteLeft)
        ok = yield ("until", lambda: reselecting() and prop("natronGeoPickMode") == "primitives" and pattern() == OLD_GROUP,
                    POLL_TIMEOUT_S)
        check(ok, "backtick with the GeoDelete selected shows its group in Primitives mode (mode %r, selection %r)"
              % (prop("natronGeoPickMode"), pattern()))
        require(ok, "reselect mode is on")
        check(viewer_shows(grid)(), "the Viewer shows the GeoDelete's input meanwhile (its input is %r)" % (input_name(viewer),))
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield settle()
        shot("m361-reselect-loaded.png")

        added = yield from add_face(5)
        require(added, "a Shift+click adds a face (%d selected)" % count())
        added = yield from add_face(6)
        require(added, "a second Shift+click adds another face (%d selected)" % count())
        extended = pattern()
        report("NOTE extended selection pattern: %r" % (extended,))
        check(extended != OLD_GROUP, "the selection pattern changed (%r)" % (extended,))
        yield settle()
        shot("m361-reselect-extended.png")

        key(Qt.Key_Return)
        ok = yield ("until", lambda: delete.getParam("group").getValue() == extended, PICK_TIMEOUT_S)
        check(ok, "Enter writes the extended pattern into the GeoDelete's Group (%r, expected %r)"
              % (delete.getParam("group").getValue(), extended))
        check(delete.getParam("groupType").getValue() == PRIMITIVES_OPTION,
              "and the Group Type is Primitives (option %d)" % (delete.getParam("groupType").getValue(),))
        check(not reselecting(), "reselect mode is off")
        ok = yield ("until", viewer_shows(delete), POLL_TIMEOUT_S)
        check(ok, "the Viewer shows the GeoDelete again (its input is %r)" % (input_name(viewer),))
        yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield settle()
        shot("m361-reselect-written.png")

        action = edit_menu_action("undo")
        require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
        action.trigger()
        QApplication.processEvents()
        ok = yield ("until", lambda: delete.getParam("group").getValue() == OLD_GROUP, POLL_TIMEOUT_S)
        check(ok, "one Undo puts the old Group back (%r)" % (delete.getParam("group").getValue(),))
        yield settle()
        shot("m361-reselect-undone.png")

        select_only(delete)
        key(Qt.Key_QuoteLeft)
        ok = yield ("until", lambda: reselecting() and pattern() == OLD_GROUP, POLL_TIMEOUT_S)
        require(ok, "backtick reselects again (selection %r)" % (pattern(),))
        added = yield from add_face(5)
        check(added, "a face is added (%d selected)" % count())
        key(Qt.Key_Escape)
        ok = yield ("until", lambda: not reselecting(), PICK_TIMEOUT_S)
        check(ok, "Escape leaves reselect mode")
        check(delete.getParam("group").getValue() == OLD_GROUP, "and leaves the Group as it was (%r)" % (delete.getParam("group").getValue(),))
        ok = yield ("until", viewer_shows(delete), POLL_TIMEOUT_S)
        check(ok, "the Viewer shows the GeoDelete again (its input is %r)" % (input_name(viewer),))
        yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield settle()
        shot("m361-reselect-cancelled.png")

        select_only(merge)
        key(Qt.Key_QuoteLeft)
        yield ("sleep", SETTLE_MS)
        check(not reselecting(), "backtick with only the GeoMerge selected does not start reselecting")
        check(viewer_shows(delete)(), "and the Viewer is unchanged (its input is %r)" % (input_name(viewer),))
        message = status_text()
        check("no group" in message, "the status bar says the node has no group (%r)" % (message,))
        shot("m361-reselect-no-group.png")

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
