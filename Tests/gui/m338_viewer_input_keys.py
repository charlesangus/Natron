"""Checks for the viewer input keys and locks.

Number keys never wipe. From the node graph with one usable node selected, a number key pipes that
node to the input and shows it; everywhere else it only shows the input. Alt plus a number locks an
input to the selected node by name, the number key recalls the lock from any focus, and Alt-Shift
plus a number unlocks it. Locked viewers say so in their label and turn dark grey.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m338/out GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m338_viewer_input_keys.py build/fast/App/Natron'

Keys are synthetic QKeyEvents sent to the widget that has the focus in the case at hand; a key the
widget ignores travels up its parents as a real one does. A panel gets no shortcut of its own, so
sending to it exercises the application-wide path. Connections go through the Effect API, since
NodeGraph.scene() raises in this binary. Undo and redo are triggered from the Edit menu, because
Qt's shortcut map takes Ctrl+Z before a sent key event reaches the graph.
"""
import itertools
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 250
STEP_MS = 40
LOCK_GREY = 0x11 / 255.0
COLOUR_TOLERANCE = 0.01
WIPE_NONE = 0
WIPE_UNDER = 1
NUMBER_KEYS = ["Key_1", "Key_2", "Key_3", "Key_4", "Key_5", "Key_6", "Key_7", "Key_8", "Key_9", "Key_0"]

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, Q_ARG, Qt
    from PySide6.QtGui import QCursor, QKeyEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QWidget

    import guitest as gt
    from guitest import check

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    host = app
    # PySide invalidates a child wrapper once the wrapper of the parent it was found through is collected.
    _held_tabs = []

    def create(plugin_id, x, y, group=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = host.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def viewer_tab():
        tabs = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        return tabs[0] if tabs else None

    def settings_panel():
        panels = [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]
        return panels[0] if panels else None

    def focus_widget(focus):
        return {"graph": root_graph, "viewer": viewer_tab, "panel": settings_panel}[focus]()

    def number_key(input_nb):
        return getattr(Qt, NUMBER_KEYS[input_nb])

    def send_key(widget, key, modifiers=Qt.NoModifier):
        if not check(widget is not None, "a widget exists to send the key event to"):
            return
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def press(focus, input_nb, modifiers=Qt.NoModifier):
        send_key(focus_widget(focus), number_key(input_nb), modifiers)

    def trigger_edit_action(prefix):
        window = main_window()
        for act in window.menuBar().actions() if window is not None else []:
            menu = gt.menu_of(act)
            if menu is None or "edit" not in act.text().lower():
                continue
            for action in menu.actions():
                if action.text().lower().startswith(prefix) and action.isEnabled():
                    action.trigger()
                    QApplication.processEvents()
                    return True
        return check(False, "found an enabled '%s' action in the Edit menu" % prefix)

    def undo():
        return trigger_edit_action("undo")

    def redo():
        return trigger_edit_action("redo")

    def pipe(viewer, input_nb):
        source = viewer.getInput(input_nb)
        return source.getScriptName() if source is not None else None

    def active(viewer):
        return (viewer.getParam("aInput").get(), viewer.getParam("bInput").get())

    def operator(viewer):
        return viewer.getParam("operation").get()

    def lock(viewer, input_nb):
        return viewer.getParam("lockedInput%d" % (input_nb + 1)).get()

    def label(viewer):
        return viewer.getParam("userTextArea").get()

    def is_lock_grey(viewer):
        colour = viewer.getColor()
        return all(abs(c - LOCK_GREY) < COLOUR_TOLERANCE for c in colour)

    def name_of(node):
        return node.getScriptName()

    def select(*nodes):
        host.clearSelection()
        for node in nodes:
            host.selectNode(node, False)
        QApplication.processEvents()

    def input_menu(input_side):
        tab = viewer_tab()
        _held_tabs.append(tab)
        tooltip = "Viewer input %s." % input_side
        for w in tab.findChildren(QWidget) if tab is not None else []:
            try:
                if gt.class_name(w) == "ComboBox" and tooltip in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def menu_items(menu):
        return [str(i) for i in (menu.property("natronItems") or [])]

    def menu_current(menu):
        return str(menu.property("natronCurrentText") or "")

    def pick_menu_item(menu, text):
        items = menu_items(menu)
        if not check(text in items, "the menu lists %r (got %r)" % (text, items)):
            return
        QMetaObject.invokeMethod(menu, "setCurrentIndex", Q_ARG(int, items.index(text)))
        QApplication.processEvents()

    def steps():
        global host

        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active", "an inactive window drops shortcuts and graph clicks")

        viewer = host.getNode("Viewer1") or create("Viewer", 900, 600)
        ok = yield ("until", lambda: viewer_tab() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the viewer tab is up")
        viewer.setPosition(900, 600)
        source0 = create("Constant", 300, 100)
        source1 = create("Constant", 500, 100)
        source2 = create("Blur", 700, 100)
        source3 = create("Blur", 900, 100)
        backdrop = create("Backdrop", 1200, 100)
        gt.close_all_panels()

        def reset_viewer():
            for i in range(10):
                if viewer.getInput(i) is not None:
                    viewer.disconnectInput(i)
                viewer.getParam("lockedInput%d" % (i + 1)).set("")
            viewer.connectInput(0, source0)
            viewer.connectInput(1, source1)
            viewer.getParam("aInput").set(0)
            viewer.getParam("bInput").set(0)
            viewer.getParam("operation").set(WIPE_NONE)
            viewer.getParam("userTextArea").set("")
            viewer.resetColor()
            host.clearSelection()
            QApplication.processEvents()

        # ---- number keys never wipe; only the B side turns it on ------------------------------
        gt.section("no-wipe")
        reset_viewer()
        yield ("sleep", SETTLE_MS)
        for _ in range(3):
            for input_nb in (0, 1):
                press("viewer", input_nb)
                yield ("sleep", STEP_MS)
                check(active(viewer)[0] == input_nb, "pressing %d shows that input" % (input_nb + 1))
                check(operator(viewer) == WIPE_NONE,
                      "pressing %d leaves the operator at None" % (input_nb + 1))
        press("viewer", 1, Qt.ShiftModifier)
        yield ("sleep", STEP_MS)
        check(operator(viewer) == WIPE_UNDER, "Shift-2 turns on Wipe Under (got %r)" % operator(viewer))
        check(active(viewer)[1] == 1, "Shift-2 sets input B")
        gt.shot("m338-shift-wipe.png")
        send_key(viewer_tab(), Qt.Key_W)
        yield ("sleep", STEP_MS)
        check(operator(viewer) == WIPE_NONE, "W toggles the wipe off")
        send_key(viewer_tab(), Qt.Key_W)
        yield ("sleep", STEP_MS)
        check(operator(viewer) == WIPE_UNDER, "W toggles the wipe back on")

        reset_viewer()
        menu_b = input_menu("B")
        if check(menu_b is not None, "found the B input menu"):
            pick_menu_item(menu_b, source1.getLabel())
            yield ("sleep", STEP_MS)
            check(active(viewer)[1] == 1, "picking B in the dropdown sets input B")
            check(operator(viewer) == WIPE_UNDER, "picking B in the dropdown turns on Wipe Under")

        # ---- every combination of focus and selection, on each side ---------------------------
        gt.section("routing")
        selections = {
            "none": [],
            "one": [source2],
            "two": [source2, source3],
            "viewer": [viewer],
            "non-connectable": [backdrop],
        }
        panel_owner = create("Blur", 1100, 100)
        for focus, kind, side in itertools.product(("graph", "viewer", "panel"), selections, ("A", "B")):
            case = "%s focus, %s selected, %s side" % (focus, kind, side)
            modifiers = Qt.NoModifier if side == "A" else Qt.ShiftModifier
            side_index = 0 if side == "A" else 1
            for input_nb, connected_to in ((1, name_of(source1)), (2, None)):
                reset_viewer()
                select(*selections[kind])
                yield ("sleep", STEP_MS)
                press(focus, input_nb, modifiers)
                yield ("sleep", STEP_MS)
                pipes_selection = focus == "graph" and kind == "one"
                want_pipe = name_of(source2) if pipes_selection else connected_to
                check(pipe(viewer, input_nb) == want_pipe,
                      "%s: key %d leaves input %d piped from %r (got %r)"
                      % (case, input_nb + 1, input_nb + 1, want_pipe, pipe(viewer, input_nb)))
                check(pipe(viewer, 0) == name_of(source0), "%s: input 1 is untouched" % case)
                if want_pipe is None:
                    check(active(viewer) == (0, 0),
                          "%s: an unconnected input changes nothing (got %r)" % (case, active(viewer)))
                else:
                    check(active(viewer)[side_index] == input_nb,
                          "%s: key %d shows that input (got %r)" % (case, input_nb + 1, active(viewer)))
                    if side == "B":
                        check(active(viewer)[0] == 0, "%s: B leaves input A alone" % case)
                if side == "A":
                    check(operator(viewer) == WIPE_NONE, "%s: no wipe" % case)
        gt.close_all_panels()
        panel_owner.destroy()
        yield ("sleep", SETTLE_MS)

        # A selected node already on the input only gets shown.
        reset_viewer()
        select(source1)
        press("graph", 1)
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 1) == name_of(source1) and active(viewer)[0] == 1,
              "a node already on the input activates it")

        # The mouse leaves the graph for the viewer without a click: the viewer takes the key.
        gt.section("focus-follows-mouse")
        reset_viewer()
        select(source2)
        graph = root_graph()
        tab = viewer_tab()
        QCursor.setPos(graph.mapToGlobal(graph.rect().center()))
        yield ("sleep", SETTLE_MS)
        QCursor.setPos(tab.mapToGlobal(tab.rect().center()))
        ok = yield ("until", lambda: gt.class_name(QApplication.focusWidget() or tab) != "NodeGraph", 3.0)
        check(ok, "moving the mouse onto the viewer takes the focus from the graph")
        send_key(QApplication.focusWidget() or tab, number_key(1))
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 1) == name_of(source1), "the key does not pipe the selection")
        check(active(viewer)[0] == 1, "the key shows the input")
        QCursor.setPos(graph.mapToGlobal(graph.rect().center()))
        yield ("sleep", SETTLE_MS)

        # A graph inside a Group drives the viewer in that Group.
        gt.section("nested-group")
        root = root_graph()
        group = create("Group", 300, 400)
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        inner_graphs = [w for w in gt.widgets_of("NodeGraph") if w != root]
        if check(len(inner_graphs) >= 1, "the Group has a node graph"):
            inner_source = create("Constant", 0, 0, group)
            inner_viewer = create("Viewer", 200, 200, group)
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
            root_before = (pipe(viewer, 2), active(viewer))
            select(inner_source)
            send_key(inner_graphs[-1], number_key(2))
            yield ("sleep", SETTLE_MS)
            check(pipe(inner_viewer, 2) == name_of(inner_source), "the inner viewer gets the pipe")
            check(active(inner_viewer)[0] == 2, "the inner viewer shows the input")
            check((pipe(viewer, 2), active(viewer)) == root_before, "the top-level viewer is untouched")
            inner_viewer.destroy()
        group.destroy()
        yield ("sleep", SETTLE_MS)

        # ---- menus are keyed by input number ---------------------------------------------------
        gt.section("menus")
        reset_viewer()
        source0.setLabel("Same")
        source1.setLabel("Same")
        yield ("sleep", STEP_MS)
        menu_a = input_menu("A")
        if check(menu_a is not None, "found the A input menu"):
            items = menu_items(menu_a)
            check(items[1:] == ["Same (Input 1)", "Same (Input 2)"],
                  "duplicate labels name their input (got %r)" % items)
            pick_menu_item(menu_a, "Same (Input 2)")
            yield ("sleep", STEP_MS)
            check(active(viewer)[0] == 1, "picking the second entry sets input 2")
            check(menu_current(menu_a) == "Same (Input 2)", "the menu shows the picked entry")
            pick_menu_item(menu_a, "Same (Input 1)")
            yield ("sleep", STEP_MS)
            check(active(viewer)[0] == 0, "picking the first entry sets input 1")
            gt.shot("m338-duplicate-labels.png")
            source1.setLabel("Renamed")
            yield ("sleep", STEP_MS)
            items = menu_items(menu_a)
            check(items[1:] == ["Same", "Renamed"], "renaming relabels only that entry (got %r)" % items)
            source1.setLabel("Same")
            yield ("sleep", STEP_MS)
            viewer.disconnectInput(1)
            yield ("sleep", STEP_MS)
            items = menu_items(menu_a)
            check(items[1:] == ["Same"], "disconnecting removes only that entry (got %r)" % items)
        source0.setLabel(name_of(source0))
        source1.setLabel(name_of(source1))

        # ---- one node on several inputs -------------------------------------------------------
        gt.section("multi-pipe")
        reset_viewer()
        viewer.disconnectInput(1)
        select(source2)
        press("graph", 0)
        yield ("sleep", STEP_MS)
        press("graph", 2)
        yield ("sleep", STEP_MS)
        both = lambda: pipe(viewer, 0) == name_of(source2) and pipe(viewer, 2) == name_of(source2)
        check(both(), "one node feeds inputs 1 and 3")
        gt.shot("m338-one-node-two-inputs.png")
        menu_a = input_menu("A")
        if check(menu_a is not None, "found the A input menu"):
            label2 = source2.getLabel()
            check(menu_items(menu_a)[1:] == ["%s (Input 1)" % label2, "%s (Input 3)" % label2],
                  "both inputs are listed (got %r)" % menu_items(menu_a))
        press("viewer", 0)
        yield ("sleep", STEP_MS)
        check(active(viewer)[0] == 0, "key 1 shows input 1")
        press("viewer", 2)
        yield ("sleep", STEP_MS)
        check(active(viewer)[0] == 2, "key 3 shows input 3")

        undo()
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 2) is None and pipe(viewer, 0) == name_of(source2), "undo restores input 3 only")
        redo()
        yield ("sleep", STEP_MS)
        check(both(), "redo connects input 3 again")
        undo()
        undo()
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 0) == name_of(source0) and pipe(viewer, 2) is None, "undo restores input 1 too")
        redo()
        redo()
        yield ("sleep", STEP_MS)
        check(both(), "redo restores both pipes")

        select(source2)
        send_key(root_graph(), Qt.Key_Backspace)
        yield ("sleep", SETTLE_MS)
        check(pipe(viewer, 0) is None and pipe(viewer, 2) is None, "deleting the node clears both inputs")
        undo()
        yield ("sleep", SETTLE_MS)
        check(both(), "undoing the delete restores both inputs")

        # ---- lock and unlock ------------------------------------------------------------------
        gt.section("lock")
        reset_viewer()
        yield ("sleep", STEP_MS)
        select(source2)
        press("graph", 2, Qt.AltModifier)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == name_of(source2), "Alt-3 stores the node's script name")
        check(pipe(viewer, 2) == name_of(source2), "Alt-3 pipes the node")
        check(active(viewer)[0] == 2, "Alt-3 shows the input as A")
        undo()
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == "", "undo restores the lock")
        check(pipe(viewer, 2) is None, "undo restores the pipe")
        check(active(viewer)[0] == 0, "undo restores the active input (got %r)" % (active(viewer),))
        redo()
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == name_of(source2) and pipe(viewer, 2) == name_of(source2), "redo locks again")

        select(source3)
        press("graph", 2, Qt.AltModifier)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == name_of(source3), "Alt-3 on a locked input replaces the lock")
        undo()
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == name_of(source2), "undo brings the replaced lock back")

        before = pipe(viewer, 2), active(viewer)
        host.clearSelection()
        press("graph", 2, Qt.AltModifier | Qt.ShiftModifier)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == "", "Alt-Shift-3 clears the lock")
        check((pipe(viewer, 2), active(viewer)) == before, "Alt-Shift-3 leaves the pipe and active inputs alone")
        press("graph", 2, Qt.AltModifier | Qt.ShiftModifier)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == "" and (pipe(viewer, 2), active(viewer)) == before,
              "Alt-Shift-3 on an unlocked input does nothing")

        reset_viewer()
        for kind in ("none", "two", "viewer", "non-connectable"):
            select(*selections[kind])
            press("graph", 3, Qt.AltModifier)
            yield ("sleep", STEP_MS)
            check(lock(viewer, 3) == "" and pipe(viewer, 3) is None, "Alt-4 with %s selected does nothing" % kind)

        reset_viewer()
        viewer.getParam("downscaleLevel").set(2)
        yield ("sleep", STEP_MS)
        select(source2)
        press("viewer", 0, Qt.AltModifier)
        yield ("sleep", STEP_MS)
        check(viewer.getParam("downscaleLevel").get() == 0, "Alt-1 in the viewer sets the proxy level")
        check(lock(viewer, 0) == "" and pipe(viewer, 0) == name_of(source0),
              "Alt-1 in the viewer does not lock")

        # ---- recalling a locked input ---------------------------------------------------------
        gt.section("recall")
        for focus in ("graph", "viewer", "panel"):
            reset_viewer()
            viewer.connectInput(2, source2)
            viewer.getParam("lockedInput3").set(name_of(source2))
            panel_node = None
            if focus == "panel":
                panel_node = create("Blur", 1100, 300)
                yield ("sleep", SETTLE_MS)
            select(source3)
            press(focus, 2)
            yield ("sleep", STEP_MS)
            check(pipe(viewer, 2) == name_of(source2), "%s focus: the locked pipe stays" % focus)
            check(active(viewer)[0] == 2, "%s focus: the input is shown" % focus)

            viewer.disconnectInput(2)
            yield ("sleep", STEP_MS)
            press(focus, 2)
            yield ("sleep", STEP_MS)
            check(pipe(viewer, 2) == name_of(source2), "%s focus: a pipe dragged off comes back" % focus)
            check(active(viewer)[0] == 2, "%s focus: the restored input is shown" % focus)

            viewer.connectInput(2, source3)
            yield ("sleep", STEP_MS)
            viewer.getParam("aInput").set(0)
            press(focus, 2, Qt.ShiftModifier)
            yield ("sleep", STEP_MS)
            check(pipe(viewer, 2) == name_of(source2), "%s focus: Shift-3 restores the locked pipe" % focus)
            check(active(viewer)[1] == 2 and active(viewer)[0] == 0,
                  "%s focus: Shift-3 sets B and keeps A (got %r)" % (focus, active(viewer)))
            if panel_node is not None:
                gt.close_all_panels()
                panel_node.destroy()
                yield ("sleep", SETTLE_MS)

        reset_viewer()
        viewer.connectInput(2, source2)
        viewer.getParam("lockedInput3").set(name_of(source2))
        select(source2)
        press("graph", 0)
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 0) == name_of(source2) and pipe(viewer, 2) == name_of(source2),
              "a node locked to 3 can be piped to 1 and stays on 3")
        check(active(viewer)[0] == 0, "input 1 is shown")

        reset_viewer()
        viewer.connectInput(2, source2)
        viewer.getParam("lockedInput3").set("NoSuchNode")
        select(source3)
        press("graph", 2)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == "", "a lock whose node cannot be found is cleared")
        check(pipe(viewer, 2) == name_of(source3), "the key then acts as if unlocked")

        for focus in ("graph", "viewer"):
            doomed = create("Blur", 1300, 300)
            gt.close_all_panels()
            reset_viewer()
            viewer.connectInput(2, doomed)
            viewer.getParam("lockedInput3").set(name_of(doomed))
            doomed.destroy()
            yield ("sleep", SETTLE_MS)
            host.clearSelection()
            press(focus, 2)
            yield ("sleep", STEP_MS)
            check(lock(viewer, 2) == "", "%s focus: deleting the locked node clears the lock on press" % focus)
            check(pipe(viewer, 2) is None, "%s focus: nothing stays piped" % focus)

        renamed = create("Blur", 1300, 300)
        gt.close_all_panels()
        reset_viewer()
        viewer.connectInput(2, renamed)
        viewer.getParam("lockedInput3").set(name_of(renamed))
        renamed.setScriptName("m338Renamed")
        yield ("sleep", STEP_MS)
        host.clearSelection()
        press("viewer", 2)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 2) == "", "renaming the locked node clears the lock on press")
        check(pipe(viewer, 2) == "m338Renamed" and active(viewer)[0] == 2,
              "the pipe stays and the input is shown")
        renamed.destroy()
        yield ("sleep", SETTLE_MS)

        # ---- label and colour -----------------------------------------------------------------
        gt.section("decoration")
        reset_viewer()
        viewer.getParam("userTextArea").set("Hero shot")
        select(source2)
        press("graph", 2, Qt.AltModifier)
        yield ("sleep", STEP_MS)
        line3 = "Locked input 3 to %s" % name_of(source2)
        line5 = "Locked input 5 to %s" % name_of(source3)
        check(label(viewer) == "Hero shot\n" + line3, "the first lock adds its line (got %r)" % label(viewer))
        check(is_lock_grey(viewer), "a locked viewer is dark grey (got %r)" % (viewer.getColor(),))
        select(source3)
        press("graph", 4, Qt.AltModifier)
        yield ("sleep", STEP_MS)
        check(label(viewer) == "Hero shot\n" + line3 + "\n" + line5,
              "the second lock adds a line (got %r)" % label(viewer))
        check(is_lock_grey(viewer), "still dark grey with two locks")
        host.clearSelection()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        graph.centerOn(900, 400)
        yield ("sleep", SETTLE_MS)
        gt.shot("m338-locked-viewer-graph.png")

        press("graph", 2, Qt.AltModifier | Qt.ShiftModifier)
        yield ("sleep", STEP_MS)
        check(label(viewer) == "Hero shot\n" + line5, "unlocking removes only that line (got %r)" % label(viewer))
        check(is_lock_grey(viewer), "still dark grey with one lock")

        path = os.path.join(gt.OUT, "m338_viewer_input_keys.ntpz")
        viewer_name = name_of(viewer)
        source3_name = name_of(source3)
        gt.require(host.saveProjectAs(path), "save the project to " + path)
        gt.require(host.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        reloaded = host.loadProject(path)
        gt.require(reloaded is not None, "reopen " + path)
        host = reloaded
        ok = yield ("until", lambda: host.getNode(viewer_name) is not None and viewer_tab() is not None,
                    LOAD_TIMEOUT_S)
        gt.require(ok, "the viewer is back after reopening")
        yield ("sleep", SETTLE_MS)
        viewer = host.getNode(viewer_name)
        source3 = host.getNode(source3_name)
        check(lock(viewer, 4) == source3_name, "the lock survives a reload")
        check(label(viewer) == "Hero shot\n" + line5, "the label survives a reload (got %r)" % label(viewer))
        check(is_lock_grey(viewer), "the colour survives a reload")
        press("viewer", 4)
        yield ("sleep", STEP_MS)
        check(pipe(viewer, 4) == source3_name and active(viewer)[0] == 4, "the reloaded lock recalls")

        viewer.getParam("lockedInput5").set("NoSuchNode")
        press("viewer", 4)
        yield ("sleep", STEP_MS)
        check(lock(viewer, 4) == "", "a lost node's lock is cleared on press")
        check(label(viewer) == "Hero shot", "clearing on press removes its line (got %r)" % label(viewer))
        check(not is_lock_grey(viewer), "with no lock left the colour goes back")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
