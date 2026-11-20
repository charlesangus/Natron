"""Paints the RotoPaint stroke projects that tools/bench/roto_render_bench.py renders.

The Python roto API cannot create strokes, so this drives the viewer the way roto_bench.py's stroke
ops do: a RotoPaint node with its settings panel open owns the viewer interact, and synthetic
press, move and release events on the ViewerGL paint solid brush strokes. Run it under Xvfb:

    Tests/gui/run-gui-test.sh tools/bench/gui/make_roto_strokes.py build/release/Natron.AppImage

For every format and stroke count it saves <dir>/roto_strokes_<count>_<format>.ntp: a Viewer and one
RotoPaint node with no input and no image file, holding exactly <count> strokes whose life time is
All, so they render on every frame. A project that already exists is left alone.

Environment:
    ROTO_STROKES_COUNTS   stroke counts (default 50,300)
    ROTO_STROKES_FORMATS  2k (2K_DCP) and/or uhd (UHD_4K) (default 2k,uhd)
    ROTO_STROKES_BRUSH    brush size in image pixels (default 50)
    ROTO_STROKES_EVENTS   pen moves per stroke (default 16)
    ROTO_STROKES_SEED     seed of the stroke paths (default 1)
    ROTO_STROKES_DIR      output directory (default build/bench/fixtures)
    GUI_TEST_TIMEOUT      run-gui-test.sh's limit; 300 strokes take a few minutes under llvmpipe, so
                          give it 1800 or more
"""
import math
import os
import random
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())


def env_list(name, default):
    return [v.strip() for v in os.environ.get(name, default).split(",") if v.strip()]


COUNTS = [int(v) for v in env_list("ROTO_STROKES_COUNTS", "50,300")]
FORMAT_KEYS = env_list("ROTO_STROKES_FORMATS", "2k,uhd")
BRUSH = float(os.environ.get("ROTO_STROKES_BRUSH", "50"))
EVENTS = int(os.environ.get("ROTO_STROKES_EVENTS", "16"))
SEED = int(os.environ.get("ROTO_STROKES_SEED", "1"))
OUT_DIR = os.environ.get("ROTO_STROKES_DIR") or os.path.join(ROOT, "build", "bench", "fixtures")
FORMATS = {"2k": "2K_DCP", "uhd": "UHD_4K"}
STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 300
RENDER_MS = 1500
PEN_INTERVAL_MS = 4
# A stroke the interact drops (an event lost while a render held the main thread) is painted again,
# up to this many times the requested count.
MAX_ATTEMPTS_FACTOR = 2

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def activate():
        win = main_window()
        if win is not None:
            win.activateWindow()
            win.raise_()
            QApplication.processEvents()

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def send_key(widget, key):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def create(plugin_id):
        # An open settings panel makes the node's viewer interact the active one.
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        return node

    def set_format(key):
        name = FORMATS[key]
        fmt = app.getProjectParam("outputFormat")
        options = [str(o) for o in fmt.getOptions()]
        index = next((i for i, o in enumerate(options) if name in o), None)
        gt.require(index is not None, "the project offers the %s format" % name)
        fmt.setValue(index)
        QApplication.processEvents()

    def view(viewer, node):
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        if node is not None:
            gt.require(viewer.connectInput(0, node), "the Viewer shows %s" % node.getLabel())
        QApplication.processEvents()

    def strokes_of(roto_ctx):
        return list(roto_ctx.getBaseLayer().getChildren())

    def stroke_path(gl, rng):
        # Inside the middle 60% of the viewer, which a fitted 2K or UHD frame covers.
        w, h = float(gl.width()), float(gl.height())
        cx = w * rng.uniform(0.3, 0.7)
        cy = h * rng.uniform(0.3, 0.7)
        r = min(w, h) * rng.uniform(0.04, 0.12)
        phase = rng.uniform(0.0, 2.0 * math.pi)
        return [QPointF(cx + r * math.cos(phase + 3.0 * i / EVENTS), cy + 0.6 * r * math.sin(phase + 5.0 * i / EVENTS))
                for i in range(EVENTS + 1)]

    def paint(gl, pts):
        send_mouse(gl, QEvent.MouseButtonPress, pts[0], Qt.LeftButton, Qt.LeftButton)
        yield ("sleep", PEN_INTERVAL_MS)
        for p in pts[1:]:
            send_mouse(gl, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", PEN_INTERVAL_MS)
        send_mouse(gl, QEvent.MouseButtonRelease, pts[-1], Qt.LeftButton, Qt.NoButton)
        yield ("sleep", 0)

    def make_project(viewer, fkey, count):
        path = os.path.join(OUT_DIR, "roto_strokes_%d_%s.ntp" % (count, fkey))
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            report("NOTE %s exists; left alone" % path)
            return
        gt.section("strokes-%d-%s" % (count, fkey))
        gt.close_all_panels()
        set_format(fkey)
        rp = create("RotoPaint")
        roto_ctx = rp.getRotoContext()
        gt.require(roto_ctx is not None, "the RotoPaint node has a roto context")
        view(viewer, rp)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        gt.require(gl is not None, "the viewer is visible")
        send_mouse(gl, QEvent.MouseMove, QPointF(gl.width() / 2.0, gl.height() / 2.0), Qt.NoButton, Qt.NoButton)
        send_key(gl, Qt.Key_F)
        yield ("sleep", RENDER_MS)
        rp.getParam("sizeSpinbox").setValue(BRUSH)
        yield ("sleep", SETTLE_MS)

        rng = random.Random(SEED * 1000003 + count)
        attempts = 0
        while len(strokes_of(roto_ctx)) < count and attempts < MAX_ATTEMPTS_FACTOR * count:
            attempts += 1
            for cmd in paint(gl, stroke_path(gl, rng)):
                yield cmd
        items = strokes_of(roto_ctx)
        gt.require(len(items) == count, "painted %d strokes (got %d after %d attempts)" % (count, len(items), attempts))

        for item in items:
            life = item.getParam("lifeTime")
            gt.require(life is not None, "stroke %s has a lifeTime param" % item.getScriptName())
            life.set(0)
        view(viewer, None)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        app.saveProject(path)
        check(os.path.isfile(path) and os.path.getsize(path) > 0, "saved %s" % path)
        rp.destroy(False)
        yield ("sleep", SETTLE_MS)

    def steps():
        ok = yield ("until", lambda: main_window() is not None and viewer_gl() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window and viewer are up")
        if not os.path.isdir(OUT_DIR):
            os.makedirs(OUT_DIR)
        gt.close_all_panels()
        activate()
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        gt.require(viewer is not None, "a Viewer node exists")
        for fkey in FORMAT_KEYS:
            gt.require(fkey in FORMATS, "ROTO_STROKES_FORMATS names 2k or uhd, not %s" % fkey)
            for count in COUNTS:
                for cmd in make_project(viewer, fkey, count):
                    yield cmd

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
