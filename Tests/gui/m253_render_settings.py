"""Delegate render settings (M253 - Delegate Render Settings): a USDRenderSettings node authors a
RenderSettings prim with a tab of settings per renderer, USDRender is a thin consumer of it, and
the scene view takes the active prim's settings for its renderer.

Graph: USDRead(scene-cycles.usda) into USDRenderSettings into USDRender, and Viewer1 on the
USDRenderSettings node, so the scene view shows the stage with the prim. In a 1920x1080 window the
script:
  - checks the view starts on Storm (M248: Storm stays the viewer's default);
  - sets the prim's Camera to /World/Cam, opens the USDRenderSettings panel and shoots its Storm,
    Cycles and MoonRay tabs, one screenshot each (a renderer this build lacks is skipped);
  - opens the USDRender panel and checks it is thin (Settings Prim and Renderer, no camera or
    resolution knobs of its own), its Settings Prim menu lists Stage Default and the prim, and its
    Renderer menu starts with Scene Default; shoots it;
  - posts the view's context menu and checks its Renderer submenu starts with Scene Default and
    has Storm checked; shoots it open;
  - picks Scene Default: with the prim's Default Renderer on Storm the view renders with Storm;
  - with Cycles (skipped without it): the prim's Default Renderer on Cycles makes the view render
    with Cycles; the Cycles tab's Samples is the view's Max Samples; the Viewer's maxSamples knob
    applies over it and clearing it brings the prim's back; a MoonRay tab edit does not restart
    the refinement; Make Active off falls back to Storm, and Storm picked in the view beats the
    prim's renderer.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
REFINE_TIMEOUT_S = 120.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
STORM_ID = "HdStormRendererPlugin"
CYCLES_ID = "HdCyclesPlugin"
MOONRAY_ID = "HdMoonrayRendererPlugin"
SCENE_DEFAULT_ID = "sceneDefault"
STAGE_DEFAULT_ID = "stageDefault"
PRIM_PATH = "/Render/rendersettings"
CAMERA = "/World/Cam"
PRIM_SAMPLES = 7
VIEWER_SAMPLES = 3

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, QTimer
    from PySide6.QtGui import QContextMenuEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTabWidget

    from guitest import (FIXTURES, check, close_all_panels, finish, plugin_problem, report, require, run, section,
                         shot, viewer_up, widgets_of)

    read = app.createNode("USDRead")
    require(read is not None, "create USDRead", plugin_problem("USDRead"))
    require(read.getParam("filename") is not None, "USDRead has a filename parameter")
    read.getParam("filename").setValue(os.path.join(FIXTURES, "scene-cycles.usda"))
    settings = app.createNode("USDRenderSettings")
    require(settings is not None, "create USDRenderSettings", plugin_problem("USDRenderSettings"))
    render = app.createNode("USDRender")
    require(render is not None, "create USDRender", plugin_problem("USDRender"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    read.setPosition(0, 0)
    settings.setPosition(0, 100)
    render.setPosition(0, 200)
    viewer.setPosition(200, 200)

    # Wrappers whose children are used later: a child's wrapper dies with its parent's.
    _held = []

    def hold(w):
        if w is not None:
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

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

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

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
                        return hold(panel)
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                return hold(panel)
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

    def panel_tabs(node):
        panel = node_panel(node)
        if panel is None:
            return None
        tabs = [t for t in panel.findChildren(QTabWidget) if t.count() > 0]
        return hold(tabs[0]) if tabs else None

    def tab_texts(node):
        tabs = panel_tabs(node)
        return [tabs.tabText(i).replace("&", "") for i in range(tabs.count())] if tabs is not None else []

    def show_tab(node, names):
        """Shows the first tab of node's panel whose label is one of names (case-insensitive);
        returns its label, or None."""
        tabs = panel_tabs(node)
        if tabs is None:
            return None
        wanted = [n.lower() for n in names]
        for i in range(tabs.count()):
            text = tabs.tabText(i).replace("&", "")
            if text.lower() in wanted:
                tabs.setCurrentIndex(i)
                QApplication.processEvents()
                return text
        return None

    def options(param):
        return [str(o) for o in param.getOptions()]

    def choose(param, option_id):
        """Sets a choice parameter to the entry of option_id; returns whether it has one."""
        ids = options(param)
        if option_id not in ids:
            return False
        param.set(ids.index(option_id))
        return True

    def chosen(param):
        return param.getOption(param.get())

    def available(labels, start):
        """Whether the Renderer submenu's labels have an entry starting with start that is not
        marked unavailable."""
        return any(l.startswith(start) and "unavailable" not in l for l, _ in labels)

    def popup():
        return QApplication.activePopupWidget()

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
        post_context_menu(view(), state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        if not ok:
            return None
        menu = hold(popup())
        action = None
        for a in menu.actions():
            if a.text().replace("&", "") == "Renderer" and a.menu() is not None:
                action = a
        if action is None:
            return None
        # The submenu's wrapper lives only as long as the action's.
        hold(action)
        sub = hold(action.menu())
        menu.setActiveAction(action)
        QTest.keyClick(menu, Qt.Key_Right)
        yield ("until", lambda: sub.isVisible(), 5.0)
        return sub

    def finish_menu(state):
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)

    def entries(sub):
        return [a for a in sub.actions() if not a.isSeparator()]

    def pick_renderer(label_start):
        """Steps that pick the first Renderer submenu entry whose label starts with label_start;
        returns whether there was one."""
        state = {}
        sub = yield from open_renderer_submenu(state)
        found = [a for a in entries(sub) if a.text().replace("&", "").startswith(label_start)] if sub is not None else []
        if found:
            found[0].trigger()
        yield from finish_menu(state)
        return bool(found)

    def active_is(renderer_id):
        return prop("natronSceneActiveRenderer") == renderer_id

    def steps():
        gui = main_gui()
        if gui is not None:
            gui.resize(1920, 1080)
        require(settings.connectInput(0, read), "connect the USDRenderSettings to the USDRead")
        require(render.connectInput(0, settings), "connect the USDRender to the USDRenderSettings")
        require(viewer.connectInput(0, settings), "connect the Viewer to the USDRenderSettings")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        check(prop("natronSceneRenderer") == STORM_ID, "the view starts on Storm (%r)" % (prop("natronSceneRenderer"),))

        # Which renderers this build has, from the view's Renderer submenu.
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the view's context menu has a Renderer submenu")
        labels = [(a.text().replace("&", ""), a.isChecked()) for a in entries(sub)]
        yield from finish_menu(state)
        cycles = available(labels, "Cycles")
        moonray = available(labels, "MoonRay") or available(labels, "Moonray")
        report("NOTE Renderer submenu %r; Default Renderer options %r; Cycles %s, MoonRay %s"
               % (labels, options(settings.getParam("defaultRenderer")), cycles, moonray))
        check(chosen(settings.getParam("defaultRenderer")) == STORM_ID, "the prim's Default Renderer starts on Storm")

        # The USDRenderSettings tabs.
        section("USDRenderSettings")
        camera = settings.getParam("camera")
        require(camera is not None, "USDRenderSettings has a camera parameter")
        ok = yield ("until", lambda: CAMERA in options(camera), POLL_TIMEOUT_S)
        check(ok and choose(camera, CAMERA), "the Camera menu lists %s (%r)" % (CAMERA, options(camera)))
        check(settings.getParam("primPath").get() == PRIM_PATH, "the prim is %s by default" % PRIM_PATH)
        opened = yield from open_panel(settings)
        require(opened, "the USDRenderSettings panel opens")
        yield ("sleep", SETTLE_MS)
        report("NOTE USDRenderSettings tabs: %r" % (tab_texts(settings),))
        for renderer_id, names, name, wanted in ((STORM_ID, ("Storm", "GL"), "storm", True),
                                                 (CYCLES_ID, ("Cycles",), "cycles", cycles),
                                                 (MOONRAY_ID, ("MoonRay", "Moonray"), "moonray", moonray)):
            if not wanted:
                report("SKIP the %s tab: the renderer is not available" % name)
                continue
            shown = show_tab(settings, names)
            check(shown is not None, "the panel has a %s tab (%r)" % (name, tab_texts(settings)))
            if shown is not None:
                yield ("sleep", SETTLE_MS)
                shot("m253-usdrendersettings-%s-tab.png" % name)

        # USDRender's thin panel.
        section("USDRender")
        prim_menu = render.getParam("settingsPrim")
        renderer = render.getParam("renderer")
        require(prim_menu is not None and renderer is not None, "USDRender has settingsPrim and renderer parameters")
        for gone in ("camera", "fitMode", "conformPolicy", "NatronParamFormatSize", "NatronParamFormatPar"):
            check(render.getParam(gone) is None, "USDRender has no %s parameter of its own" % gone)
        ok = yield ("until", lambda: PRIM_PATH in options(prim_menu), POLL_TIMEOUT_S)
        check(ok and options(prim_menu)[0] == STAGE_DEFAULT_ID,
              "the Settings Prim menu lists Stage Default, then %s (%r)" % (PRIM_PATH, options(prim_menu)))
        check(chosen(prim_menu) == STAGE_DEFAULT_ID, "the Settings Prim starts on Stage Default")
        check(options(renderer)[:1] == [SCENE_DEFAULT_ID] and chosen(renderer) == SCENE_DEFAULT_ID,
              "the Renderer menu starts with Scene Default, chosen (%r)" % (options(renderer),))
        opened = yield from open_panel(render)
        check(opened, "the USDRender panel opens")
        yield ("sleep", SETTLE_MS)
        shot("m253-usdrender-panel.png")
        close_all_panels()

        # The view's Renderer submenu.
        section("Viewer")
        state = {}
        sub = yield from open_renderer_submenu(state)
        require(sub is not None, "the view's context menu has a Renderer submenu")
        labels = [(a.text().replace("&", ""), a.isChecked()) for a in entries(sub)]
        report("NOTE Renderer submenu: %r" % (labels,))
        check(bool(labels) and labels[0][0] == "Scene Default" and not labels[0][1],
              "the submenu starts with Scene Default, unchecked")
        check([l for l, c in labels if c][:1] and [l for l, c in labels if c][0].startswith("Storm"),
              "Storm is the only checked entry (%r)" % ([l for l, c in labels if c],))
        yield ("sleep", SETTLE_MS)
        shot("m253-viewer-renderer-menu.png")
        yield from finish_menu(state)

        # Scene Default follows the prim's renderer, Storm here.
        ok = yield from pick_renderer("Scene Default")
        require(ok, "the submenu has a Scene Default entry")
        ok = yield ("until", lambda: prop("natronSceneRenderer") == SCENE_DEFAULT_ID, POLL_TIMEOUT_S)
        check(ok, "picking Scene Default sets it (%r)" % (prop("natronSceneRenderer"),))
        ok = yield ("until", lambda: active_is(STORM_ID) and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "the prim naming Storm renders with Storm (%r)" % (prop("natronSceneActiveRenderer"),))

        if not cycles:
            report("SKIP the Cycles precedence and samples checks: Cycles is not available")
            return

        # The prim's renderer, then its samples, with the viewer's cap over them.
        section("Cycles")
        require(choose(settings.getParam("defaultRenderer"), CYCLES_ID), "the prim's Default Renderer lists Cycles")
        ok = yield ("until", lambda: active_is(CYCLES_ID), POLL_TIMEOUT_S)
        check(ok, "Scene Default follows the prim to Cycles (%r)" % (prop("natronSceneActiveRenderer"),))
        check(not prop("natronSceneRendererError"), "no renderer error (%r)" % (prop("natronSceneRendererError"),))
        samples = settings.getParam("cyclesSamples")
        require(samples is not None, "the Cycles tab has a cyclesSamples parameter")
        samples.set(PRIM_SAMPLES)
        ok = yield ("until", lambda: num("natronSceneMaxSamples") == PRIM_SAMPLES, REFINE_TIMEOUT_S)
        check(ok, "the view refines to the prim's samples (%r)" % (num("natronSceneMaxSamples"),))
        ok = yield ("until", lambda: bool(prop("natronSceneRefineConverged")) and num("natronSceneSamples") == PRIM_SAMPLES,
                    REFINE_TIMEOUT_S)
        check(ok, "the view converges at the prim's samples (samples %r)" % (num("natronSceneSamples"),))
        shot("m253-viewer-cycles-prim-samples.png")
        viewer.getParam("maxSamples").set(VIEWER_SAMPLES)
        ok = yield ("until", lambda: num("natronSceneMaxSamples") == VIEWER_SAMPLES and bool(prop("natronSceneRefineConverged"))
                    and num("natronSceneSamples") == VIEWER_SAMPLES, REFINE_TIMEOUT_S)
        check(ok, "the Viewer's Max Samples applies over the prim's (max %r, samples %r)"
              % (num("natronSceneMaxSamples"), num("natronSceneSamples")))

        # A MoonRay tab edit leaves the Cycles refinement alone.
        pixel = settings.getParam("moonrayPixelSamples")
        if pixel is None:
            report("SKIP the MoonRay edit check: no moonrayPixelSamples parameter")
        else:
            restarts = num("natronSceneRestartCount")
            frames = num("natronSceneRefineFrames")
            pixel.set(int(pixel.get()) + 3)
            yield ("sleep", 1500)
            check(num("natronSceneRestartCount") == restarts and num("natronSceneRefineFrames") == frames,
                  "a MoonRay tab edit does not restart the Cycles refinement (restarts %r -> %r, frames %r -> %r)"
                  % (restarts, num("natronSceneRestartCount"), frames, num("natronSceneRefineFrames")))
        viewer.getParam("maxSamples").set(0)
        ok = yield ("until", lambda: num("natronSceneMaxSamples") == PRIM_SAMPLES, REFINE_TIMEOUT_S)
        check(ok, "clearing Max Samples brings back the prim's samples (%r)" % (num("natronSceneMaxSamples"),))

        # Make Active off: the stage has no active prim, Storm.
        settings.getParam("makeActive").set(False)
        ok = yield ("until", lambda: active_is(STORM_ID) and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "with Make Active off Scene Default renders with Storm (%r)" % (prop("natronSceneActiveRenderer"),))
        settings.getParam("makeActive").set(True)
        ok = yield ("until", lambda: active_is(CYCLES_ID), POLL_TIMEOUT_S)
        check(ok, "Make Active on again: Cycles (%r)" % (prop("natronSceneActiveRenderer"),))

        # A renderer picked in the view beats the prim's.
        ok = yield from pick_renderer("Storm")
        check(ok, "the submenu has a Storm entry")
        ok = yield ("until", lambda: prop("natronSceneRenderer") == STORM_ID and active_is(STORM_ID) and scene_drawn(),
                    POLL_TIMEOUT_S)
        check(ok, "Storm picked in the view beats the prim's Cycles (%r, %r)"
              % (prop("natronSceneRenderer"), prop("natronSceneActiveRenderer")))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
