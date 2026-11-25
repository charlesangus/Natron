"""Pickwalk keys of the 3D view: Alt+Left parent, Alt+Right child, Alt+Up and Alt+Down siblings.

Graph: USDRead(a generated hierarchy) -> Viewer1. The hierarchy has /geo/group_1/a_thing/mesh,
/geo/group_1/b_thing and /geo/group_2/c_thing.

Checks:
  - from the mesh, Alt+Left twice selects /geo/group_1, and Alt+Right goes back to a_thing, the
    child the walk came from;
  - Alt+Down from group_1 selects group_2, and again wraps to group_1; Alt+Up wraps to group_2;
  - Alt+Left on the top-level /geo and Alt+Right on the leaf mesh leave the selection alone;
  - with the mesh and /geo/group_2/c_thing selected, Alt+Left selects a_thing and group_2, and a
    second one selects group_1 and /geo, the current prim staying last; with a_thing and b_thing
    selected, Alt+Left merges them into their one parent, group_1;
  - a click on the breadcrumb bar's background leaves the focus with the 3D view, whose keys walk;
  - the four Pickwalk rows are in the Shortcut Editor's 3DViewer group with their Alt bindings, and
    rebinding Pickwalk Parent to Alt+Shift+Left moves the walk to the new key and frees Alt+Left.

Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
MESH = "/geo/group_1/a_thing/mesh"
THING = "/geo/group_1/a_thing"
GROUP_1 = "/geo/group_1"
GROUP_2 = "/geo/group_2"
C_THING = "/geo/group_2/c_thing"
GEO = "/geo"


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "pickwalk.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "geo"\n{\n')
        f.write('    def Xform "group_1"\n    {\n')
        f.write('        def Xform "a_thing"\n        {\n            def Cube "mesh"\n            {\n                double size = 1\n            }\n        }\n')
        f.write('        def Cube "b_thing"\n        {\n            double size = 1\n        }\n')
        f.write('    }\n')
        f.write('    def Xform "group_2"\n    {\n        def Cube "c_thing"\n        {\n            double size = 1\n        }\n    }\n')
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, Qt
    from PySide6.QtTest import QTest
    from shiboken6 import getCppPointer
    from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPushButton, QTreeWidget

    from guitest import OUT, check, finish, menu_of, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_pickwalk")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
    usd.setPosition(0, 0)
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def bar():
        bars = [w for w in widgets_of("SceneBreadcrumbBar") if w.isVisible()]
        return hold(bars[0]) if bars else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def selection():
        v = view()
        return [str(p) for p in (v.property("natronSceneSelection") or [])] if v is not None else []

    def select(paths):
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, Q_ARG("QStringList", list(paths)))

    def press(key, modifiers=Qt.AltModifier, widget=None):
        QTest.keyClick(widget or view(), key, modifiers)

    def find_item(tree, text, parent_text=None):
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

    def walk(label, key, expected, modifiers=Qt.AltModifier, widget=None):
        press(key, modifiers, widget)
        ok = yield ("until", lambda: selection() == expected, POLL_TIMEOUT_S)
        check(ok, "%s (got %r, wanted %r)" % (label, selection(), expected))

    def stays(label, key, expected):
        press(key)
        yield ("sleep", 400)
        check(selection() == expected, "%s (got %r)" % (label, selection()))

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        v = view()
        v.setFocus()
        QApplication.processEvents()

        select([MESH])
        ok = yield ("until", lambda: selection() == [MESH], POLL_TIMEOUT_S)
        yield from walk("Alt+Left from the mesh selects a_thing", Qt.Key_Left, [THING])
        yield from walk("Alt+Left again selects group_1", Qt.Key_Left, [GROUP_1])
        yield from walk("Alt+Right goes back to a_thing, the remembered child", Qt.Key_Right, [THING])
        yield from walk("Alt+Right again goes back to the mesh", Qt.Key_Right, [MESH])
        yield from stays("Alt+Right on the leaf mesh leaves it selected", Qt.Key_Right, [MESH])

        select([GROUP_1])
        ok = yield ("until", lambda: selection() == [GROUP_1], POLL_TIMEOUT_S)
        yield from walk("Alt+Down from group_1 selects group_2", Qt.Key_Down, [GROUP_2])
        yield from walk("Alt+Down from the last sibling wraps to group_1", Qt.Key_Down, [GROUP_1])
        yield from walk("Alt+Up from the first sibling wraps to group_2", Qt.Key_Up, [GROUP_2])
        yield from walk("Alt+Right with nothing remembered selects the first child", Qt.Key_Right, [C_THING])

        select([GEO])
        ok = yield ("until", lambda: selection() == [GEO], POLL_TIMEOUT_S)
        yield from stays("Alt+Left on a top-level prim leaves it selected", Qt.Key_Left, [GEO])

        select([MESH, C_THING])
        ok = yield ("until", lambda: selection() == [MESH, C_THING], POLL_TIMEOUT_S)
        yield from walk("Alt+Left walks every selected prim, the current one last", Qt.Key_Left, [THING, GROUP_2])
        yield from walk("a second Alt+Left walks both again", Qt.Key_Left, [GROUP_1, GEO])
        select([THING, MESH])
        ok = yield ("until", lambda: selection() == [THING, MESH], POLL_TIMEOUT_S)
        yield from walk("Alt+Left on a_thing and its child the mesh walks both", Qt.Key_Left, [GROUP_1, THING])
        select([THING, "/geo/group_1/b_thing"])
        ok = yield ("until", lambda: selection() == [THING, "/geo/group_1/b_thing"], POLL_TIMEOUT_S)
        yield from walk("Alt+Left on two siblings merges them into their one parent", Qt.Key_Left, [GROUP_1])

        select([MESH])
        ok = yield ("until", lambda: selection() == [MESH], POLL_TIMEOUT_S)
        b = bar()
        require(b is not None, "the breadcrumb bar is visible")
        # The view had the focus from the start, so it is taken away for the click to have to give it
        # back. The window may not be the active one under Xvfb, so the focus is read from the window
        # rather than the application, and the key goes to the widget that holds it.
        view().clearFocus()
        background = QPoint(b.width() - 4, b.height() // 2)
        require(b.childAt(background) is None, "the bar's right end is its bare background")
        QTest.mouseClick(b, Qt.LeftButton, Qt.NoModifier, background)
        QApplication.processEvents()
        # The focus widget's wrapper dies with the window's, so both are held, and the view is found
        # again rather than trusted from before the click.
        window = hold(bar().window())
        focused = hold(window.focusWidget())
        focused_class = str(focused.metaObject().className()) if focused is not None else None
        current_view = view()
        check(focused is not None and current_view is not None and getCppPointer(focused) == getCppPointer(current_view),
              "a click on the bar's background gives the 3D view the focus (got %r)" % (focused_class,))
        if focused is not None:
            yield from walk("Alt+Left with the bar clicked walks", Qt.Key_Left, [THING], widget=focused)

        panel = open_preferences()
        require(panel is not None, "the Preferences panel opens")
        page_trees = [w for w in panel.findChildren(QTreeWidget) if w.columnCount() == 1]
        page = find_item(page_trees[0], "Shortcut Editor") if len(page_trees) == 1 else None
        require(page is not None, "found the Shortcut Editor page")
        page_trees[0].setCurrentIndex(page)
        QApplication.processEvents()
        trees = [w for w in panel.findChildren(QTreeWidget)
                 if w.columnCount() == 3 and w.headerItem() is not None and w.headerItem().text(0) == "Command"]
        require(len(trees) == 1, "found the shortcuts tree")
        tree = trees[0]
        group = find_item(tree, "3DViewer")
        require(group is not None, "the shortcuts tree has a 3DViewer group")
        tree.expand(group)
        wanted = [("Pickwalk Parent", "Left"), ("Pickwalk Child", "Right"),
                  ("Pickwalk Previous Sibling", "Up"), ("Pickwalk Next Sibling", "Down")]
        for name, arrow in wanted:
            row = find_item(tree, name, "3DViewer")
            check(row is not None and "Alt" in str(row.siblingAtColumn(1).data()) and arrow in str(row.siblingAtColumn(1).data()),
                  "the 3DViewer group lists %s on Alt+%s (got %r)" % (name, arrow, row.siblingAtColumn(1).data() if row else None))
        row = find_item(tree, "Pickwalk Parent", "3DViewer")
        require(row is not None, "the Pickwalk Parent row exists")
        tree.setCurrentIndex(row)
        tree.scrollTo(row)
        QApplication.processEvents()
        yield ("sleep", 300)
        shot("scene_pickwalk_shortcuts.png")
        fields = [w for w in panel.findChildren(QLineEdit) if w.isVisible() and w.placeholderText() == "Type to set shortcut"]
        validate = [w for w in panel.findChildren(QPushButton) if w.text() == "Validate" and w.isVisible()]
        require(len(fields) == 1 and len(validate) == 1, "found the primary binding field and its Validate button")
        field = fields[0]
        field.setFocus()
        field.clear()
        QTest.keyClick(field, Qt.Key_Left, Qt.AltModifier | Qt.ShiftModifier)
        QApplication.processEvents()
        report("NOTE binding field after Alt+Shift+Left: %r" % (field.text(),))
        validate[0].click()
        QApplication.processEvents()
        row = find_item(tree, "Pickwalk Parent", "3DViewer")
        check(row is not None and "Shift" in str(row.siblingAtColumn(1).data()),
              "the Pickwalk Parent row shows the new binding (got %r)" % (row.siblingAtColumn(1).data() if row else None))
        panel.close()
        QApplication.processEvents()

        view().setFocus()
        QApplication.processEvents()
        select([MESH])
        ok = yield ("until", lambda: selection() == [MESH], POLL_TIMEOUT_S)
        yield from stays("the old Alt+Left no longer walks", Qt.Key_Left, [MESH])
        yield from walk("the rebound Alt+Shift+Left walks to the parent", Qt.Key_Left, [THING], Qt.AltModifier | Qt.ShiftModifier)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
