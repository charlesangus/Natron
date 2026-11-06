"""Edit-to-viewer latency of a camera RAW chain, with the per-node cache state of each edit.

Runs inside Natron (GUI) through Tests/gui/run-gui-test.sh; tools/bench/edit_latency.sh runs the
configuration matrix. The graph is Read(raw file) -> RawPrepare -> RawTemperature -> RawHighlights ->
RawDemosaic -> RawInputColor -> <edited node> -> Viewer, where the edited node is a Grade (case
"grade") or an HSVKeyer (case "hsv"). After a warm-up the script changes one knob of the edited node
N times and writes one JSON line per edit.

Environment:
  NATRON_RENDER_PROFILE  required: the prefix of the render profile; the process appends its pid
  BENCH_CASE             grade (default) or hsv
  BENCH_ZOOM             fit (default) or 100: the viewer's zoom while editing
  BENCH_PANEL            closed (default) or open: whether the edited node's settings panel is open
  BENCH_AUTOPROXY        off (default) or on: the "autoProxyScrubbing" preference
  BENCH_EDITS            edits recorded (default 20)
  BENCH_WARMUP           untimed edits before them (default 3)
  BENCH_RAW_FILE         the raw file (default Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2)
  BENCH_LABEL            free text stored with every line
  BENCH_RESULT           JSON-lines file appended to (default $NATRON_GUI_TEST_OUT/edit_latency.jsonl)
  BENCH_TIMEOUT_S        per-wait timeout (default 600 for the first frame, 120 for an edit)
  NATRON_GPU             recorded only; it is read by Natron at startup

Each line holds latency_ms, the time from setting the knob to the profile file being written with
the first completed viewer frame of the edit. The file is flushed when a frame finishes, so its
modification time is the completion time; it is only as fine as the kernel's file timestamp (a few
milliseconds). detect_ms is the same interval as seen by a 50 ms poll, an upper bound. "nodes" holds,
for the completing frame, each node's device, cache state and whether it executed (any state but a
whole-image cache hit). "upstream_executed" counts the executed nodes other than the edited one and
the frame's root.

Edits are set programmatically, not dragged, so auto-proxy's drag-start draft switch is not exercised;
the preference still governs the viewer's other proxy paths.
"""
import glob
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

# Wrappers of child widgets die with their parent wrapper, so the ones the script drives stay referenced here.
KEEP = []

CHAIN = ("RawPrepare", "RawTemperature", "RawHighlights", "RawDemosaic", "RawInputColor")
SETTLE_S = 0.4


def log(msg):
    print("[edit-latency] " + msg)
    sys.stdout.flush()


class Profile:
    """Reads the records the render scheduler appends to <NATRON_RENDER_PROFILE>.<pid>."""

    def __init__(self, prefix):
        self.prefix = prefix
        self.offset = 0
        self.path = None
        self.grew_at_ns = None
        self.last_growth_s = time.time()

    def _locate(self):
        if self.path is None:
            found = glob.glob(self.prefix + ".*")
            if found:
                self.path = found[0]
        return self.path

    def read_new(self):
        """The records appended since the last call; a partial last line waits for the next one."""
        path = self._locate()
        if path is None:
            return []
        try:
            stat = os.stat(path)
        except OSError:
            return []
        if stat.st_size <= self.offset:
            return []
        with open(path, "rb") as f:
            f.seek(self.offset)
            data = f.read(stat.st_size - self.offset)
        end = data.rfind(b"\n")
        if end < 0:
            return []
        self.offset += end + 1
        self.grew_at_ns = stat.st_mtime_ns
        self.last_growth_s = time.time()
        records = []
        for line in data[:end].splitlines():
            line = line.strip()
            if line:
                records.append(json.loads(line))
        return records

    def quiet_for(self, seconds):
        self.read_new()
        return time.time() - self.last_growth_s >= seconds


def frame_roots(tasks):
    """The task no other task of the frame depends on."""
    depended = set()
    for rec in tasks.values():
        depended.update(rec.get("deps", []))
    roots = [t for t in tasks if t not in depended]
    return roots[-1] if roots else None


def summarise(frames, edited_node):
    """Per-node cache state of the first completed frame, and the frames the edit produced."""
    done = None
    for seq in sorted(frames):
        tasks = frames[seq]
        root = frame_roots(tasks)
        if root is not None and tasks[root].get("ok", True):
            done = (seq, tasks, root)
            break
    if done is None:
        return None
    seq, tasks, root = done
    nodes = {}
    upstream_executed = 0
    hits = 0
    for t, rec in sorted(tasks.items()):
        if not rec.get("ok", True):
            continue
        executed = rec.get("cache", "bypass") != "hit"
        nodes[rec["node"]] = {
            "plugin": rec["plugin"],
            "device": rec.get("device", "cpu"),
            "cache": rec.get("cache", "bypass"),
            "executed": executed,
            "region": rec.get("regionId", -1),
            "mip": rec["mipmapLevel"],
            "wall_ms": round(rec["wallNs"] / 1e6, 3),
        }
        if not executed:
            hits += 1
        elif t != root and rec["node"] != edited_node:
            upstream_executed += 1
    return {
        "frame": seq,
        "root": tasks[root]["node"],
        "mip": tasks[root]["mipmapLevel"],
        "nodes": nodes,
        "upstream_executed": upstream_executed,
        "hits": hits,
        "frames_after_edit": len(frames),
    }


try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, Q_ARG
    from PySide6.QtWidgets import QApplication
    import guitest as gt
    from guitest import FIXTURES, OUT, check, require, run

    case = os.environ.get("BENCH_CASE", "grade")
    zoom = os.environ.get("BENCH_ZOOM", "fit")
    panel = os.environ.get("BENCH_PANEL", "closed")
    autoproxy = os.environ.get("BENCH_AUTOPROXY", "off")
    edits = int(os.environ.get("BENCH_EDITS", "20"))
    warmup = int(os.environ.get("BENCH_WARMUP", "3"))
    first_timeout = float(os.environ.get("BENCH_TIMEOUT_S", "600"))
    edit_timeout = min(first_timeout, 120.0)
    raw = os.environ.get("BENCH_RAW_FILE") or os.path.join(FIXTURES, "raw", "canon-eos-5d-mark-iii.cr2")
    result_path = os.environ.get("BENCH_RESULT", os.path.join(OUT, "edit_latency.jsonl"))
    prefix = os.environ.get("NATRON_RENDER_PROFILE", "")
    require(bool(prefix), "NATRON_RENDER_PROFILE is set")
    require(case in ("grade", "hsv"), "BENCH_CASE is grade or hsv")
    require(zoom in ("fit", "100"), "BENCH_ZOOM is fit or 100")
    require(panel in ("open", "closed"), "BENCH_PANEL is open or closed")
    require(autoproxy in ("on", "off"), "BENCH_AUTOPROXY is on or off")
    require(os.path.isfile(raw), "the raw file exists: " + raw)

    settings = NatronEngine.natron.getSettings()
    proxy_param = settings.getParam("autoProxyScrubbing")
    require(proxy_param is not None, "the autoProxyScrubbing preference exists")
    proxy_param.setValue(autoproxy == "on")

    read = app.createReader(raw)
    require(read is not None, "create a reader for the raw file")
    previous = read
    for plugin_id in CHAIN:
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id)
        node.connectInput(0, previous)
        previous = node
    if case == "grade":
        edited = app.createNode("Grade")
    else:
        edited = app.createNode("HSVKeyer")
    require(edited is not None, "create the edited node")
    edited.connectInput(0, previous)
    edited_name = edited.getScriptName()

    if case == "grade":
        knob = edited.getParam("multiply")

        def set_edit(i):
            value = 1.0 + 0.01 * (i + 1)
            knob.set(value, 1.0, 1.0, 1.0)
            return value
    else:
        knob = edited.getParam("lumaRange")

        def set_edit(i):
            value = 0.30 + 0.005 * (i + 1)
            knob.setValue(value, 0)
            return value

    require(knob is not None, "the edited node has its knob")

    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

    def visible(class_name):
        found = [w for w in gt.widgets_of(class_name) if w.isVisible()]
        return found[0] if found else None

    def set_panel():
        gt.close_all_panels()
        if panel == "open":
            graph = visible("NodeGraph")
            require(graph is not None, "a node graph is visible")
            KEEP.append(graph)
            app.clearSelection()
            app.selectNode(edited, True)
            QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
            QApplication.processEvents()
            require(gt.any_panel_open(), "the edited node's settings panel is open")
        else:
            require(not gt.any_panel_open(), "no settings panel is open")

    def set_zoom():
        gl = visible("ViewerGL")
        require(gl is not None, "a viewer is visible")
        KEEP.append(gl)
        if zoom == "fit":
            QMetaObject.invokeMethod(gl, "fitImageToFormat")
        else:
            QMetaObject.invokeMethod(gl, "zoomSlot", Q_ARG(int, 100))
        QApplication.processEvents()

    profile = Profile(prefix)
    pending = {"frames": {}, "complete": False, "at_ns": None}

    def collect():
        for rec in profile.read_new():
            if rec.get("kind") == "region":
                continue
            frame = pending["frames"].setdefault(rec["frame"], {})
            frame[rec["task"]] = rec
            root = frame_roots(frame)
            if root is not None and frame[root].get("ok", True) and not pending["complete"]:
                pending["complete"] = True
                pending["at_ns"] = profile.grew_at_ns
        return pending["complete"]

    def reset():
        pending["frames"] = {}
        pending["complete"] = False
        pending["at_ns"] = None

    def settled():
        collect()
        return profile.quiet_for(SETTLE_S)

    def steps():
        set_panel()
        reset()
        require(viewer.connectInput(0, edited), "connect the Viewer")
        ok = yield ("until", collect, first_timeout)
        require(ok, "the viewer renders the first frame")
        ok = yield ("until", settled, first_timeout)
        require(ok, "the first frame settles")
        set_zoom()
        reset()
        ok = yield ("until", settled, first_timeout)
        require(ok, "the view settles after the zoom change")

        recorded = 0
        for i in range(warmup + edits):
            reset()
            ok = yield ("until", settled, edit_timeout)
            check(ok, "quiet before edit %d" % i)
            reset()
            t0_ns = time.time_ns()
            value = set_edit(i)
            ok = yield ("until", collect, edit_timeout)
            detected_ns = time.time_ns()
            timed_out = not ok
            if ok:
                yield ("until", settled, edit_timeout)
            if i < warmup:
                continue
            summary = summarise(pending["frames"], edited_name) if ok else None
            at_ns = pending["at_ns"]
            record = {
                "label": os.environ.get("BENCH_LABEL", ""),
                "case": case,
                "zoom": zoom,
                "panel": panel,
                "autoproxy": autoproxy,
                "gpu": os.environ.get("NATRON_GPU", "unset"),
                "file": os.path.basename(raw),
                "edit": i - warmup,
                "value": round(value, 6),
                "timed_out": timed_out,
                "latency_ms": round(max(0.0, (at_ns - t0_ns) / 1e6), 2) if at_ns else None,
                "detect_ms": round((detected_ns - t0_ns) / 1e6, 2),
                "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            if summary:
                record.update(summary)
            with open(result_path, "a") as f:
                f.write(json.dumps(record) + "\n")
            recorded += 1
            check(not timed_out, "edit %d reached the viewer" % (i - warmup))
        log("%s zoom=%s panel=%s autoproxy=%s: %d edits recorded in %s" % (case, zoom, panel, autoproxy, recorded, result_path))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
