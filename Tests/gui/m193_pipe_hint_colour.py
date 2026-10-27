"""Screenshots the drop-on-pipe hint in the default and a changed Preferences Selection colour, and
checks that the highlighted pipe is drawn in that colour.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m193-shots/out Tests/gui/run-gui-test.sh Tests/gui/m193_pipe_hint_colour.py build/fast/App/Natron
Output: hint-default.png and hint-changed.png (the node graph viewport).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

SETTLE_MS = 400
TOLERANCE = 40

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def centre(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x + w / 2.0, y + h / 2.0)

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def selection_rgb():
        p = NatronEngine.natron.getSettings().getParam("selection")
        return tuple(int(max(0.0, min(1.0, p.getValue(d))) * 255 + 0.5) for d in range(3))

    def set_selection(r, g, b):
        NatronEngine.natron.getSettings().getParam("selection").set(r, g, b)
        QApplication.processEvents()

    def viewport_image(graph):
        QApplication.processEvents()
        vp = graph.viewport()
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        return pix

    def count_near(img, rgb):
        n = 0
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if (abs(c.red() - rgb[0]) <= TOLERANCE and abs(c.green() - rgb[1]) <= TOLERANCE
                        and abs(c.blue() - rgb[2]) <= TOLERANCE):
                    n += 1
        return n

    def snap(graph, name, rgb):
        pix = viewport_image(graph)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)
        hits = count_near(pix.toImage(), rgb)
        check(hits > 0, "%s: the highlighted pipe is drawn in the Selection colour %s (%d px)" % (name, rgb, hits))

    def steps():
        gt.section("pipe-hint-colour")
        src = create("net.sf.openfx.ConstantPlugin", 0, 0)
        dst = create("net.sf.cimg.CImgBlur", 0, 300)
        dst.connectInput(0, src)
        mover = create("net.sf.cimg.CImgBlur", 400, 150)
        app.clearSelection()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        graph = root_graph()
        gt.require(graph is not None, "a visible node graph")
        nodes = (src, dst, mover)
        xs = [n.getPosition()[0] for n in nodes]
        ys = [n.getPosition()[1] for n in nodes]
        graph.resetTransform()
        graph.scale(1.5, 1.5)
        graph.centerOn((min(xs) + max(xs) + 100) / 2.0, (min(ys) + max(ys) + 50) / 2.0)
        yield ("sleep", SETTLE_MS)

        sc, mc = centre(src), centre(mover)
        target = (sc[0] + mover.getSize()[0] / 2.0 + 9.0, (sc[1] + centre(dst)[1]) / 2.0)
        vp = graph.viewport()
        p0 = graph.mapFromScene(QPointF(mc[0], mc[1]))
        send_mouse(vp, QEvent.MouseButtonPress, p0, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 5):
            f = i / 4.0
            p = graph.mapFromScene(QPointF(mc[0] + (target[0] - mc[0]) * f, mc[1] + (target[1] - mc[1]) * f))
            send_mouse(vp, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)

        snap(graph, "hint-default", selection_rgb())
        set_selection(0.1, 0.4, 1.0)
        vp.update()
        pend = graph.mapFromScene(QPointF(target[0], target[1]))
        send_mouse(vp, QEvent.MouseMove, pend, Qt.NoButton, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        snap(graph, "hint-changed", selection_rgb())
        send_mouse(vp, QEvent.MouseButtonRelease, pend, Qt.LeftButton, Qt.NoButton)
        set_selection(0.95, 0.54, 0.0)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
