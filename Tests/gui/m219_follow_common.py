"""Plate, scene and follow helpers shared by the two Follow Vectors GUI checks,
m219_curves_follow_vectors.py and m219_follow_abort.py.

The scripts import this after putting Tests/gui on sys.path, and call bind(app) with the application
object Natron hands to a GUI script before using anything else.
"""
import html
import json
import os
import re
import subprocess
import time

from PySide6.QtCore import QPoint, QRect, Qt
from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter
from PySide6.QtWidgets import QApplication, QLabel, QMessageBox, QTreeWidget, QTreeWidgetItemIterator, QWidget

import guitest as gt
import roto_gui as rg
from guitest import check, report, require


def bind(application):
    # Not defined at import: a star import of this module would otherwise overwrite the script's own app.
    global app
    app = application

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
SEEK_MS = 400
SHOT_MS = 900
POLL_TIMEOUT_S = 30.0
FOLLOW_TIMEOUT_S = 600.0
# Under the runner's 900 s, so a follow that never ends fails with the script's notes written, not a kill.
ABORT_TIMEOUT_S = 600.0
ABORT_END_S = 10.0
DIALOG_TIMEOUT_S = 3.0
TICK_MS = 20
PANEL_MENU_DELAY_MS = 400

WIDTH, HEIGHT = 320, 200
PLATE_FRAMES = 30
MAIN_LAST = 12
START = 1
PAN_U, PAN_V = 2, 1
BG_X0, BG_Y0 = 100, 40

SQ_X, SQ_Y, SQ_SIZE = 130.0, 130.0, 60.0
SQ2_X, SQ2_Y = 60.0, 170.0
BRIDGE_KEY = 9
BRIDGE_OFFSET = (8.0, 0.0)
LATER_ONLY_KEY = 9
POINTS = 4

STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
STALL_LIMIT_MS = 50.0
# The script's own tick also waits behind viewer repaints, so its limit is looser than the monitor's.
TICK_GAP_LIMIT_MS = 250.0
MIN_TICKS = 8

# Fine DIS reads a pure pan to a few hundredths of a pixel; TV-L1 is only held to a looser bound.
POS_TOL = 0.5
TVL1_TOL = 1.0
KEY_TOL = 1e-3
BRIDGE_TOL = 1.0
UNCHANGED_TOL = 1e-6
MATRIX_LINEAR_TOL = 0.02
MATRIX_PERSPECTIVE_TOL = 1e-3

ABORT_AFTER_KEYS = 4

HINTS = {
    "model": "How a shape follows the vectors",
    "rangeBackward": "backward over the whole range",
    "rangeForward": "forward over the whole range",
    "backwardToStart": "back to the start of the range",
    "backwardOne": "Follows the selection one frame back",
    "forwardOne": "Follows the selection one frame forward",
    "forwardToEnd": "to the end of the range",
    "blend": "Fills the frames between the keys",
    "threshold": "A vector with less confidence than this",
}
BUTTON_KEYS = ["rangeBackward", "backwardToStart", "backwardOne", "forwardOne", "forwardToEnd", "rangeForward", "blend"]
BUTTON_LABELS = {
    "rangeBackward": "Follow Range Backward",
    "rangeForward": "Follow Range Forward",
    "backwardToStart": "Follow Backward To Start",
    "backwardOne": "Follow Backward One",
    "forwardOne": "Follow Forward One",
    "forwardToEnd": "Follow Forward To End",
    "blend": "Blend Between Keys",
}
VECTORS_INPUT_LABEL = "Vectors"
THRESHOLD_LABEL = "Follow Confidence Threshold:"
MODELS = ["Per Point", "Translate", "Affine", "Perspective"]
# getOptions() returns the option ids, not the labels the menu shows.
MODEL_IDS = {"Per Point": "perPoint", "Translate": "translate", "Affine": "affine", "Perspective": "perspective"}

BASE = os.path.join(gt.ROOT, "build", "m219-curves-follow-vectors")

# Python wrappers die with their last reference and take the C++ side's script handles with them.
_keep = []
_viewer = []

def keep(obj):
    _keep.append(obj)
    return obj

def mono_ms():
    return time.monotonic() * 1000.0

# ---- graph -------------------------------------------------------------------------------

def param(node, name):
    p = node.getParam(name)
    require(p is not None, "%s has a parameter %r" % (node.getLabel(), name))
    return keep(p)

def viewer_node():
    if not _viewer:
        _viewer.append(app.getNode("Viewer1") or app.createNode("Viewer"))
        require(_viewer[0] is not None, "the project has a Viewer")
    return _viewer[0]

def seek(frame):
    app.getViewer(viewer_node().getScriptName()).seek(frame)

def set_choice(node, name, option_id):
    p = param(node, name)
    options = [str(o) for o in p.getOptions()]
    require(option_id in options, "%s.%s offers %r (has %r)" % (node.getLabel(), name, option_id, options))
    p.setValue(options.index(option_id))

def set_project_range(first, last):
    app.getProjectParam("frameRange").set(first, last)

def run_tool(args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout

def make_texture(directory, path, width, height, seed):
    """Noise at three scales, so the pyramid's coarse levels still see structure the way they do on a photograph."""
    layers = []
    for n, (divisor, weight) in enumerate(((12, 1.0), (4, 0.5), (2, 0.25))):
        layer = os.path.join(directory, "layer%d.exr" % n)
        run_tool(["oiiotool", "--pattern", "noise:type=uniform:min=0:max=1:seed=%d" % (seed + n),
                  "%dx%d" % (max(2, width // divisor), max(2, height // divisor)), "3",
                  "--resize:filter=blackman-harris", "%dx%d" % (width, height), "--mulc", str(weight), "-d", "float", "-o", layer])
        layers.append(layer)
    run_tool(["oiiotool", layers[0], layers[1], "--add", layers[2], "--add", "--mulc", "0.55", "-d", "float", "-o", path])

def make_plate(directory):
    """Writes frames 1..PLATE_FRAMES as plate.NNNN.exr; returns the file pattern."""
    os.makedirs(directory, exist_ok=True)
    background = os.path.join(directory, "background.exr")
    make_texture(directory, background, WIDTH + 100, HEIGHT + 220, 7)
    for f in range(1, PLATE_FRAMES + 1):
        path = os.path.join(directory, "plate.%04d.exr" % f)
        # Cutting at a smaller x and a larger y moves the content +x and +y in Natron's y-up pixels.
        run_tool(["oiiotool", background, "--cut", "%dx%d+%d+%d" % (WIDTH, HEIGHT, BG_X0 - PAN_U * f, BG_Y0 + PAN_V * f),
                  "-d", "float", "-o", path])
    return os.path.join(directory, "plate.####.exr")

def make_generator(read, x, y, outputs="forwardBackward", method="dis"):
    node = app.createNode("VectorGenerator")
    require(node is not None, "create a VectorGenerator", gt.plugin_problem("VectorGenerator"))
    node.setPosition(x, y)
    require(node.connectInput(0, read), "connect %s to %s" % (node.getLabel(), read.getLabel()))
    set_choice(node, "quality", "fine")
    set_choice(node, "method", method)
    set_choice(node, "outputs", outputs)
    return keep(node)

# ---- screenshots -------------------------------------------------------------------------

def shot(name, caption):
    """The whole screen under a caption strip, so a reviewer reads the numbers beside the picture."""
    QApplication.processEvents()
    screen = QGuiApplication.primaryScreen().grabWindow(0).toImage()
    bar = 36
    out = QImage(screen.width(), screen.height() + bar, QImage.Format_RGB32)
    out.fill(QColor(16, 16, 16))
    painter = QPainter(out)
    painter.drawImage(0, bar, screen)
    painter.setPen(QColor(255, 255, 255))
    font = QFont()
    font.setPixelSize(15)
    painter.setFont(font)
    painter.drawText(8, 0, out.width() - 16, bar, int(Qt.AlignVCenter | Qt.AlignLeft), caption)
    painter.end()
    path = os.path.join(gt.OUT, name)
    if out.save(path):
        report("SHOT " + path)
        report("NOTE caption of %s: %s" % (name, caption))
    else:
        report("NOTE could not save the screenshot " + path)

# ---- shapes and selection ----------------------------------------------------------------

def pos(bez, i, t):
    return tuple(bez.getControlPointPosition(i, t))

def shifted(p, dx, dy):
    return (p[0] + dx, p[1] + dy, p[2] + dx, p[3] + dy, p[4] + dx, p[5] + dy)

def deviation(a, b):
    return max(abs(a[k] - b[k]) for k in range(6))

def known_shift(t):
    return (PAN_U * (t - START), PAN_V * (t - START))

def snapshot(bez, frames):
    return {t: [pos(bez, i, t) for i in range(POINTS)] for t in frames}

def max_deviation(bez, frames, expected_of):
    """Largest component error of the shape's points against expected_of(frame, index) over the frames."""
    worst = 0.0
    for t in frames:
        for i in range(POINTS):
            worst = max(worst, deviation(pos(bez, i, t), expected_of(t, i)))
    return worst

def new_shape(ctx, x=SQ_X, y=SQ_Y):
    bez = ctx.createRectangle(x, y, SQ_SIZE, START)
    require(bez is not None, "create a Roto rectangle")
    require(bez.getNumControlPoints() == POINTS, "the rectangle has %d points" % POINTS)
    return keep(bez)

def keys_of(bez):
    return sorted(float(k) for k in bez.getKeyframes())

def shape_tree(label):
    for w in QApplication.allWidgets():
        try:
            if not isinstance(w, QTreeWidget) or not w.isVisible() or rg.ancestor_of_class(w, "NodeSettingsPanel") is None:
                continue
            it = QTreeWidgetItemIterator(w)
            while it.value():
                if it.value().text(0) == label:
                    return w
                it += 1
        except RuntimeError:
            continue
    return None

def tree_item(tree, label):
    it = QTreeWidgetItemIterator(tree)
    while it.value():
        if it.value().text(0) == label:
            return keep(it.value())
        it += 1
    return None

def select_shapes(shapes):
    """Selects the shapes in the panel's tree, which is what a click on their rows does."""
    tree = shape_tree(shapes[0].getLabel())
    require(tree is not None, "the panel lists %s" % shapes[0].getLabel())
    keep(tree)
    tree.clearSelection()
    for bez in shapes:
        item = tree_item(tree, bez.getLabel())
        require(item is not None, "the panel lists %s" % bez.getLabel())
        item.setSelected(True)
    QApplication.processEvents()
    return tree

def reset_shapes(ctx):
    rg.clear_shapes(ctx)
    QApplication.processEvents()

# ---- the follow --------------------------------------------------------------------------

def message(node):
    return str(node.getPersistentMessage() or "")

def report_dialog():
    """The modal dialog showing a follow's report, as (dialog, its plain text), or None. Only a follow that
    stopped on a point it could not track, failed to read or lost its source opens one; a clean follow, or
    one the user stopped, keeps the report as the node's message."""
    w = QApplication.activeModalWidget()
    if w is None or not w.isVisible():
        return None
    texts = [w.text()] if isinstance(w, QMessageBox) else []
    texts += [lab.text() for lab in w.findChildren(QLabel)]
    text = html.unescape(re.sub(r"<[^>]*>", " ", "\n".join(texts))).replace("\xa0", " ")
    return (w, text) if "Follow Vectors" in text else None

def press(node, name):
    param(node, name).trigger()
    QApplication.processEvents()

def vectors_input(node):
    for i in range(node.getMaxInputCount()):
        if str(node.getInputLabel(i)) == VECTORS_INPUT_LABEL:
            return i
    require(False, "%s has a %s input" % (node.getScriptName(), VECTORS_INPUT_LABEL))

def ensure_source(node, source):
    """Connects source to node's Vectors input."""
    index = vectors_input(node)
    current = node.getInput(index)
    if current is None or current.getScriptName() != source.getScriptName():
        if current is not None:
            node.disconnectInput(index)
        require(node.connectInput(index, source), "connect %s to the %s input of %s" % (source.getScriptName(), VECTORS_INPUT_LABEL, node.getScriptName()))
    QApplication.processEvents()

def disconnect_vectors(node):
    node.disconnectInput(vectors_input(node))
    QApplication.processEvents()

def wait_follow(scene, label, timeout, on_tick=None):
    """Ticks the GUI thread every TICK_MS until Roto.isFollowingVectors() turns false, then takes the
    report Roto.getLastFollowReport() keeps, which is None when the press was refused, and closes the
    report's dialog. on_tick may return (seconds, label) to end the wait that many seconds later
    instead, failing with that label. Returns (the report, stats)."""
    stats = {"ticks": 0, "max_gap": 0.0, "start": mono_ms(), "end": 0.0}
    deadline = time.time() + timeout
    what = "%s: the follow ends within %.0f s" % (label, timeout)
    last = time.time()
    ended = False
    while time.time() < deadline:
        yield ("sleep", TICK_MS)
        now = time.time()
        stats["max_gap"] = max(stats["max_gap"], (now - last) * 1000.0 - TICK_MS)
        stats["ticks"] += 1
        last = now
        if on_tick is not None:
            tightened = on_tick()
            if tightened is not None:
                deadline = time.time() + tightened[0]
                what = tightened[1]
        if not scene.ctx.isFollowingVectors():
            ended = True
            break
    stats["end"] = mono_ms()
    require(ended, what)
    outcome = scene.ctx.getLastFollowReport()
    require(outcome is not None, "%s: the follow ran and left a report (node message %r)" % (label, message(scene.roto)))
    yield from close_report_dialog(scene, label, outcome)
    return outcome, stats

def is_user_stop(outcome):
    if outcome["status"] != "aborted" or not outcome["message"].startswith("Follow Vectors was aborted"):
        return False
    return all(not e["passStops"] and not e.get("stopped") for e in outcome["shapes"].values())

def is_clean(outcome):
    if outcome["status"] != "done":
        return False
    return all(not e["passStops"] and not e["unfollowed"] and not e.get("stopped") for e in outcome["shapes"].values())

def close_report_dialog(scene, label, outcome):
    headline = outcome["message"].split("\n")[0]
    stored = message(scene.roto)
    check(stored == outcome["message"], "%s: the node keeps the report as its message (message %r, report %r)" % (label, stored, outcome["message"]))
    if is_clean(outcome) or is_user_stop(outcome):
        yield ("sleep", SETTLE_MS)
        shown = report_dialog()
        if shown is not None:
            shown[0].done(0)
            yield ("sleep", SETTLE_MS)
        check(shown is None, "%s: a clean or user-stopped follow opens no dialog (dialog %r)" % (label, shown[1] if shown else None))
        return
    shown = yield ("until", lambda: report_dialog() is not None, DIALOG_TIMEOUT_S)
    text = ""
    if shown:
        dialog, text = report_dialog()
        dialog.done(0)
        # The dialog's event loop only returns once this tick does.
        yield ("sleep", SETTLE_MS)
    check(headline in text, "%s: the user is shown the report %r in a dialog (dialog %r)" % (label, headline, text))

def run_follow(scene, button, label, timeout=FOLLOW_TIMEOUT_S):
    press(scene.roto, button)
    outcome, stats = yield from wait_follow(scene, label, timeout)
    report("NOTE %s: the follow took %.0f ms, %d ticks, worst GUI tick gap %.0f ms; report %r"
           % (label, stats["end"] - stats["start"], stats["ticks"], stats["max_gap"], outcome["message"]))
    check(outcome["status"] == "done", "%s: the follow is done (status %r, %r)" % (label, outcome["status"], outcome["message"]))
    return outcome

def read_stalls():
    if not STALL_LOG or not os.path.isfile(STALL_LOG):
        return []
    out = []
    with open(STALL_LOG) as f:
        for raw in f:
            raw = raw.strip()
            if not raw:
                continue
            try:
                s = json.loads(raw)
                out.append((float(s["start_ms"]), float(s["duration_ms"])))
            except (KeyError, TypeError, ValueError):
                continue
    return out

def settle(ms=SHOT_MS):
    yield ("sleep", ms)

def go_to(frame, ms=SEEK_MS):
    seek(frame)
    yield ("sleep", ms)

# ---- viewer row --------------------------------------------------------------------------

def row_widgets():
    found = {}
    for w in QApplication.allWidgets():
        try:
            tip = w.toolTip() or ""
            if not tip or not w.isVisible() or rg.ancestor_of_class(w, "ViewerTab") is None:
                continue
            for key, hint in HINTS.items():
                if hint in tip and key not in found:
                    found[key] = w
        except RuntimeError:
            continue
    return found

def on_screen(w):
    top_left = w.mapToGlobal(w.rect().topLeft())
    centre = w.mapToGlobal(w.rect().center())
    screen = QGuiApplication.primaryScreen().geometry()
    return w.width() > 0 and w.height() > 0 and screen.contains(centre) and screen.contains(top_left)

def viewer_labels():
    found = []
    for w in QApplication.allWidgets():
        try:
            if isinstance(w, QLabel) and w.isVisible() and rg.ancestor_of_class(w, "ViewerTab") is not None:
                found.append(w)
        except RuntimeError:
            continue
    return found

def global_rect(w):
    return QRect(w.mapToGlobal(QPoint(0, 0)), w.size())

def check_row(host_label, node, source):
    """With node's Vectors input disconnected the row is hidden; connecting source shows it, laid out
    with the threshold's label in the row, over nothing else, icon-only buttons and no node name."""
    disconnect_vectors(node)
    yield ("sleep", SETTLE_MS)
    ok = yield ("until", lambda: not row_widgets(), POLL_TIMEOUT_S)
    check(ok, "%s: the viewer hides the Follow Vectors row while the Vectors input is disconnected (shown: %r)"
          % (host_label, sorted(row_widgets())))

    ensure_source(node, source)
    ok = yield ("until", lambda: len(row_widgets()) == len(HINTS), POLL_TIMEOUT_S)
    found = row_widgets()
    missing = sorted(set(HINTS) - set(found))
    check(ok and not missing, "%s: the viewer shows the Follow Vectors row once the Vectors input is connected (missing: %r)" % (host_label, missing))
    hidden = sorted(k for k, w in found.items() if not on_screen(w))
    check(not hidden, "%s: every control of the row is on screen (off screen: %r)" % (host_label, hidden))

    for key in BUTTON_KEYS:
        button = found.get(key)
        if button is None:
            continue
        text = button.text() if hasattr(button, "text") else ""
        check(text == "", "%s: the %s button shows no text (has %r)" % (host_label, key, text))
        tip = html.unescape(re.sub(r"<[^>]*>", " ", button.toolTip()))
        check(BUTTON_LABELS[key] in tip and "Keyboard shortcut" in tip,
              "%s: the %s tool tip names the action and its shortcut (%r)" % (host_label, key, tip))
    labels = [str(lab.text()) for lab in viewer_labels()]
    stray = [t for t in labels if any(t.startswith(BUTTON_LABELS[k]) for k in BUTTON_KEYS)]
    check(not stray, "%s: no text label sits by the follow buttons (%r)" % (host_label, stray))

    node_label = str(node.getLabel())
    named = [t for t in labels if t.strip() == node_label]
    check(not named, "%s: the viewer shows no node-name label (%r)" % (host_label, named))

    threshold = [lab for lab in viewer_labels() if str(lab.text()) == THRESHOLD_LABEL]
    check(len(threshold) == 1, "%s: the viewer shows one %r label (found %d)" % (host_label, THRESHOLD_LABEL, len(threshold)))
    anchor = found.get("forwardOne")
    if threshold and anchor is not None:
        label = threshold[0]
        row = anchor.parentWidget()
        check(label.parentWidget() is row, "%s: the threshold label is in the follow row" % host_label)
        inside = global_rect(row).contains(global_rect(label))
        check(inside, "%s: the threshold label %r lies inside the follow row %r" % (host_label, global_rect(label), global_rect(row)))
        others = [w for w in row.findChildren(QWidget) if w is not label and w.isVisible() and w.parentWidget() is row]
        others += [w for w in found.values() if w.parentWidget() is not row]
        overlaps = sorted(set(gt.class_name(w) + ":" + (w.toolTip() or "")[:40] for w in others
                              if global_rect(w).intersects(global_rect(label))))
        check(not overlaps, "%s: the threshold label overlaps no other control (%r)" % (host_label, overlaps))
    return len(found)


class Scene(object):
    pass


def open_scene_roto(scene):
    """Creates the Roto with its panel open and connects it to the plate."""
    scene.roto, scene.ctx = rg.open_roto(app, "Roto")
    require(scene.roto is not None and scene.ctx is not None, "create a Roto with its panel open", gt.plugin_problem("Roto"))
    keep(scene.roto)
    keep(scene.ctx)
    require(scene.roto.connectInput(0, scene.read), "connect Roto to the plate")
    rg.activate()
    yield from settle()


def start_scene():
    """Waits for the node graph and viewer, writes the plate and builds the Read and the three
    VectorGenerators; returns the Scene without a Roto."""
    ok = yield ("until", lambda: gt.widgets_of("NodeGraph") != [], STARTUP_TIMEOUT_S)
    require(ok, "the node graph is up")
    viewer_node()
    ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
    require(ok, "the viewer is up")
    gt.close_all_panels()
    yield ("sleep", SETTLE_MS)
    rg.activate()

    pattern = make_plate(BASE)
    scene = Scene()
    scene.read = keep(app.createReader(pattern))
    require(scene.read is not None, "create a Read for " + pattern)
    scene.read.setPosition(0, 0)
    set_project_range(START, MAIN_LAST)
    scene.gen = make_generator(scene.read, 0, 150)
    scene.forward_gen = make_generator(scene.read, 150, 150, outputs="forward")
    scene.slow_gen = make_generator(scene.read, 300, 150, method="tvl1")
    yield ("sleep", SETTLE_MS)
    return scene
