"""Budget harness for the scene viewer on the heavy fixtures.

  Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py [natron-binary]
      Runs inside Natron (GUI, under Xvfb) and is configured by the environment:
        BENCH_FIXTURE    flat (default) or city: which tools/bench/heavy_scene_make.py output USDRead reads
        BENCH_FILE       overrides the fixture path (the fixture kind still selects the budgets)
        BENCH_SCENARIOS  comma-separated actions run in order (default open,orbit)
        BENCH_ORBIT_S    seconds the orbit action runs (default 5)
        BENCH_PAYLOAD_MARK  substring of a layer identifier that marks a payload file (default
                         heavy_city_payloads)
        BENCH_GPU        1 when the display is a real GPU; otherwise frame-dependent budgets are
                         reported as not valid (llvmpipe and GitHub runners do not count)
        NATRON_DRAW_STATE_MECHANISM  filter (default) or session: read by Natron and recorded here
        NATRON_DRAW_STATE_LOAD_NONE  1 or 0: open stages with payloads unloaded (default session on,
                         filter off)
        NATRON_DRAW_STATE_RESTORE  session only: remove (default) drops a restored row's spec, value
                         keeps it and sets model:drawMode=default, which recomposes nothing
        BENCH_SIBLING_ROW  prim path sibling_state acts on (default city /World/Block0/B1, flat
                         /World/c5)
        BENCH_BRANCH     prim path prune_restore, noedit and reveal_cancel act on (default city
                         /World/Block0, flat /World)
        BENCH_REVEAL_BRANCH  prim path reveal acts on (default /World)
        BENCH_REVEAL_BATCH   prims per frame in reveal (default 2000)
        BENCH_REVEAL_MAX_S   seconds reveal runs before it is cancelled (default 120)
        BENCH_BOXES      boxes orbit10k draws (default 10000)
        BENCH_BOX_DEPTH  path depth of the prims that get a box (default flat 2, city 4, or 3 with
                         payloads unloaded)
        BENCH_LOAD_BUILDING  payload prim load_building_* loads (default /World/Block0/B1)
        BENCH_LOAD_BLOCK     prim load_block_*, load_orbit and load_hover load (default /World/Block1)
        BENCH_LOAD_MODE      preopen (default) or direct: how load_orbit and load_hover load
        BENCH_CHAIN_BLOCK    block chain_load loads and the chain's patterns reach (default
                         /World/Block2)
        BENCH_CHAIN_MODE     deactivate (default) or hide: what the chain's matching nodes do
        BENCH_LABEL      free text stored with each result
        BENCH_TIMEOUT_S  per-wait timeout (default 600)
        BENCH_RESULT     JSON-lines file the results are appended to
                         (default $NATRON_GUI_TEST_OUT/heavy_scene_bench.jsonl)

Each scenario runs one action under a 10 ms heartbeat on the main thread and prints one JSON line:
the longest gap between heartbeats, the layers loaded by the action, and the action's own numbers,
each judged against the design's budgets as pass, fail, not_valid (measured, but not a number the
display can vouch for) or not_applicable. Actions are the ACTIONS table; a later milestone adds its
own (twirl, reveal, draw state) by adding an entry.

The Hydra filter actions need a binary with SceneHydraFilter. populate_pruned and populate_full open
the file themselves, so one of them comes first in place of open, once per process. An action
restarts the heartbeat after its setup, so stall_ms covers only what it measures.

The session mechanism takes the same commands: prune authors bounds on a row, restore clears them,
reveal releases direct children per frame, and boxes authors bounds on the rows with a population
mask holding out the rest, which a later restore lifts at the cost of recomposing the whole stage;
run orbit10k last. It has no filter to time the stage scene index, so first_render_ms is the upper
bound on population, and stage_edits is reported rather than judged since it edits by design.

The load actions need payloads unloaded and the city. Each unloads everything first, then loads on
a worker: it walks the prim for its payload assets under the stage lock, opens them with no lock in
preopen mode only, and takes the lock for LoadAndUnload. load_hold_ms is the comparison between the
modes, and it only means something while load_layers_already_open is 0, so compare across processes
or across prims: a page cache warmed by an earlier run favours whichever mode runs second.
load_frames and load_busy_paints count paints that drew or found the lock taken, per phase (walk,
open, wait, hold). load_orbit tumbles throughout; load_hover moves the mouse with no button.

The chain actions run in this order in one process: open_chain, chain_load, rematch_chain,
rematch_nomatch, rematch_match, rematch_offset. open_chain puts ten USDPrune nodes below USDRead,
even ones matching a floor inside the chain block's payloads and odd ones matching nothing, so
nothing matches until chain_load. A load re-cooks no node by itself, so rematch_chain changes the
head's knob, which re-cooks all ten with the load set and is the deferred re-match; run again, it
selects the same prims. rematch_nomatch and rematch_match change only the last node.
rematch_ms is borrow_ms plus match_ms, and borrow_ms includes query_load_ms. swap_ms is the viewer
stage's sync; swap_frame_ms is the frame that drew it. root_resyncs counts resyncs of /, the whole
stage. rematch_offset is the control: a USDRead frame offset is the edit known to resync the whole
stage, so root_resyncs at 0 there means the counter is not working.

Budgets: main-thread stall under 100 ms; first boxes under 2 s on the payload city; no payload file
opened that the action did not request; orbit at or above 30 fps.
"""
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STALL_BUDGET_MS = 100.0
FIRST_BOXES_BUDGET_MS = 2000.0
ORBIT_BUDGET_FPS = 30.0
HEARTBEAT_MS = 10
TIMEOUT_S = float(os.environ.get("BENCH_TIMEOUT_S", "600"))

try:
    from PySide6.QtCore import QEvent, QPoint, QPointF, QTimer, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication
    import NatronEngine

    from guitest import HOST_DISPLAY, OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
    fixture = os.environ.get("BENCH_FIXTURE", "flat")
    require(fixture in ("flat", "city"), "BENCH_FIXTURE %r is one of flat, city" % fixture)
    default_files = {"flat": "heavy_flat.usdc", "city": "heavy_city.usda"}
    path = os.environ.get("BENCH_FILE", os.path.join(ROOT, "build", "bench", default_files[fixture]))
    require(os.path.isfile(path), "the fixture exists: " + path)
    result_path = os.environ.get("BENCH_RESULT", os.path.join(OUT, "heavy_scene_bench.jsonl"))
    payload_mark = os.environ.get("BENCH_PAYLOAD_MARK", "heavy_city_payloads")
    gpu = os.environ.get("BENCH_GPU") == "1" or (HOST_DISPLAY and os.environ.get("BENCH_GPU") != "0")
    label = os.environ.get("BENCH_LABEL", "")
    orbit_s = float(os.environ.get("BENCH_ORBIT_S", "5"))

    try:
        from pxr import Sdf
    except ImportError:
        Sdf = None

    def loaded_layers():
        if Sdf is None:
            return None
        return set(layer.identifier for layer in Sdf.Layer.GetLoadedLayers())

    class Heartbeat(object):
        """A QTimer whose callbacks measure how long the main thread went without servicing it."""

        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(HEARTBEAT_MS)
            self.timer.timeout.connect(self._tick)
            self.last = None
            self.longest = 0.0

        def _tick(self):
            now = time.perf_counter()
            if self.last is not None:
                self.longest = max(self.longest, (now - self.last) * 1000.0)
            self.last = now

        def start(self):
            self.last = time.perf_counter()
            self.longest = 0.0
            self.timer.start()

        def stop(self):
            self._tick()
            self.timer.stop()
            return self.longest

    heartbeat = Heartbeat()

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    usd = create("USDRead", 0, 0)
    viewer = app.getNode("Viewer1") or create("Viewer", 0, 150)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def shown():
        return bool(prop("natronSceneShown", False)) and not prop("natronSceneError")

    def first_frame_drawn():
        painted = prop("natronSceneLastPaintMs")
        return shown() and (painted is not None or prop("natronSceneSynced") is None)

    class Measure(object):
        """What an action reports: its numbers, and the payload files it asked for."""

        def __init__(self):
            self.values = {}
            self.requested_layers = set()

    scene_output = [usd]

    def action_open(m):
        t0 = time.time()
        usd.getParam("filename").setValue(path)
        require(viewer.connectInput(0, scene_output[0]), "connect the Viewer to the scene")
        ok = yield ("until", first_frame_drawn, TIMEOUT_S)
        check(ok, "open: the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        painted = prop("natronSceneLastPaintMs")
        # No viewer yet marks the first box frame, so the first finished frame stands in for it.
        boxes_at = prop("natronSceneFirstBoxesMs")
        end = boxes_at if boxes_at is not None else (painted if painted is not None else time.time() * 1000.0)
        m.values["first_boxes_ms"] = round(end - t0 * 1000.0, 1)
        m.values["first_boxes_signal"] = "natronSceneFirstBoxesMs" if boxes_at is not None else "first_finished_frame"

    def action_edit(m):
        knob = usd.getParam("frameOffset")
        require(knob is not None, "USDRead has a frameOffset parameter")
        t0 = time.time() * 1000.0
        knob.setValue(float(prop("natronSceneSyncRequests", 0) + 1))
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) >= t0 and prop("natronSceneLastPaintMs", 0) >= prop("natronSceneSyncedAtMs", 0)) or prop("natronSceneSyncedAtMs") is None, TIMEOUT_S)
        check(ok, "edit: the edit reached the screen")
        m.values["edit_ms"] = round(prop("natronSceneLastPaintMs", time.time() * 1000.0) - t0, 1)

    def begin_navigation():
        """Maya navigation on a focused view; returns the view and what puts the preference back."""
        navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        maya = 1
        navigation.setValue(maya)
        v = view()
        require(v is not None, "orbit: a scene view is visible")
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        return v, lambda: navigation.setValue(original)

    def send_mouse(v, kind, pos, button, buttons):
        event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier)
        QApplication.sendEvent(v, event)

    def action_orbit(m):
        v, end_navigation = begin_navigation()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            send_mouse(v, kind, pos, button, buttons)

        paints = set()
        deadline = time.time() + orbit_s
        started = time.time()
        step = 0
        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        while time.time() < deadline:
            step += 1
            offset = QPoint(int((step % 40) * 4 - 80), int((step % 20) * 2 - 20))
            mouse(QEvent.MouseMove, centre + offset, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", 0)
            painted = prop("natronSceneLastPaintMs")
            if painted is not None:
                paints.add(painted)
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        elapsed = time.time() - started
        end_navigation()
        m.values["orbit_s"] = round(elapsed, 2)
        m.values["orbit_frames"] = len(paints)
        m.values["orbit_fps"] = round(len(paints) / elapsed, 1) if elapsed > 0 else None
        m.values["boxes_drawn"] = prop("natronSceneBoxCount")

    root_prim = "/World"
    branch = os.environ.get("BENCH_BRANCH", {"city": "/World/Block0", "flat": root_prim}[fixture])
    reveal_branch = os.environ.get("BENCH_REVEAL_BRANCH", root_prim)
    reveal_batch = int(os.environ.get("BENCH_REVEAL_BATCH", "2000"))
    reveal_max_s = float(os.environ.get("BENCH_REVEAL_MAX_S", "120"))
    box_count = int(os.environ.get("BENCH_BOXES", "10000"))
    mechanism = os.environ.get("NATRON_DRAW_STATE_MECHANISM", "filter")
    require(mechanism in ("filter", "session"), "NATRON_DRAW_STATE_MECHANISM is filter or session, not %r" % mechanism)
    session = mechanism == "session"
    load_none = os.environ.get("NATRON_DRAW_STATE_LOAD_NONE", "1" if session else "0") == "1"
    box_depth = int(os.environ.get("BENCH_BOX_DEPTH", {"city": "3" if load_none else "4", "flat": "2"}[fixture]))
    sibling_row = os.environ.get("BENCH_SIBLING_ROW", {"city": "/World/Block0/B1", "flat": "/World/c5"}[fixture])

    def spike_stats():
        text = prop("natronHydraSpikeStats")
        return json.loads(text) if text else {}

    def spike(command):
        """Runs one filter command, up to the end of the frame that rendered it; returns the stats."""
        v = view()
        require(v is not None, "%s: a scene view is visible" % command)
        target = spike_stats().get("done", 0) + 1
        v.setProperty("natronHydraSpikeCommand", command)
        # A cancel with nothing to cancel changes nothing, so no frame ever reports it done.
        ok = yield ("until", lambda: spike_stats().get("done", 0) >= target, 10 if command == "cancel" else TIMEOUT_S)
        if command == "cancel":
            return spike_stats()
        check(ok, "%s: rendered" % command)
        return spike_stats()

    def clear_pruned(*paths):
        """Restores whatever hides paths; a prefix above them has to go too."""
        for prefix in list(spike_stats().get("pruned", [])):
            if any(p == prefix or p.startswith(prefix + "/") or prefix.startswith(p + "/") for p in paths):
                yield from spike("restore " + prefix)

    def prepare_pruned(path):
        if path not in spike_stats().get("pruned", []):
            yield from clear_pruned(path)
            yield from spike("prune " + path)

    def populate(m):
        yield from action_open(m)
        yield ("until", lambda: session or spike_stats().get("filterBound"), 10)
        stats = spike_stats()
        check(session or stats.get("filterBound"), "populate: the filter was appended to this viewer's engine (%r; renderer error %r)" % (stats.get("renderInstanceId"), prop("natronSceneRendererError")))
        m.values["render_instance_id"] = stats.get("renderInstanceId")
        m.values["first_render_ms"] = round(stats.get("firstRenderMs", -1), 1)
        m.values["stage_scene_index_population_ms"] = None if session else round(stats.get("populateMs", -1), 1)
        m.values["prims_added_upstream"] = stats.get("inputAdded")
        m.values["rows_without_extent"] = stats.get("rowsWithoutExtent")

    def action_populate_pruned(m):
        # The first render must already find the prefix, and no scene view exists to send it to.
        os.environ["NATRON_HYDRA_SPIKE_PRUNE"] = root_prim
        try:
            yield from populate(m)
        finally:
            del os.environ["NATRON_HYDRA_SPIKE_PRUNE"]

    def action_populate_full(m):
        yield from populate(m)

    def action_prune_restore(m):
        yield from clear_pruned(branch)
        # Counted ahead of the heartbeat: the filter gets this for free, and session has to walk.
        counted = (yield from spike("count " + branch)) if session else None
        heartbeat.start()
        pruned = yield from spike("prune " + branch)
        restored = yield from spike("restore " + branch)
        m.values["branch"] = branch
        m.values["branch_prims"] = counted["counted"] if session else restored.get("addedSent")
        for name, stats in (("prune", pruned), ("restore", restored)):
            m.values[name + "_apply_ms"] = round(stats["applyMs"], 2)
            m.values[name + "_frame_ms"] = round(stats["frameMs"], 2)
            m.values[name + "_total_ms"] = round(stats["totalMs"], 2)

    def action_reveal(m):
        yield from prepare_pruned(reveal_branch)
        heartbeat.start()
        yield from spike("reveal %s %d" % (reveal_branch, reveal_batch))
        finished = yield ("until", lambda: not spike_stats().get("revealActive", True), reveal_max_s)
        stats = spike_stats()
        if not finished:
            yield from spike("cancel")
        m.values["reveal_branch"] = reveal_branch
        m.values["reveal_batch"] = reveal_batch
        m.values["reveal_complete"] = bool(finished)
        m.values["reveal_prims"] = stats["revealPrims"]
        m.values["reveal_frames"] = stats["revealFrames"]
        m.values["reveal_ms"] = round(stats["revealMs"], 1)
        m.values["reveal_fps"] = round(stats["revealFrames"] * 1000.0 / stats["revealMs"], 1) if stats["revealMs"] > 0 else None
        m.values["reveal_worst_frame_ms"] = round(stats["revealWorstFrameMs"], 1)

    def action_reveal_cancel(m):
        yield from prepare_pruned(branch)
        heartbeat.start()
        started = yield from spike("reveal %s 50" % branch)
        ok = yield ("until", lambda: spike_stats().get("revealFrames", 0) >= 3 or not spike_stats().get("revealActive", True), TIMEOUT_S)
        check(ok, "reveal_cancel: the reveal advanced")
        active = spike_stats().get("revealActive")
        cancelled = yield from spike("cancel")
        settled = yield from spike("cancel")
        m.values["cancel_branch"] = branch
        m.values["cancel_was_active"] = bool(active)
        m.values["cancel_total_ms"] = round(cancelled["totalMs"], 2)
        m.values["cancel_prims_before"] = cancelled["revealPrims"]
        m.values["cancel_stopped"] = not settled["revealActive"] and settled["revealPrims"] == cancelled["revealPrims"]
        m.values["reveal_first_frame_ms"] = round(started["frameMs"], 2)

    def action_orbit10k(m):
        # Only boxes are left for the frame rate to pay for: the filter prunes everything, and
        # session's boxes command masks out everything but its rows.
        if session:
            yield from clear_pruned(root_prim)
        else:
            yield from prepare_pruned(root_prim)
        yield from spike("boxes %d %d" % (box_depth, box_count))
        ok = yield ("until", lambda: not spike_stats().get("boxesBuilding", True) and spike_stats().get("boxesDrawn", 0) == spike_stats().get("boxesReady", -1), TIMEOUT_S)
        check(ok, "orbit10k: the boxes were built and drawn")
        stats = spike_stats()
        m.values["boxes_requested"] = box_count
        m.values["boxes_build_ms"] = round(stats["boxesBuildMs"], 1)
        heartbeat.start()
        yield from action_orbit(m)

    def action_boxes_shot(m):
        """orbit10k's setup without the orbit, then a screenshot to look at."""
        if session:
            yield from clear_pruned(root_prim)
        else:
            yield from prepare_pruned(root_prim)
        yield from spike("boxes %d %d" % (box_depth, box_count))
        ok = yield ("until", lambda: not spike_stats().get("boxesBuilding", True) and spike_stats().get("boxesDrawn", 0) > 0, TIMEOUT_S)
        check(ok, "boxes_shot: boxes were drawn")
        yield ("sleep", 500)
        m.values["boxes_drawn"] = spike_stats().get("boxesDrawn")
        m.values["screenshot"] = shot("boxes_%s_%s.png" % (mechanism, fixture))

    def action_sibling_state(m):
        yield from clear_pruned(sibling_row)
        before = spike_stats()
        heartbeat.start()
        for pass_name in ("first", "second"):
            pruned = yield from spike("prune " + sibling_row)
            restored = yield from spike("restore " + sibling_row)
            for name, stats in (("set", pruned), ("clear", restored)):
                m.values["sibling_%s_%s_apply_ms" % (pass_name, name)] = round(stats["applyMs"], 2)
                m.values["sibling_%s_%s_frame_ms" % (pass_name, name)] = round(stats["frameMs"], 2)
                m.values["sibling_%s_%s_total_ms" % (pass_name, name)] = round(stats["totalMs"], 2)
        after = spike_stats()
        m.values["sibling_row"] = sibling_row
        m.values["restore_mode"] = after.get("restoreMode")
        m.values["sibling_resynced_paths"] = after["resyncedPaths"] - before["resyncedPaths"]
        m.values["sibling_info_only_paths"] = after["infoOnlyPaths"] - before["infoOnlyPaths"]

    def action_noedit(m):
        def edits():
            stats = spike_stats()
            return stats["stageNotices"], stats["stageEdits"]

        yield from clear_pruned(branch)
        heartbeat.start()
        before = edits()
        yield from spike("prune " + branch)
        yield from spike("restore " + branch)
        yield from spike("prune " + branch)
        yield from spike("reveal %s 50" % branch)
        yield from spike("cancel")
        yield from spike("restore " + branch)
        yield ("sleep", 200)
        after = edits()
        m.values["stage_notices"] = after[0] - before[0]
        m.values["stage_edit_count"] = after[1] - before[1]
        m.values["stage_edits"] = m.values["stage_notices"] + m.values["stage_edit_count"]

    load_building = os.environ.get("BENCH_LOAD_BUILDING", "/World/Block0/B1")
    load_block = os.environ.get("BENCH_LOAD_BLOCK", "/World/Block1")
    load_mode = os.environ.get("BENCH_LOAD_MODE", "preopen")
    require(load_mode in ("preopen", "direct"), "BENCH_LOAD_MODE is preopen or direct, not %r" % load_mode)

    def prepare_load():
        require(fixture == "city" and load_none, "load actions need the city with NATRON_DRAW_STATE_LOAD_NONE=1")
        return (yield from spike("unload_all"))

    def request_load(prim, mode):
        """Starts a load; returns the loadSerial its first frame will publish."""
        v = view()
        require(v is not None, "load: a scene view is visible")
        target = spike_stats().get("loadSerial", 0) + 1
        v.setProperty("natronHydraSpikeCommand", "load %s %s" % (prim, mode))
        return target

    def record_load(m, before, prim, mode):
        stats = spike_stats()
        m.values["load_path"] = prim
        m.values["load_mode"] = mode
        m.values["load_assets"] = len(stats["loadAssets"])
        m.values["load_layers_already_open"] = stats["loadLayersAlreadyOpen"]
        m.values["load_walk_lock_ms"] = round(stats["loadWalkMs"], 2)
        m.values["load_open_ms"] = round(stats["loadOpenMs"], 2)
        m.values["load_wait_ms"] = round(stats["loadWaitMs"], 2)
        m.values["load_hold_ms"] = round(stats["loadHoldMs"], 2)
        m.values["load_first_frame_ms"] = round(stats["loadFrameMs"], 2)
        m.values["load_no_new_frame_ms"] = round(stats["loadFrozenMs"], 2)
        m.values["load_frames"] = stats["loadFrames"]
        m.values["load_busy_paints"] = stats["loadBusyPaints"]
        m.values["load_hold_camera_moves"] = stats["loadHoldCameraMoves"]
        m.values["load_resynced_paths"] = stats["resyncedPaths"] - before["resyncedPaths"]
        m.values["load_root_resyncs"] = stats["rootResyncs"] - before["rootResyncs"]
        names = set(os.path.basename(a) for a in stats["loadAssets"])
        m.requested_layers.update(i for i in (loaded_layers() or ()) if os.path.basename(i) in names)

    def load_action(prim, mode):
        def action(m):
            before = yield from prepare_load()
            heartbeat.start()
            target = request_load(prim, mode)
            ok = yield ("until", lambda: spike_stats().get("loadSerial", 0) >= target, TIMEOUT_S)
            check(ok, "load %s %s: its first frame was drawn" % (prim, mode))
            record_load(m, before, prim, mode)
        return action

    def load_while_moving(m, drag):
        before = yield from prepare_load()
        v, end_navigation = begin_navigation()
        centre = v.rect().center()
        buttons = Qt.LeftButton if drag else Qt.NoButton
        heartbeat.start()
        target = request_load(load_block, load_mode)
        deadline = time.time() + TIMEOUT_S
        if drag:
            send_mouse(v, QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        step = 0
        while spike_stats().get("loadSerial", 0) < target and time.time() < deadline:
            step += 1
            offset = QPoint(int((step % 40) * 4 - 80), int((step % 20) * 2 - 20))
            send_mouse(v, QEvent.MouseMove, centre + offset, Qt.NoButton, buttons)
            yield ("sleep", 0)
        if drag:
            send_mouse(v, QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        end_navigation()
        check(spike_stats().get("loadSerial", 0) >= target, "load while moving: its first frame was drawn")
        m.values["mouse_moves_sent"] = step
        record_load(m, before, load_block, load_mode)

    def action_load_orbit(m):
        yield from load_while_moving(m, True)

    def action_load_hover(m):
        yield from load_while_moving(m, False)

    chain_length = 10
    chain_block = os.environ.get("BENCH_CHAIN_BLOCK", "/World/Block2")
    chain_mode = os.environ.get("BENCH_CHAIN_MODE", "deactivate")
    require(chain_mode in ("deactivate", "hide"), "BENCH_CHAIN_MODE is deactivate or hide, not %r" % chain_mode)
    chain = []
    # A pattern used before could come back from the node's cache, with nothing re-matched.
    pattern_serial = [0]

    def next_pattern_serial():
        pattern_serial[0] += 1
        return pattern_serial[0]

    def chain_pattern(i):
        if i % 2 == 0:
            return "%s//Floor%d" % (chain_block, i)
        return "%s//NoMatch%d*" % (chain_block, i)

    def action_open_chain(m):
        require(fixture == "city" and load_none, "open_chain needs the city with NATRON_DRAW_STATE_LOAD_NONE=1")
        previous = usd
        for i in range(chain_length):
            node = app.createNode("USDPrune")
            require(node is not None, "create USDPrune %d" % i)
            require(node.connectInput(0, previous), "connect USDPrune %d" % i)
            node.getParam("primPattern").setValue(chain_pattern(i))
            node.getParam("mode").setValue(1 if chain_mode == "hide" else 0)
            chain.append(node)
            previous = node
        scene_output[0] = previous
        heartbeat.start()
        yield from action_open(m)

    def measure_edit(m, knob, value):
        # Stats publish only from a paint, so the baseline has to come from one.
        before = yield from spike("stats %d" % next_pattern_serial())
        heartbeat.start()
        t0 = int(time.time() * 1000.0)

        def on_screen():
            # Forces the paint that publishes stats, whether or not the sync edited anything.
            view().update()
            return prop("natronSceneSyncedAtMs", 0) >= t0 and prop("natronSceneLastPaintMs", 0) > prop("natronSceneSyncedAtMs", 0)

        knob.setValue(value)
        ok = yield ("until", on_screen, TIMEOUT_S)
        check(ok, "the edit reached the screen")
        after = spike_stats()

        def change(name):
            return after[name] - before[name]

        m.values["rematch_calls"] = change("matchCalls")
        m.values["rematch_matches"] = change("matchCount")
        m.values["rematch_ms"] = round(change("borrowMs") + change("matchMs"), 2)
        m.values["borrow_ms"] = round(change("borrowMs"), 2)
        m.values["query_load_ms"] = round(change("queryLoadMs"), 2)
        m.values["match_ms"] = round(change("matchMs"), 2)
        m.values["author_ms"] = round(change("authorMs"), 2)
        m.values["swap_edits"] = change("swapEdits")
        m.values["swap_ms"] = round(change("swapMs"), 2)
        m.values["swap_frame_ms"] = round(after["swapFrameMs"], 2) if change("swapFrames") > 0 else None
        m.values["resynced_paths"] = change("resyncedPaths")
        m.values["info_only_paths"] = change("infoOnlyPaths")
        m.values["root_resyncs"] = change("rootResyncs")
        m.values["edit_to_screen_ms"] = round(prop("natronSceneLastPaintMs", 0) - t0, 1)

    def pattern_knob(node):
        require(chain, "run open_chain first")
        return node.getParam("primPattern")

    def action_rematch_chain(m):
        head = pattern_knob(chain[0])
        yield from measure_edit(m, head, "%s /World/Unused%d" % (chain_pattern(0), next_pattern_serial()))

    def action_rematch_nomatch(m):
        last = pattern_knob(chain[-1])
        yield from measure_edit(m, last, "%s//NoMatchAgain%d*" % (chain_block, next_pattern_serial()))

    def action_rematch_match(m):
        last = pattern_knob(chain[-1])
        yield from measure_edit(m, last, "%s//Floor%d" % (chain_block, chain_length - 1))

    def action_rematch_offset(m):
        require(chain, "run open_chain first")
        knob = usd.getParam("frameOffset")
        require(knob is not None, "USDRead has a frameOffset parameter")
        m.values["control"] = True
        yield from measure_edit(m, knob, float(next_pattern_serial()))

    ACTIONS = {
        "open": action_open,
        "edit": action_edit,
        "orbit": action_orbit,
        "populate_pruned": action_populate_pruned,
        "populate_full": action_populate_full,
        "prune_restore": action_prune_restore,
        "reveal": action_reveal,
        "reveal_cancel": action_reveal_cancel,
        "orbit10k": action_orbit10k,
        "boxes_shot": action_boxes_shot,
        "sibling_state": action_sibling_state,
        "noedit": action_noedit,
        "load_building_preopen": load_action(load_building, "preopen"),
        "load_building_direct": load_action(load_building, "direct"),
        "load_block_preopen": load_action(load_block, "preopen"),
        "load_block_direct": load_action(load_block, "direct"),
        "load_orbit": action_load_orbit,
        "load_hover": action_load_hover,
        "open_chain": action_open_chain,
        "chain_load": load_action(chain_block, load_mode),
        "rematch_chain": action_rematch_chain,
        "rematch_nomatch": action_rematch_nomatch,
        "rematch_match": action_rematch_match,
        "rematch_offset": action_rematch_offset,
    }

    def verdict(value, limit, below_is_good, valid=True, applicable=True):
        if not applicable:
            status = "not_applicable"
        elif value is None:
            status = "not_valid"
        elif not valid:
            status = "not_valid"
        else:
            status = "pass" if ((value <= limit) if below_is_good else (value >= limit)) else "fail"
        return {"value": value, "limit": limit, "status": status}

    def judge(action, m, stall_ms, new_layers):
        budgets = {"stall_ms": verdict(round(stall_ms, 1), STALL_BUDGET_MS, True)}
        if "first_boxes_ms" in m.values:
            budgets["first_boxes_ms"] = verdict(m.values["first_boxes_ms"], FIRST_BOXES_BUDGET_MS, True,
                                                valid=gpu, applicable=(fixture == "city"))
        if new_layers is None:
            budgets["unrequested_payloads"] = {"value": None, "limit": 0, "status": "not_valid"}
        else:
            unrequested = sorted(i for i in new_layers if payload_mark in i and i not in m.requested_layers)
            budgets["unrequested_payloads"] = verdict(len(unrequested), 0, True)
            budgets["unrequested_payloads"]["sample"] = [os.path.basename(i) for i in unrequested[:5]]
        if "stage_edits" in m.values:
            budgets["stage_edits"] = verdict(m.values["stage_edits"], 0, True, applicable=not session)
        if "swap_ms" in m.values:
            budgets["swap_ms"] = verdict(m.values["swap_ms"], STALL_BUDGET_MS, True, applicable=not m.values.get("control"))
        if "orbit_fps" in m.values:
            budgets["orbit_fps"] = verdict(m.values["orbit_fps"], ORBIT_BUDGET_FPS, False, valid=gpu)
        return budgets

    def steps():
        names = [s.strip() for s in os.environ.get("BENCH_SCENARIOS", "open,orbit").split(",") if s.strip()]
        for name in names:
            require(name in ACTIONS, "scenario %r is a known action (%s)" % (name, ", ".join(sorted(ACTIONS))))
        for name in names:
            m = Measure()
            before = loaded_layers()
            heartbeat.start()
            yield from ACTIONS[name](m)
            yield ("sleep", 200)
            stall_ms = heartbeat.stop()
            after = loaded_layers()
            new_layers = None if before is None or after is None else after - before
            budgets = judge(name, m, stall_ms, new_layers)
            record = {
                "label": label,
                "scenario": name,
                "fixture": fixture,
                "mechanism": mechanism,
                "load_none": load_none,
                "file": os.path.basename(path),
                "gpu": bool(gpu),
                "frame_rate": "valid" if gpu else "not valid on llvmpipe",
                "layers_before": None if before is None else len(before),
                "layers_after": None if after is None else len(after),
                "layers_loaded": None if new_layers is None else len(new_layers),
                "budgets": budgets,
                "result": "fail" if any(b["status"] == "fail" for b in budgets.values()) else "pass",
                "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            record.update(m.values)
            line = json.dumps(record)
            print(line)
            with open(result_path, "a") as f:
                f.write(line + "\n")
            report("RESULT " + line)
            check(record["result"] == "pass", "%s: within budget" % name)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
