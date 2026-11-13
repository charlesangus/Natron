"""The breadcrumb bar above the 3D view.

Graph: USDRead(a generated hierarchy) -> USDPrune -> Viewer1. The hierarchy has
/geo/group_1/a_thing/mesh, /geo/group_1/b_thing, /geo/other and a path of long names. The prune
starts out removing nothing; the script then makes it remove the mesh.

Checks:
  - the bar sits above the scene view and shows only "/" with nothing selected;
  - selecting /geo/group_1/a_thing/mesh reads geo, group_1, a_thing, mesh, all present, and the
    HUD text no longer has the path;
  - a click on group_1 selects /geo/group_1, a Shift+click on other adds it, the "/" button clears;
  - a "+1" badge shows when two prims are selected;
  - pruning the mesh greys it while its ancestors stay clickable, and a click on a greyed segment
    changes nothing;
  - split to half the screen's width, a path of long names folds its middle segments into a "..."
    button whose menu lists them, first and last stay.

Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
MESH = "/geo/group_1/a_thing/mesh"
GROUP = "/geo/group_1"
OTHER = "/geo/other"
LONG_NAMES = ["a_long_segment_name_%d_for_the_bar" % i for i in range(1, 7)] + ["leaf"]
LONG_PATH = "/" + "/".join(LONG_NAMES)


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "breadcrumbs.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "geo"\n{\n')
        f.write('    def Xform "group_1"\n    {\n')
        f.write('        def Xform "a_thing"\n        {\n            def Cube "mesh"\n            {\n                double size = 1\n            }\n        }\n')
        f.write('        def Cube "b_thing"\n        {\n            double size = 1\n        }\n')
        f.write('    }\n')
        f.write('    def Cube "other"\n    {\n        double size = 1\n    }\n')
        f.write('}\n')
        f.write('def Xform "%s"\n{\n' % LONG_NAMES[0])
        indent = "    "
        for name in LONG_NAMES[1:-1]:
            f.write('%sdef Xform "%s"\n%s{\n' % (indent, name, indent))
            indent += "    "
        f.write('%sdef Cube "leaf"\n%s{\n%s    double size = 1\n%s}\n' % (indent, indent, indent, indent))
        for _ in LONG_NAMES[1:-1]:
            indent = indent[:-4]
            f.write('%s}\n' % indent)
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QToolButton

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_breadcrumbs")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    prune = app.createNode("USDPrune")
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    prune.getParam("primPattern").setValue("/nothing")
    require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
    usd.setPosition(0, 0)
    prune.setPosition(0, 100)
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
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, QStringList_arg(paths))

    def segments():
        b = bar()
        return [dict(s) for s in (b.property("natronBreadcrumbSegments") or [])] if b is not None else []

    def texts(visible_only=False):
        return [str(s["text"]) for s in segments() if not (visible_only and s["elided"])]

    def states():
        return [str(s["state"]) for s in segments()]

    def buttons(name):
        b = bar()
        return [hold(w) for w in b.findChildren(QToolButton, name) if w.isVisibleTo(b)] if b is not None else []

    def segment_button(text):
        for w in buttons("breadcrumbSegment"):
            if str(w.text()) == text:
                return w
        return None

    def click(button, modifier=Qt.NoModifier):
        QTest.mouseClick(button, Qt.LeftButton, modifier)

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def QStringList_arg(paths):
        return Q_ARG("QStringList", list(paths))

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: bar() is not None, POLL_TIMEOUT_S)
        require(ok, "the breadcrumb bar is visible with the scene view")
        v, b = view(), bar()
        check(b.mapTo(b.window(), b.rect().topLeft()).y() < v.mapTo(v.window(), v.rect().topLeft()).y(),
              "the bar is above the scene view")

        check(texts() == [] and len(buttons("breadcrumbRoot")) == 1, "nothing selected: only the \"/\" button")
        height = b.height()

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        check(ok, "the mesh's path reads geo, group_1, a_thing, mesh (got %r)" % (texts(),))
        ok = yield ("until", lambda: states() == ["present"] * 4, POLL_TIMEOUT_S)
        check(ok, "every segment is present (got %r)" % (states(),))
        check(b.height() == height, "the bar keeps its height with a selection")
        hud = str(v.property("natronSceneHud") or "")
        check(bool(hud) and "group_1" not in hud and MESH not in hud, "the HUD has no path (got %r)" % (hud,))
        shot("scene_breadcrumbs_mesh.png")

        button = segment_button("group_1")
        require(button is not None and button.isEnabled(), "the group_1 button is clickable")
        click(button)
        ok = yield ("until", lambda: selection() == [GROUP], POLL_TIMEOUT_S)
        check(ok, "a click on group_1 selects /geo/group_1 (got %r)" % (selection(),))
        ok = yield ("until", lambda: texts() == ["geo", "group_1"], POLL_TIMEOUT_S)
        check(ok, "the bar follows the selection (got %r)" % (texts(),))

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        click(segment_button("group_1"), Qt.ShiftModifier)
        ok = yield ("until", lambda: selection() == [MESH, GROUP], POLL_TIMEOUT_S)
        check(ok, "a Shift+click on group_1 adds it to the selection (got %r)" % (selection(),))
        ok = yield ("until", lambda: int(bar().property("natronBreadcrumbMore") or 0) == 1 and len(buttons("breadcrumbMore")) == 1,
                    POLL_TIMEOUT_S)
        check(ok, "a +1 badge shows for two selected prims")
        check(texts() == ["geo", "group_1"], "the bar shows the current (last) prim's path (got %r)" % (texts(),))

        click(buttons("breadcrumbRoot")[0])
        ok = yield ("until", lambda: selection() == [], POLL_TIMEOUT_S)
        check(ok, "the \"/\" button clears the selection (got %r)" % (selection(),))
        check(texts() == [] and not buttons("breadcrumbMore"), "an empty selection leaves only \"/\"")
        check(b.height() == height, "the bar keeps its height with no selection")

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        prune.getParam("primPattern").setValue(MESH)
        ok = yield ("until", lambda: states() == ["present", "present", "present", "missing"], POLL_TIMEOUT_S)
        check(ok, "pruning the mesh greys the mesh alone (got %r)" % (states(),))
        yield ("sleep", SETTLE_MS)
        mesh_button = segment_button("mesh")
        check(mesh_button is not None and not mesh_button.isEnabled(), "the greyed segment is still shown and cannot be clicked")
        check(segment_button("a_thing").isEnabled(), "the ancestors stay clickable")
        shot("scene_breadcrumbs_pruned.png")
        QTest.mouseClick(mesh_button, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        check(selection() == [MESH], "a click on the greyed segment changes nothing (got %r)" % (selection(),))

        prune.getParam("primPattern").setValue("/nothing")
        select([])
        check(not buttons("breadcrumbElision"), "a short path in a wide bar has no \"...\" button")
        home = ancestor_of_class(v, "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        split = None
        yield ("sleep", SETTLE_MS)
        select([LONG_PATH])
        ok = yield ("until", lambda: texts() == LONG_NAMES, POLL_TIMEOUT_S)
        check(ok, "the long path reads in full in the model (got %r)" % (texts(),))
        ok = yield ("until", lambda: bool(bar().property("natronBreadcrumbElided")) and len(buttons("breadcrumbElision")) == 1,
                    POLL_TIMEOUT_S)
        check(ok, "the narrow bar folds the middle into a \"...\" button")
        shown = texts(True)
        check(shown and shown[0] == LONG_NAMES[0] and shown[-1] == LONG_NAMES[-1] and len(shown) < len(LONG_NAMES),
              "the first and last segments stay (got %r)" % (shown,))
        check(segment_button(LONG_NAMES[0]) is not None and segment_button(LONG_NAMES[-1]) is not None,
              "the first and last buttons are shown")
        elision = buttons("breadcrumbElision")[0]
        menu = elision.menu()
        folded = [str(s["text"]) for s in segments() if s["elided"]]
        check(menu is not None and [str(a.text()) for a in menu.actions()] == folded and len(folded) > 0,
              "the \"...\" menu lists the folded segments (got %r)" % (folded,))
        shot("scene_breadcrumbs_elided.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
