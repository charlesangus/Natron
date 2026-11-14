"""Dragging paths from the breadcrumb bar onto prim path and pattern knobs.

Graph: USDRead(a small hierarchy with /geo/group_1 and /geo/group_2) -> USDPrune -> Viewer1, with the
USDPrune's settings panel open and a USDScopePrim beside it. The script asks the bar for the mime data
a drag of a segment and of the "+N" badge would carry, and sends drag-enter, drag-move and drop events
carrying it to the knobs' fields (synthesized window-system drags do not reach Natron's widgets under
Xvfb).

Checks:
  - the group_1 segment's mime data carries /geo/group_1 under application/x-natron-prim-paths and as
    text, and dropped on the USDPrune's Prim Pattern field it writes /geo/group_1;
  - with group_1 and group_2 selected, the badge's mime data carries both lines, in the format the Scene
    Graph panel uses, and dropped on the Prim Pattern field it writes both paths;
  - the same mime data dropped on the USDScopePrim's Prim Path field writes the first path only.

Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
MIME = "application/x-natron-prim-paths"
GROUP = "/geo/group_1"
SIBLING = "/geo/group_2"
MARKER = "/NatronDropTarget"
MARKER_PATH = "/NatronDropPathTarget"


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "breadcrumbs_drag.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "geo"\n{\n')
        f.write('    def Xform "group_1"\n    {\n        def Cube "a_thing"\n        {\n            double size = 1\n        }\n    }\n')
        f.write('    def Xform "group_2"\n    {\n        def Cube "c_thing"\n        {\n            double size = 1\n        }\n    }\n')
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QMetaObject, QMimeData, QPointF, Qt
    from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
    from PySide6.QtWidgets import QApplication, QToolButton

    from guitest import OUT, check, finish, plugin_problem, require, run, viewer_up, widgets_of

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_breadcrumbs_drag")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
    prune = app.createNode("USDPrune", -1, None, props)
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    pattern = prune.getParam("primPattern")
    require(pattern is not None, "USDPrune has a primPattern parameter")
    pattern.setValue(MARKER)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
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

    def select(paths):
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, Q_ARG("QStringList", list(paths)))

    def segment_texts():
        b = bar()
        return [str(s["text"]) for s in (b.property("natronBreadcrumbSegments") or [])] if b is not None else []

    def segment_states():
        b = bar()
        return [str(s["state"]) for s in (b.property("natronBreadcrumbSegments") or [])] if b is not None else []

    def badge_visible():
        b = bar()
        return b is not None and any(w.isVisibleTo(b) for w in b.findChildren(QToolButton, "breadcrumbMore"))

    def mime_for(segment_path):
        b = bar()
        QMetaObject.invokeMethod(b, "exposeMimeData", Qt.DirectConnection, Q_ARG("QString", segment_path))
        mime = b.findChild(QMimeData, "breadcrumbMime")
        return hold(mime)

    def field_showing(text):
        """The line edit showing text in an open settings panel (KnobLineEdit has no Q_OBJECT)."""
        for w in QApplication.allWidgets():
            try:
                if w.inherits("QLineEdit") and w.isVisible() and w.text() == text:
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def send_drag(widget, mime):
        """Sends drag-enter, drag-move and a drop; returns whether the move was accepted."""
        pos = widget.rect().center()
        enter = QDragEnterEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(widget, enter)
        move = QDragMoveEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(widget, move)
        accepted = enter.isAccepted() and move.isAccepted()
        if accepted:
            QApplication.sendEvent(widget, QDropEvent(QPointF(pos), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier))
        QApplication.processEvents()
        return accepted

    def carried(mime):
        return bytes(mime.data(MIME)).decode("utf-8").split("\n")

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: bar() is not None, POLL_TIMEOUT_S)
        require(ok, "the breadcrumb bar is visible")
        ok = yield ("until", lambda: field_showing(MARKER) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDPrune's Prim Pattern field is shown")

        scope = app.createNode("USDScopePrim", -1, None, props)
        require(scope is not None, "create USDScopePrim", plugin_problem("USDScopePrim"))
        scope.setPosition(200, 100)
        scope_path = scope.getParam("primPath")
        require(scope_path is not None, "USDScopePrim has a primPath parameter")
        scope_path.setValue(MARKER_PATH)
        ok = yield ("until", lambda: field_showing(MARKER_PATH) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDScopePrim's Prim Path field is shown")

        select([GROUP])
        ok = yield ("until", lambda: segment_texts() == ["geo", "group_1"] and segment_states() == ["present"] * 2, POLL_TIMEOUT_S)
        require(ok, "the bar shows geo, group_1")
        mime = mime_for(GROUP)
        require(mime is not None and mime.hasFormat(MIME), "the group_1 segment makes %s mime data" % MIME)
        check(carried(mime) == [GROUP], "the segment's mime data carries /geo/group_1 (got %r)" % (carried(mime),))
        check(mime.hasText() and mime.text() == GROUP, "the segment's mime data has the path as text (got %r)" % (mime.text(),))
        check(send_drag(field_showing(MARKER), mime), "the Prim Pattern field accepts the segment's drag")
        check(str(pattern.getValue()) == GROUP, "the drop writes /geo/group_1 to the pattern (got %r)" % (pattern.getValue(),))
        pattern.setValue(MARKER)

        select([GROUP, SIBLING])
        ok = yield ("until", lambda: badge_visible() and segment_texts() == ["geo", "group_2"], POLL_TIMEOUT_S)
        require(ok, "the bar shows group_2 with a +1 badge")
        mime = mime_for("")
        require(mime is not None and mime.hasFormat(MIME), "the badge makes %s mime data" % MIME)
        check(carried(mime) == [GROUP, SIBLING], "the badge's mime data carries both selected paths (got %r)" % (carried(mime),))
        check(mime.text() == GROUP + "\n" + SIBLING, "the badge's mime data has both lines as text (got %r)" % (mime.text(),))
        ok = yield ("until", lambda: field_showing(MARKER) is not None, POLL_TIMEOUT_S)
        require(ok, "the Prim Pattern field is shown again")
        check(send_drag(field_showing(MARKER), mime), "the Prim Pattern field accepts the badge's drag")
        got = str(pattern.getValue())
        check(got.split(" ") == [GROUP, SIBLING], "the drop writes both paths to the pattern (got %r)" % (got,))
        pattern.setValue(MARKER)

        mime = mime_for("")
        check(send_drag(field_showing(MARKER_PATH), mime), "the Prim Path field accepts the badge's drag")
        check(str(scope_path.getValue()) == GROUP,
              "the drop writes the first path only to a path knob (got %r)" % (scope_path.getValue(),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
