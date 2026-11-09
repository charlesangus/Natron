"""DirBlur's per-mode controls and the GodRays node in the app (M130 - Native Edge-Preserving And
Directional Filters).

Sections (the source is a CheckerBoard, shown in the Viewer through the node under test):
  - dirblur: for Directional, Directional with Use Target, Radial and Zoom, the panel shows only the
    controls that mode uses (Direction, Use Target, Target, Center, Reset Center, Pixel Offset are
    checked shown or hidden) and has no transform controls; a full-screen shot of each mode;
  - godrays: the GodRays panel has the transform, From Color, To Color, Gamma, Samples and Max knobs,
    labelled as their script names read, with Uniform and Skew Order hidden; a full-screen shot at the
    defaults and with rays (Scale 1.4, To Color dark, Gamma 2).
Screenshots: m130-dirblur-directional.png, m130-dirblur-directional-target.png,
m130-dirblur-radial.png, m130-dirblur-zoom.png, m130-godrays-defaults.png, m130-godrays-rays.png.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup; the harness must
run Xvfb at 1920x1080):
    docker exec -e NATRON_GUI_TEST_OUT=build/m130-dirblur-godrays/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m130_dirblur_godrays.py build/release/App/Natron
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 300
SETTLE_MS = 500
RENDER_MS = 2000
POLL_TIMEOUT_S = 20.0
PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_DIRBLUR = "DirBlur"
PLUGIN_GODRAYS = "GodRays"

# The DirBlur knobs whose visibility follows the mode.
DIRBLUR_VARYING = ["direction", "useTarget", "target", "center", "resetCenter", "pixelOffset"]
# Knobs DirBlur shows in every mode.
DIRBLUR_ALWAYS = ["mode", "length", "samples", "centered", "falloff", "max", "fromColor", "toColor", "ramp"]
# Knobs that moved to GodRays.
DIRBLUR_GONE = ["translate", "rotate", "scale", "uniform", "skewX", "skewY", "skewOrder", "transformAmount", "invert", "interactive"]
# (shot name, mode option, use target, the varying knobs shown).
DIRBLUR_MODES = [
    ("m130-dirblur-directional", "directional", False, {"direction", "useTarget"}),
    ("m130-dirblur-directional-target", "directional", True, {"useTarget", "target", "center", "resetCenter"}),
    ("m130-dirblur-radial", "radial", False, {"center", "resetCenter"}),
    ("m130-dirblur-zoom", "zoom", False, {"center", "resetCenter", "pixelOffset"}),
]
# Script name, label, shown.
GODRAYS_KNOBS = [
    ("translate", "Translate", True),
    ("rotate", "Rotate", True),
    ("scale", "Scale", True),
    ("skewX", "Skew X", True),
    ("skewY", "Skew Y", True),
    ("center", "Center", True),
    ("fromColor", "From Color", True),
    ("toColor", "To Color", True),
    ("gamma", "Gamma", True),
    ("samples", "Samples", True),
    ("max", "Max", True),
    ("uniform", "Uniform", False),
    ("skewOrder", "Skew Order", False),
]

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    import guitest as gt
    from guitest import check, report, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    # Wrappers of widgets looked up in a panel die with a temporary wrapper of the panel.
    _held = []
    _viewer = []

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
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened."""
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            activate_main_window()
            app.selectNode(node, True)
            QApplication.processEvents()
            view = graph_view()
            invoked = view is not None and QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            if not invoked:
                report("NOTE showSelectedNodeSettingsPanel was not invoked (attempt %d)" % (attempt + 1))
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        require(node.connectInput(input_nb, source),
                "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def set_choice(node, name, option):
        p = param(node, name)
        options = [str(o).lower() for o in p.getOptions()]
        matches = [i for i, o in enumerate(options) if o.startswith(option)]
        require(matches, "%s's %s offers %s (%r)" % (node.getLabel(), name, option, options))
        p.setValue(matches[0])

    def viewer_node():
        if not _viewer:
            _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
            require(_viewer[0] is not None, "the project has a Viewer")
        return _viewer[0]

    def show(node):
        viewer = viewer_node()
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def section_dirblur(source):
        gt.section("dirblur")
        node = create(PLUGIN_DIRBLUR, 0, 150)
        connect(node, 0, source)
        show(node)
        for name in DIRBLUR_GONE:
            check(node.getParam(name) is None, "DirBlur has no %s knob" % name)
        for shot_name, mode, use_target, shown in DIRBLUR_MODES:
            set_choice(node, "mode", mode)
            param(node, "useTarget").setValue(use_target)
            what = mode + (" with Use Target" if use_target else "")
            for name in DIRBLUR_ALWAYS:
                check(param(node, name).getIsVisible(), "%s: DirBlur shows %s" % (what, name))
            for name in DIRBLUR_VARYING:
                expected = name in shown
                check(param(node, name).getIsVisible() == expected,
                      "%s: DirBlur %s %s" % (what, "shows" if expected else "hides", name))
            ok = yield from open_panel(node)
            check(ok, "DirBlur's panel opens in %s" % what)
            yield ("sleep", RENDER_MS)
            gt.shot(shot_name + ".png")
        param(node, "useTarget").setValue(False)
        gt.close_all_panels()
        yield ("sleep", 300)

    def section_godrays(source):
        gt.section("godrays")
        node = create(PLUGIN_GODRAYS, 150, 150)
        connect(node, 0, source)
        show(node)
        for name, label, shown in GODRAYS_KNOBS:
            p = param(node, name)
            check(p.getLabel() == label, "GodRays' %s is labelled %r (got %r)" % (name, label, p.getLabel()))
            check(p.getIsVisible() == shown, "GodRays %s %s" % ("shows" if shown else "hides", name))
        for name in ["transformAmount", "invert", "steps", "hostUnPremultBy"]:
            check(node.getParam(name) is None, "GodRays has no %s knob" % name)
        ok = yield from open_panel(node)
        check(ok, "GodRays' panel opens")
        yield ("sleep", RENDER_MS)
        gt.shot("m130-godrays-defaults.png")

        param(node, "scale").set(1.4, 1.4)
        param(node, "toColor").set(0.1, 0.05, 0.0, 1.0)
        param(node, "gamma").setValue(2.0)
        yield ("sleep", RENDER_MS)
        gt.shot("m130-godrays-rays.png")
        gt.close_all_panels()
        yield ("sleep", 300)

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        source = create(PLUGIN_CHECKER, 0, 0)
        yield from section_dirblur(source)
        yield from section_godrays(source)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
