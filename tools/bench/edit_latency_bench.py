"""Edit-to-viewer latency of a camera RAW chain, with the per-node cache state of each edit.

Runs inside Natron (GUI) through Tests/gui/run-gui-test.sh; tools/bench/edit_latency.sh runs the
configuration matrix. The graph is Read(raw file) -> RawPrepare -> RawTemperature -> RawHighlights ->
RawDemosaic -> RawInputColor -> <edited node> -> Viewer, where the edited node is a Grade (case
"grade") or an HSVKeyer (case "hsv"). After a warm-up the script changes one knob of the edited node
N times and writes one JSON line per edit.

Scenario "offbranch" adds work off the active viewer's branch: RawInputColor -> K Grades, each with an
animated multiply -> Merge input A, with the edited node on input B, -> Viewer2 in a pane of its own.
Viewer1 is made the active viewer, so only the chain and the edited node carry the viewer-branch signal.
Every edit re-renders both viewers. The K Grades keep their hashes across edits, and being animated
they carry no static-output reason, so they are what the two admission policies treat differently.

Environment:
  NATRON_RENDER_PROFILE  required: the prefix of the render profile; the process appends its pid
  BENCH_CASE             grade (default) or hsv
  BENCH_SCENARIO         chain (default) or offbranch
  BENCH_OFFBRANCH_NODES  K, the off-branch Grades of "offbranch" (default 8)
  BENCH_ZOOM             fit (default) or 100: the viewer's zoom while editing
  BENCH_PANEL            closed (default) or open: whether the edited node's settings panel is open
  BENCH_AUTOPROXY        off (default) or on: the "autoProxyScrubbing" preference
  BENCH_RAM_PERCENT      the "maxRAMPercent" preference, set before the first render; empty or
                         "default" leaves it alone
  BENCH_REP              repetition index, recorded only
  BENCH_EDITS            edits recorded (default 20)
  BENCH_WARMUP           untimed edits before them (default 3)
  BENCH_RAW_FILE         the raw file (default Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2)
  BENCH_LABEL            free text stored with every line
  BENCH_RESULT           JSON-lines file appended to (default $NATRON_GUI_TEST_OUT/edit_latency.jsonl)
  BENCH_TIMEOUT_S        per-wait timeout (default 600 for the first frame, 120 for an edit)
  NATRON_GPU             recorded only; it is read by Natron at startup
  NATRON_CACHE_ADMISSION recorded as "policy" ("default" when unset); it is read by Natron at startup

Memory and disk, read after each edit has settled: vm_hwm_mb and vm_rss_mb (VmHWM and VmRSS of
/proc/self/status, so vm_hwm_mb of the last edit is the run's peak), io_write_bytes (write_bytes of
/proc/self/io since just before the first render, which counts the disk tier's writes with every other
write of the process) and io_write_bytes_edit (the same across this edit alone), and tier_bytes and
tier_files (allocated bytes and file count under the disk tier's directory), with tier_fs its
filesystem type. The tier has no counter reachable from Python.

Each line holds latency_ms, the time from setting the knob to the profile file being written with
the first completed viewer frame of the edit. The file is flushed when a frame finishes, so its
modification time is the completion time; it is only as fine as the kernel's file timestamp (a few
milliseconds). detect_ms is the same interval as seen by a 50 ms poll, an upper bound. "nodes" holds,
for the completing frame, each node's device, cache state and whether it executed (any state but a
whole-image cache hit). "upstream_executed" counts the executed nodes other than the edited one and
the frame's root.

In "offbranch" the viewed frame is the first completed frame without the Merge, and "offbranch" holds
the first completed frame with it: its latency_ms, upstream_executed, offbranch_executed (how many of the
K Grades executed) and nodes.

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


def config_tag(r):
    """The configuration's directory name under the out-dir; tools/bench/edit_latency.sh builds the same."""
    return "%s-%s-%s-panel-%s-gpu-%s-proxy-%s-ram-%s-adm-%s-rep-%s" % (
        r.get("scenario", "chain"), r["case"], r["zoom"], r["panel"], r["gpu"], r["autoproxy"],
        r.get("ram_percent", "default"), r.get("policy", "default"), r.get("rep", 0))


def proc_fields(path, names):
    found = {}
    try:
        with open(path) as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() in names:
                    found[key.strip()] = int(value.split()[0])
    except OSError:
        pass
    return found


def memory_mb():
    status = proc_fields("/proc/self/status", ("VmHWM", "VmRSS"))
    return {"vm_hwm_mb": round(status.get("VmHWM", 0) / 1024.0, 1), "vm_rss_mb": round(status.get("VmRSS", 0) / 1024.0, 1)}


def write_bytes():
    return proc_fields("/proc/self/io", ("write_bytes",)).get("write_bytes")


def tier_usage(root):
    allocated = 0
    files = 0
    for directory, _, names in os.walk(root):
        for name in names:
            try:
                st = os.lstat(os.path.join(directory, name))
            except OSError:
                continue
            allocated += st.st_blocks * 512
            files += 1
    return allocated, files


def filesystem_of(path):
    path = os.path.realpath(path)
    best = ("", "unknown")
    try:
        with open("/proc/self/mounts") as f:
            for line in f:
                fields = line.split()
                if len(fields) < 3:
                    continue
                mount = fields[1].replace("\\040", " ")
                inside = path == mount or path.startswith(mount.rstrip("/") + "/")
                if inside and len(mount) > len(best[0]):
                    best = (mount, fields[2])
    except OSError:
        pass
    return best[1]


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
    ("behind_hit", ("M300 - Unified Tiered Cache", "a dependency ran though its consumer's image was cached by then: planning prunes only the hits it can see")),
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
            groups.setdefault(config_tag(r), []).append(r)
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
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QMouseEvent
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
    scenario = os.environ.get("BENCH_SCENARIO", "chain")
    offbranch_count = int(os.environ.get("BENCH_OFFBRANCH_NODES", "8"))
    ram_percent = os.environ.get("BENCH_RAM_PERCENT", "") or "default"
    policy = os.environ.get("NATRON_CACHE_ADMISSION", "") or "default"
    require(scenario in ("chain", "offbranch"), "BENCH_SCENARIO is chain or offbranch")
    require(offbranch_count > 0, "BENCH_OFFBRANCH_NODES is positive")

    settings = NatronEngine.natron.getSettings()
    proxy_param = settings.getParam("autoProxyScrubbing")
    require(proxy_param is not None, "the autoProxyScrubbing preference exists")
    proxy_param.setValue(autoproxy == "on")
    if ram_percent != "default":
        ram_param = settings.getParam("maxRAMPercent")
        require(ram_param is not None, "the maxRAMPercent preference exists")
        ram_param.setValue(int(ram_percent))
        require(ram_param.getValue() == int(ram_percent), "maxRAMPercent is " + ram_percent)
    tier_param = settings.getParam("diskTierLocation")
    tier_root = (tier_param.getValue() if tier_param is not None else "") or os.path.join(os.path.expanduser("~"), ".cache", "Natron", "tier")
    tier_size_param = settings.getParam("diskTierSize")
    tier_size_gib = tier_size_param.getValue() if tier_size_param is not None else None
    tier_disabled = os.environ.get("NATRON_CACHE_DISK_TIER") == "off" or tier_size_gib == 0

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

    offbranch_names = []
    merge_name = None
    if scenario == "offbranch":
        off_previous = previous
        for i in range(offbranch_count):
            off = app.createNode("Grade")
            require(off is not None, "create off-branch Grade %d" % i)
            off.connectInput(0, off_previous)
            multiply = off.getParam("multiply")
            require(multiply is not None, "the off-branch Grade has multiply")
            for dimension in range(3):
                multiply.setValueAtTime(0.9 - 0.01 * i, 0, dimension)
                multiply.setValueAtTime(1.1, 1000, dimension)
            preview = off.getParam("enablePreview")
            if preview is not None:
                preview.setValue(False)
            off_previous = off
            offbranch_names.append(off.getScriptName())
            chain_nodes.append(off)
        merge = app.createNode("Merge")
        require(merge is not None, "create the Merge")
        require(merge.connectInput(0, edited), "connect the edited node to Merge B")
        require(merge.connectInput(1, off_previous), "connect the off-branch to Merge A")
        merge_name = merge.getScriptName()
        chain_nodes.append(merge)

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

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def viewer_in_pane(class_name, pane_name):
        for w in gt.widgets_of(class_name):
            pane = ancestor_of_class(w, "TabWidget")
            if w.isVisible() and pane is not None and pane.objectName() == pane_name:
                return w
        return None

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

    def set_zoom(gl, tab):
        require(gl is not None and tab is not None, "a viewer is visible")
        KEEP.extend([gl, tab])
        # invokeMethod only warns about a name that is no slot, so the outcome has to be checked.
        if zoom == "fit":
            invoked = QMetaObject.invokeMethod(tab, "centerViewer")
        else:
            invoked = QMetaObject.invokeMethod(gl, "zoomSlot", Q_ARG(int, 100))
        require(invoked is not False, "the zoom slot ran")
        QApplication.processEvents()

    def make_active(gl):
        """Only a press in a viewer makes it the active viewer. A middle press and release pans by nothing."""
        centre = QPointF(gl.width() / 2.0, gl.height() / 2.0)
        for kind, buttons in ((QEvent.MouseButtonPress, Qt.MiddleButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
            event = QMouseEvent(kind, centre, QPointF(gl.mapToGlobal(centre.toPoint())), Qt.MiddleButton, buttons, Qt.NoModifier)
            QApplication.sendEvent(gl, event)
        QApplication.processEvents()

    profile = Profile(prefix)
    pending = {"frames": {}, "done": {}}
    expected_kinds = ("viewed",)

    def kind_of(tasks):
        return "offbranch" if merge_name is not None and any(r["node"] == merge_name for r in tasks.values()) else "viewed"

    def collect():
        for rec in profile.read_new():
            if rec.get("kind") == "region":
                continue
            frame = pending["frames"].setdefault(rec["frame"], {})
            frame[rec["task"]] = rec
            root = frame_roots(frame)
            kind = kind_of(frame)
            if root is not None and frame[root].get("ok", True) and kind not in pending["done"]:
                pending["done"][kind] = profile.grew_at_ns
        return all(k in pending["done"] for k in expected_kinds)

    def reset():
        pending["frames"] = {}
        pending["done"] = {}

    def settled():
        collect()
        return profile.quiet_for(SETTLE_S)

    def frames_of(kind):
        return {seq: tasks for seq, tasks in pending["frames"].items() if kind_of(tasks) == kind}

    def ms_since(t0_ns, at_ns):
        return round(max(0.0, (at_ns - t0_ns) / 1e6), 2) if at_ns else None

    def tier_fields():
        if tier_disabled:
            return {"tier_bytes": None, "tier_files": None}
        allocated, files = tier_usage(tier_root)
        return {"tier_bytes": allocated, "tier_files": files}

    def steps():
        global expected_kinds
        set_panel()
        reset()
        io_base = write_bytes()
        require(viewer.connectInput(0, edited), "connect the Viewer")
        ok = yield ("until", collect, first_timeout)
        require(ok, "the viewer renders the first frame")
        ok = yield ("until", settled, first_timeout)
        require(ok, "the first frame settles")
        gl1 = visible("ViewerGL")
        tab1 = visible("ViewerTab")
        set_zoom(gl1, tab1)
        reset()
        yield ("sleep", 1000)
        ok = yield ("until", settled, first_timeout)
        require(ok, "the view settles after the zoom change")

        if scenario == "offbranch":
            home = ancestor_of_class(tab1, "TabWidget")
            require(home is not None, "Viewer1 is in a pane")
            home_name = home.objectName()
            viewer2 = app.createNode("Viewer")
            require(viewer2 is not None, "create the second Viewer")
            QApplication.processEvents()
            split = app.getTabWidget(home_name).splitHorizontally()
            require(split is not None, "split Viewer1's pane")
            split_name = split.getScriptName()
            require(app.moveTab(viewer2.getScriptName(), split), "move the second Viewer to its own pane")
            split = None
            QApplication.processEvents()
            ok = yield ("until", lambda: viewer_in_pane("ViewerGL", home_name) is not None and viewer_in_pane("ViewerGL", split_name) is not None, 30)
            require(ok, "both viewers are visible")
            gl1 = viewer_in_pane("ViewerGL", home_name)
            set_zoom(viewer_in_pane("ViewerGL", split_name), viewer_in_pane("ViewerTab", split_name))
            make_active(gl1)
            require(app.getActiveViewer() is not None, "a viewer is active after the press in Viewer1")
            reset()
            expected_kinds = ("offbranch",)
            require(viewer2.connectInput(0, merge), "connect the second Viewer to the Merge")
            ok = yield ("until", collect, first_timeout)
            require(ok, "the second viewer renders its first frame")
            ok = yield ("until", settled, first_timeout)
            require(ok, "the second viewer's first frame settles")
            expected_kinds = ("viewed", "offbranch")
        gt.shot("%s-%s-%s-%s-%s.png" % (scenario, case, zoom, panel, autoproxy))

        recorded = 0
        for i in range(warmup + edits):
            reset()
            ok = yield ("until", settled, edit_timeout)
            check(ok, "quiet before edit %d" % i)
            reset()
            hashes_before = content_hashes()
            io_before = write_bytes()
            t0_ns = time.time_ns()
            value = set_edit(i)
            ok = yield ("until", collect, edit_timeout)
            detected_ns = time.time_ns()
            timed_out = "viewed" not in pending["done"]
            if ok:
                yield ("until", settled, edit_timeout)
            if i < warmup:
                continue
            summary = summarise(frames_of("viewed"), edited_name) if not timed_out else None
            io_now = write_bytes()
            record = {
                "label": os.environ.get("BENCH_LABEL", ""),
                "scenario": scenario,
                "case": case,
                "zoom": zoom,
                "panel": panel,
                "autoproxy": autoproxy,
                "gpu": os.environ.get("NATRON_GPU", "unset"),
                "policy": policy,
                "ram_percent": ram_percent,
                "rep": int(os.environ.get("BENCH_REP", "0")),
                "edited": edited_name,
                "hashes_before": hashes_before,
                "hashes": content_hashes(),
                "loadavg": round(os.getloadavg()[0], 1),
                "file": os.path.basename(raw),
                "edit": i - warmup,
                "value": round(value, 6),
                "timed_out": timed_out,
                "latency_ms": ms_since(t0_ns, pending["done"].get("viewed")),
                "detect_ms": round((detected_ns - t0_ns) / 1e6, 2),
                "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "io_write_bytes": io_now - io_base if io_now is not None and io_base is not None else None,
                "io_write_bytes_edit": io_now - io_before if io_now is not None and io_before is not None else None,
                "tier_fs": filesystem_of(tier_root if os.path.isdir(tier_root) else os.path.dirname(tier_root)),
                "tier_size_gib": tier_size_gib,
            }
            record.update(memory_mb())
            record.update(tier_fields())
            if summary:
                record.update(summary)
            if scenario == "offbranch":
                off_timed_out = "offbranch" not in pending["done"]
                off = summarise(frames_of("offbranch"), edited_name) if not off_timed_out else None
                record["offbranch_nodes"] = len(offbranch_names)
                record["offbranch"] = {"timed_out": off_timed_out, "latency_ms": ms_since(t0_ns, pending["done"].get("offbranch"))}
                if off:
                    off["offbranch_executed"] = sum(1 for n in offbranch_names if off["nodes"].get(n, {}).get("executed"))
                    record["offbranch"].update(off)
                    if summary:
                        record["frame"] = min(summary["frame"], off["frame"])
                check(not off_timed_out, "edit %d reached the second viewer" % (i - warmup))
            with open(result_path, "a") as f:
                f.write(json.dumps(record) + "\n")
            recorded += 1
            check(not timed_out, "edit %d reached the viewer" % (i - warmup))
            if summary:
                expected_mip = summary["mip"] == 0 if zoom == "100" else summary["mip"] > 0
                check(expected_mip, "edit %d rendered at the zoom's mip (%d)" % (i - warmup, summary["mip"]))
        gt.report("NOTE %s %s zoom=%s panel=%s autoproxy=%s policy=%s ram=%s: %d edits recorded in %s" % (
            scenario, case, zoom, panel, autoproxy, policy, ram_percent, recorded, result_path))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
