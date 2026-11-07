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

Outside Natron, "python3 -I edit_latency_bench.py --classify <out-dir>" attributes every upstream
execution of every recorded edit to a cause, reading edit_latency.jsonl and each configuration's render
profile, and exits 1 if any execution is left unclassified. See docs/perf/interactive-edit-attribution.md.
"""
import collections
import glob
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

# Wrappers of child widgets die with their parent wrapper, so the ones the script drives stay referenced here.
KEEP = []

# lumaRange is Outer Low, Inner Low, Centre, Inner High, Outer High; the offsets only move the key's falloffs,
# which the image's luma may never reach, whereas the centre moves the whole range.
LUMA_CENTRE_DIMENSION = 2

CHAIN = ("RawPrepare", "RawTemperature", "RawHighlights", "RawDemosaic", "RawInputColor")
SETTLE_S = 0.4
# Records reach the file only when a frame ends, so a quiet file proves nothing during a 15 s render.
IDLE_CPU_FRACTION = 0.25


class Profile:
    """Reads the records the render scheduler appends to <NATRON_RENDER_PROFILE>.<pid>."""

    def __init__(self, prefix):
        self.prefix = prefix
        self.offset = 0
        self.path = None
        self.grew_at_ns = None
        self.last_growth_s = time.time()
        self.cpu_mark = (time.time(), time.process_time())

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
        now = time.time()
        if now - self.cpu_mark[0] >= seconds:
            busy = (time.process_time() - self.cpu_mark[1]) / (now - self.cpu_mark[0])
            self.cpu_mark = (now, time.process_time())
            return busy < IDLE_CPU_FRACTION and now - self.last_growth_s >= seconds
        return False


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
        elif t != root and rec["node"] != edited_node and not did_no_work(rec):
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


CAUSES = collections.OrderedDict([
    ("not_admitted", ("M300 - Unified Tiered Cache", "Node::shouldCacheOutput false, so renderRoI sets createInCache false")),
    ("behind_hit", ("M300 - Unified Tiered Cache", "the task graph runs every dependency though a consumer's image is cached")),
    ("gpu_region_member", ("M301 - GPU Cache Tier", "GpuRegionTask storeOutputs writes only the per-frame store")),
    ("mip_or_draft_key", ("M303 - Interactive Edit Latency", "no cached image at this mip; draft is in ImageKey")),
    ("placement_iscached_miss", ("M301 - GPU Cache Tier", "placed on the device although the host cache held the image")),
    ("eviction", ("M300 - Unified Tiered Cache", "stored earlier under the same hash and mip, gone now")),
    ("hash_change", ("none: the render is required", "the node's content hash changed across the edit")),
    ("first_store", ("none: nothing to reuse yet", "the node's first cached image of the run")),
    ("partial_roi", ("M300 - Unified Tiered Cache", "the cached image lacks part of the requested RoI")),
    ("unclassified", ("-", "the evidence fits no cause")),
])
# renderRoI returns before its cache lookup for an identity node or an empty RoI, leaving the state bypass.
NO_WORK_MS = 0.5


def on_device(rec):
    return rec.get("device") == "gpu" or rec.get("regionId", -1) >= 0


def did_no_work(rec):
    # A region member's bypass is the default of a task that never reached renderRoI, not an identity return.
    return rec.get("cache", "bypass") == "bypass" and not on_device(rec) and rec["wallNs"] / 1e6 < NO_WORK_MS


def behind_a_hit(tasks):
    """The tasks no executing consumer needs: every consumer is a hit or itself behind one. A dependency that
    also feeds a task that ran stays out, since that task needed it."""
    by_index = {rec["task"]: rec for rec in tasks}
    consumers = collections.defaultdict(list)
    for rec in tasks:
        for dep in rec.get("deps", []):
            if dep in by_index:
                consumers[dep].append(rec["task"])
    unneeded = {t for t, rec in by_index.items() if rec.get("cache") == "hit"}
    behind = set()
    changed = True
    while changed:
        changed = False
        for t, users in consumers.items():
            if t not in behind and all(u in unneeded for u in users):
                behind.add(t)
                unneeded.add(t)
                changed = True
    return behind


def classify_task(rec, edit, history, behind):
    """The cause of one executed task, or None if the task did no work. history holds the same node's
    earlier cached (mip, state) pairs, oldest first, from every earlier frame of the run. The profile carries
    no draft flag, so a draft key change at an unchanged mip would read as an eviction; programmatic edits
    never switch to draft, so in this benchmark it cannot occur."""
    node = rec["node"]
    wall_ms = rec["wallNs"] / 1e6
    state = rec.get("cache", "bypass")
    if did_no_work(rec):
        return None
    before = edit.get("hashes_before", {}).get(node)
    after = edit.get("hashes", {}).get(node)
    if before is None or after is None:
        return "unclassified"
    if before != after:
        return "hash_change"
    if rec["task"] in behind and wall_ms >= NO_WORK_MS:
        return "behind_hit"
    if on_device(rec):
        return "placement_iscached_miss" if any(mip == rec["mipmapLevel"] for mip, _ in history) else "gpu_region_member"
    if state == "bypass":
        return "not_admitted"
    if state == "partial":
        return "partial_roi"
    if state == "miss":
        if not history:
            return "first_store"
        # A finer image serves a coarser request through the lookup's downscale, so only those count.
        if any(mip <= rec["mipmapLevel"] for mip, _ in history):
            return "eviction"
        return "mip_or_draft_key"
    return "unclassified"


def load_profile(path):
    frames = collections.OrderedDict()
    for line in open(path):
        line = line.strip()
        if line:
            rec = json.loads(line)
            if rec.get("kind") != "region":
                frames.setdefault(rec["frame"], []).append(rec)
    return frames


def classify_run(profile_path, edits, edited_node):
    """(per-cause execution counts, per-cause wall ms, per-node cause counts, tasks that did no work)
    of the recorded edits, for one configuration."""
    # A timed-out edit has no frame, so the frames after the last edit before it cannot be told apart from its own.
    ordered = sorted(edits, key=lambda e: e.get("edit", 0))
    unattributable = []
    for i, e in enumerate(ordered):
        if e.get("frame") is None:
            before = [p["frame"] for p in ordered[:i] if p.get("frame") is not None]
            after = [n["frame"] for n in ordered[i + 1:] if n.get("frame") is not None]
            unattributable.append((max(before) if before else -1, min(after) if after else float("inf")))
    edits = sorted((e for e in edits if e.get("frame") is not None), key=lambda e: e["frame"])
    counts = collections.Counter()
    wall = collections.Counter()
    per_node = collections.defaultdict(collections.Counter)
    no_work = 0
    history = collections.defaultdict(list)
    for seq, tasks in sorted(load_profile(profile_path).items()):
        edit = None
        for candidate in edits:
            if candidate["frame"] <= seq:
                edit = candidate
        if any(low < seq < high for low, high in unattributable):
            edit = None
            counts["unclassified"] += len(tasks)
        behind = behind_a_hit(tasks)
        depended = set()
        for rec in tasks:
            depended.update(rec.get("deps", []))
        for rec in tasks:
            state = rec.get("cache", "bypass")
            is_upstream = rec["node"] != edited_node and rec["task"] in depended
            if edit is not None and is_upstream and state != "hit":
                cause = classify_task(rec, edit, history[rec["node"]], behind)
                if cause is None:
                    no_work += 1
                else:
                    counts[cause] += 1
                    wall[cause] += rec["wallNs"] / 1e6
                    per_node[rec["node"]][cause] += 1
            if state in ("miss", "partial", "hit"):
                history[rec["node"]].append((rec["mipmapLevel"], state))
    return counts, wall, per_node, no_work


def classify_main(out_dir):
    groups = collections.OrderedDict()
    for line in open(os.path.join(out_dir, "edit_latency.jsonl")):
        if line.strip():
            r = json.loads(line)
            tag = "%s-%s-panel-%s-gpu-%s-proxy-%s" % (r["case"], r["zoom"], r["panel"], r["gpu"], r["autoproxy"])
            groups.setdefault(tag, []).append(r)
    unclassified = 0
    names = [c for c in CAUSES]
    print("| configuration | edits | upstream executions | " + " | ".join(names) + " | no work | wall ms per edit |")
    print("|---|---|---|" + "---|" * (len(names) + 2))
    for tag, edits in groups.items():
        found = glob.glob(os.path.join(out_dir, tag, "render.prof.*"))
        if len(found) != 1:
            print("| %s | no single render profile: %d found |" % (tag, len(found)))
            unclassified += 1
            continue
        edited = edits[0].get("edited")
        counts, wall, _, no_work = classify_run(found[0], edits, edited)
        unclassified += counts["unclassified"]
        total = sum(counts.values())
        print("| %s | %d | %d | %s | %d | %.0f |" % (
            tag, len(edits), total, " | ".join(str(counts[c]) for c in names), no_work,
            sum(wall.values()) / max(1, len(edits))))
    print()
    print("unclassified: %d" % unclassified)
    return 1 if unclassified else 0


if "--classify" in getattr(sys, "argv", []):
    sys.exit(classify_main(sys.argv[sys.argv.index("--classify") + 1]))

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
    chain_nodes = [read]
    for plugin_id in CHAIN:
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id)
        node.connectInput(0, previous)
        previous = node
        chain_nodes.append(node)
    if case == "grade":
        edited = app.createNode("Grade")
    else:
        edited = app.createNode("HSVKeyer")
    require(edited is not None, "create the edited node")
    edited.connectInput(0, previous)
    edited_name = edited.getScriptName()
    chain_nodes.append(edited)

    def content_hashes():
        return {n.getScriptName(): n.getContentHash() for n in chain_nodes}

    if case == "grade":
        knob = edited.getParam("multiply")

        def set_edit(i):
            value = 1.0 + 0.01 * (i + 1)
            knob.set(value, 1.0, 1.0, 1.0)
            return value
    else:
        knob = edited.getParam("lumaRange")

        def set_edit(i):
            value = 0.5 + 0.005 * (i + 1)
            knob.setValue(value, LUMA_CENTRE_DIMENSION)
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
        tab = visible("ViewerTab")
        require(gl is not None and tab is not None, "a viewer is visible")
        KEEP.extend([gl, tab])
        # invokeMethod only warns about a name that is no slot, so the outcome has to be checked.
        if zoom == "fit":
            invoked = QMetaObject.invokeMethod(tab, "centerViewer")
        else:
            invoked = QMetaObject.invokeMethod(gl, "zoomSlot", Q_ARG(int, 100))
        require(invoked is not False, "the zoom slot ran")
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
        yield ("sleep", 1000)
        ok = yield ("until", settled, first_timeout)
        require(ok, "the view settles after the zoom change")
        gt.shot("%s-%s-%s-%s.png" % (case, zoom, panel, autoproxy))

        recorded = 0
        for i in range(warmup + edits):
            reset()
            ok = yield ("until", settled, edit_timeout)
            check(ok, "quiet before edit %d" % i)
            reset()
            hashes_before = content_hashes()
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
                "edited": edited_name,
                "hashes_before": hashes_before,
                "hashes": content_hashes(),
                "loadavg": round(os.getloadavg()[0], 1),
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
            if summary:
                expected_mip = summary["mip"] == 0 if zoom == "100" else summary["mip"] > 0
                check(expected_mip, "edit %d rendered at the zoom's mip (%d)" % (i - warmup, summary["mip"]))
        gt.report("NOTE %s zoom=%s panel=%s autoproxy=%s: %d edits recorded in %s" % (case, zoom, panel, autoproxy, recorded, result_path))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
