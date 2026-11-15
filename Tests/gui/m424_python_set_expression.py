"""A Python setExpression on a viewed Constant's colour refreshes the viewer with no other edit,
and clearing the expression (setExpression with an empty string) brings the old colour back.

Graph: Constant (black) -> Viewer. A second Constant, viewed by nothing, takes an expression too,
which must not raise.

Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 20.0
WATCHDOG_S = 150
SETTLE_MS = 1000
PLUGIN_CONSTANT = "Constant"
PLUGIN_VIEWER = "Viewer"
BRIGHT = 150
DARK = 30

try:
    import faulthandler

    import NatronEngine

    import guitest as gt
    from guitest import check

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x=0, y=0):
        node = app.createNode(plugin_id, -1, None, {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)})
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def centre_max():
        colour = gt.viewer_centre_colour()
        return None if colour is None else max(colour)

    def bright():
        value = centre_max()
        return value is not None and value >= BRIGHT

    def dark():
        value = centre_max()
        return value is not None and value <= DARK

    def steps():
        constant = create(PLUGIN_CONSTANT, 0, 0)
        color = constant.getParam("color")
        gt.require(color is not None, "the Constant has a color parameter")
        color.set(0.0, 0.0, 0.0, 1.0)

        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = create(PLUGIN_VIEWER, 0, 200)
        gt.require(viewer.connectInput(0, constant), "connect the Viewer to the Constant")

        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("baseline")
        ok = yield ("until", dark, RENDER_TIMEOUT_S)
        check(ok, "the viewer shows the black Constant: %r" % (gt.viewer_centre_colour(),))
        gt.shot("m424_baseline.png")

        gt.section("set-expression")
        for dimension in range(3):
            check(color.setExpression("0.9", False, dimension), "setExpression on color dimension %d" % dimension)
        ok = yield ("until", bright, RENDER_TIMEOUT_S)
        check(ok, "the viewer shows the expression's colour with no other edit: %r" % (gt.viewer_centre_colour(),))
        check(not gt.viewer_messages(), "the viewer shows no error messages: %r" % (gt.viewer_messages(),))
        gt.shot("m424_expression.png")

        gt.section("clear-expression")
        for dimension in range(3):
            check(color.setExpression("", False, dimension), "clear the expression on color dimension %d" % dimension)
        ok = yield ("until", dark, RENDER_TIMEOUT_S)
        check(ok, "the viewer shows the black Constant again: %r" % (gt.viewer_centre_colour(),))
        gt.shot("m424_cleared.png")

        gt.section("no-viewer")
        unviewed = create(PLUGIN_CONSTANT, 300, 0)
        try:
            ok = unviewed.getParam("color").setExpression("0.5", False, 0)
            check(ok, "setExpression on a Constant no viewer shows succeeds")
        except Exception:
            check(False, "setExpression on a Constant no viewer shows raised: " + traceback.format_exc())
        yield ("sleep", SETTLE_MS)
        check(dark(), "the viewer still shows the black Constant: %r" % (gt.viewer_centre_colour(),))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
