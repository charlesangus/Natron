"""A Read of each layered document format (OpenRaster, Photoshop PSB and PSD, Krita, GIMP) lists the
document's layers in the viewer's layer menu, shows the composite, and shows a chosen layer, with
the pixels the committed fixture was built with (Tests/fixtures/layered/make-*-fixtures.py):
  - the layer menu holds every layer that has pixels under the name the reader gives it (group path
    and layer name joined by "_", other characters replaced by "_"), and none of the layers that
    have no pixels;
  - the composite and each probed layer show the expected colours at known canvas pixels. The
    colours go through the display transform, so each probe tests a colour family (red, blue, grey,
    black, ...) rather than an exact value, and an ordering probe tests that stepped values rise;
  - a layer is converted from the input colourspace like the composite, so where the composite is
    exactly one layer the two show the same colour on screen.
A format's fixtures are skipped, with a note, when the file is missing or a Git LFS pointer.

Shots, under $NATRON_GUI_TEST_OUT: m79-<fixture>-composite.png and m79-<fixture>-<layer>.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m79_layered_read.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
MENU_TIMEOUT_S = 90.0
PROBE_TIMEOUT_S = 90.0
POLL_MS = 500
FRAME_FRACTION = 0.2
# Through the default ACES 2.0 SDR view an 8-bit sRGB value of 40 shows at about 2, 64 at 35 and
# 255 white at 180.
DOMINANCE = 40
DARK_MAX = 20
LIT_MIN = 25
GREY_SPREAD = 14
WHITE_MIN = 150
MATTED_MIN = 100
AGREE_TOLERANCE = 2
NO_LAYER_ITEM = "-"
COLOUR_PLANE_ITEM = "rgba"

try:
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    LAYERED = os.path.join(gt.FIXTURES, "layered")

    # PySide wrappers stay referenced here while the script uses them.
    state = {}

    def red(c):
        return c[0] > c[1] + DOMINANCE and c[0] > c[2] + DOMINANCE

    def green(c):
        return c[1] > c[0] + DOMINANCE and c[1] > c[2] + DOMINANCE

    def blue(c):
        return c[2] > c[0] + DOMINANCE and c[2] > c[1] + DOMINANCE

    def yellow(c):
        return c[0] > c[2] + DOMINANCE and c[1] > c[2] + DOMINANCE and abs(c[0] - c[1]) < 60

    def purple(c):
        return c[0] > 60 and c[2] > 40 and c[1] < 60

    def dark(c):
        return max(c) < DARK_MAX

    def grey(c):
        return max(c) > LIT_MIN and max(c) - min(c) < GREY_SPREAD

    def white(c):
        return min(c) > WHITE_MIN

    # groups8.psd stores 200/210/220 at alpha 128 matted against white; with the matte removed
    # the premultiplied composite is 73/83/93, which shows a little above the 64 grey Background.
    # A value still carrying the matte would show far above MATTED_MIN.
    def unmatted(c):
        return LIT_MIN < c[0] < c[1] < c[2] < MATTED_MIN

    def red_only(c):
        return c[0] > 15 and c[1] < 15 and c[2] < 15

    # name, file, canvas size, layers listed, layers not listed, probes. A probe set is
    # (item or None for the composite, [(x, y, predicate)], [x of points whose red must rise]) with
    # the ordering points taken on the row y given as the fourth element. "agree" lists
    # (item, x, y) points, probed in both the composite and the item, where the composite is that
    # layer alone.
    FIXTURES = [
        {
            "name": "flatten-ora", "file": "flatten.ora", "size": (4, 4),
            "layers": ["Background", "Hidden", "Top"], "absent": [],
            "states": [
                (None, [(0, 0, blue), (1, 1, purple), (3, 3, blue)], None),
                ("Top", [(1, 1, red), (0, 0, dark)], None),
                ("Background", [(0, 0, blue), (3, 3, blue)], None),
                ("Hidden", [(0, 0, white)], None),
            ],
            "agree": [("Background", 0, 0), ("Background", 3, 3)],
        },
        {
            "name": "layers-psb", "file": "layers.psb", "size": (8, 6),
            "layers": ["Base", "Hidden_Group_Spot"], "absent": [],
            "states": [
                (None, [(0, 0, dark), (3, 3, dark)], None),
                ("Base", [(0, 0, grey), (7, 5, grey)], None),
                ("Hidden_Group_Spot", [(2, 3, yellow), (0, 0, dark)], None),
            ],
        },
        {
            "name": "groups8-psd", "file": "groups8.psd", "size": (8, 6),
            "layers": ["Background", "Group_A_Inner", "Group_A_Masked", "Top_Layer"],
            "absent": ["Empty", "Group_A_Empty", "Levels"],
            "states": [
                (None, [(0, 0, unmatted)], None),
                ("Background", [(0, 0, grey)], None),
                ("Group_A_Inner", [(2, 1, green), (0, 0, dark)], None),
                ("Group_A_Masked", [(4, 3, red), (6, 3, dark)], None),
            ],
        },
        {
            "name": "tiles-kra", "file": "tiles.kra", "size": (256, 64),
            "layers": ["Strip"], "absent": [],
            "states": [
                (None, [(32, 32, dark), (224, 32, dark)], None),
                ("Strip", [(32, 32, dark), (96, 32, red_only), (160, 32, dark), (224, 32, red_only)],
                 (32, [32, 96, 224])),
            ],
        },
        {
            "name": "tiles-xcf", "file": "tiles.xcf", "size": (256, 64),
            "layers": ["Strip"], "absent": [],
            "states": [
                (None, [(32, 32, dark), (96, 32, red_only), (160, 32, red_only), (224, 32, red_only)],
                 (32, [32, 96, 160, 224])),
                ("Strip", [(32, 32, dark), (96, 32, red_only), (160, 32, red_only), (224, 32, red_only)],
                 (32, [32, 96, 160, 224])),
            ],
            "agree": [("Strip", 32, 32), ("Strip", 96, 32), ("Strip", 160, 32), ("Strip", 224, 32)],
        },
    ]

    def is_real_fixture(path):
        """A decodable fixture rather than a Git LFS pointer file."""
        if not os.path.exists(path) or os.path.getsize(path) < 16:
            return False
        with open(path, "rb") as f:
            return not f.read(32).startswith(b"version https://git-lfs")

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

    def drawn_image_rect(image):
        """(left, top, right, bottom) of the picture on screen, from the rows and columns that are
        mostly not the viewer's background (the viewer draws a frame round the image)."""
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

    def layer_combo():
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and "Layer:" in (w.toolTip() or ""):
                    items = [str(i) for i in (w.property("natronItems") or [])]
                    if items and items[0] == NO_LAYER_ITEM:
                        state["combo"] = w
                        return w, items
            except RuntimeError:
                continue
        return None, []

    def menu_items():
        return layer_combo()[1]

    def current_item():
        combo, _items = layer_combo()
        return None if combo is None else str(combo.property("natronCurrentText"))

    def choose(item):
        combo, items = layer_combo()
        if combo is None or item not in items:
            return False
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(item)))
        QApplication.processEvents()
        return True

    def probe_colour(image, rect, size, x, y):
        left, top, right, bottom = rect
        sx = int(left + (x + 0.5) * (right - left) / float(size[0]))
        sy = int(top + (y + 0.5) * (bottom - top) / float(size[1]))
        return rgb(image, sx, sy)

    def read_probes(size, probes, ordering, rect):
        image = viewer_crop()
        if image is None:
            return None
        colours = [probe_colour(image, rect, size, x, y) for (x, y, _p) in probes]
        rises = None
        if ordering:
            row, xs = ordering
            reds = [probe_colour(image, rect, size, x, row)[0] for x in xs]
            rises = all(reds[i] < reds[i + 1] for i in range(len(reds) - 1))
            return colours, rises, reds
        return colours, rises, None

    def probes_hold(size, probes, ordering, rect):
        got = read_probes(size, probes, ordering, rect)
        if got is None:
            return False
        colours, rises, _reds = got
        return all(p(c) for c, (_x, _y, p) in zip(colours, probes)) and rises is not False

    def show_state(label, fixture, item, probes, ordering, rect_holder, seen):
        size = fixture["size"]
        shot = "m79-%s-%s.png" % (fixture["name"], label)
        waited = 0.0
        while waited < PROBE_TIMEOUT_S:
            yield ("sleep", POLL_MS)
            waited += POLL_MS / 1000.0
            if rect_holder.get("rect") is None:
                image = viewer_crop()
                if image is not None:
                    rect = drawn_image_rect(image)
                    if rect[2] > rect[0] and rect[3] > rect[1]:
                        rect_holder["rect"] = rect
            if rect_holder.get("rect") and probes_hold(size, probes, ordering, rect_holder["rect"]):
                break
        gt.shot(shot)
        rect = rect_holder.get("rect")
        check(rect is not None, "%s %s: the picture is drawn in the viewer" % (fixture["name"], label))
        if rect is None:
            return
        got = read_probes(size, probes, ordering, rect)
        colours, rises, reds = got
        for c, (x, y, p) in zip(colours, probes):
            seen[(item, x, y)] = c
            check(p(c), "%s %s: pixel (%d, %d) is %s, got %r" % (fixture["name"], label, x, y, p.__name__, c))
        if ordering:
            check(rises, "%s %s: the red steps rise along row %d, got %r" % (fixture["name"], label, ordering[0], reds))
        messages = gt.viewer_messages()
        check(not messages, "%s %s: the viewer shows no error (%r)" % (fixture["name"], label, messages))

    def steps():
        # A viewer widget only exists once a Viewer node does.
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        state["viewer"] = viewer

        for number, fixture in enumerate(FIXTURES):
            path = os.path.join(LAYERED, fixture["file"])
            gt.section(fixture["name"])
            if not is_real_fixture(path):
                report("NOTE %s is missing or a Git LFS pointer: skipped" % path)
                continue
            read = app.createReader(path)
            check(read is not None, "create a Read for %s" % fixture["file"])
            if read is None:
                continue
            read.setPosition(200 * number, 0)
            state["read"] = read
            viewer.disconnectInput(0)
            check(viewer.connectInput(0, read), "the Viewer accepts the Read")
            fit_viewer()

            expected = fixture["layers"]
            ok = yield ("until", lambda: all(name in menu_items() for name in expected), MENU_TIMEOUT_S)
            items = menu_items()
            report("NOTE %s layer menu: %r" % (fixture["name"], items))
            check(ok, "%s: the layer menu lists %r" % (fixture["name"], expected))
            for name in fixture["absent"]:
                check(name not in items, "%s: the layer menu has no %r" % (fixture["name"], name))
            composite = COLOUR_PLANE_ITEM
            report("NOTE %s composite menu item: %r" % (fixture["name"], composite))

            rect_holder = {}
            seen = {}
            for item, probes, ordering in fixture["states"]:
                target = composite if item is None else item
                if not choose(target):
                    check(False, "%s: the layer menu offers %r" % (fixture["name"], target))
                    continue
                if item is None:
                    check(current_item() == COLOUR_PLANE_ITEM,
                          "%s: the composite is the %r menu item, got %r"
                          % (fixture["name"], COLOUR_PLANE_ITEM, current_item()))
                fit_viewer()
                label = "composite" if item is None else item
                yield from show_state(label, fixture, item, probes, ordering, rect_holder, seen)
            for layer, x, y in fixture.get("agree", []):
                composite_colour = seen.get((None, x, y))
                layer_colour = seen.get((layer, x, y))
                check(composite_colour is not None and layer_colour is not None
                      and max(abs(a - b) for a, b in zip(composite_colour, layer_colour)) <= AGREE_TOLERANCE,
                      "%s: the composite and %s agree at (%d, %d), got %r and %r"
                      % (fixture["name"], layer, x, y, composite_colour, layer_colour))

            viewer.disconnectInput(0)
            read.destroy()
            state.pop("read", None)
            QApplication.processEvents()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
