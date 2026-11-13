"""Dropping Scene Graph rows on prim path and pattern knobs.

Graph: USDRead(scene-reference.usda) -> USDPrune -> Viewer1, with the USDPrune's settings panel
open. The script takes the mime data the Scene Graph panel's tree makes for the /World/Box and
/World/Ball rows (read through the model with QModelIndex alone) and sends drag-enter, drag-move
and drop events carrying it to the USDPrune's Prim Pattern field (synthesized window-system drags
do not reach Natron's widgets under Xvfb).

Checks:
  - the tree's mime data carries both paths under application/x-natron-prim-paths;
  - the Prim Pattern field accepts the drag, and the drop sets "/World/Box /World/Ball";
  - the tree then lists only /World/Cam under /World: both prims are pruned;
  - one Undo restores the field's previous value and the prims come back;
  - the same two rows dropped on a USDScopePrim's Prim Path field set it to the first row's path,
    /World/Box, and one Undo restores it;
  - the same drop on a USDAssignMaterial's Prim Pattern field sets both paths, and one Undo
    restores it;
  - the same drop on its Material Path field sets the first row's path only, and one Undo restores it.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 400
MIME = "application/x-natron-prim-paths"
WORLD = "/World"
BOX = "/World/Box"
BALL = "/World/Ball"
CAM = "/World/Cam"
MARKER = "/NatronDropTarget"
MARKER_PATH = "/NatronDropPathTarget"
MARKER_ASSIGN = "/NatronDropAssignTarget"
MARKER_MATERIAL = "/NatronDropMaterialTarget"

try:
    import NatronEngine
    from PySide6.QtCore import QModelIndex, QPointF, Qt
    from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QTreeView

    from guitest import (FIXTURES, check, class_name, finish, menu_of, plugin_problem, report, require, run, shot,
                         viewer_up, widgets_of)

    PATH_ROLE = Qt.UserRole + 1

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
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

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def tree():
        p = panel()
        try:
            return hold(p.findChild(QTreeView, "sceneGraphTree")) if p is not None else None
        except RuntimeError:
            return None

    def model():
        t = tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def rows(parent=QModelIndex()):
        """parent's child rows; none while the tree is being rebuilt."""
        m = model()
        if m is None:
            return []
        try:
            while m.canFetchMore(parent):
                m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def row_of(path, parent=QModelIndex()):
        for index in rows(parent):
            if path_of(index) == path:
                return index
        return QModelIndex()

    def world_children():
        world = row_of(WORLD)
        return sorted(path_of(i) for i in rows(world)) if world.isValid() else []

    def field_showing(text):
        """The line edit showing text in an open settings panel.

        KnobLineEdit has no Q_OBJECT, so its meta-object class name is its base's; match any QLineEdit."""
        for w in QApplication.allWidgets():
            try:
                if w.inherits("QLineEdit") and w.isVisible() and w.text() == text:
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and class_name(w) == "Gui":
                return w
        return None

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def send_drag(widget, mime, drop):
        """Sends drag-enter and drag-move, then a drop when drop; returns whether the move was accepted."""
        pos = widget.rect().center()
        enter = QDragEnterEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(widget, enter)
        move = QDragMoveEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(widget, move)
        accepted = enter.isAccepted() and move.isAccepted()
        if drop and accepted:
            event = QDropEvent(QPointF(pos), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
            QApplication.sendEvent(widget, event)
        QApplication.processEvents()
        return accepted

    def steps():
        require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
        require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        require(panel() is not None, "the Scene Graph panel exists")
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        require(ok, "the tree lists Box, Ball and Cam under /World (got %r)" % (world_children(),))

        world = row_of(WORLD)
        box = row_of(BOX, world)
        ball = row_of(BALL, world)
        mime = model().mimeData([box, ball])
        require(mime is not None and mime.hasFormat(MIME), "the tree's rows make %s mime data" % MIME)
        carried = sorted(bytes(mime.data(MIME)).decode("utf-8").split("\n"))
        check(carried == sorted([BOX, BALL]), "the mime data carries both paths (got %r)" % (carried,))

        ok = yield ("until", lambda: field_showing(MARKER) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDPrune's Prim Pattern field is shown")
        field = field_showing(MARKER)

        check(send_drag(field, mime, True), "the Prim Pattern field accepts the drag")
        dropped = str(pattern.getValue())
        check(sorted(dropped.split(" ")) == sorted([BOX, BALL]),
              "the drop sets the pattern to both paths, separated by a space (got %r)" % (dropped,))
        ok = yield ("until", lambda: world_children() == [CAM], POLL_TIMEOUT_S)
        check(ok, "both prims are pruned: /World holds only Cam (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-drop-pruned.png")

        undo = edit_menu_action("undo")
        require(undo is not None and undo.isEnabled(), "an enabled Undo action is in the Edit menu")
        undo.trigger()
        QApplication.processEvents()
        check(str(pattern.getValue()) == MARKER, "one Undo restores the previous pattern (got %r)" % (pattern.getValue(),))
        ok = yield ("until", lambda: world_children() == sorted([BOX, BALL, CAM]), POLL_TIMEOUT_S)
        check(ok, "after Undo the prims come back (got %r)" % (world_children(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-graph-drop-undone.png")

        def undo_once():
            action = edit_menu_action("undo")
            require(action is not None and action.isEnabled(), "an enabled Undo action is in the Edit menu")
            action.trigger()
            QApplication.processEvents()

        scope = app.createNode("USDScopePrim", -1, None, props)
        require(scope is not None, "create USDScopePrim", plugin_problem("USDScopePrim"))
        scope.setPosition(200, 100)
        scope_path = scope.getParam("primPath")
        require(scope_path is not None, "USDScopePrim has a primPath parameter")
        scope_path.setValue(MARKER_PATH)
        ok = yield ("until", lambda: field_showing(MARKER_PATH) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDScopePrim's Prim Path field is shown")
        check(send_drag(field_showing(MARKER_PATH), mime, True), "the Prim Path field accepts the drag")
        check(str(scope_path.getValue()) == BOX,
              "the drop sets a path knob to the first row's path (got %r)" % (scope_path.getValue(),))
        undo_once()
        check(str(scope_path.getValue()) == MARKER_PATH,
              "one Undo restores the path knob (got %r)" % (scope_path.getValue(),))

        assign = app.createNode("USDAssignMaterial", -1, None, props)
        require(assign is not None, "create USDAssignMaterial", plugin_problem("USDAssignMaterial"))
        assign.setPosition(400, 100)
        assign_pattern = assign.getParam("primPattern")
        require(assign_pattern is not None, "USDAssignMaterial has a primPattern parameter")
        assign_pattern.setValue(MARKER_ASSIGN)
        assign_material = assign.getParam("materialPath")
        require(assign_material is not None, "USDAssignMaterial has a materialPath parameter")
        assign_material.setValue(MARKER_MATERIAL)
        ok = yield ("until", lambda: field_showing(MARKER_ASSIGN) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDAssignMaterial's Prim Pattern field is shown")
        check(send_drag(field_showing(MARKER_ASSIGN), mime, True), "USDAssignMaterial's Prim Pattern field accepts the drag")
        got = str(assign_pattern.getValue())
        check(got.split(" ") == [BOX, BALL],
              "the drop sets USDAssignMaterial's pattern to both paths in order (got %r)" % (got,))
        undo_once()
        check(str(assign_pattern.getValue()) == MARKER_ASSIGN,
              "one Undo restores USDAssignMaterial's pattern (got %r)" % (assign_pattern.getValue(),))

        ok = yield ("until", lambda: field_showing(MARKER_MATERIAL) is not None, POLL_TIMEOUT_S)
        require(ok, "the USDAssignMaterial's Material Path field is shown")
        check(send_drag(field_showing(MARKER_MATERIAL), mime, True), "USDAssignMaterial's Material Path field accepts the drag")
        got = str(assign_material.getValue())
        check(got == BOX,
              "the drop sets the Material Path to the first of two paths only (got %r)" % (got,))
        check(str(assign_pattern.getValue()) == MARKER_ASSIGN,
              "the Material Path drop leaves the Prim Pattern alone (got %r)" % (assign_pattern.getValue(),))
        undo_once()
        check(str(assign_material.getValue()) == MARKER_MATERIAL,
              "one Undo restores the Material Path (got %r)" % (assign_material.getValue(),))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
