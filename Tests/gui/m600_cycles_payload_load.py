"""The Cycles viewer renders again when payloads are loaded on a stage that opened unloaded.

Graph: USDRead of a payload city (every building a payload, opened unloaded) into Viewer1. If the
view's Renderer submenu has no available Cycles entry the run is skipped (exit 0). Then it:
  - picks Cycles, sets a small Max Samples and waits for the render to converge on the stage with
    no building loaded;
  - runs Load With Descendants on /World from the Scene Graph row menu;
  - checks natronSceneRestartCount rises, the render converges again at Max Samples, and the screen
    over the view shows more pixels that differ from its background than before the load.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
REFINE_TIMEOUT_S = 120.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
CYCLES_ID = "HdCyclesPlugin"
CAPPED_SAMPLES = 8
WORLD = "/World"
GRID = 24


def write_city(directory):
    """Writes the root layer and the payload files and returns the root's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)

    def building(name, x, z):
        with open(os.path.join(payloads, name + ".usda"), "w") as f:
            f.write('#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
                    'def Xform "Building"\n{\n'
                    '    def Cube "Geo"\n    {\n        double size = 1.5\n'
                    '        double3 xformOp:translate = (%g, 0, %g)\n'
                    '        uniform token[] xformOpOrder = ["xformOp:translate"]\n    }\n}\n' % (x, z))
        return '@./payloads/%s.usda@' % name

    def row(prim, arc):
        return '        def Xform "%s" (\n            prepend payload = %s\n        )\n        {\n        }\n' % (prim, arc)

    path = os.path.join(directory, "payload_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\ndef Xform "World"\n{\n')
        f.write('    def Xform "Block0"\n    {\n')
        f.write(row("B0", building("b0_0", -2, 0)))
        f.write(row("B1", building("b0_1", 2, 0)))
        f.write(row("B2", building("b0_2", 0, 3)))
        f.write('    }\n}\n')
    return path


try:
    from PySide6.QtCore import QEvent, QModelIndex, QItemSelectionModel, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QTreeView

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    fixture_dir = os.path.realpath(os.path.join(OUT, "m600_cycles_payload_load"))
    scene_path = write_city(fixture_dir)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
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

    def tree():
        p = panel()
        return hold(p.findChild(QTreeView, "sceneGraphTree")) if p is not None else None

    def model():
        t = tree()
        return hold(t.model()) if t is not None else None

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def num(name, default=-1):
        value = prop(name)
        return default if value is None else value

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

    def find(path):
        """The index of path's row, reading the tree down to it; an invalid index when absent."""
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def row_menu(path, action_name):
        """Opens the row menu on path and triggers action_name."""
        def act():
            menu = QApplication.activePopupWidget()
            if menu is None:
                return
            for action in menu.actions():
                if action.objectName() == action_name:
                    action.trigger()
            menu.close()

        t = tree()
        index = find(path)
        t.selectionModel().clearSelection()
        t.selectionModel().select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
        t.selectionModel().setCurrentIndex(index, QItemSelectionModel.NoUpdate)
        QTimer.singleShot(200, act)
        t.customContextMenuRequested.emit(t.visualRect(index).center())
        QApplication.processEvents()

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def popup():
        return QApplication.activePopupWidget()

    def submenu_of(menu, title):
        for action in menu.actions():
            if action.text().replace("&", "") == title and action.menu() is not None:
                return action
        return None

    def post_context_menu(v, state):
        centre = v.rect().center()

        def post():
            press = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(v.mapToGlobal(centre)),
                                Qt.RightButton, Qt.RightButton, Qt.NoModifier)
            QApplication.sendEvent(v, press)
            event = QContextMenuEvent(QContextMenuEvent.Mouse, centre, v.mapToGlobal(centre), Qt.NoModifier)
            QApplication.sendEvent(v, event)
            state["closed"] = True

        QTimer.singleShot(0, post)

    def close_menus():
        for _ in range(4):
            p = popup()
            if p is None:
                break
            QTest.keyClick(p, Qt.Key_Escape)
            QApplication.processEvents()

    def open_renderer_submenu(state):
        """Steps that post the view's context menu and open its Renderer submenu; returns the
        submenu, or None."""
        activate_main_window()
        v = view()
        post_context_menu(v, state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        if not ok:
            return None
        menu = popup()
        hold(menu)
        action = submenu_of(menu, "Renderer")
        if action is None:
            return None
        hold(action)
        sub = action.menu()
        hold(sub)
        menu.setActiveAction(action)
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        return sub

    def finish_menu(state):
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def foreground_pixels():
        """How many of a grid of screen pixels over the view differ from the view's corner pixel."""
        v = view()
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        origin = v.mapToGlobal(v.rect().topLeft())
        corner = image.pixelColor(origin.x() + 2, origin.y() + 2)
        count = 0
        for gy in range(GRID):
            for gx in range(GRID):
                x = origin.x() + 2 + (v.width() - 4) * gx // GRID
                y = origin.y() + 2 + (v.height() - 4) * gy // GRID
                c = image.pixelColor(x, y)
                if abs(c.red() - corner.red()) + abs(c.green() - corner.green()) + abs(c.blue() - corner.blue()) > 24:
                    count += 1
        return count

    def converged():
        return bool(prop("natronSceneRefineConverged")) and num("natronSceneSamples") == CAPPED_SAMPLES \
            and num("natronSceneDrawnSamples") == CAPPED_SAMPLES

    def steps():
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        require(panel() is not None, "the Scene Graph panel exists")

        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the context menu has a Renderer submenu")
        labels = [a.text().replace("&", "") for a in sub.actions() if not a.isSeparator()]
        cycles = [a for a in sub.actions() if a.text().replace("&", "").startswith("Cycles")]
        if not cycles or not cycles[0].isEnabled() or "unavailable" in cycles[0].text():
            yield from finish_menu(state)
            report("SKIP Cycles is not available in this build (%r)" % (labels,))
            return
        cycles[0].trigger()
        yield from finish_menu(state)
        ok = yield ("until", lambda: prop("natronSceneRenderer") == CYCLES_ID, POLL_TIMEOUT_S)
        require(ok, "picking Cycles switches the view (%r)" % (prop("natronSceneRenderer"),))
        viewer.getParam("maxSamples").set(CAPPED_SAMPLES)
        ok = yield ("until", converged, REFINE_TIMEOUT_S)
        require(ok, "Cycles converges at Max Samples on the unloaded stage (samples %r, drawn %r)"
                % (num("natronSceneSamples"), num("natronSceneDrawnSamples")))
        yield ("sleep", SETTLE_MS)
        before = foreground_pixels()
        shot("m600-cycles-payloads-unloaded.png")

        restarts = num("natronSceneRestartCount")
        ok = yield ("until", lambda: find(WORLD).isValid(), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph lists %s" % WORLD)
        row_menu(WORLD, "sceneGraphLoadWithDescendants")
        ok = yield ("until", lambda: num("natronSceneRestartCount") > restarts, POLL_TIMEOUT_S)
        check(ok, "loading payloads restarts the Cycles render (restarts %r -> %r)" % (restarts, num("natronSceneRestartCount")))
        ok = yield ("until", lambda: not prop("natronSceneLoadsPending") and converged(), REFINE_TIMEOUT_S)
        check(ok, "the render converges again at Max Samples (samples %r, converged %r)"
              % (num("natronSceneSamples"), prop("natronSceneRefineConverged")))
        yield ("sleep", SETTLE_MS)
        after = foreground_pixels()
        shot("m600-cycles-payloads-loaded.png")
        check(after > before, "the loaded buildings show over the background (%d -> %d of %d sampled pixels)"
              % (before, after, GRID * GRID))
        check(after > 0, "a pixel over a building is not the background (%d sampled pixels differ)" % (after,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
