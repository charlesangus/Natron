"""M184 gate: IBlur, IErode and IDilate in the GUI.

(a) Eye blur: a CheckerBoard through IBlur whose Map is a feathered Roto ellipse. The viewer shows the
    blur strong inside the shape and none outside; the script compares the local contrast of two
    patches of the viewer.
(b) Key: a white disc with alpha through IErode and through IDilate, each sized by a horizontal ramp
    Map (no change at the left edge of the frame, the full Size at the right edge). The disc is
    symmetric, so the lit pixels left and right of its centre are equal until a ramp-sized filter
    shrinks or grows its right side more than its left.
Screenshots: the viewer crop of each result, and the node graph with the three I-node icons.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 90.0
WATCHDOG_S = 800
SETTLE_MS = 700
GRAPH_MARGIN = 120.0
PATCH = 90
LIT = 128

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def setp(node, name, *values):
        prm = node.getParam(name)
        gt.require(prm is not None, "%s has parameter %s" % (node.getLabel(), name),
                   "has: " + ", ".join(sorted(p.getScriptName() for p in node.getParams())))
        prm.set(*values)

    def check_map_knobs(node):
        names = [p.getScriptName() for p in node.getParams()]
        label = node.getLabel()
        check("sizeAtBlack" in names and "size" in names and names.index("sizeAtBlack") < names.index("size"),
              "%s shows Size At Black before Size" % label)
        check("mapResponse" in names and "mapGamma" in names,
              "%s has Map Response and Map Gamma" % label)
        check("mapBlack" not in names and "mapWhite" not in names,
              "%s has no Map Black or Map White" % label)
        response = node.getParam("mapResponse")
        gamma = node.getParam("mapGamma")
        if response is not None and gamma is not None:
            check(response.getValue() == 0 and abs(gamma.getValue() - 1.0) < 1e-9,
                  "%s defaults to a Linear response and gamma 1" % label)

    def connect(node, input_nb, source):
        return check(node.connectInput(input_nb, source),
                     "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def viewer_gl():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def viewer_tab():
        found = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        return found[0] if found else None

    _keep = []

    def root_graph():
        found = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        if found:
            _keep[:] = [found[0], found[0].viewport()]
        return found[0] if found else None

    def viewer_image():
        """(QImage of the viewer's screen area, image-space to screen mapping) or None."""
        gl = viewer_gl()
        if gl is None:
            return None
        QApplication.processEvents()
        origin = gl.mapToGlobal(gl.rect().topLeft())
        w, h = gl.width(), gl.height()
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), w, h).toImage()
        return img

    def lum(img, x, y):
        c = img.pixelColor(x, y)
        return 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue()

    def contrast(img, cx, cy):
        """Mean absolute horizontal and vertical neighbour difference in a PATCH square at (cx, cy)."""
        total = n = 0.0
        x0, y0 = int(cx - PATCH / 2), int(cy - PATCH / 2)
        for y in range(max(0, y0), min(img.height() - 1, y0 + PATCH)):
            for x in range(max(0, x0), min(img.width() - 1, x0 + PATCH)):
                v = lum(img, x, y)
                total += abs(lum(img, x + 1, y) - v) + abs(lum(img, x, y + 1) - v)
                n += 1
        return total / n if n else -1.0

    def lit_halves(img):
        """Lit pixel counts left and right of the viewer's centre column."""
        cx = img.width() // 2
        left = right = 0
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                if lum(img, x, y) > LIT:
                    if x < cx:
                        left += 1
                    else:
                        right += 1
        return left, right

    def show(node):
        viewer = app.getNode("Viewer1")
        viewer.disconnectInput(0)
        gt.require(viewer.connectInput(0, node), "connect the Viewer to " + node.getLabel())

    def fit_viewer():
        tab = viewer_tab()
        if tab is not None:
            QMetaObject.invokeMethod(tab, "centerViewer")
        QApplication.processEvents()

    def save_viewer(name):
        img = viewer_image()
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if img is not None and img.save(path) else "NOTE could not save ") + path)

    def frame_graph(nodes):
        graph = root_graph()
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y = n.getPosition()
            w, h = n.getSize()
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        bw = max(xe) - min(xs) + 2 * GRAPH_MARGIN
        bh = max(ye) - min(ys) + 2 * GRAPH_MARGIN
        vp = _keep[1]
        want = min(1.5, vp.width() / bw, vp.height() / bh)
        graph.resetTransform()
        graph.scale(want, want)
        graph.centerOn((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0)
        QApplication.processEvents()

    def save_graph(name):
        root_graph()
        vp = _keep[1]
        origin = vp.mapToGlobal(vp.rect().topLeft())
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), vp.width(), vp.height())
        path = os.path.join(gt.OUT, name)
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph is up")
        create("fr.inria.built-in.Viewer", 600, 700)
        ok = yield ("until", lambda: viewer_gl() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the viewer is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        fmt = app.getProjectParam("outputFormat")
        report("NOTE project format %r" % (fmt.get() if fmt is not None else None))

        # ---- (a) eye blur ---------------------------------------------------------------
        gt.section("eye-blur")
        checker = create("net.sf.openfx.CheckerBoardPlugin", 0, 0)
        setp(checker, "boxSize", 48, 48)
        roto = create("fr.inria.built-in.Roto", 300, 0)
        ctx = roto.getRotoContext()
        gt.require(ctx is not None, "the Roto node has a roto context")
        ellipse = ctx.createEllipse(960.0, 540.0, 440.0, True, 1.0)
        gt.require(ellipse is not None, "created a Roto ellipse")
        feather = ellipse.getParam("feather")
        gt.require(feather is not None, "the ellipse has a feather parameter")
        feather.set(160.0)
        iblur = create("IBlur", 150, 250)
        check_map_knobs(iblur)
        setp(iblur, "size", 60.0, 60.0)
        connect(iblur, 0, checker)
        connect(iblur, 2, roto)
        show(iblur)
        fit_viewer()
        sharp = [0.0]

        def blur_settled():
            img = viewer_image()
            if img is None:
                return False
            w, h = img.width(), img.height()
            inside = contrast(img, w / 2, h / 2)
            outside = contrast(img, w / 2 - 0.40 * min(w, h * 16 / 9.0) , h / 2)
            sharp[0] = outside
            report("NOTE eye blur contrast inside %.2f outside %.2f" % (inside, outside))
            return outside > 4.0 and inside < outside * 0.35

        ok = yield ("until", blur_settled, RENDER_TIMEOUT_S)
        check(ok, "IBlur blurs inside the Roto Map and leaves the outside sharp (outside contrast %.2f)" % sharp[0])
        yield ("sleep", SETTLE_MS)
        save_viewer("m184-eye-blur.png")

        # ---- (b) key --------------------------------------------------------------------
        gt.section("erode-dilate")
        disc = create("net.sf.openfx.Radial", 0, 500)
        setp(disc, "softness", 0.0)
        small = create("net.sf.openfx.TransformPlugin", 150, 600)
        setp(small, "center", 960.0, 540.0)
        setp(small, "scale", 0.5, 0.5)
        connect(small, 0, disc)
        ramp = create("net.sf.openfx.Ramp", 400, 500)
        setp(ramp, "point0", 0.0, 0.0)
        setp(ramp, "point1", 1920.0, 0.0)
        setp(ramp, "color0", 0.0, 0.0, 0.0, 0.0)
        setp(ramp, "color1", 1.0, 1.0, 1.0, 1.0)
        ierode = create("IErode", 150, 800)
        idilate = create("IDilate", 400, 800)
        check_map_knobs(ierode)
        check_map_knobs(idilate)
        for node in (ierode, idilate):
            setp(node, "size", 80.0, 80.0)
            connect(node, 0, small)
            connect(node, 2, ramp)

        counts = {}

        def counted(key):
            def probe():
                img = viewer_image()
                if img is None:
                    return False
                left, right = lit_halves(img)
                counts[key] = (left, right)
                return left > 200
            return probe

        show(small)
        fit_viewer()
        ok = yield ("until", counted("disc"), RENDER_TIMEOUT_S)
        check(ok, "the disc shows in the viewer")
        yield ("sleep", 2500)
        counted("disc")()
        save_viewer("m184-key-source.png")
        l0, r0 = counts["disc"]
        base_ratio = r0 / float(l0)
        check(abs(l0 - r0) <= 0.05 * (l0 + r0), "the disc is symmetric (left %d, right %d lit)" % (l0, r0))

        for key, node, shot_name, want_less in (("erode", ierode, "m184-ierode.png", True),
                                                ("dilate", idilate, "m184-idilate.png", False)):
            show(node)
            fit_viewer()
            ok = yield ("until", counted(key), RENDER_TIMEOUT_S)
            yield ("sleep", 2000)
            counted(key)()
            left, right = counts[key]
            report("NOTE %s lit left %d right %d (disc %d / %d)" % (key, left, right, l0, r0))
            ratio = right / float(left)
            if want_less:
                check(left > 200 and ratio < 0.9 * base_ratio,
                      "IErode erodes the ramp's right side more than its left (right/left %.2f, disc %.2f)" % (ratio, base_ratio))
            else:
                check(ratio > 1.1 * base_ratio,
                      "IDilate grows the ramp's right side more than its left (right/left %.2f, disc %.2f)" % (ratio, base_ratio))
            save_viewer(shot_name)

        # ---- node graph with the three icons --------------------------------------------
        gt.section("graph")
        app.clearSelection()
        gt.close_all_panels()
        iblur.setPosition(100, 900)
        ierode.setPosition(400, 900)
        idilate.setPosition(700, 900)
        ramp.setPosition(900, 700)
        yield ("sleep", SETTLE_MS)
        frame_graph([iblur, ierode, idilate])
        yield ("sleep", SETTLE_MS)
        save_graph("m184-nodegraph-icons.png")
        frame_graph([checker, roto, iblur, disc, small, ramp, ierode, idilate])
        yield ("sleep", SETTLE_MS)
        save_graph("m184-nodegraph-all.png")
        gt.shot("m184-window.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
