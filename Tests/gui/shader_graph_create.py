"""A GUI test of every way a user creates a shader node inside a USD Material Builder: the tab menu
with a typed filter and Enter, the tab menu's row clicked, and the graph's right-click menu's Create
Node, each asserting the node lands inside the builder. The right-click menu's node submenus offer
no shader, and a shader action of the network nodes menu triggered over the main graph creates
nothing there. Inside the builder the right-click menu's and the toolbar's node menus offer no
compositing node (Backdrop and Dot stay; the toolbar lists the builder's shader nodes by target),
and a Python createNode of Blur into the builder is refused without a message box (a script call
is silent) and creates nothing. It is driven by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/create GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_create.py <AppImage or build/fast/App/Natron>'

Shots: shader_graph_create_click.png (the menu before the row click) and shader_graph_create.png
(the builder's graph holding the nodes created).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
MENU_TIMEOUT_S = 10.0
BSDF = "cycles_principled_bsdf"
ADD = "cycles_add_closure"
MIX = "cycles_mix_closure"
CREATE_NODE = "Create Node"
KEPT_IN_BUILDER = ("Backdrop", "Dot")

try:
    import faulthandler

    from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication, QMenu

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg
    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def children_of(group, plugin_id):
        return [n for n in group.getChildren() if n.getPluginID() == plugin_id]

    def root_children_of(plugin_id):
        return [n for n in app.getChildren() if n.getPluginID() == plugin_id]

    def take_python_errors():
        """What Python wrote to stderr since the last call, from the GUI session's stderr catcher,
        which also takes exceptions raised in Qt slots such as the menu's choice."""
        catcher = getattr(sys.modules.get("__main__"), "catchErr", None)
        text = getattr(catcher, "value", "") if catcher is not None else ""
        if text:
            catcher.value = ""
        return text or ""

    def row_of(list_view, plugin_id):
        items = getattr(list_view.model(), "_items", [])
        return next((r for r, item in enumerate(items) if item["menuobj"].property("natronPluginID") == plugin_id), -1)

    def filter_to(line_edit, list_view, text, plugin_id):
        """Types text into the open menu; the row of plugin_id, -1 when it is not listed."""
        line_edit.setText(text)
        QApplication.processEvents()
        row = row_of(list_view, plugin_id)
        check(row >= 0, "the menu filtered by %r offers %s" % (text, plugin_id))
        return row

    def choose_with_enter(popup, line_edit, list_view, text, plugin_id):
        row = filter_to(line_edit, list_view, text, plugin_id)
        if row < 0:
            return False
        list_view.setCurrentIndex(list_view.model().index(row, 0))
        take_python_errors()
        sg.key_click(line_edit, Qt.Key_Return)
        QApplication.processEvents()
        # A posted key press is not spontaneous, so Qt first offers it to the shortcut map, which
        # may take it before the line edit; the menu's choice then never ran.
        if popup.isVisible():
            report("NOTE Enter left the menu open (Python errors: %r); choosing through returnPressed" % take_python_errors())
            line_edit.returnPressed.emit()
            QApplication.processEvents()
        errors = take_python_errors()
        check(not errors, "choosing %s raises no Python error (got %r)" % (plugin_id, errors))
        check(not popup.isVisible(), "choosing %s closes the menu" % plugin_id)
        return True

    def choose_with_click(popup, line_edit, list_view, text, plugin_id):
        row = filter_to(line_edit, list_view, text, plugin_id)
        if row < 0:
            return False
        index = list_view.model().index(row, 0)
        list_view.scrollTo(index)
        QApplication.processEvents()
        centre = QPointF(list_view.visualRect(index).center())
        viewport = list_view.viewport()
        gt.shot("shader_graph_create_click.png")
        take_python_errors()
        for kind, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
            event = QMouseEvent(kind, centre, viewport.mapToGlobal(centre), Qt.LeftButton, buttons, Qt.NoModifier)
            QApplication.sendEvent(viewport, event)
            QApplication.processEvents()
        errors = take_python_errors()
        check(not errors, "clicking %s raises no Python error (got %r)" % (plugin_id, errors))
        check(not popup.isVisible(), "clicking %s's row closes the menu" % plugin_id)
        return True

    def created_in_builder(builder, plugin_id, before, how):
        ok = yield ("until", lambda: len(children_of(builder, plugin_id)) > before, sg.POLL_TIMEOUT_S)
        if not ok:
            report("NOTE builder children %r, root children %r" % (
                sorted(n.getPluginID() for n in builder.getChildren()), sorted(n.getPluginID() for n in app.getChildren())))
        check(ok, "%s creates %s inside the builder" % (how, plugin_id))
        check(not root_children_of(plugin_id), "%s creates no %s in the main graph" % (how, plugin_id))
        yield ("sleep", SETTLE_MS)
        return ok

    def open_menus():
        return [w for w in QApplication.topLevelWidgets() if isinstance(w, QMenu) and w.isVisible()]

    def menu_plugin_ids(menu, found, depth=0):
        """The plug-in IDs the menu's tree offers: its visible actions only."""
        for action in menu.actions():
            if not action.isVisible():
                continue
            value = action.property("natronPluginID")
            if isinstance(value, str) and value:
                found.add(value)
            sub = gt.menu_of(action)
            if sub is not None and depth < 8:
                menu_plugin_ids(sub, found, depth + 1)
        return found

    def right_click_create_node(graph, outcome):
        """Right-clicks graph's viewport centre on the next event loop turn; the menu's exec() runs
        there, and outcome gets the menu's plug-in IDs and whether Create Node was chosen."""
        viewport = graph.viewport()
        centre = QPointF(viewport.rect().center())

        def pick():
            menus = open_menus()
            if not menus:
                if outcome.setdefault("tries", 0) < 40:
                    outcome["tries"] += 1
                    QTimer.singleShot(50, pick)
                return
            menu = menus[0]
            outcome["ids"] = menu_plugin_ids(menu, set())
            create = next((a for a in menu.actions() if a.text().replace("&", "") == CREATE_NODE), None)
            outcome["create"] = create is not None
            if create is None:
                menu.close()
                return
            menu.setActiveAction(create)
            sg.key_click(menu, Qt.Key_Return)

        def press():
            QTimer.singleShot(SETTLE_MS, pick)
            event = QMouseEvent(QEvent.MouseButtonPress, centre, viewport.mapToGlobal(centre), Qt.RightButton, Qt.RightButton, Qt.NoModifier)
            QApplication.sendEvent(viewport, event)
            outcome["closed"] = True

        QTimer.singleShot(0, press)

    def toolbar_buttons():
        """The left toolbar's buttons: inside the builder one per target and Backdrop."""
        return sg.toolbox_buttons()

    def press_toolbar_button(button, outcome):
        """Presses button on the next event loop turn; its menu's exec() runs there, and outcome
        gets the plug-in IDs the menu offers while it is shown, none when it never shows."""
        centre = QPointF(button.rect().center())

        def pick():
            menus = open_menus()
            if not menus:
                if outcome.setdefault("tries", 0) < 6:
                    outcome["tries"] += 1
                    QTimer.singleShot(50, pick)
                return
            outcome["ids"] = menu_plugin_ids(menus[0], set())
            menus[0].close()

        def press():
            QTimer.singleShot(SETTLE_MS // 2, pick)
            event = QMouseEvent(QEvent.MouseButtonPress, centre, button.mapToGlobal(centre), Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
            QApplication.sendEvent(button, event)
            release = QMouseEvent(QEvent.MouseButtonRelease, centre, button.mapToGlobal(centre), Qt.LeftButton, Qt.NoButton, Qt.NoModifier)
            QApplication.sendEvent(button, release)
            outcome["closed"] = True

        QTimer.singleShot(0, press)

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        sg.park_viewer(app)
        require(ok, "the main window is up")
        shaders = sg.shader_targets()
        require(BSDF in shaders and ADD in shaders and MIX in shaders, "the manifests list %s, %s and %s" % (BSDF, ADD, MIX))

        gt.section("builder graph")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        graph = sg.graph_of(builder)

        gt.section("tab menu, typed and Enter")
        popup, line_edit, list_view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu in the builder's graph")
        target = tabtabtab_natron._network_target(tabtabtab_natron._plugin.context() or {})
        require(target == "cycles", "the builder's menu starts on the cycles target (got %r)" % target)
        before = len(children_of(builder, BSDF))
        if choose_with_enter(popup, line_edit, list_view, "principled", BSDF):
            yield from created_in_builder(builder, BSDF, before, "Tab, typing and Enter")
        else:
            sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)

        gt.section("tab menu, row clicked")
        popup, line_edit, list_view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu again")
        before = len(children_of(builder, ADD))
        if choose_with_click(popup, line_edit, list_view, "add", ADD):
            yield from created_in_builder(builder, ADD, before, "clicking the menu's row")
        else:
            sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)

        gt.section("right-click Create Node")
        focused = yield from sg.focus_graph(graph)
        require(focused, "the builder's graph has the focus")
        outcome = {}
        right_click_create_node(graph, outcome)
        ok = yield ("until", lambda: outcome.get("closed"), MENU_TIMEOUT_S)
        require(ok, "the right-click menu closes")
        require(outcome.get("create"), "the right-click menu offers %s" % CREATE_NODE)
        offered = sorted(pid for pid in outcome.get("ids", ()) if pid in shaders)
        check(not offered, "the right-click menu's node submenus offer no shader (got %r)" % offered[:5])
        compositing = sorted(pid for pid in outcome.get("ids", ()) if pid not in KEPT_IN_BUILDER)
        check(not compositing, "the right-click menu's node submenus offer no compositing node (got %r)" % compositing[:5])
        check("Backdrop" in outcome.get("ids", ()), "the right-click menu's node submenus still offer Backdrop")
        popup, line_edit, list_view = yield from sg.wait_for_popup()
        require(popup is not None, "%s opens the tab menu" % CREATE_NODE)
        check(tabtabtab_natron._is_network_context(tabtabtab_natron._plugin.context()),
              "%s's menu is filtered for the builder" % CREATE_NODE)
        before = len(children_of(builder, MIX))
        if choose_with_enter(popup, line_edit, list_view, "mix", MIX):
            yield from created_in_builder(builder, MIX, before, "the right-click menu's %s" % CREATE_NODE)
        else:
            sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)
        graph.centerOn(0, 0)
        QApplication.processEvents()
        gt.shot("shader_graph_create.png")

        gt.section("toolbar and Python")
        yield ("until", sg.toolbox_settled(lambda n: n > 1), 20.0)
        buttons = toolbar_buttons()
        require(len(buttons) > 1, "the toolbar has the target buttons and Backdrop (%d)" % len(buttons))
        shown = 0
        kept = set()
        for button in buttons:
            focused = yield from sg.focus_graph(graph)
            require(focused, "the builder's graph has the focus")
            outcome = {}
            press_toolbar_button(button, outcome)
            ok = yield ("until", lambda: outcome.get("closed") and (outcome.get("ids") is not None or outcome.get("tries", 0) >= 6), MENU_TIMEOUT_S)
            require(ok, "the toolbar button's menu closes")
            yield ("sleep", SETTLE_MS)
            ids = outcome.get("ids")
            if ids is None:
                continue
            shown += 1
            kept.update(ids)
            compositing = sorted(pid for pid in ids if pid not in KEPT_IN_BUILDER and pid not in shaders)
            check(not compositing, "the toolbar button %r offers no compositing node inside the builder (got %r)"
                  % (button.toolTip(), compositing[:5]))
        check("Backdrop" in kept, "the toolbar still offers Backdrop inside the builder (offered %r)" % sorted(kept))
        check(BSDF in kept, "the toolbar offers %s inside the builder" % BSDF)
        check(shown >= 1, "a toolbar button's menu was shown inside the builder (%d)" % shown)

        before = sorted(n.getPluginID() for n in builder.getChildren())
        created, asked = sg.answering(lambda: app.createNode("Blur", -1, builder))
        check(created is None, "a Python createNode of Blur into the builder returns None")
        check(not asked, "the refusal of a silent script createNode shows no message box (answered: %r)" % asked)
        check(sorted(n.getPluginID() for n in builder.getChildren()) == before, "the refused node is not in the builder")
        check(not root_children_of("Blur"), "nor in the main graph")
        backdrop = app.createNode("Backdrop", -1, builder)
        check(backdrop is not None and backdrop.getPluginID() == "Backdrop", "a Python createNode of Backdrop into the builder is allowed")
        yield ("sleep", SETTLE_MS)

        gt.section("main graph")
        root = sg.show_root_graph(app)
        require(root is not None, "the main graph is shown")
        popup, _line_edit, _list_view = yield from sg.open_tab_menu(root)
        require(popup is not None, "Tab opens the menu in the main graph")
        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)
        nodes, _infos = tabtabtab_natron._network_node_items()
        action = next((item["menuobj"] for item in nodes if item["menuobj"].property("natronPluginID") == BSDF), None)
        require(action is not None, "the network nodes menu lists %s" % BSDF)
        in_builder = len(children_of(builder, BSDF))
        app.triggerTabMenuAction(action)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check(not root_children_of(BSDF), "the network nodes menu's %s over the main graph creates nothing there" % BSDF)
        check(len(children_of(builder, BSDF)) == in_builder, "nor does it create one in the builder")
        action = None

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
