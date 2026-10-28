"""Render delegate selection (M248 - Render Delegate Selection): the scene view's Renderer submenu
lists the registered Hydra delegates with Storm checked, and USDRender has a Renderer knob whose
default is Storm.

Graph: USDCube into Viewer1, and a USDRender. Once the scene is drawn the script checks the
SceneViewport's natronSceneRenderer and natronSceneRendererError properties, posts the view's
context menu, opens its Renderer submenu and checks the entries. Then it reads the options of the
USDRender's Renderer knob. If a delegate other than Storm is listed as available it is picked in
the viewer, and the view must show either a drawn scene or a non-empty natronSceneRendererError;
Storm is then picked again and must draw with no renderer error.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
STORM_ID = "HdStormRendererPlugin"

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import check, close_all_panels, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    cube = app.createNode("USDCube")
    require(cube is not None, "create USDCube", plugin_problem("USDCube"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    cube.setPosition(0, 0)
    viewer.setPosition(0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name):
        v = view()
        return v.property(name) if v is not None else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def renderer_is(rid):
        return prop("natronSceneRenderer") == rid

    def renderer_settled(rid):
        return renderer_is(rid) and (scene_drawn() or bool(prop("natronSceneRendererError")))

    def popup():
        return QApplication.activePopupWidget()

    def submenu_of(menu, title):
        for action in menu.actions():
            if action.text().replace("&", "") == title and action.menu() is not None:
                return action
        return None

    def open_context_menu(v, state):
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

    _held = []

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def node_panel(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        _held.append(panel)
                        return panel
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                _held.append(panel)
                return panel
        return None

    def open_panel(node):
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            activate_main_window()
            app.selectNode(node, True)
            QApplication.processEvents()
            gv = graph_view()
            invoked = gv is not None and QMetaObject.invokeMethod(gv, "showSelectedNodeSettingsPanel")
            if not invoked:
                report("NOTE showSelectedNodeSettingsPanel was not invoked (attempt %d)" % (attempt + 1))
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def steps():
        require(viewer.connectInput(0, cube), "connect the Viewer to the USDCube")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the cube (%r)" % (prop("natronSceneError"),))
        yield ("sleep", SETTLE_MS)
        check(renderer_is(STORM_ID), "the scene view's renderer is Storm by default (natronSceneRenderer %r)"
              % (prop("natronSceneRenderer"),))
        check(not prop("natronSceneRendererError"), "no renderer error with Storm (%r)" % (prop("natronSceneRendererError"),))
        shot("m248-viewer-storm.png")

        # The right-click menu and its Renderer submenu.
        v = view()
        state = {}
        open_context_menu(v, state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        require(ok, "a right-click opens the context menu")
        menu = popup()
        renderer_action = submenu_of(menu, "Renderer")
        check(renderer_action is not None, "the context menu has a Renderer submenu (entries %r)"
              % ([a.text() for a in menu.actions()],))
        delegates = []
        if renderer_action is not None:
            sub = renderer_action.menu()
            delegates = [(a.text().replace("&", ""), a.isChecked(), a.isEnabled(), a.toolTip())
                         for a in sub.actions() if not a.isSeparator()]
            report("NOTE Renderer submenu: %r" % (delegates,))
            check(bool(delegates), "the Renderer submenu lists at least one delegate")
            storm = [d for d in delegates if d[0].startswith("Storm")]
            check(len(storm) == 1, "the Renderer submenu lists Storm once (%r)" % ([d[0] for d in delegates],))
            check(bool(storm) and storm[0][1], "Storm is the checked entry")
            check(sum(1 for d in delegates if d[1]) == 1, "exactly one delegate is checked")
            check(bool(storm) and "unavailable" not in storm[0][0], "Storm is not marked unavailable")
            # Open the submenu for the screenshot.
            menu.setActiveAction(renderer_action)
            QTest.keyClick(menu, Qt.Key_Right)
            yield ("until", lambda: sub.isVisible(), 5.0)
            yield ("sleep", SETTLE_MS)
            shot("m248-renderer-submenu.png")
        close_menus()
        ok = yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        require(ok, "Esc closes the context menu")
        check(popup() is None, "no popup is left open")

        # USDRender's Renderer knob.
        render = app.createNode("USDRender")
        require(render is not None, "create USDRender", plugin_problem("USDRender"))
        render.setPosition(200, 0)
        knob = render.getParam("renderer")
        require(knob is not None, "USDRender has a renderer parameter")
        options = [str(o) for o in knob.getOptions()]
        report("NOTE USDRender renderer options (IDs): %r, current %r" % (options, knob.get()))
        check(STORM_ID in options, "the USDRender Renderer options include Storm's id (%r)" % (options,))
        if STORM_ID in options:
            check(knob.getOption(knob.get()) == STORM_ID, "the USDRender Renderer defaults to Storm (index %r)" % (knob.get(),))
        yield ("sleep", SETTLE_MS)
        opened = yield from open_panel(render)
        yield ("sleep", SETTLE_MS)
        panels = [p for p in widgets_of("NodeSettingsPanel") if p.isVisible()]
        check(opened and bool(panels), "the USDRender panel is open (%d visible node panels)" % len(panels))
        shot("m248-usdrender-panel.png")

        # A delegate other than Storm, when the build has one.
        others = [d[0] for d in delegates if not d[0].startswith("Storm") and "unavailable" not in d[0]]
        report("NOTE non-Storm delegates available: %r" % (others,))
        if others:
            state2 = {}
            open_context_menu(v, state2)
            ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
            require(ok, "the context menu opens again")
            menu = popup()
            renderer_action = submenu_of(menu, "Renderer")
            require(renderer_action is not None, "the Renderer submenu is still there")
            target = [a for a in renderer_action.menu().actions() if a.text().replace("&", "") == others[0]][0]
            target.trigger()
            close_menus()
            yield ("until", lambda: state2.get("closed", False), POLL_TIMEOUT_S)
            ok = yield ("until", lambda: (prop("natronSceneRenderer") or "") != STORM_ID, POLL_TIMEOUT_S)
            check(ok, "picking %s switches the view's renderer (now %r)" % (others[0], prop("natronSceneRenderer")))
            current = prop("natronSceneRenderer")
            ok = yield ("until", lambda: renderer_settled(current), POLL_TIMEOUT_S)
            error = prop("natronSceneRendererError")
            check(ok and (scene_drawn() or bool(error)),
                  "%s shows an image or a renderer error (drawn %r, error %r)" % (others[0], scene_drawn(), error))
            yield ("sleep", SETTLE_MS)
            shot("m248-viewer-other-delegate.png")

            open_context_menu(v, {})
            ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
            require(ok, "the context menu opens to switch back")
            menu = popup()
            renderer_action = submenu_of(menu, "Renderer")
            storm_action = [a for a in renderer_action.menu().actions() if a.text().replace("&", "").startswith("Storm")][0]
            storm_action.trigger()
            close_menus()
            yield ("sleep", SETTLE_MS)
            ok = yield ("until", lambda: renderer_is(STORM_ID) and scene_drawn(), POLL_TIMEOUT_S)
            check(ok, "switching back to Storm draws the scene again (renderer %r)" % (prop("natronSceneRenderer"),))
            check(not prop("natronSceneRendererError"), "no renderer error after switching back to Storm (%r)"
                  % (prop("natronSceneRendererError"),))
            shot("m248-viewer-storm-again.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
