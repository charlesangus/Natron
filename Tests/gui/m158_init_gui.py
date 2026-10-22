"""Checks when initGui.py runs and that menubar commands added from Python show at once.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m158-init-gui/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m158_init_gui.py build/release/App/Natron

run-gui-test.sh puts Tests/gui/m158_init_gui.natronpath/ on NATRON_PLUGIN_PATH, so its init.py and
initGui.py run at startup. They record what they saw in builtins; initGui.py also adds an action to
natron.nodesMenu() and a menubar command through natron.addMenuCommand.

Sections:
  - startup: init.py ran before initGui.py; initGui.py ran once, with the first window and the
    populated nodes menu already there; its nodes-menu action and its menubar command are present
    once the window is up, and the command runs its function;
  - live: a command added from the Script Editor shows in the menubar immediately and runs;
  - second window: a window opened later has both commands, initGui.py did not run again, and a
    command added while both windows are open shows in both menubars at once.
"""
import builtins
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500

NODES_MENU_ITEM = "M158 initGui item"
STARTUP_COMMAND = ["M158 Init", "Startup command"]
LIVE_COMMAND = ["M158 Live", "Live command"]
SHARED_COMMAND = ["M158 Live", "Shared command"]

LIVE_SCRIPT = """import builtins
import NatronGui

def m158LiveCommand():
    builtins.m158_init_gui_state["live_fired"] += 1

NatronGui.natron.addMenuCommand("M158 Live/Live command", "m158LiveCommand")
"""

SHARED_SCRIPT = """import NatronGui

def m158SharedCommand():
    pass

NatronGui.natron.addMenuCommand("M158 Live/Shared command", "m158SharedCommand")
"""

try:
    import NatronGui
    import shiboken6
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QApplication, QGraphicsView, QPlainTextEdit

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def gui_widgets():
        return [w for w in gt.widgets_of("Gui") if w.isVisible()]

    def graph_of(gui):
        for g in gui.findChildren(QGraphicsView):
            if gt.class_name(g) == "NodeGraph" and g.isVisible():
                return g
        return None

    def menubar_action(gui, path):
        """The menubar action at path (menu titles, then the item's text), or None."""
        actions = gui.menuBar().actions()
        for i, text in enumerate(path):
            found = next((a for a in actions if a.text() == text), None)
            if found is None:
                return None
            if i == len(path) - 1:
                return found
            if found.menu() is None:
                return None
            actions = found.menu().actions()
        return None

    def script_editor_of(gui):
        editors = gt.widgets_of("ScriptEditor")
        mine = [e for e in editors if ptr(e.window()) == ptr(gui)]
        return (mine or editors or [None])[0]

    def run_in_script_editor(gui, script):
        """Types script into gui's Script Editor and runs it the way its Execute button does."""
        editor = script_editor_of(gui)
        if not check(editor is not None, "found a Script Editor"):
            return False
        inputs = [w for w in editor.findChildren(QPlainTextEdit) if gt.class_name(w) == "InputScriptTextEdit"]
        if not check(len(inputs) == 1, "the Script Editor has one input field (got %d)" % len(inputs)):
            return False
        inputs[0].setPlainText(script)
        QMetaObject.invokeMethod(editor, "onExecScriptClicked")
        return True

    def steps():
        ok = yield ("until", lambda: len(gui_widgets()) > 0 and graph_of(gui_widgets()[0]) is not None,
                    STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        main_gui = gui_widgets()[0]
        yield ("sleep", SETTLE_MS)

        gt.section("startup")
        order = getattr(builtins, "m158_startup_order", None)
        state = getattr(builtins, "m158_init_gui_state", None)
        require(order is not None, "init.py ran (is run-gui-test.sh putting m158_init_gui.natronpath on the path?)")
        require(state is not None, "initGui.py ran")
        report("NOTE initGui.py saw: %r" % state)
        check(order == ["init.py", "initGui.py"], "init.py ran before initGui.py, each once (got %r)" % order)
        check(state["runs"] == 1, "initGui.py ran once (got %d)" % state["runs"])
        check(state["windows"] == 1, "initGui.py ran after the first window was built (saw %d)" % state["windows"])
        check(state["had_app1"], "initGui.py ran with app1 declared")
        check(state["had_nodes_menu"] and state["nodes_menu_actions"] > 0,
              "initGui.py saw the populated nodes menu (%d top-level entries)" % state["nodes_menu_actions"])
        menu = NatronGui.natron.nodesMenu()
        require(menu is not None, "natron.nodesMenu() returns a menu")
        check(any(a.text() == NODES_MENU_ITEM for a in menu.actions()),
              "the nodes menu holds the action initGui.py added")
        startup = menubar_action(main_gui, STARTUP_COMMAND)
        if check(startup is not None, "the menubar holds the command initGui.py added"):
            startup.trigger()
            QApplication.processEvents()
            check(state["startup_fired"] == 1, "triggering it runs its function (ran %d times)" % state["startup_fired"])

        gt.section("live")
        check(menubar_action(main_gui, LIVE_COMMAND) is None, "the live command is not in the menubar yet")
        if run_in_script_editor(main_gui, LIVE_SCRIPT):
            live = menubar_action(main_gui, LIVE_COMMAND)
            if check(live is not None, "a command added from the Script Editor is in the menubar at once"):
                live.trigger()
                QApplication.processEvents()
                check(state["live_fired"] == 1, "triggering it runs its function (ran %d times)" % state["live_fired"])
        gt.shot("m158-init-gui-window1.png")

        gt.section("second window")
        app2 = app.newProject()
        require(app2 is not None, "app.newProject() opened a second window")
        ok = yield ("until", lambda: len(gui_widgets()) >= 2 and all(graph_of(g) is not None for g in gui_widgets()),
                    STARTUP_TIMEOUT_S)
        require(ok, "the second window and its node graph are up")
        yield ("sleep", SETTLE_MS)
        second_gui = next(g for g in gui_widgets() if ptr(g) != ptr(main_gui))
        check(state["runs"] == 1 and builtins.m158_startup_order == ["init.py", "initGui.py"],
              "opening a second window does not run initGui.py again")
        check(menubar_action(second_gui, STARTUP_COMMAND) is not None,
              "window 2's menubar holds the command initGui.py added")
        check(menubar_action(second_gui, LIVE_COMMAND) is not None,
              "window 2's menubar holds the command added from the Script Editor before it opened")
        check(menubar_action(main_gui, LIVE_COMMAND) is not None
              and len([a for a in menubar_action(main_gui, LIVE_COMMAND[:1]).menu().actions()
                       if a.text() == LIVE_COMMAND[1]]) == 1,
              "window 1 still has the live command, once")

        if run_in_script_editor(second_gui, SHARED_SCRIPT):
            check(menubar_action(main_gui, SHARED_COMMAND) is not None,
                  "a command added with two windows open is in window 1's menubar at once")
            check(menubar_action(second_gui, SHARED_COMMAND) is not None,
                  "...and in window 2's")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
