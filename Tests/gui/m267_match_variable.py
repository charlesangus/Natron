"""RFVSwitch's Match Variable button writes one pattern row per option of a Choice project variable.

Sections:
  - three options: a Choice project variable with the options alpha, a*b and gamma; an RFVSwitch's
    Variable names it. Before the press the node has one empty row. Pressing Match Variable on the
    open panel makes patternCount 3 with the rows alpha, re:a\\*b (the label with its * escaped, so
    it matches literally) and gamma; the panel shows the three rows and the rest stay hidden;
  - twenty options: a second Choice variable with 20 options, named in Variable. The node has 16
    inputs, so Match Variable fills 16 rows and the node's warning says the last 4 were left out.
Screenshots: m267-before.png (before the first press), m267-after.png (after it) and
m267-left-out.png (after the second).

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m267-match-variable/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m267_match_variable.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 200
SETTLE_MS = 500
POLL_TIMEOUT_S = 15.0
PLUGIN_SWITCH = "RFVSwitch"
INPUT_COUNT = 16
SMALL = "m267Small"
BIG = "m267Big"
SMALL_OPTIONS = ["alpha", "a*b", "gamma"]
SMALL_PATTERNS = ["alpha", "re:a\\*b", "gamma"]
BIG_COUNT = 20

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel

    import guitest as gt
    from guitest import check, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up in a panel die with a temporary wrapper of the panel.
    _held = []

    def main_gui():
        guis = [w for w in gt.widgets_of("Gui") if w.isVisible()]
        if guis:
            _held.append(guis[0])
            return guis[0]
        return None

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def node_panel(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLabel):
                if w.text() == label:
                    _held.append(panel)
                    return panel
            if label in (panel.windowTitle() or ""):
                _held.append(panel)
                return panel
        return None

    def match_button(panel):
        for b in panel.findChildren(QAbstractButton):
            if b.isVisible() and b.text().replace("&", "").strip() == "Match Variable":
                _held.append(b)
                return b
        return None

    def declare_choice(name, options):
        param = app.createChoiceParam(name, name)
        require(param is not None, "app.createChoiceParam made " + name)
        for option in options:
            param.addOption(option, "")
        app.addRFVKnob(param)
        declared = app.getProjectParam(name)
        require(declared is not None and declared.getParent() is not None
                and declared.getParent().getScriptName() == "rfvVariables",
                "%s is a project variable" % name)
        ids = list(declared.getOptions())
        check(ids == list(options), "%s has the options %r (got %r)" % (name, options, ids))

    def pattern_rows(node):
        count = node.getParam("patternCount").getValue()
        return count, [node.getParam("input%d" % i).getValue() for i in range(count)]

    def shown_row_labels(panel):
        return [w.text() for w in panel.findChildren(QLabel) if w.isVisible() and w.text().startswith("Input ")]

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("three options")
        declare_choice(SMALL, SMALL_OPTIONS)
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        switch = app.createNode(PLUGIN_SWITCH, -1, None, props)
        require(switch is not None, "create " + PLUGIN_SWITCH, gt.plugin_problem(PLUGIN_SWITCH))
        switch.setPosition(0, 0)
        switch.getParam("variable").setValue(SMALL)
        count, rows = pattern_rows(switch)
        check(count == 1 and rows == [""], "before the press the node has one empty row (got %d %r)" % (count, rows))

        activate_main_window()
        app.selectNode(switch, True)
        from PySide6.QtCore import QMetaObject
        QMetaObject.invokeMethod(graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: node_panel(switch) is not None, POLL_TIMEOUT_S)
        require(ok, "RFVSwitch's panel opens")
        yield ("sleep", SETTLE_MS)
        panel = node_panel(switch)
        button = match_button(panel)
        require(button is not None, "the panel shows a Match Variable button")
        gt.shot("m267-before.png")

        activate_main_window()
        button.click()
        ok = yield ("until", lambda: pattern_rows(switch)[0] == len(SMALL_OPTIONS), POLL_TIMEOUT_S)
        count, rows = pattern_rows(switch)
        check(ok, "Match Variable made patternCount %d (got %d)" % (len(SMALL_OPTIONS), count))
        check(rows == SMALL_PATTERNS, "the rows are one per option, the * escaped: %r (got %r)" % (SMALL_PATTERNS, rows))
        check(switch.getParam("input%d" % len(SMALL_OPTIONS)).getValue() == "", "the row after the last option is empty")
        yield ("sleep", SETTLE_MS)
        labels = shown_row_labels(node_panel(switch))
        for i in range(len(SMALL_OPTIONS)):
            check(any(t.startswith("Input %d" % i) for t in labels), "the panel shows row Input %d" % i)
        check(not any(t.startswith("Input %d" % len(SMALL_OPTIONS)) for t in labels),
              "the panel hides row Input %d (shown %r)" % (len(SMALL_OPTIONS), labels))
        check(switch.getPersistentMessage() == "", "no warning when every option fits (got %r)" % switch.getPersistentMessage())
        gt.shot("m267-after.png")

        gt.section("twenty options")
        declare_choice(BIG, ["opt%02d" % i for i in range(BIG_COUNT)])
        switch.getParam("variable").setValue(BIG)
        activate_main_window()
        button = match_button(node_panel(switch))
        require(button is not None, "the panel still shows the Match Variable button")
        button.click()
        ok = yield ("until", lambda: pattern_rows(switch)[0] == INPUT_COUNT, POLL_TIMEOUT_S)
        count, rows = pattern_rows(switch)
        check(ok, "Match Variable filled all %d rows (got %d)" % (INPUT_COUNT, count))
        check(rows == ["opt%02d" % i for i in range(INPUT_COUNT)], "the rows are the first %d options (got %r)" % (INPUT_COUNT, rows))
        ok = yield ("until", lambda: "left out" in switch.getPersistentMessage(), POLL_TIMEOUT_S)
        message = switch.getPersistentMessage()
        check(ok, "the node's warning names the options left out (got %r)" % message)
        check(str(BIG_COUNT - INPUT_COUNT) in message and BIG in message,
              "the warning names the variable and the %d left out (got %r)" % (BIG_COUNT - INPUT_COUNT, message))
        yield ("sleep", SETTLE_MS)
        gt.shot("m267-left-out.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
