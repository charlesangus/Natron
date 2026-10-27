"""RFVRead's Variables list follows the viewer and the graph without a render.

Graph: CheckerBoard -> RFVRead -> SetA (var = 'a') -> Viewer input 1 and
       RFVRead -> SetB (var = 'b') -> Viewer input 2. The project declares shot = 'sh010'.

Sections:
  - panel: RFVRead's settings panel opens, fits the 1920x1080 screen, and its Variables field is
    read-only but selectable (for copy);
  - viewer: with the Viewer's A input on SetA the list reads var = 'a'; switching A to SetB makes it
    var = 'b', and the header names the Viewer;
  - set edit: changing SetB's var to 'c' updates the list to var = 'c';
  - graph edit: reconnecting SetB to the CheckerBoard takes RFVRead off every viewer path, so the
    list falls back to the project's variables ("from the project", shot listed, var gone);
  - timeline: seeking to another frame updates the header's frame.
Each check polls both the knob's value and the text the panel shows, so an update that reaches the
knob but not the widget fails. Nothing waits for a render: the list must change on its own.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m72-rfvread/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m72_rfv_read.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
POLL_TIMEOUT_S = 15.0
SCREEN = (1920, 1080)

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_READ = "RFVRead"
PLUGIN_SET = "ReverseFlowVariableSet"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"
VARIABLES = "variables"

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QMetaObject, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTextEdit

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up in a panel die with a temporary wrapper of the panel.
    _held = []

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def string_set(name, value, x, y):
        setter = create(PLUGIN_SET, x, y)
        param = setter.createStringParam(name, name)
        require(param is not None, "created a String on " + setter.getLabel())
        setter.addRFVKnob(param)
        param = setter.getParam(name)
        require(param is not None, "%s is a variable of %s" % (name, setter.getLabel()))
        param.setValue(value)
        return setter, param

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def node_panel(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                if w.text() == label:
                    _held.append(panel)
                    return panel
        return None

    def variables_field(node):
        panel = node_panel(node)
        if panel is None:
            return None
        for w in panel.findChildren(QTextEdit):
            if gt.class_name(w) == "AnimatingTextEdit" and w.isVisible():
                _held.append(w)
                return w
        return None

    def shown_text(node):
        field = variables_field(node)
        return field.toPlainText() if field is not None else None

    def knob_text(node):
        return node.getParam(VARIABLES).getValue()

    def wait_for(node, label, predicate):
        """Polls until predicate holds for both the knob's text and the panel's, then checks it."""
        def both():
            QApplication.processEvents()
            shown = shown_text(node)
            return shown is not None and predicate(knob_text(node)) and predicate(shown)

        ok = yield ("until", both, POLL_TIMEOUT_S)
        check(ok, "%s (knob %r, panel %r)" % (label, knob_text(node), shown_text(node)))
        return ok

    def has_line(line):
        return lambda text: line in text.split("\n")

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()

        screen = QGuiApplication.primaryScreen().geometry()
        check((screen.width(), screen.height()) == SCREEN,
              "the screen is %dx%d (got %dx%d)" % (SCREEN[0], SCREEN[1], screen.width(), screen.height()))

        frame_range = app.getProjectParam("frameRange")
        if frame_range is not None:
            frame_range.setValue(1, 0)
            frame_range.setValue(20, 1)

        gt.section("graph")
        shot = app.createStringParam("shot", "Shot")
        require(shot is not None, "created a project String")
        shot.setValue("sh010")
        app.addRFVKnob(shot)

        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = create(PLUGIN_VIEWER, 0, 600)
        checker = create(PLUGIN_CHECKER, 0, 0)
        reader = create(PLUGIN_READ, 0, 150)
        set_a, var_a = string_set("var", "a", -200, 300)
        set_b, var_b = string_set("var", "b", 200, 300)
        connect(reader, 0, checker)
        connect(set_a, 0, reader)
        connect(set_b, 0, reader)
        connect(viewer, 0, set_a)
        connect(viewer, 1, set_b)
        ok = yield ("until", gt.viewer_up, POLL_TIMEOUT_S)
        require(ok, "the viewer is up")
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        py_viewer.setAInput(0)
        yield ("sleep", SETTLE_MS)

        gt.section("panel")
        gt.close_all_panels()
        view = graph_view()
        app.selectNode(reader, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: variables_field(reader) is not None, POLL_TIMEOUT_S)
        require(ok, "RFVRead's panel shows its Variables field")
        panel = node_panel(reader)
        top_left = panel.mapToGlobal(panel.rect().topLeft())
        check(top_left.x() >= 0 and top_left.y() >= 0
              and top_left.x() + panel.width() <= SCREEN[0] and top_left.y() + panel.height() <= SCREEN[1],
              "the panel lies within %dx%d (at %d,%d size %dx%d)"
              % (SCREEN[0], SCREEN[1], top_left.x(), top_left.y(), panel.width(), panel.height()))
        field = variables_field(reader)
        check(field.isReadOnly(), "Variables is read-only")
        check(bool(field.textInteractionFlags() & Qt.TextSelectableByMouse), "Variables is selectable for copy")

        gt.section("viewer")
        yield from wait_for(reader, "viewing SetA lists var = 'a'", has_line("var = 'a'"))
        yield from wait_for(reader, "the header names the viewer",
                            lambda t: t.startswith("from %s, frame " % viewer.getScriptName()))
        check(has_line("shot = 'sh010'")(knob_text(reader)), "the project's shot is listed")
        gt.shot("m72-rfvread-view-a.png")

        py_viewer.setAInput(1)
        yield from wait_for(reader, "switching the viewer to SetB lists var = 'b'", has_line("var = 'b'"))
        gt.shot("m72-rfvread-view-b.png")

        gt.section("set edit")
        var_b.setValue("c")
        yield from wait_for(reader, "editing SetB's var lists var = 'c'", has_line("var = 'c'"))
        gt.shot("m72-rfvread-set-edit.png")

        gt.section("graph edit")
        connect(set_b, 0, checker)
        yield from wait_for(reader, "off every viewer path the list falls back to the project",
                            lambda t: t.startswith("from the project, frame ") and "var = " not in t
                            and has_line("shot = 'sh010'")(t))
        gt.shot("m72-rfvread-off-path.png")
        connect(set_b, 0, reader)
        yield from wait_for(reader, "reconnecting SetB lists var = 'c' again", has_line("var = 'c'"))

        gt.section("timeline")
        py_viewer.seek(7)
        yield from wait_for(reader, "seeking to frame 7 updates the header",
                            lambda t: t.split("\n")[0].endswith(", frame 7"))
        gt.shot("m72-rfvread-frame7.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
