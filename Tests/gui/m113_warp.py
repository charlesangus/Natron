"""Checks the native warp nodes in the running GUI: STMap and IDistort reading U and V from a
non-colour layer, the CornerPin viewer overlay, and the Tracker's export to a CornerPin.

The source is a CheckerBoard whose four boxes around the image centre are green (top left), red
(top right), yellow (bottom right) and blue (bottom left), so the colour on the viewer at a point
says which part of the source the warp sampled. The UV maps are Constant nodes, copied by a Shuffle
into a project layer called "warp" with the first two channels swapped. The swap makes the colour
plane of the UV input a decoy: a node that read it instead of the layer would land in another box.

Sections (all by default; WARP_GUI_SECTIONS selects some, comma-separated):
    stmap-layer, idistort-layer, cornerpin-overlay, tracker-export

The whole run needs GUI_TEST_TIMEOUT of about 600 seconds. Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 560
SETTLE_MS = 600
POLL_TIMEOUT_S = 25.0
SOLVE_TIMEOUT_S = 40.0
ALL_SECTIONS = ("stmap-layer", "idistort-layer", "cornerpin-overlay", "tracker-export")
FALLBACK_SIZE = (1920.0, 1080.0)

PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
PLUGIN_SHUFFLE = "fr.natron.Shuffle"
PLUGIN_STMAP = "STMap"
PLUGIN_IDISTORT = "IDistort"
PLUGIN_CORNERPIN = "CornerPin"
PLUGIN_TRACKER = "fr.inria.built-in.Tracker"
PLUGIN_VIEWER = "fr.inria.built-in.Viewer"

# Checker box colours (linear), by quadrant around the image centre, and the viewer-side class of each.
BOX_COLOURS = {
    "color0": (0.9, 0.1, 0.1, 1.0),
    "color1": (0.1, 0.9, 0.1, 1.0),
    "color2": (0.1, 0.1, 0.9, 1.0),
    "color3": (0.9, 0.9, 0.1, 1.0),
}
QUADRANT_CLASS = {"top-left": "green", "top-right": "red", "bottom-right": "yellow", "bottom-left": "blue"}

CENTRE_AND_NEIGHBOURS = ((0.5, 0.5), (0.45, 0.5), (0.55, 0.5), (0.5, 0.45), (0.5, 0.55))
STMAP_POINTS = ((0.5, 0.5), (0.35, 0.5), (0.65, 0.5), (0.5, 0.35), (0.5, 0.65))
SPREAD_POINTS = ((0.5, 0.5), (0.2, 0.2), (0.8, 0.2), (0.2, 0.8), (0.8, 0.8))
PIXEL_DIFF_MIN = 60
CHANGED_PIXELS_MIN = 150

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    wanted = os.environ.get("WARP_GUI_SECTIONS", "").strip()
    SECTIONS = [s.strip() for s in wanted.split(",") if s.strip()] if wanted else list(ALL_SECTIONS)
    for _s in SECTIONS:
        if _s not in ALL_SECTIONS:
            raise RuntimeError("unknown section %r in WARP_GUI_SECTIONS; known: %s" % (_s, ", ".join(ALL_SECTIONS)))

    # ---- graph helpers -------------------------------------------------------------------

    def create(plugin_id, x=0, y=0, panel=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(panel)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        gt.require(p is not None, "%s has a parameter %r" % (node.getPluginID(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        return gt.require(node.connectInput(input_nb, source),
                          "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def destroy_all(nodes):
        for n in nodes:
            try:
                n.destroy(False)
            except Exception:
                report("NOTE could not destroy %r: %s" % (n, traceback.format_exc().splitlines()[-1]))
        QApplication.processEvents()

    def rod_size(node):
        try:
            rod = node.getRegionOfDefinition(1, 0)
            w, h = rod.x2 - rod.x1, rod.y2 - rod.y1
            if w > 0 and h > 0:
                return rod.x1, rod.y1, w, h
        except Exception:
            pass
        report("NOTE no region of definition from %s; assuming %dx%d at the origin" %
               (node.getLabel(), FALLBACK_SIZE[0], FALLBACK_SIZE[1]))
        return 0.0, 0.0, FALLBACK_SIZE[0], FALLBACK_SIZE[1]

    def make_checker(x, y):
        """A CheckerBoard with boxes half the image across and no lines, and its size."""
        checker = create(PLUGIN_CHECKER, x, y)
        _x, _y, w, h = rod_size(checker)
        param(checker, "boxSize").set(w / 2.0, h / 2.0)
        for name, rgba in BOX_COLOURS.items():
            param(checker, name).set(*rgba)
        for name in ("lineWidth", "centerlineWidth"):
            try:
                param(checker, name).setValue(0.0)
            except Exception:
                report("NOTE could not zero %s: %s" % (name, traceback.format_exc().splitlines()[-1]))
        return checker, w, h

    def make_uv_layer_source(x, y, rgb):
        """Constant(rgb) -> Shuffle copying channels into the project layer "warp" with R and G
        swapped, so warp.R holds rgb[1] and warp.G holds rgb[0] while rgba.R and rgba.G hold the
        reverse. Returns (constant, shuffle)."""
        const = create(PLUGIN_CONSTANT, x, y)
        param(const, "color").set(rgb[0], rgb[1], rgb[2], 1.0)
        shuffle = create(PLUGIN_SHUFFLE, x, y + 120)
        connect(shuffle, 0, const)
        shuffle.getParam("out1").setLayer("warp")
        mapping = param(shuffle, "mapping")
        mapping.connect("in1.#1", "out1.#0")
        mapping.connect("in1.#0", "out1.#1")
        mapping.connect("0", "out1.#2")
        return const, shuffle

    def ensure_warp_layer():
        try:
            if app.getProjectLayer("warp") is None:
                app.addProjectLayer("warp", ["R", "G", "B"])
        except Exception:
            report("NOTE could not register the warp layer: " + traceback.format_exc().splitlines()[-1])

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 600)
        return v

    # ---- viewer pixels -------------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def screen_image():
        QApplication.processEvents()
        return QGuiApplication.primaryScreen().grabWindow(0).toImage()

    def viewer_origin_and_size(widget):
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return top_left.x(), top_left.y(), widget.width(), widget.height()

    def viewer_pixels(fractions):
        """Display RGB at each (fx, fy) fraction of the first visible viewer widget."""
        widget = visible_viewer()
        if widget is None:
            return None
        x0, y0, w, h = viewer_origin_and_size(widget)
        img = screen_image()
        out = []
        for fx, fy in fractions:
            c = img.pixelColor(int(x0 + fx * w), int(y0 + fy * h))
            out.append((c.red(), c.green(), c.blue()))
        return out

    def classify(rgb):
        r, g, b = rgb
        hi, lo = 170, 140
        if r > hi and g < lo and b < lo:
            return "red"
        if g > hi and r < lo and b < lo:
            return "green"
        if b > hi and r < lo and g < lo:
            return "blue"
        if r > hi and g > hi and b < lo:
            return "yellow"
        return "other"

    def classes_at(fractions):
        px = viewer_pixels(fractions)
        return None if px is None else [classify(c) for c in px]

    def matches(expected, fractions, at_least):
        cls = classes_at(fractions)
        return cls is not None and sum(1 for c in cls if c == expected) >= at_least

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        x0, y0, w, h = viewer_origin_and_size(widget)
        return screen_image().copy(x0, y0, w, h)

    def changed_pixels(a, b, stride=2):
        """How many sampled pixels differ clearly between two same-sized viewer grabs, and their bounding box."""
        if a is None or b is None or a.size() != b.size():
            return -1, None
        count = 0
        x1 = y1 = 10 ** 9
        x2 = y2 = -1
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
                    x1, y1, x2, y2 = min(x1, x), min(y1, y), max(x2, x), max(y2, y)
        return count, ((x1, y1, x2, y2) if count else None)

    def fmt(v):
        return ", ".join("%.1f" % x for x in v)

    # ---- sections ------------------------------------------------------------------------

    def uv_layer_section(label, plugin_id, source_input, uv_input, uv_rgb_of, expected, decoy, fractions, offset_checks):
        """Shared body of the STMap and IDistort sections. uv_rgb_of(w, h) is the Constant colour."""
        gt.section(label)
        viewer = viewer_node()
        ensure_warp_layer()
        checker, w, h = make_checker(0, 0)
        const, shuffle = make_uv_layer_source(300, 0, uv_rgb_of(w, h))
        warp = create(plugin_id, 150, 300)
        connect(warp, source_input, checker)
        connect(warp, uv_input, shuffle)

        param(warp, "channelU").set("warp.R")
        param(warp, "channelV").set("warp.G")
        check(param(warp, "channelU").get() == "warp.R" and param(warp, "channelV").get() == "warp.G",
              "U and V select warp.R and warp.G (got %r, %r)" % (param(warp, "channelU").get(), param(warp, "channelV").get()))
        gt.close_all_panels()

        # The unwarped source, for contrast.
        connect(viewer, 0, checker)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: viewer_pixels(SPREAD_POINTS) is not None and
                    any(c != "other" for c in classes_at(SPREAD_POINTS)), POLL_TIMEOUT_S)
        before = classes_at(SPREAD_POINTS)
        check(ok and len(set(before)) >= 2,
              "the unwarped CheckerBoard shows at least two different box colours at five points (%s)" % before)
        unwarped_centre = classes_at(((0.5, 0.5),))
        gt.shot(label + "-source.png")

        connect(viewer, 0, warp)
        ok = yield ("until", lambda: matches(expected, fractions, 4), POLL_TIMEOUT_S)
        got = classes_at(fractions)
        check(ok, "%s output shows the %s box, the part of the source the warp layer points at (classes: %s)" %
              (plugin_id, expected, got))
        check(got is not None and got[0] != decoy,
              "%s output at the centre is not the %s box that the colour plane of the UV input would give" %
              (plugin_id, decoy))
        report("NOTE %s: unwarped centre class %s, warped centre class %s" % (label, unwarped_centre, got[:1] if got else None))
        yield ("sleep", SETTLE_MS)
        gt.shot(label + ".png")

        for desc, rgb, want in offset_checks(w, h):
            param(const, "color").set(rgb[0], rgb[1], rgb[2], 1.0)
            ok = yield ("until", lambda want=want: matches(want, fractions, 4), POLL_TIMEOUT_S)
            check(ok, "%s: %s shows the %s box (classes: %s)" % (plugin_id, desc, want, classes_at(fractions)))
        yield ("sleep", SETTLE_MS)
        gt.shot(label + "-edited.png")

        connect(viewer, 0, checker)
        destroy_all([warp, shuffle, const, checker])

    def section_stmap():
        # warp.R = U = rgb[1], warp.G = V = rgb[0]. U .375, V .625 is inside the top-left box.
        def uv(w, h):
            return (0.625, 0.375, 0.0)

        def edits(w, h):
            return [("U .625, V .375 in the layer", (0.375, 0.625, 0.0), QUADRANT_CLASS["bottom-right"]),
                    ("U .625, V .625 in the layer", (0.625, 0.625, 0.0), QUADRANT_CLASS["top-right"])]

        yield from uv_layer_section("stmap-layer", PLUGIN_STMAP, 1, 0, uv,
                                    QUADRANT_CLASS["top-left"], QUADRANT_CLASS["bottom-right"],
                                    STMAP_POINTS,
                                    edits)

    def section_idistort():
        # warp.R = U offset = rgb[1] = +w/8, warp.G = V offset = rgb[0] = -h/8: the output centre
        # samples the source right of and below its centre. The colour plane holds the reverse.
        def uv(w, h):
            return (-h / 8.0, w / 8.0, 0.0)

        def edits(w, h):
            return [("a +w/8, +h/8 offset in the layer", (h / 8.0, w / 8.0, 0.0), QUADRANT_CLASS["top-right"]),
                    ("a -w/8, +h/8 offset in the layer", (h / 8.0, -w / 8.0, 0.0), QUADRANT_CLASS["top-left"]),
                    ("a -w/8, -h/8 offset in the layer", (-h / 8.0, -w / 8.0, 0.0), QUADRANT_CLASS["bottom-left"])]

        yield from uv_layer_section("idistort-layer", PLUGIN_IDISTORT, 0, 1, uv,
                                    QUADRANT_CLASS["bottom-right"], QUADRANT_CLASS["top-left"],
                                    CENTRE_AND_NEIGHBOURS, edits)

    def section_cornerpin_overlay():
        gt.section("cornerpin-overlay")
        viewer = viewer_node()
        checker, w, h = make_checker(0, 0)
        x0, y0, _w, _h = rod_size(checker)
        cx, cy = x0 + w / 2.0, y0 + h / 2.0
        pin = create(PLUGIN_CORNERPIN, 0, 200, panel=True)
        connect(pin, 0, checker)
        connect(viewer, 0, pin)
        yield ("sleep", SETTLE_MS)

        to_names = ("to1", "to2", "to3", "to4")
        to_params = [param(pin, n) for n in to_names]
        # Connecting a source resets the points to its corners once the node has seen it.
        ok = yield ("until", lambda: any(abs(p.get().x - cx) > 1 or abs(p.get().y - cy) > 1 for p in to_params), 10.0)
        check(ok, "CornerPin's to points follow the connected source (to1=%s)" % ((to_params[0].get().x, to_params[0].get().y),))
        original = [(p.get().x, p.get().y) for p in to_params]
        if not ok:
            report("NOTE the points did not reset; shrinking from whatever they hold")
        shrunk = [(cx + 0.6 * (x - cx), cy + 0.6 * (y - cy)) for x, y in original]
        for p, (x, y) in zip(to_params, shrunk):
            p.set(x, y)
        shifted_to1 = (shrunk[0][0] + 0.12 * w, shrunk[0][1] + 0.12 * h)

        def settle():
            yield ("sleep", SETTLE_MS * 2)

        yield from settle()
        panel_open = gt.any_panel_open()
        check(panel_open, "the CornerPin settings panel is open")

        open_a = viewer_crop()
        gt.shot("cornerpin-overlay-open.png")

        to_params[0].set(*shifted_to1)
        yield from settle()
        open_b = viewer_crop()
        gt.shot("cornerpin-overlay-open-moved.png")

        gt.close_all_panels()
        yield from settle()
        closed_b = viewer_crop()
        to_params[0].set(*shrunk[0])
        yield from settle()
        closed_a = viewer_crop()
        gt.shot("cornerpin-overlay-closed.png")

        sizes_equal = all(i is not None for i in (open_a, open_b, closed_a, closed_b)) and \
            open_a.size() == closed_a.size() == open_b.size() == closed_b.size()
        check(sizes_equal, "the viewer keeps its size when the settings panel closes")
        if sizes_equal:
            drawn_a, box_a = changed_pixels(open_a, closed_a)
            drawn_b, box_b = changed_pixels(open_b, closed_b)
            check(drawn_a >= CHANGED_PIXELS_MIN,
                  "the overlay draws handles with the panel open (%d sampled pixels differ from the closed panel, box %s)" % (drawn_a, box_a))
            check(drawn_b >= CHANGED_PIXELS_MIN,
                  "the overlay still draws after to1 moves (%d pixels, box %s)" % (drawn_b, box_b))
            image_moved, image_box = changed_pixels(closed_a, closed_b)
            check(image_moved >= CHANGED_PIXELS_MIN,
                  "moving to1 changes the CornerPin output (%d sampled pixels differ, box %s)" % (image_moved, image_box))
            handles_moved, _hb = changed_pixels(open_a, open_b)
            check(handles_moved > image_moved,
                  "moving to1 changes the overlay as well as the image (%d vs %d pixels differ)" % (handles_moved, image_moved))
        report("NOTE the handle position is not asserted: Python has no mapping from canonical to viewer-widget "
               "coordinates, so the overlay is checked by differencing viewer grabs with the panel open and closed; "
               "see the cornerpin-overlay screenshots")
        destroy_all([pin, checker])

    TRACK_OFFSETS = ((12.0, 5.0), (-8.0, 9.0), (15.0, -6.0), (4.0, 11.0))
    TRACK_FRAMES = (1, 2, 3)

    def section_tracker_export():
        gt.section("tracker-export")
        viewer = viewer_node()
        checker, w, h = make_checker(0, 0)
        x0, y0, _w, _h = rod_size(checker)
        cx, cy = x0 + w / 2.0, y0 + h / 2.0
        base = [(cx - 0.2 * w, cy - 0.2 * h), (cx + 0.2 * w, cy - 0.2 * h),
                (cx + 0.2 * w, cy + 0.2 * h), (cx - 0.2 * w, cy + 0.2 * h)]

        def track_at(i, frame):
            dx, dy = TRACK_OFFSETS[i]
            return base[i][0] + (frame - 1) * dx, base[i][1] + (frame - 1) * dy

        tracker = create(PLUGIN_TRACKER, 0, 150)
        connect(tracker, 0, checker)
        context = tracker.getTrackerContext()
        gt.require(context is not None, "the Tracker node has a tracker context")

        # Off first: each edit below would otherwise start a solve of its own.
        param(tracker, "autoComputeransform").set(False)
        param(tracker, "transformType").set("CornerPin")
        param(tracker, "motionType").set("Match-Move")
        param(tracker, "referenceFrame").set(1)
        check(param(tracker, "transformType").get() == 1 and param(tracker, "motionType").get() == 2,
              "Tracker is set to CornerPin and Match-Move (indices %s, %s)" %
              (param(tracker, "transformType").get(), param(tracker, "motionType").get()))

        for i in range(4):
            param(tracker, "from%d" % (i + 1)).set(*base[i])
            param(tracker, "enable%d" % (i + 1)).set(True)

        tracks = []
        for i in range(4):
            track = context.createTrack()
            gt.require(track is not None, "create track %d" % (i + 1))
            centre = track.getParam("centerPoint")
            gt.require(centre is not None, "track %d has a centerPoint parameter" % (i + 1))
            for frame in TRACK_FRAMES:
                x, y = track_at(i, frame)
                centre.setValueAtTime(x, frame, 0)
                centre.setValueAtTime(y, frame, 1)
            tracks.append(track)
        check(len(tracks) == 4, "four tracks hold centre keyframes at frames %s" % (TRACK_FRAMES,))

        # The Tracker only acts on its buttons while a viewer shows it, so the solve needs one.
        connect(viewer, 0, tracker)
        yield ("sleep", SETTLE_MS)
        param(tracker, "computeTransform").trigger()

        def solved():
            p = tracker.getParam("to1")
            q = track_at(0, TRACK_FRAMES[-1])
            t = p.get(TRACK_FRAMES[-1])
            return abs(t.x - q[0]) < 0.5 and abs(t.y - q[1]) < 0.5

        ok = yield ("until", solved, SOLVE_TIMEOUT_S)
        check(ok, "the tracker solves its CornerPin from the four tracks")
        def worst_error(node, label):
            worst = 0.0
            for i in range(4):
                p = param(node, "to%d" % (i + 1))
                for frame in TRACK_FRAMES:
                    t = p.get(frame)
                    ex, ey = track_at(i, frame)
                    worst = max(worst, abs(t.x - ex), abs(t.y - ey))
            report("NOTE %s: worst distance between a to point and its track over frames %s is %.4f px" %
                   (label, TRACK_FRAMES, worst))
            return worst

        check(worst_error(tracker, "tracker") < 0.5,
              "the tracker's to points follow the four tracks at frames %s" % (TRACK_FRAMES,))

        def exported(link):
            before = set(n.getScriptName() for n in app.getChildren() if n.getPluginID() == PLUGIN_CORNERPIN)
            param(tracker, "exportLink").set(link)
            param(tracker, "export").trigger()
            QApplication.processEvents()
            made = [n for n in app.getChildren()
                    if n.getPluginID() == PLUGIN_CORNERPIN and n.getScriptName() not in before]
            return made

        linked_nodes = exported(True)
        check(len(linked_nodes) == 1, "exporting with Link on creates one CornerPin (%d)" % len(linked_nodes))
        baked_nodes = exported(False)
        check(len(baked_nodes) == 1, "exporting with Link off creates one CornerPin (%d)" % len(baked_nodes))
        if len(linked_nodes) != 1 or len(baked_nodes) != 1:
            gt.shot("tracker-export-failed.png")
            return
        linked, baked = linked_nodes[0], baked_nodes[0]
        linked.setPosition(250, 300)
        baked.setPosition(450, 300)

        for node, label in ((linked, "linked"), (baked, "copied")):
            check(worst_error(node, label) < 0.5,
                  "the %s export's to points follow the tracks at frames %s" % (label, TRACK_FRAMES))
            from_ok = all(abs(param(node, "from%d" % (i + 1)).get().x - base[i][0]) < 1e-6 and
                          abs(param(node, "from%d" % (i + 1)).get().y - base[i][1]) < 1e-6 for i in range(4))
            check(from_ok, "the %s export's from points are the tracker's at the reference frame" % label)
            check(all(param(node, "enable%d" % (i + 1)).get() for i in range(4)),
                  "the %s export has all four points enabled" % label)

        # Edit the tracker's to1 at frame 2: only the linked export may follow.
        edit_before = param(linked, "to1").get(2).x
        param(tracker, "to1").setValueAtTime(edit_before + 25.0, 2, 0)
        QApplication.processEvents()
        check(abs(param(linked, "to1").get(2).x - (edit_before + 25.0)) < 1e-6,
              "the linked export follows an edit of the tracker's to1 at frame 2")
        check(abs(param(baked, "to1").get(2).x - edit_before) < 1e-6,
              "the copied export keeps its own animation after the tracker's to1 is edited")
        param(tracker, "to1").setValueAtTime(edit_before, 2, 0)

        # Show the linked export warping the checker at the last frame, then at the first.
        connect(linked, 0, checker)
        connect(viewer, 0, linked)
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        images = {}
        for frame in (1, 3):
            py_viewer.seek(frame)
            yield ("sleep", SETTLE_MS * 2)
            images[frame] = viewer_crop()
            gt.shot("tracker-export-frame%d.png" % frame)
        moved, box = changed_pixels(images[1], images[3])
        check(moved >= CHANGED_PIXELS_MIN,
              "the exported CornerPin warps the image differently at frames 1 and 3 (%d sampled pixels differ, box %s)" % (moved, box))
        py_viewer.seek(1)
        destroy_all([linked, baked, tracker, checker])

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield ("sleep", 1000)
        gt.close_all_panels()

        runners = {
            "stmap-layer": section_stmap,
            "idistort-layer": section_idistort,
            "cornerpin-overlay": section_cornerpin_overlay,
            "tracker-export": section_tracker_export,
        }
        for name in SECTIONS:
            report("NOTE running section " + name)
            try:
                yield from runners[name]()
            except SystemExit:
                raise
            except Exception:
                check(False, "section %s raised: %s" % (name, traceback.format_exc()))
                gt.shot("error-%s.png" % name)
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
