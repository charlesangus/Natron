"""The raw nodes' viewer aids: each view draws over the viewer's picture while its node's panel is open
and the view is on, and leaves the render alone. A Canon EOS 5D Mark III CR2 goes through Read and
RawPrepare, and the node under test is made with its panel open and fed to a Viewer. For each view:
  - the Viewer settles with the view off, and shows no error;
  - turning the view on changes at least MIN_CHANGED_PX viewer pixels, a view with a limit covers no
    more of the picture than that (a sparse mask must not flood its tile), and the node's content
    hash is the one it had before;
  - turning it off brings the picture back to the one before, and the hash is still the same.
Switching RawHighlights directly from one view to another replaces the first view's mask.
The views are RawHighlights' Clipped, Combine, Candidating and Strength, RawDemosaic's Dual, Sharpened
Areas and Radius, RawHotPixels' Mark Fixed and RawColorCalibration's Show Patches.
Without the fixture (a checkout without Git LFS has a pointer file) the script ends after
reporting that.

Run in the natron-dev container (the window is pinned to 1920x1080):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m165-raw-aids/out GUI_TEST_TIMEOUT=900 \\
        Tests/gui/run-gui-test.sh Tests/gui/m165_raw_aids.py <AppImage or build/fast/App/Natron>'

Shots, one per view with the view on, under $NATRON_GUI_TEST_OUT: m165-raw-aids-<node>-<view>.png.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 120.0
POLL_S = 0.5
WATCHDOG_S = 880
CHANNEL_DELTA = 8
MIN_CHANGED_PX = 200
MIN_SWITCH_PX = 50
MAX_RESIDUAL_PX = 50
MAX_SPARSE_SHARE = 0.5

# The fixture is dim: at its own clipping threshold only a few photosites clip, with no candidates and
# no segment clipped in all channels, so those views would rightly stay empty.
HIGHLIGHTS_SETUP = {"mode": "segments", "recovery": "generic", "strength": 1.0, "clipOverride": True, "clip": 0.2}

# The viewer keeps its last picture while a render runs, so a chain's picture has settled only after
# holding for longer than that chain takes to render.
#
# (node id, upstream chain before it, other knobs, seconds to hold,
#  [(knob, option or bool, shot name, largest share of the picture the view may cover)])
CHAINS = [
    ("RawHighlights", ["RawTemperature"], HIGHLIGHTS_SETUP, 30.0, [
        ("maskView", "clipped", "highlights-clipped", MAX_SPARSE_SHARE),
        ("maskView", "combine", "highlights-combine", MAX_SPARSE_SHARE),
        ("maskView", "candidating", "highlights-candidating", MAX_SPARSE_SHARE),
        ("maskView", "strength", "highlights-strength", MAX_SPARSE_SHARE),
    ]),
    ("RawDemosaic", ["RawTemperature", "RawHighlights"], {"method": "rcdDual"}, 100.0, [
        ("maskView", "dual", "demosaic-dual", MAX_SPARSE_SHARE),
    ]),
    # Zoomed out, these two cover the whole picture: every pixel has a value, and the largest of each
    # block is what shows.
    ("RawDemosaic", ["RawTemperature", "RawHighlights"], {"captureSharpen": True}, 100.0, [
        ("maskView", "sharpenedAreas", "demosaic-sharpened-areas", None),
        ("maskView", "radius", "demosaic-radius", None),
    ]),
    ("RawHotPixels", [], {"strength": 1.0, "threshold": 0.01}, 20.0, [
        ("markFixed", True, "hotpixels-mark-fixed", MAX_SPARSE_SHARE),
    ]),
    ("RawColorCalibration", ["RawTemperature", "RawHighlights", "RawDemosaic", "RawInputColor"], {}, 100.0, [
        ("checkerShowPatches", True, "colorcalibration-show-patches", None),
    ]),
]

try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

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
        """The viewer as drawn on screen, from a screen grab: a widget grab of the GL viewer can come
        out blank under llvmpipe."""
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

    def changed_pixels(a, b):
        """How many pixels differ by more than CHANNEL_DELTA in any channel."""
        if a is None or b is None or a.size() != b.size():
            return -1
        from PySide6.QtGui import QImage
        da = bytes(a.convertToFormat(QImage.Format_RGB32).constBits())
        db = bytes(b.convertToFormat(QImage.Format_RGB32).constBits())
        count = 0
        for i in range(0, min(len(da), len(db)), 4):
            if (abs(da[i] - db[i]) > CHANNEL_DELTA or abs(da[i + 1] - db[i + 1]) > CHANNEL_DELTA
                    or abs(da[i + 2] - db[i + 2]) > CHANNEL_DELTA):
                count += 1
        return count

    def where_changed(a, b):
        """Bounding box and up to 40 coordinates of the pixels changed_pixels counts, for diagnosis."""
        from PySide6.QtGui import QImage
        da = bytes(a.convertToFormat(QImage.Format_RGB32).constBits())
        db = bytes(b.convertToFormat(QImage.Format_RGB32).constBits())
        width = a.width()
        points = []
        for i in range(0, min(len(da), len(db)), 4):
            if (abs(da[i] - db[i]) > CHANNEL_DELTA or abs(da[i + 1] - db[i + 1]) > CHANNEL_DELTA
                    or abs(da[i + 2] - db[i + 2]) > CHANNEL_DELTA):
                points.append(((i // 4) % width, (i // 4) // width, da[i + 2], da[i + 1], da[i], db[i + 2], db[i + 1], db[i]))
        if not points:
            return "none"
        xs = [q[0] for q in points]
        ys = [q[1] for q in points]
        return "n=%d bbox x %d..%d y %d..%d sample (x,y,rgb_before,rgb_now) %r" % (
            len(points), min(xs), max(xs), min(ys), max(ys), points[:40])

    def picture_pixels(image):
        """How many pixels differ from the viewer's background."""
        from PySide6.QtGui import QImage
        data = bytes(image.convertToFormat(QImage.Format_RGB32).constBits())
        background = data[8:12]
        return sum(1 for i in range(0, len(data), 4) if data[i:i + 4] != background)

    def settle(hold_s):
        """Waits until the viewer has held the same picture for hold_s, and that picture is not just its
        background. Returns that picture, or None on a timeout."""
        previous = None
        held = 0.0
        waited = 0.0
        while waited < RENDER_TIMEOUT_S + hold_s:
            yield ("sleep", int(POLL_S * 1000))
            waited += POLL_S
            image = viewer_crop()
            if image is None:
                continue
            background = image.pixelColor(2, 2)
            varied = any(image.pixelColor(x, y) != background
                         for x in range(0, image.width(), 16) for y in range(0, image.height(), 16))
            held = held + POLL_S if (varied and previous is not None and image == previous) else 0.0
            previous = image
            if held >= hold_s:
                return image
        return None

    def set_knob(node, name, value):
        param = node.getParam(name)
        require(param is not None, "%s has a %s parameter" % (node.getScriptName(), name))
        if isinstance(value, str):
            options = [param.getOption(i) for i in range(param.getNumOptions())]
            require(value in options, "%s offers %s (%r)" % (name, value, options))
            value = options.index(value)
        param.setValue(value)

    def build(node_id, upstream, setup):
        """Read, RawPrepare, the upstream nodes and the node under test with its panel open, fed to the
        Viewer. Returns the node under test."""
        for node in state.get("chain", []):
            node.destroy()
        state["chain"] = []
        previous = state["prepare"]
        for plugin_id in upstream + [node_id]:
            props = {}
            if plugin_id == node_id:
                props["CreateNodeArgsPropSettingsOpened"] = NatronEngine.BoolNodeCreationProperty(True)
            node = app.createNode(plugin_id, -1, None, props)
            require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
            require(node.connectInput(0, previous), "%s takes its input" % plugin_id)
            node.setPosition(0, 100 * (len(state["chain"]) + 2))
            state["chain"].append(node)
            previous = node
        for name, value in setup.items():
            set_knob(node, name, value)
        state["viewer"].disconnectInput(0)
        require(state["viewer"].connectInput(0, node), "the Viewer takes " + node_id)
        return node

    def steps():
        # A viewer widget only exists once a Viewer node does.
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists", gt.plugin_problem("Viewer"))
        state["viewer"] = viewer
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        if not is_real_fixture(CR2):
            report("NOTE %s is missing or a Git LFS pointer: nothing to check" % CR2)
            return

        read = app.createReader(CR2)
        require(read is not None, "create a Read for the CR2")
        read.setPosition(0, 0)
        prepare = app.createNode("RawPrepare")
        require(prepare is not None, "create RawPrepare", gt.plugin_problem("RawPrepare"))
        prepare.setPosition(0, 100)
        require(prepare.connectInput(0, read), "RawPrepare takes the Read")
        viewer.setPosition(400, 300)
        state.update(read=read, prepare=prepare)

        for node_id, upstream, setup, hold_s, views in CHAINS:
            gt.section("%s %s" % (node_id, views[0][2]))
            gt.close_all_panels()
            node = build(node_id, upstream, setup)
            fit_viewer()
            before = yield from settle(hold_s)
            require(before is not None, "%s: the Viewer settles with the views off" % node_id)
            messages = gt.viewer_messages()
            check(not messages, "%s: the Viewer shows no error (%r)" % (node_id, messages))
            hash_before = node.getContentHash()

            for knob, value, shot_name, max_share in views:
                set_knob(node, knob, value)
                ok = yield ("until", lambda: changed_pixels(viewer_crop(), before) >= MIN_CHANGED_PX, RENDER_TIMEOUT_S)
                yield ("sleep", 500)
                gt.shot("m165-raw-aids-%s.png" % shot_name)
                changed = changed_pixels(viewer_crop(), before)
                check(ok, "%s: turning the view on changes the picture (%d pixels)" % (shot_name, changed))
                if max_share is not None:
                    limit = int(max_share * picture_pixels(before))
                    check(changed <= limit, "%s: the view covers at most %d pixels (%d)" % (shot_name, limit, changed))
                check(node.getContentHash() == hash_before, "%s: the view leaves the node's render hash alone" % shot_name)

                set_knob(node, knob, "off" if isinstance(value, str) else False)
                ok = yield ("until", lambda: 0 <= changed_pixels(viewer_crop(), before) <= MAX_RESIDUAL_PX, RENDER_TIMEOUT_S)
                check(ok, "%s: turning the view off restores the picture (%d pixels differ)" % (shot_name, changed_pixels(viewer_crop(), before)))
                report("NOTE %s residual: %s" % (shot_name, where_changed(viewer_crop(), before)))
                check(node.getContentHash() == hash_before, "%s: the hash is unchanged after the view is off" % shot_name)

            if node_id == "RawHighlights":
                set_knob(node, "maskView", "clipped")
                yield ("until", lambda: changed_pixels(viewer_crop(), before) >= MIN_CHANGED_PX, RENDER_TIMEOUT_S)
                yield ("sleep", 500)
                first = viewer_crop()
                set_knob(node, "maskView", "candidating")
                ok = yield ("until", lambda: changed_pixels(viewer_crop(), first) >= MIN_SWITCH_PX, RENDER_TIMEOUT_S)
                check(ok, "switching directly between views replaces the mask (%d pixels)" % changed_pixels(viewer_crop(), first))
                set_knob(node, "maskView", "off")
                yield ("until", lambda: 0 <= changed_pixels(viewer_crop(), before) <= MAX_RESIDUAL_PX, RENDER_TIMEOUT_S)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
