"""M349 scratch: lays out a small port network (two prototype port nodes joined port to port) and
saves port-nodes-1920x1080.png. Run through Tests/gui/run-gui-test.sh, which sets
NATRON_GUI_TEST_NODES=1. Screenshot and results.txt go to $NATRON_GUI_TEST_OUT.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

try:
    import NatronEngine
    import guitest as gt
    from PySide6.QtWidgets import QApplication

    app = NatronEngine.natron.getActiveInstance()
    PREFIX = "test.natron.built-in.PortNode."

    def graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def make(kind, x, y):
        node = app.createNode(PREFIX + kind, -1, None)
        gt.require(node is not None, "create " + kind, gt.plugin_problem(PREFIX + kind))
        node.setPosition(x, y)
        return node

    def input_index(node, label):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == label:
                return i
        return -1

    def steps():
        ok = yield ("until", lambda: graph() is not None, 60.0)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        tex = make("ImageTexture", 0, 0)
        maps = [make("Map", 0, 200 + 110 * i) for i in range(2)]
        bsdf = make("PrincipledBsdf", 320, 60)
        mat = make("DwaBaseMaterial", 320, 420)
        bsdf.connectInput(input_index(bsdf, "Float 0"), tex)
        bsdf.getParam("inputOutputs").setValue("float0=Alpha")
        bsdf.connectInput(input_index(bsdf, "Color 0"), maps[0])
        mat.connectInput(input_index(mat, "Float 0"), maps[1])
        yield ("sleep", 800)
        g = graph()
        g.resetTransform()
        g.centerOn(250, 300)
        QApplication.processEvents()
        yield ("sleep", 400)
        gt.shot("port-nodes-1920x1080.png")
        gt.report("CAPTION port-nodes-1920x1080: two prototype port nodes joined port to port")

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
