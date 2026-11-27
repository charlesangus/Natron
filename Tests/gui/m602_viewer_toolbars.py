"""3D viewer toolbars (M602 - 3D Viewer Toolbars): the 3D view's tool bar on the left, display
bar on the right and top bar above it, their controls, and the per-viewer bar flags.

Graph: GeoPrimitive -> Viewer1 (geometry), GeoPrimitive -> GeoToUSD -> USDEditTransform. Then it:
  - checks the three bars show in the 3D view and the side bars are 28 px icon columns, that Hide
    All hides them and Show All brings them back, and that the draw mode menu shows its keys
    before any context menu has been opened;
  - clicks the Translate button (natronSceneTransformTool follows), presses Q and T in the view
    (the buttons follow) and checks the tool tooltips carry their shortcut;
  - clicks the Points pick button (natronGeoPickMode follows) and back to Objects;
  - toggles the grid from the display bar, picks Wireframe from its draw mode menu and checks the
    view's context menu shows the same state;
  - toggles Point Markers from the bar (the pointMarkers knob follows), sets the knob from Python
    (the button and the context menu toggle follow), and sets Normal Scale and Highlight Group from
    the Display Options popup;
  - views GeoToUSD, a scene: the geometry toggles are disabled, not hidden;
  - picks the renderer and the free camera from the top bar's menus; on Storm the progressive
    controls are hidden and Render Everything shows;
  - shows each purpose set from the Purposes menu and checks the button's tile (P, R, G, D for the
    default prims alone, stacked for several); switches the view to Front and back and checks the
    View button's camera (orthographic, perspective);
  - opens the USDEditTransform panel: its Mode shows in the top bar's tool options; Select draws
    none of its handles, the node handle tool draws them, and closing the panel hides the options
    and switches back to Select;
  - with no panel open, Return passes on; with the panel open, the view takes it and it picks the
    node handle tool, which the context menu names after the node's handle; a panel closed during
    a handle drag switches back to Select at the release;
  - views a Constant, an image: the bars hide by default;
  - hides the display bar, saves, resets and reopens: the display bar stays hidden, the tool bar
    shows, and the Display Bar action is unchecked.

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots (m602-*.png) and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
LOAD_TIMEOUT_S = 60.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
STORM_ID = "HdStormRendererPlugin"
BAR_WIDTH = 28
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
MESH = "/geo/mesh_0"

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, QTimer
    from PySide6.QtGui import QAction, QContextMenuEvent, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QDoubleSpinBox, QLineEdit, QWidget

    import guitest as gt
    from guitest import check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    primitive = app.createNode("GeoPrimitive")
    require(primitive is not None, "create GeoPrimitive", plugin_problem("GeoPrimitive"))
    to_usd = app.createNode("GeoToUSD")
    require(to_usd is not None, "create GeoToUSD", plugin_problem("GeoToUSD"))
    require(to_usd.connectInput(0, primitive), "connect the GeoToUSD to the GeoPrimitive")
    edit = app.createNode("USDEditTransform")
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    require(edit.connectInput(0, to_usd), "connect the USDEditTransform to the GeoToUSD")
    edit.getParam("primPattern").setValue(MESH)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    primitive.setPosition(0, 0)
    to_usd.setPosition(0, 100)
    edit.setPosition(0, 200)
    viewer.setPosition(0, 320)
    _held = []

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

    def tab():
        tabs = widgets_of("ViewerTab")
        if tabs:
            _held.append(tabs[0])
            return tabs[0]
        return None

    def find(cls, name):
        t = tab()
        w = t.findChild(cls, name) if t is not None else None
        if w is not None:
            # A child's wrapper lives only as long as its parent's.
            _held.append(w)
        return w

    def bar(name):
        return find(QWidget, name)

    def control(bar_name, name):
        b = bar(bar_name)
        if b is None:
            return None
        found = [w for w in b.findChildren(QWidget) if w.objectName() == name]
        if found:
            _held.append(found[0])
        return found[0] if found else None

    def shown(name):
        b = bar(name)
        return b is not None and b.isVisible()

    def knob(name):
        return viewer.getParam(name).get()

    def icon(bar_name, button_name):
        """The icon a bar's button shows, as its path under the icon resources."""
        button = control(bar_name, button_name)
        value = button.property("natronIcon") if button is not None else None
        return str(value) if value else None

    def handles():
        """The transform handles the view draws, by handle id."""
        value = prop("natronSceneTransformHandles")
        return [int(h[0]) for h in value] if value else []

    def handle_points():
        """Where the view draws its transform handles, in widget coordinates."""
        value = prop("natronSceneTransformHandles")
        return [QPointF(float(h[1]), float(h[2])) for h in value] if value else []

    def mouse(kind, pos, button, buttons):
        v = view()
        event = QMouseEvent(kind, QPointF(pos), v.mapToGlobal(QPointF(pos)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(v, event)
        QApplication.processEvents()

    def override_accepted(k):
        """Whether the view claims k over the window shortcuts around it."""
        event = QKeyEvent(QEvent.ShortcutOverride, k, Qt.NoModifier)
        event.ignore()
        QApplication.sendEvent(view(), event)
        return event.isAccepted()

    def set_purposes(wanted):
        """Shows exactly the purposes in wanted from the display bar's menu."""
        for name in ("proxy", "render", "guide"):
            action = bar_menu_action("sceneDisplayBar", "scenePurposes", name)
            require(action is not None, "the purposes menu has %s" % name)
            if action.isChecked() != (name in wanted):
                action.trigger()
                QApplication.processEvents()

    def settle():
        v = view()
        if v is not None:
            v.update()
        return ("sleep", SETTLE_MS)

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

    def key(k):
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
        v = view()
        v.setFocus()
        QApplication.processEvents()
        QTest.keyClick(v, k)
        QApplication.processEvents()

    def click(button):
        button.click()
        QApplication.processEvents()

    def bar_menu_action(bar_name, button_name, action_name):
        """The action named action_name of the menu of a bar's button, filled as on opening."""
        button = control(bar_name, button_name)
        menu = button.menu() if button is not None else None
        if menu is None:
            return None
        _held.append(menu)
        menu.aboutToShow.emit()
        for action in menu.actions():
            if action.objectName() == action_name or action.text().replace("&", "") == action_name:
                _held.append(action)
                return action
        return None

    def bar_menu_texts(bar_name, button_name):
        button = control(bar_name, button_name)
        menu = button.menu() if button is not None else None
        if menu is None:
            return []
        _held.append(menu)
        menu.aboutToShow.emit()
        return [a.text().replace("&", "") for a in menu.actions() if not a.isSeparator()]

    def popup():
        return QApplication.activePopupWidget()

    def post_context_menu(state):
        v = view()
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

    def menu_actions(menu, path):
        """The actions of the submenu of menu at path (titles), menu's own for an empty path."""
        for title in path:
            found = [a for a in menu.actions() if a.text().replace("&", "") == title and a.menu() is not None]
            if not found:
                return []
            _held.append(found[0])
            menu = found[0].menu()
            _held.append(menu)
        return list(menu.actions())

    def context_menu_state(path, name):
        """Steps that open the view's context menu and return whether the action named name in the
        submenu at path is checked, or None if it is missing."""
        result = yield from context_menu_read(path, name, lambda action: action.isChecked())
        return result

    def context_menu_read(path, name, read):
        """Steps that open the view's context menu and return read() of the action named name in
        the submenu at path, or None if it is missing."""
        state = {}
        post_context_menu(state)
        ok = yield ("until", lambda: popup() is not None, POLL_TIMEOUT_S)
        result = None
        if ok:
            menu = popup()
            _held.append(menu)
            for action in menu_actions(menu, path):
                if action.objectName() == name:
                    result = read(action)
        close_menus()
        yield ("until", lambda: state.get("closed", False), POLL_TIMEOUT_S)
        return result

    def open_edit_panel():
        """Opens only the USDEditTransform's settings panel."""
        gt.close_all_panels()
        app.clearSelection()
        app.selectNode(edit, True)
        graphs = [w for w in widgets_of("NodeGraph") if w.isVisible()]
        require(graphs, "the node graph is on screen")
        QMetaObject.invokeMethod(graphs[0], "showSelectedNodeSettingsPanel")

    def view_input(node, label):
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to the " + label)

    def steps():
        view_input(primitive, "GeoPrimitive")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the geometry (%r)" % (prop("natronSceneError"),))
        yield settle()

        section("bars")
        for name in ("sceneToolBar", "sceneDisplayBar", "sceneTopBar"):
            check(shown(name), "the %s shows in the 3D view" % name)
        for name in ("sceneToolBar", "sceneDisplayBar"):
            b = bar(name)
            check(b is not None and b.width() == BAR_WIDTH, "the %s is %d px wide (%r)" % (name, BAR_WIDTH, b.width() if b else None))
        QMetaObject.invokeMethod(tab(), "hideAllToolbars")
        yield settle()
        for name in ("sceneToolBar", "sceneDisplayBar", "sceneTopBar"):
            check(not shown(name), "Hide All hides the %s" % name)
        QMetaObject.invokeMethod(tab(), "showAllToolbars")
        yield settle()
        for name in ("sceneToolBar", "sceneDisplayBar", "sceneTopBar"):
            check(shown(name), "Show All brings the %s back" % name)
        wire = bar_menu_action("sceneDisplayBar", "sceneDrawMode", "wireframe")
        check(wire is not None and wire.shortcut().toString() == "Ctrl+4",
              "the draw mode menu shows Wireframe's key before any context menu (%r)" % (wire.shortcut().toString() if wire else None,))
        slot = control("sceneToolBar", "snapSlot")
        check(slot is not None and not slot.isVisible(), "the tool bar holds a hidden snapSlot")
        node_handle = control("sceneToolBar", "sceneToolNodeHandle")
        check(node_handle is not None and not node_handle.isEnabled() and "No node" in node_handle.toolTip(),
              "the node handle tool is disabled and says why while no node offers a handle")
        shot("m602-geometry.png")

        section("transform tools")
        translate = control("sceneToolBar", "sceneToolTranslate")
        select = control("sceneToolBar", "sceneToolSelect")
        require(translate is not None and select is not None, "the tool bar has the transform tool buttons")
        check(translate.text() == "" and not translate.icon().isNull(), "the tool buttons are icon-only")
        check("Keyboard shortcut" in translate.toolTip() and "T" in translate.toolTip(),
              "the Translate tooltip carries its shortcut (%r)" % translate.toolTip())
        click(translate)
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "translate", POLL_TIMEOUT_S)
        check(ok and translate.isChecked(), "the Translate button picks the Translate tool (%r)" % (prop("natronSceneTransformTool"),))
        key(Qt.Key_Q)
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "select", POLL_TIMEOUT_S)
        check(ok and select.isChecked() and not translate.isChecked(), "Q picks Select and the Select button follows")
        key(Qt.Key_T)
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "translate", POLL_TIMEOUT_S)
        check(ok and translate.isChecked(), "T picks Translate and the Translate button follows")
        key(Qt.Key_Q)

        section("pick modes")
        points = control("sceneToolBar", "geoPickPoints")
        objects = control("sceneToolBar", "geoPickObjects")
        require(points is not None and objects is not None, "the tool bar has the pick mode buttons")
        check(points.isEnabled(), "the pick modes are enabled on geometry")
        click(points)
        ok = yield ("until", lambda: prop("natronGeoPickMode") == "points", POLL_TIMEOUT_S)
        check(ok and points.isChecked(), "the Points button switches the pick mode (%r)" % (prop("natronGeoPickMode"),))
        click(objects)
        ok = yield ("until", lambda: prop("natronGeoPickMode") == "objects", POLL_TIMEOUT_S)
        check(ok, "the Objects button switches the pick mode back")

        section("scene display")
        grid = control("sceneDisplayBar", "sceneShowGrid")
        require(grid is not None, "the display bar has a grid button")
        before = bool(prop("natronSceneShowGrid"))
        click(grid)
        ok = yield ("until", lambda: bool(prop("natronSceneShowGrid")) != before, POLL_TIMEOUT_S)
        check(ok and grid.isChecked() == (not before), "the grid button toggles the grid")
        menu_grid = yield from context_menu_state(["Display"], "showGrid")
        check(menu_grid == (not before), "the context menu's Show Grid follows the bar (%r)" % (menu_grid,))
        click(grid)
        wireframe = bar_menu_action("sceneDisplayBar", "sceneDrawMode", "wireframe")
        require(wireframe is not None, "the draw mode menu has Wireframe")
        wireframe.trigger()
        ok = yield ("until", lambda: prop("natronSceneDrawMode") == "wireframe", POLL_TIMEOUT_S)
        check(ok, "the draw mode menu picks Wireframe (%r)" % (prop("natronSceneDrawMode"),))
        menu_wire = yield from context_menu_state([], "wireframe")
        check(menu_wire is True, "the context menu's Wireframe is checked (%r)" % (menu_wire,))
        shaded = bar_menu_action("sceneDisplayBar", "sceneDrawMode", "shaded")
        if shaded is not None:
            shaded.trigger()
        check(len(bar_menu_texts("sceneDisplayBar", "scenePurposes")) == 3, "the purposes menu has the three purposes")
        purposes_before = [n for n in ("proxy", "render", "guide")
                           if bar_menu_action("sceneDisplayBar", "scenePurposes", n).isChecked()]
        for wanted, tile in (([], "purpose-default"), (["proxy"], "purpose-proxy"), (["render"], "purpose-render"),
                             (["guide"], "purpose-guide"), (["proxy", "guide"], "purpose-several")):
            set_purposes(wanted)
            expected = "natron/%s.svg" % tile
            ok = yield ("until", lambda: icon("sceneDisplayBar", "scenePurposes") == expected, POLL_TIMEOUT_S)
            check(ok, "showing %s puts %s on the Purposes button (%r)" % (wanted or "no purpose", tile, icon("sceneDisplayBar", "scenePurposes")))
        set_purposes(purposes_before)
        check(len(bar_menu_texts("sceneDisplayBar", "sceneLighting")) == 3, "the lighting menu has the three modes")

        section("geometry display")
        markers = control("sceneDisplayBar", "pointMarkers")
        require(markers is not None, "the display bar has a pointMarkers toggle")
        for name in ("pointNumbers", "primitiveNumbers", "normals", "sceneDisplayOptions"):
            check(control("sceneDisplayBar", name) is not None, "the display bar has a %s control" % name)
        viewer.getParam("pointMarkers").setValue(False)
        click(markers)
        ok = yield ("until", lambda: knob("pointMarkers") is True, POLL_TIMEOUT_S)
        check(ok, "the Point Markers button sets the pointMarkers knob")
        shot("m602-point-markers.png")
        viewer.getParam("pointMarkers").setValue(False)
        yield settle()
        check(not markers.isChecked(), "the Point Markers button follows its knob")
        viewer.getParam("normals").setValue(True)
        menu_normals = yield from context_menu_state(["Geometry Display"], "normals")
        check(menu_normals is True, "the context menu's Normals follows its knob (%r)" % (menu_normals,))
        viewer.getParam("normals").setValue(False)
        scale = control("sceneDisplayBar", "normalScale")
        group = control("sceneDisplayBar", "highlightGroup")
        require(isinstance(scale, QDoubleSpinBox) and isinstance(group, QLineEdit), "the Display Options popup has Normal Scale and Highlight Group")
        scale.setValue(0.5)
        ok = yield ("until", lambda: abs(knob("normalScale") - 0.5) < 1e-9, POLL_TIMEOUT_S)
        check(ok, "Normal Scale sets the normalScale knob (%r)" % (knob("normalScale"),))
        group.setText("picked")
        group.editingFinished.emit()
        ok = yield ("until", lambda: knob("highlightGroup") == "picked", POLL_TIMEOUT_S)
        check(ok, "Highlight Group sets the highlightGroup knob (%r)" % (knob("highlightGroup"),))
        viewer.getParam("highlightGroup").setValue("")
        viewer.getParam("normalScale").setValue(0.1)

        section("scene input")
        view_input(to_usd, "GeoToUSD")
        ok = yield ("until", lambda: scene_drawn() and not prop("natronGeoComponents"), POLL_TIMEOUT_S)
        require(ok, "the 3D view shows the GeoToUSD scene")
        yield settle()
        markers = control("sceneDisplayBar", "pointMarkers")
        check(markers is not None and markers.isVisible() and not markers.isEnabled(),
              "the geometry toggles are disabled, not hidden, for a scene")
        points = control("sceneToolBar", "geoPickPoints")
        check(points is not None and points.isVisible() and not points.isEnabled(), "the pick modes are disabled for a scene")
        draw = control("sceneDisplayBar", "sceneDrawMode")
        check(draw is not None and draw.isEnabled(), "the draw mode menu stays enabled for a scene")

        section("top bar")
        renderers = bar_menu_texts("sceneTopBar", "sceneRenderer")
        report("NOTE renderer menu: %r" % (renderers,))
        check("Scene Default" in renderers, "the renderer menu lists the renderers")
        button = control("sceneTopBar", "sceneRenderer")
        menu = button.menu()
        _held.append(menu)
        menu.aboutToShow.emit()
        storm = [a for a in menu.actions() if a.isCheckable() and a.isChecked()]
        others = [a for a in menu.actions() if a.isEnabled() and a.isCheckable() and not a.isChecked()
                  and a.text().replace("&", "") != "Scene Default" and "unavailable" not in a.text()]
        if others:
            _held.extend(others[:1] + storm[:1])
            others[0].trigger()
            ok = yield ("until", lambda: prop("natronSceneRenderer") not in (None, STORM_ID), POLL_TIMEOUT_S)
            check(ok, "the renderer menu switches the renderer to %s (%r)" % (others[0].text(), prop("natronSceneRenderer")))
            back = bar_menu_action("sceneTopBar", "sceneRenderer", storm[0].text().replace("&", "")) if storm else None
            if back is not None:
                back.trigger()
            ok = yield ("until", lambda: prop("natronSceneRenderer") == STORM_ID and scene_drawn(), POLL_TIMEOUT_S)
            check(ok, "the renderer menu switches back to Storm (%r)" % (prop("natronSceneRenderer"),))
        else:
            report("NOTE only one renderer is available; switching is not exercised")
        samples = control("sceneTopBar", "maxSamples")
        everything = control("sceneTopBar", "renderEverything")
        check(samples is not None and not samples.isVisible(), "Storm hides the progressive controls")
        check(everything is not None and everything.isVisible(), "Render Everything shows on Storm")
        free = bar_menu_action("sceneTopBar", "sceneLookThrough", "Free Camera")
        require(free is not None, "the look-through menu has Free Camera")
        free.trigger()
        yield settle()
        check(not prop("natronSceneLookThrough"), "Free Camera looks through the viewer's camera (%r)" % (prop("natronSceneLookThrough"),))
        check(len(bar_menu_texts("sceneTopBar", "sceneView")) > 1, "the view menu lists the views")
        check(icon("sceneTopBar", "sceneView") == "natron/view-perspective.svg",
              "the View button shows perspective (%r)" % (icon("sceneTopBar", "sceneView"),))
        views = bar_menu_texts("sceneTopBar", "sceneView")
        front = bar_menu_action("sceneTopBar", "sceneView", views[1])
        require(front is not None, "the view menu has an orthographic view")
        front.trigger()
        ok = yield ("until", lambda: icon("sceneTopBar", "sceneView") == "natron/view-orthographic.svg", POLL_TIMEOUT_S)
        check(ok, "an orthographic view puts the orthographic camera on the View button (%r, %r)"
              % (prop("natronSceneViewMode"), icon("sceneTopBar", "sceneView")))
        shot("m602-orthographic.png")
        back = bar_menu_action("sceneTopBar", "sceneView", views[0])
        require(back is not None, "the view menu has the perspective view")
        back.trigger()
        ok = yield ("until", lambda: icon("sceneTopBar", "sceneView") == "natron/view-perspective.svg", POLL_TIMEOUT_S)
        check(ok, "the perspective view puts the perspective camera back on the View button")
        yield settle()
        check(len(bar_menu_texts("sceneTopBar", "sceneClipping")) >= 1, "the clipping menu has its entries")

        section("tool options")
        options = control("sceneTopBar", "sceneToolOptions")
        require(options is not None, "the top bar has a tool options area")
        check(not options.isVisible(), "the tool options take no room with no panel open")
        view_input(edit, "USDEditTransform")
        yield ("until", scene_drawn, POLL_TIMEOUT_S)
        open_edit_panel()
        ok = yield ("until", lambda: options.isVisible(), POLL_TIMEOUT_S)
        check(ok, "opening the USDEditTransform panel shows its tool options in the top bar")
        mode = [w for w in options.findChildren(QWidget) if w.isVisible() and w.metaObject().className().split("::")[-1] == "ComboBox"]
        check(bool(mode), "the tool options show the Mode choice")
        node_handle = control("sceneToolBar", "sceneToolNodeHandle")
        ok = yield ("until", lambda: node_handle.isEnabled(), POLL_TIMEOUT_S)
        check(ok, "the open USDEditTransform panel enables the node handle tool")
        check("Transform Handle" in node_handle.toolTip(), "the node handle tool names the node's handle (%r)" % node_handle.toolTip())
        check(prop("natronSceneTransformTool") == "select", "the Select tool is current (%r)" % (prop("natronSceneTransformTool"),))
        yield settle()
        yield settle()
        check(handles() == [], "Select draws none of the node's handles (%r)" % (handles(),))
        shot("m602-select-no-handles.png")
        node_handle.click()
        yield settle()
        check(prop("natronSceneTransformTool") == "nodeHandle" and node_handle.isChecked(),
              "clicking the node handle tool picks it (%r)" % (prop("natronSceneTransformTool"),))
        ok = yield ("until", lambda: len(handles()) > 0, POLL_TIMEOUT_S)
        check(ok, "the node handle tool draws the USDEditTransform's handles on %s" % MESH)
        shot("m602-tool-options.png")
        gt.close_all_panels()
        ok = yield ("until", lambda: not options.isVisible(), POLL_TIMEOUT_S)
        check(ok, "closing the panel clears the tool options")
        ok = yield ("until", lambda: not node_handle.isEnabled(), POLL_TIMEOUT_S)
        check(ok, "closing the panel disables the node handle tool")
        select = control("sceneToolBar", "sceneToolSelect")
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "select", POLL_TIMEOUT_S)
        check(ok and select.isChecked() and not node_handle.isChecked(),
              "closing the panel switches the node handle tool back to Select (%r)" % (prop("natronSceneTransformTool"),))
        yield settle()
        check(handles() == [], "and the node's handles are gone (%r)" % (handles(),))

        section("node handle tool key")
        check(not override_accepted(Qt.Key_Return), "the view lets Return pass on while no node offers a handle")
        key(Qt.Key_Return)
        yield settle()
        check(prop("natronSceneTransformTool") == "select", "Return keeps Select while no node offers a handle (%r)"
              % (prop("natronSceneTransformTool"),))
        open_edit_panel()
        ok = yield ("until", lambda: node_handle.isEnabled(), POLL_TIMEOUT_S)
        require(ok, "reopening the USDEditTransform panel enables the node handle tool")
        check(override_accepted(Qt.Key_Return), "the view takes Return while the node offers a handle")
        menu_text = yield from context_menu_read(["Transform"], "nodeHandle", lambda action: action.text().replace("&", ""))
        check(menu_text == "Transform Handle", "the context menu names the node handle tool after the node's handle (%r)" % (menu_text,))
        key(Qt.Key_Return)
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "nodeHandle", POLL_TIMEOUT_S)
        check(ok and node_handle.isChecked(), "Return picks the node handle tool (%r)" % (prop("natronSceneTransformTool"),))

        section("panel closed mid-drag")
        ok = yield ("until", lambda: len(handle_points()) > 0, POLL_TIMEOUT_S)
        require(ok, "the node handle tool draws the handles to drag")
        start = handle_points()[0]
        mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        mouse(QEvent.MouseMove, start + QPointF(4, 0), Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseMove, start + QPointF(8, 0), Qt.NoButton, Qt.LeftButton)
        gt.close_all_panels()
        ok = yield ("until", lambda: not node_handle.isEnabled(), POLL_TIMEOUT_S)
        check(ok, "closing the panel mid-drag disables the node handle tool")
        yield settle()
        check(prop("natronSceneTransformTool") == "nodeHandle", "the drag under way keeps the node handle tool (%r)"
              % (prop("natronSceneTransformTool"),))
        mouse(QEvent.MouseButtonRelease, start + QPointF(8, 0), Qt.LeftButton, Qt.NoButton)
        ok = yield ("until", lambda: prop("natronSceneTransformTool") == "select", POLL_TIMEOUT_S)
        check(ok and select.isChecked() and not node_handle.isChecked(),
              "releasing the drag switches back to Select (%r)" % (prop("natronSceneTransformTool"),))

        section("image view")
        constant = app.createNode(CONSTANT_ID)
        if constant is not None:
            constant.setPosition(300, 200)
            view_input(constant, "Constant")
            ok = yield ("until", lambda: view() is None, POLL_TIMEOUT_S)
            check(ok, "the Viewer shows the image view")
            yield settle()
            for name in ("sceneToolBar", "sceneDisplayBar", "sceneTopBar"):
                check(not shown(name), "the %s hides in the 2D view by default" % name)
            top = find(QAction, "viewerToggleTopBar")
            check(top is not None and not top.isEnabled(), "the Top Bar action is disabled in the 2D view")
            shot("m602-image-view.png")
            view_input(edit, "USDEditTransform")
            ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
            require(ok, "the 3D view is back")
        else:
            report("NOTE no Constant plug-in; the 2D view defaults are not checked")

        section("saved bar flags")
        toggle = find(QAction, "viewerToggleDisplayBar")
        require(toggle is not None and toggle.isChecked(), "the Display Bar action is checked in the 3D view")
        toggle.trigger()
        yield settle()
        check(not shown("sceneDisplayBar") and shown("sceneToolBar"), "the Display Bar action hides only the display bar")
        shot("m602-display-bar-hidden.png")
        menu_bar = yield from context_menu_state(["Bars"], "viewerToggleDisplayBar")
        check(menu_bar is False, "the context menu's Display Bar is unchecked (%r)" % (menu_bar,))
        path = os.path.join(gt.OUT, "m602_viewer_toolbars.ntpz")
        require(app.saveProjectAs(path), "save the project to " + path)
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        del _held[:]
        require(app.loadProject(path) is not None, "reopen " + path)
        ok = yield ("until", lambda: app.getNode("Viewer1") is not None, LOAD_TIMEOUT_S)
        require(ok, "the Viewer is back after reopening")
        ok = yield ("until", scene_drawn, LOAD_TIMEOUT_S)
        require(ok, "the reopened Viewer shows the scene")
        yield settle()
        check(not shown("sceneDisplayBar"), "the display bar stays hidden after reopening")
        check(shown("sceneToolBar") and shown("sceneTopBar"), "the tool and top bars show after reopening")
        toggle = find(QAction, "viewerToggleDisplayBar")
        check(toggle is not None and not toggle.isChecked(), "the Display Bar action is unchecked after reopening")
        if toggle is not None:
            toggle.trigger()
            yield settle()
            check(shown("sceneDisplayBar"), "the Display Bar action shows the display bar again")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
