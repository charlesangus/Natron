"""M349 scratch: lays out two pairs of prototype port nodes, each joined output port to input port,
maximises and fits the node graph, and saves port-nodes-1920x1080.png. Run through
Tests/gui/run-gui-test.sh, which sets NATRON_GUI_TEST_NODES=1. Screenshot and results.txt go to
$NATRON_GUI_TEST_OUT.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

try:
    import NatronEngine
    import NatronGui
    import guitest as gt
    from PySide6.QtCore import QEvent, QMetaObject, Qt
    from PySide6.QtGui import QKeyEvent
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

    def connect(dst, port, src):
        i = input_index(dst, port)
        gt.require(i >= 0, "%s has a port %s" % (dst.getScriptName(), port))
        gt.check(dst.connectInput(i, src), "%s connects to %s.%s" % (src.getScriptName(), dst.getScriptName(), port))
        gt.check(dst.getInput(i) is not None and dst.getInput(i).getScriptName() == src.getScriptName(),
                 "%s.%s reads %s" % (dst.getScriptName(), port, src.getScriptName()))

    def port_edges(g):
        return [it for it in g.scene().items() if it.data(0) == "PortEdge"]

    def pane_of(widget):
        w = widget
        while w is not None and gt.class_name(w) != "TabWidget":
            w = w.parentWidget()
        return w

    def steps():
        ok = yield ("until", lambda: graph() is not None, 60.0)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        tex = make("ImageTexture", 0, 0)
        bsdf = make("PrincipledBsdf", 320, 0)
        mp = make("Map", 0, 300)
        mat = make("DwaBaseMaterial", 320, 300)
        # The output is chosen before connecting so the pipe is first drawn from the right dot.
        bsdf.getParam("inputOutputs").setValue("float0=Alpha")
        connect(bsdf, "float0", tex)
        connect(mat, "float2", mp)
        yield ("sleep", 800)

        g = graph()
        pane = pane_of(g)
        gt.require(pane is not None, "the node graph sits in a pane")
        before = pane.size()
        g.setFocus()
        QApplication.sendEvent(pane, QKeyEvent(QEvent.KeyPress, Qt.Key_Space, Qt.NoModifier))
        QApplication.sendEvent(pane, QKeyEvent(QEvent.KeyRelease, Qt.Key_Space, Qt.NoModifier))
        yield ("sleep", 600)
        after = pane.size()
        gt.report("NOTE node graph pane %dx%d -> %dx%d" % (before.width(), before.height(), after.width(), after.height()))
        gt.check(after.width() > before.width() and after.height() > before.height(), "the node graph pane is maximised")
        NatronGui.natron.getGuiInstance(app.getAppID()).clearSelection()
        QMetaObject.invokeMethod(g, "centerOnAllNodes")
        yield ("sleep", 600)
        # centerOnAllNodes stops at 1:1; zoom past it so the port names read at 1920x1080.
        xs, ys = [], []
        for node in (tex, bsdf, mp, mat):
            x, y = node.getPosition()
            w, h = node.getSize()
            xs += [x, x + w]
            ys += [y, y + h]
        vw, vh = g.viewport().width(), g.viewport().height()
        zoom = min(2.0, 0.85 * min(vw / (max(xs) - min(xs)), vh / (max(ys) - min(ys))))
        gt.report("NOTE viewport %dx%d, zoom %.2f" % (vw, vh, zoom))
        if zoom > 1.0:
            g.resetTransform()
            g.scale(zoom, zoom)
            g.centerOn((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        yield ("sleep", 400)
        QApplication.processEvents()

        edges = port_edges(g)
        gt.report("NOTE %d port-to-port pipes in the scene" % len(edges))
        gt.check(len(edges) == 2, "two port-to-port pipes are drawn")
        view = g.viewport().rect()
        for node in (tex, bsdf, mp, mat):
            x, y = node.getPosition()
            w, h = node.getSize()
            tl = g.mapFromScene(x, y)
            br = g.mapFromScene(x + w, y + h)
            gt.check(view.contains(tl) and view.contains(br), "%s is fully inside the node graph view" % node.getScriptName())

        gt.shot("port-nodes-1920x1080.png")
        gt.report("CAPTION port-nodes-1920x1080: the node graph pane maximised and fitted to four prototype "
                  "port nodes, each with its name in a title bar above its port rows (inputs on the left, "
                  "outputs on the right). Two port-to-port pipes: PortImageTexture1.Alpha (its second "
                  "output) to PortPrincipledBsdf1.float0, and PortMap1.out to PortDwaBaseMaterial1.float2. "
                  "The positional input arrows are made transparent; the black output stubs under each "
                  "node are the unchanged positional output Edge.")

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
