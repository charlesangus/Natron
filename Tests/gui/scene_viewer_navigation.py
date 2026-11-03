"""Mouse navigation in the scene view follows the Viewer 3D Navigation preference and the 3DViewer
bindings of the Shortcut Editor.

Graph: USDRead(scene-reference.usda) into Viewer1. For each preset (Nuke, Maya, Blender, Houdini)
the script drags with every tumble, pan and dolly binding of that preset and reads the free
camera from the view's natronSceneFreeCamera property (pivot x, y, z, distance, azimuth,
elevation, field of view): a tumble changes only the angles, a pan only the pivot, a dolly only
the distance. Houdini's Space bindings hold Space on the view during the drag. In every preset the
wheel dollies, and a plain left drag over empty space (a marquee) leaves the camera alone.

Then, under the Blender preset, the Shortcut Editor's 3DViewer > Tumble row is rebound to Alt+left
by clicking into its binding field: the preference turns to Custom, Alt+left tumbles and the
middle button no longer does. Choosing Nuke again brings back Ctrl+left.

Mouse and key events are sent to the SceneViewport (synthesized window-system events do not reach
Natron's widgets under Xvfb). Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go
to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
NUKE, MAYA, BLENDER, HOUDINI, CUSTOM = 0, 1, 2, 3, 4
SPACE = "space"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPushButton, QTreeWidget

    from guitest import (FIXTURES, check, finish, menu_of, plugin_problem, report, require, run, section, shot,
                         viewer_up, widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
    require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
    original_navigation = navigation.getValue()

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    filename = usd.getParam("filename")
    require(filename is not None, "USDRead has a filename parameter")
    filename.setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    viewer.setPosition(0, 150)

    L, M, R = Qt.LeftButton, Qt.MiddleButton, Qt.RightButton
    CTRL, ALT, SHIFT, NONE = Qt.ControlModifier, Qt.AltModifier, Qt.ShiftModifier, Qt.NoModifier
    # Per preset: (role, button, modifiers or SPACE).
    PRESETS = [
        ("Nuke", NUKE, [("tumble", L, CTRL), ("pan", L, ALT), ("pan", M, NONE), ("dolly", M, ALT)]),
        ("Maya", MAYA, [("tumble", L, ALT), ("pan", M, ALT), ("dolly", R, ALT)]),
        ("Blender", BLENDER, [("tumble", M, NONE), ("pan", M, SHIFT), ("dolly", M, CTRL)]),
        ("Houdini", HOUDINI, [("tumble", L, SPACE), ("pan", M, SPACE), ("dolly", R, SPACE),
                              ("tumble", L, ALT), ("pan", M, ALT), ("dolly", R, ALT)]),
    ]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError") and v.property("natronSceneFreeCamera") is not None)

    def camera():
        v = view()
        value = v.property("natronSceneFreeCamera") if v is not None else None
        return [float(x) for x in value] if value else None

    def differs(a, b, indices):
        return any(abs(a[i] - b[i]) > 1e-6 * max(1., abs(a[i])) for i in indices)

    PIVOT, DISTANCE, ANGLES = (0, 1, 2), (3,), (4, 5)

    def moved(before, after):
        """Which parts of the camera changed: a subset of pivot, distance and angles."""
        if before is None or after is None:
            return None
        parts = set()
        if differs(before, after, PIVOT):
            parts.add("pivot")
        if differs(before, after, DISTANCE):
            parts.add("distance")
        if differs(before, after, ANGLES):
            parts.add("angles")
        return parts

    EXPECTED = {"tumble": {"angles"}, "pan": {"pivot"}, "dolly": {"distance"}}

    def describe(button, modifiers):
        name = {L: "left", M: "middle", R: "right"}[button]
        if modifiers is SPACE:
            return "Space+" + name
        prefix = "".join(n + "+" for m, n in ((CTRL, "Ctrl"), (ALT, "Alt"), (SHIFT, "Shift")) if modifiers & m)
        return prefix + name

    def mouse(v, kind, pos, button, buttons, modifiers=NONE):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, modifiers)
        QApplication.sendEvent(v, event)

    def focus(v):
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()

    def drag(v, button, modifiers, start=None, delta=QPoint(60, 40)):
        start = start if start is not None else v.rect().center()
        space = modifiers is SPACE
        mods = NONE if space else modifiers
        if space:
            focus(v)
            QTest.keyPress(v, Qt.Key_Space)
        mouse(v, QEvent.MouseButtonPress, start, button, button, mods)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + delta * (i / 10.), Qt.NoButton, button, mods)
        mouse(v, QEvent.MouseButtonRelease, start + delta, button, Qt.NoButton, mods)
        if space:
            QTest.keyRelease(v, Qt.Key_Space)
        QApplication.processEvents()

    def wheel(v, modifiers=NONE):
        centre = QPointF(v.rect().center())
        event = QWheelEvent(centre, QPointF(v.mapToGlobal(v.rect().center())), QPoint(0, 0), QPoint(0, 120),
                            Qt.NoButton, modifiers, Qt.NoScrollPhase, False)
        QApplication.sendEvent(v, event)
        QApplication.processEvents()

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def frame_all(v):
        focus(v)
        QTest.keyClick(v, Qt.Key_A)
        QApplication.processEvents()

    def find_item(tree, text, parent_text=None):
        # Model indexes, not QTreeWidgetItem wrappers: the Preferences trees' item wrappers
        # report their C++ object deleted by the time they are used (see m45_tab_menu.py).
        model = tree.model()
        for idx in model.match(model.index(0, 0), Qt.DisplayRole, text, -1, Qt.MatchExactly | Qt.MatchRecursive):
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

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))

        for name, preset, bindings in PRESETS:
            section(name)
            navigation.setValue(preset)
            QApplication.processEvents()
            frame_all(v)
            yield settle()
            for role, button, modifiers in bindings:
                before = camera()
                drag(v, button, modifiers)
                yield settle()
                parts = moved(before, camera())
                check(parts == EXPECTED[role], "%s drag %ss the camera (changed %r)" % (describe(button, modifiers), role, parts))
            shot("scene-navigation-%s.png" % name.lower())

            before = camera()
            wheel(v)
            yield settle()
            parts = moved(before, camera())
            check(parts == {"distance"}, "the wheel dollies the camera (changed %r)" % (parts,))

            before = camera()
            drag(v, L, NONE, start=QPoint(2, 2), delta=QPoint(30, 20))
            yield settle()
            check(moved(before, camera()) == set(), "a plain left drag over empty space leaves the camera alone")

        section("rebind")
        navigation.setValue(BLENDER)
        QApplication.processEvents()
        panel = open_preferences()
        require(panel is not None, "the Preferences panel opens")
        page_trees = [w for w in panel.findChildren(QTreeWidget) if w.columnCount() == 1]
        page = find_item(page_trees[0], "Shortcut Editor") if len(page_trees) == 1 else None
        require(page is not None, "found the Shortcut Editor page")
        page_trees[0].setCurrentIndex(page)
        QApplication.processEvents()
        shortcut_trees = [w for w in panel.findChildren(QTreeWidget)
                          if w.columnCount() == 3 and w.headerItem() is not None and w.headerItem().text(0) == "Command"]
        require(len(shortcut_trees) == 1, "found the shortcuts tree")
        tree = shortcut_trees[0]
        group = find_item(tree, "3DViewer")
        require(group is not None, "the shortcuts tree has a 3DViewer group")
        tree.expand(group)
        tumble = find_item(tree, "Tumble", "3DViewer")
        require(tumble is not None, "the 3DViewer group has a Tumble row")
        tree.setCurrentIndex(tumble)
        tree.scrollTo(tumble)
        QApplication.processEvents()
        report("NOTE Tumble under Blender: %r" % (tumble.siblingAtColumn(1).data(),))
        check("Middle" in tumble.siblingAtColumn(1).data(), "the Tumble row shows the Blender binding")
        yield ("sleep", 300)
        shot("scene-navigation-shortcut-editor.png")

        editors = [w for w in panel.findChildren(QLineEdit)
                   if w.isVisible() and w.placeholderText().startswith("Click with modifiers")]
        validate = [w for w in panel.findChildren(QPushButton) if w.text() == "Validate" and w.isVisible()]
        require(len(editors) == 1 and len(validate) == 1, "found the Tumble binding field and its Validate button")
        editor = editors[0]
        centre = editor.rect().center()
        mouse(editor, QEvent.MouseButtonPress, centre, L, L, ALT)
        mouse(editor, QEvent.MouseButtonRelease, centre, L, Qt.NoButton, ALT)
        QApplication.processEvents()
        report("NOTE binding field after Alt+click: %r" % (editor.text(),))
        check("Alt" in editor.text(), "an Alt+click in the field records Alt+left")
        validate[0].click()
        QApplication.processEvents()
        check(navigation.getValue() == CUSTOM, "editing a 3DViewer binding turns the preference to Custom (got %r)" % (navigation.getValue(),))
        QApplication.processEvents()
        tumble = find_item(tree, "Tumble", "3DViewer")
        require(tumble is not None, "the 3DViewer group still has a Tumble row after the rebind")
        check("Alt" in tumble.siblingAtColumn(1).data(), "the Tumble row shows the new binding (%r)" % (tumble.siblingAtColumn(1).data(),))
        shot("scene-navigation-rebound.png")
        panel.close()
        QApplication.processEvents()

        frame_all(v)
        yield settle()
        before = camera()
        drag(v, L, ALT)
        yield settle()
        parts = moved(before, camera())
        check(parts == {"angles"}, "after the rebind, an Alt+left drag tumbles (changed %r)" % (parts,))
        before = camera()
        drag(v, M, NONE)
        yield settle()
        parts = moved(before, camera())
        check(parts == set(), "after the rebind, a middle drag no longer tumbles (changed %r)" % (parts,))

        navigation.setValue(NUKE)
        QApplication.processEvents()
        before = camera()
        drag(v, L, CTRL)
        yield settle()
        parts = moved(before, camera())
        check(parts == {"angles"}, "choosing Nuke again brings back Ctrl+left tumble (changed %r)" % (parts,))
        before = camera()
        drag(v, L, ALT)
        yield settle()
        parts = moved(before, camera())
        check(parts == {"pivot"}, "and Alt+left pans again (changed %r)" % (parts,))

        navigation.setValue(original_navigation)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
