"""Loop blocks in the node graph: Constant -> outer LoopBegin -> inner LoopBegin -> Blur -> inner
LoopEnd -> outer LoopEnd.

The graph's loop block item paints a hull for each of the two blocks, round its LoopBegin, LoopEnd
and the nodes between them, in a colour the pair shares; the inner hull lies inside the outer one.
Selecting a LoopEnd highlights its block. Screenshots: loopblocks.png (nothing selected) and
loopblocks_selected.png (the inner LoopEnd selected) in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m180_loop_blocks.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"
BLUR_ID = "net.sf.cimg.CImgBlur"
BEGIN_ID = "fr.natron.LoopBegin"
END_ID = "fr.natron.LoopEnd"

try:
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, require

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def block_item():
        view = node_graph_view()
        if view is None or view.scene() is None:
            return None
        for item in view.scene().items():
            obj = item.toGraphicsObject()
            if obj is not None and obj.objectName() == "LoopBlockItem":
                return obj
        return None

    def block_area():
        item = block_item()
        if item is None:
            return 0.
        rect = item.sceneBoundingRect()
        return rect.width() * rect.height()

    def steps():
        gt.section("loop blocks")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        gt.close_all_panels()
        require(block_item() is not None, "the graph has a loop block item")

        constant = app.createNode(CONSTANT_ID)
        outer_begin = app.createNode(BEGIN_ID)
        inner_begin = app.createNode(BEGIN_ID)
        blur = app.createNode(BLUR_ID)
        inner_end = app.createNode(END_ID)
        outer_end = app.createNode(END_ID)
        chain = (constant, outer_begin, inner_begin, blur, inner_end, outer_end)
        for node in chain:
            require(node is not None, "create the loop graph's nodes")
        outer_begin.getParam("name").setValue("outer")
        inner_begin.getParam("name").setValue("inner")
        for i, node in enumerate(chain):
            node.setPosition(0, i * 110)
            if i > 0:
                require(node.connectInput(0, chain[i - 1]), "connect %s" % node.getScriptName())
        app.clearSelection()

        ok = yield ("until", lambda: block_area() > 0., POLL_TIMEOUT_S)
        check(ok, "the blocks are drawn")
        outer_area = block_area()
        view = node_graph_view()
        view.centerOn(0, 300)
        yield ("sleep", 600)
        gt.shot("loopblocks.png")

        # The item spans the outer hull, which encloses both LoopBegins, both LoopEnds and the Blur.
        rect = block_item().sceneBoundingRect()
        for node in chain[1:]:
            x, y = node.getPosition()
            check(rect.contains(x, y), "the blocks cover %s" % node.getScriptName())

        app.selectNode(inner_end, False)
        yield ("sleep", 600)
        QApplication.processEvents()
        check(block_area() >= outer_area, "selecting a LoopEnd keeps every block drawn")
        gt.shot("loopblocks_selected.png")

        app.clearSelection()
        for node in reversed(chain):
            node.destroy()
        ok = yield ("until", lambda: block_area() <= 16 * 16, POLL_TIMEOUT_S)
        check(ok, "deleting the loops removes their blocks")

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
