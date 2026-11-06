"""RawPrepare's output format is the kept crop. The Canon EOS 5D Mark III CR2 goes through Read,
RawPrepare, RawDemosaic and a Viewer, and then:
  - RawPrepare, RawDemosaic and the Viewer have a format that starts at (0, 0) and is as wide and
    as high as the Read's crop (its region of definition);
  - the demosaiced picture reaches every edge of that format: samples along the top, bottom, left
    and right edges of the image as drawn in the viewer are not black, and the drawn image has the
    crop's aspect ratio.
Without the fixture (a checkout without Git LFS has a pointer file) the script ends after
reporting that.

Shots, under $NATRON_GUI_TEST_OUT: m296-read.png (the Read alone) and m296-demosaic.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m296_raw_prepare_format.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 180.0
SETTLE_POLLS = 3
EDGE_SAMPLES = 32
EDGE_INSET_PX = 2
BLACK_MAX_CHANNEL = 2
LIT_FRACTION = 0.9
ASPECT_TOLERANCE = 0.01
MIN_FILL = 0.5
FRAME_FRACTION = 0.2

try:
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    CR2 = os.path.join(gt.FIXTURES, "raw", "canon-eos-5d-mark-iii.cr2")

    # PySide wrappers stay referenced here while the script uses them.
    state = {}

    def is_real_fixture(path):
        """A decodable fixture rather than a Git LFS pointer file."""
        return os.path.exists(path) and os.path.getsize(path) > (1 << 20)

    def visible_viewer():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return views[0] if views else None

    def viewer_crop():
        """The viewer widget as drawn on screen. Read from a screen grab: a widget grab of the GL
        viewer can come out blank under llvmpipe."""
        widget = visible_viewer()
        if widget is None:
            return None
        state["widget"] = widget
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(
            top_left.x(), top_left.y(), widget.width(), widget.height())

    def fit_viewer():
        for tab in gt.widgets_of("ViewerTab"):
            if tab.isVisible():
                state["tab"] = tab
                QMetaObject.invokeMethod(tab, "centerViewer")
                break
        QApplication.processEvents()

    def rgb(image, x, y):
        c = image.pixelColor(x, y)
        return (c.red(), c.green(), c.blue())

    def show(viewer, node):
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer accepts " + node.getScriptName())

    def settle():
        """Waits until the viewer holds the same picture for several polls in a row, and that
        picture is not just its background."""
        previous = None
        same = 0
        waited = 0.0
        while waited < RENDER_TIMEOUT_S:
            yield ("sleep", 500)
            waited += 0.5
            image = viewer_crop()
            if image is None:
                continue
            background = rgb(image, 2, 2)
            varied = any(rgb(image, x, y) != background
                         for x in range(0, image.width(), 16) for y in range(0, image.height(), 16))
            same = same + 1 if (varied and previous is not None and image == previous) else 0
            previous = image
            if same >= SETTLE_POLLS:
                return True
        return False

    def drawn_image_rect(image):
        """(left, top, right, bottom) of the picture on screen, from the rows and columns that
        are mostly not the viewer's background. The viewer draws a frame round the image, so these
        include the edge rows even where the picture itself is as black as the background."""
        background = rgb(image, 2, 2)
        w, h = image.width(), image.height()
        cols = [0] * w
        rows = [0] * h
        for y in range(h):
            for x in range(w):
                if rgb(image, x, y) != background:
                    cols[x] += 1
                    rows[y] += 1
        xs = [x for x in range(w) if cols[x] >= FRAME_FRACTION * h]
        ys = [y for y in range(h) if rows[y] >= FRAME_FRACTION * w]
        if not xs or not ys:
            return 0, 0, 0, 0
        return xs[0], ys[0], xs[-1] + 1, ys[-1] + 1

    def edge_points(rect):
        left, top, right, bottom = rect
        inset = EDGE_INSET_PX
        points = {"top": [], "bottom": [], "left": [], "right": []}
        for i in range(EDGE_SAMPLES):
            t = 0.1 + 0.8 * i / (EDGE_SAMPLES - 1)
            x = int(left + (right - left) * t)
            y = int(top + (bottom - top) * t)
            points["top"].append((x, top + inset))
            points["bottom"].append((x, bottom - inset))
            points["left"].append((left + inset, y))
            points["right"].append((right - inset, y))
        return points

    def lit(colour):
        return max(colour) > BLACK_MAX_CHANNEL

    def crop_size(read):
        rod = read.getRegionOfDefinition(app.timelineGetTime(), 0)
        return (int(round(rod.right() - rod.left())), int(round(rod.top() - rod.bottom())))

    def check_format(node, label, size):
        fmt = node.getOutputFormat(app.timelineGetTime(), 0)
        report("NOTE %s format: (%d, %d) to (%d, %d)" % (label, fmt.x1, fmt.y1, fmt.x2, fmt.y2))
        check((fmt.x1, fmt.y1) == (0, 0), "%s's format starts at (0, 0)" % label)
        check((fmt.width(), fmt.height()) == size,
              "%s's format is the %dx%d crop (%dx%d)" % (label, size[0], size[1], fmt.width(), fmt.height()))

    def steps():
        # A viewer widget only exists once a Viewer node does.
        app.getNode("Viewer1") or app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        if not is_real_fixture(CR2):
            report("NOTE %s is missing or a Git LFS pointer: nothing to check" % CR2)
            return

        gt.section("Raw Prepare format")
        read = app.createReader(CR2)
        require(read is not None, "create a Read for the CR2")
        read.setPosition(0, 0)
        prepare = app.createNode("RawPrepare")
        require(prepare is not None, "create RawPrepare", gt.plugin_problem("RawPrepare"))
        prepare.setPosition(0, 100)
        require(prepare.connectInput(0, read), "RawPrepare takes the Read")
        demosaic = app.createNode("RawDemosaic")
        require(demosaic is not None, "create RawDemosaic", gt.plugin_problem("RawDemosaic"))
        demosaic.setPosition(0, 200)
        require(demosaic.connectInput(0, prepare), "RawDemosaic takes RawPrepare")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 300)
        state.update(read=read, prepare=prepare, demosaic=demosaic, viewer=viewer)

        size = crop_size(read)
        report("NOTE the Read's crop is %dx%d" % size)
        require(size[0] > 1 and size[1] > 1, "the Read has a crop")

        show(viewer, read)
        fit_viewer()
        ok = yield from settle()
        check(ok, "the Viewer settles on the Read")
        gt.shot("m296-read.png")

        show(viewer, demosaic)
        fit_viewer()
        ok = yield from settle()
        check(ok, "the Viewer settles on the demosaic")
        messages = gt.viewer_messages()
        check(not messages, "the Viewer shows no error (%r)" % (messages,))
        gt.shot("m296-demosaic.png")

        check_format(prepare, "RawPrepare", size)
        check_format(demosaic, "RawDemosaic", size)
        check_format(viewer, "the Viewer", size)

        image = viewer_crop()
        require(image is not None, "the viewer is on screen")
        rect = drawn_image_rect(image)
        left, top, right, bottom = rect
        width, height = right - left, bottom - top
        report("NOTE the drawn image spans x %d..%d, y %d..%d of the %dx%d viewer" % (left, right, top, bottom, image.width(), image.height()))
        require(width > 0 and height > 0, "the demosaic is drawn in the viewer")
        fill = max(width / float(image.width()), height / float(image.height()))
        check(fill >= MIN_FILL, "the image fills the viewer once fitted (%.2f)" % fill)
        aspect = width / float(height)
        expected = size[0] / float(size[1])
        check(abs(aspect - expected) <= ASPECT_TOLERANCE * expected,
              "the drawn image has the crop's aspect ratio (%.4f, expected %.4f)" % (aspect, expected))

        for edge, points in edge_points(rect).items():
            lit_count = sum(1 for (x, y) in points if lit(rgb(image, x, y)))
            fraction = lit_count / float(len(points))
            check(fraction >= LIT_FRACTION,
                  "no black border along the %s edge (%d of %d samples lit)" % (edge, lit_count, len(points)))

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
