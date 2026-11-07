"""Connects a Viewer to a chain and destroys the chain with no event-loop turn in between, several rounds.

A viewer request queued behind a render must survive its input node being destroyed before the
request runs. The app has to be alive and still render a fresh chain afterwards.

Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 100
ROUNDS = 8
CHAIN_LENGTH = 4
SETTLE_MS = 500
PLUGIN_CHECKER = "CheckerBoard"
PLUGIN_VIEWER = "Viewer"
PLUGIN_BLUR = "Blur"

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x=0, y=0):
        node = app.createNode(plugin_id, -1, None, {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)})
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def build_chain(length):
        nodes = [create(PLUGIN_CHECKER, 0, 0)]
        for i in range(length):
            blur = create(PLUGIN_BLUR, 0, 100 * (i + 1))
            gt.require(blur.connectInput(0, nodes[-1]), "connect chain link %d" % i)
            nodes.append(blur)
        return nodes

    def viewer_node():
        v = app.getNode("Viewer1")
        return v if v is not None else create(PLUGIN_VIEWER, 0, 800)

    def steps():
        viewer = viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)

        gt.section("destroy-race")
        for round_nb in range(ROUNDS):
            # No yield between the connect and the destroy: the viewer's request is still queued.
            nodes = build_chain(CHAIN_LENGTH)
            viewer.disconnectInput(0)
            viewer.connectInput(0, nodes[-1])
            for n in reversed(nodes):
                n.destroy(False)
            yield ("sleep", 100)
        check(True, "%d connect-and-destroy rounds ran" % ROUNDS)
        yield ("sleep", SETTLE_MS)
        check(gt.viewer_up(), "the app and its viewer are still up")

        gt.section("render-after")
        nodes = build_chain(CHAIN_LENGTH)
        viewer.disconnectInput(0)
        viewer.connectInput(0, nodes[-1])
        yield ("sleep", 2000)
        colour = gt.viewer_centre_colour()
        check(colour is not None, "the viewer is visible and can be read")
        check(not gt.viewer_messages(), "the viewer shows no error messages: %r" % (gt.viewer_messages(),))
        check(colour is not None and max(colour) > 0, "the viewer renders the chain: %r" % (colour,))
        gt.shot("m140_viewer_destroy_race.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
