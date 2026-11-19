"""Budget harness for the scene viewer on the heavy fixtures.

  Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py [natron-binary]
      Runs inside Natron (GUI, under Xvfb, or on a real display through heavy_scene_gpu_host.sh) and is
      configured by the environment:
        BENCH_FIXTURE    flat (default), city, or scene: which file USDRead reads. flat and city are
                         tools/bench/heavy_scene_make.py outputs; scene is any file named by BENCH_FILE,
                         whose rows tools/bench/heavy_scene_rows.py picks
        BENCH_FILE       overrides the fixture path (the fixture kind still selects the budgets)
        BENCH_SCENARIOS  comma-separated actions run in order (default open,orbit)
        BENCH_ORBIT_S    seconds the orbit, orbit10k and orbit_full actions run (default 5)
        BENCH_LOAD_PATHS comma-separated prims the load action asks to load (default the fixture's
                         load_building row, or load_block with BENCH_LOAD_DESCENDANTS=1); on the city,
                         /World/BlockN with BENCH_LOAD_DESCENDANTS=1 is a larger load
        BENCH_LOAD_DESCENDANTS  1 to load the payloads below those prims too
        BENCH_TWIRL_PATH the payload row the twirl action opens in the Scene Graph panel (default the
                         fixture's twirl_row row)
        BENCH_LOAD_ALL   1 when the run loads everything (the load_all scenario) before its draw state
                         scenarios: rows are then picked from the loaded scene
        BENCH_ROOT       prim path populate_pruned, orbit10k and sibling_shot's control act on (default
                         the fixture's or scene's root)
        BENCH_BRANCH     prim path prune_restore, noedit and reveal_cancel act on (default city
                         /World/Block0, flat /World)
        BENCH_SIBLING_ROW  prim path sibling_state, sibling_shot and draw_budget act on (default city
                         /World/Block0/B1, flat /World/c5)
        BENCH_WIDE_ROW   the wide row draw_budget acts on (default the picked widest_row, or /World on
                         flat and city)
        BENCH_BUDGET_REPEATS  untraced set and clear pairs draw_budget makes per row (default 2)
        BENCH_REVEAL_BRANCH  prim path reveal acts on (default /World)
        BENCH_REVEAL_BATCH   prims per frame in reveal (default 2000)
        BENCH_REVEAL_MAX_S   seconds a reveal (reveal, reveal_wide, reveal_load_all, the workflow's block)
                         runs before it is cancelled (default 500 us per prim of the scene, the picked
                         count or the viewer's, at least 120 s and at most half the scenario's
                         wall-clock limit: 900 s under the default BENCH_OPEN_LIMIT_S, 150 s under
                         BENCH_SCENARIO_LIMIT_S); records store the limit used as <scenario>_max_s
        BENCH_BOXES      boxes orbit10k asks for (default 10000)
        BENCH_BOX_DEPTH  path depth of the prims orbit10k draws as boxes (default the depth whose count
                         of geometry-holding prims is nearest BENCH_BOXES)
        BENCH_PAYLOAD_MARK  substring of a layer identifier that marks a payload file on flat and city
                         (default heavy_city_payloads)
        BENCH_ROWS_DIR   where a scene's picked rows are cached (default $NATRON_GUI_TEST_OUT/rows)
        BENCH_PICK_LIMIT_S  seconds picking a scene's rows may take (default 900)
        BENCH_GPU        1 when the display is a real GPU; otherwise frame-dependent budgets are
                         reported as not valid (llvmpipe and GitHub runners do not count, whatever
                         this says)
        BENCH_SWITCH_NODES  USDEditTransforms off USDRead that node_switch selects in turn, with the
                         Scene Graph panel following the selection (default 4); it then selects the
                         first again, whose stage the panel keeps
        BENCH_PATTERN    the prim pattern workflow's pattern node matches (default the branch's
                         descendants, or the sibling row's on flat)
        BENCH_LABEL      free text stored with each result
        BENCH_TIMEOUT_S  per-wait timeout (default 600)
        BENCH_HOLD_FILE  when set, populate_full and populate_pruned report "BEGIN populate" and wait up to
                         BENCH_HOLD_S (default 120) for this file to exist before opening the scene, so a stack
                         sampler can attach first; BENCH_PTRACER_ANY=1 lets any process of the user attach
        BENCH_PID_FILE   file this Natron process writes its pid to as the script starts, for a stack sampler
        BENCH_STACK_SAMPLED  1 when a stack sampler is attached, which stops the process for each sample;
                         stored in each record as stack_sampled, as its timings are then not the viewer's own
        BENCH_BISECT     the bisect variant the run's environment was set up for, stored in each record as
                         bisect (null without one)
        BENCH_SHADER_CACHE  warm or cold: whether the GL shader cache the run uses was filled before it,
                         stored in each record as shader_cache (null when unset)
        BENCH_WARMUP     1 when the run only fills the shader cache; stored in each record as warmup
        BENCH_SETTLE_S   seconds the load counts have to hold still before loads count as over (default 2)
        BENCH_SCENARIO_LIMIT_S  wall-clock limit of a scenario, from outside Natron (default 300)
        BENCH_OPEN_LIMIT_S      the same for one that opens or loads the whole file (default 1800)
        BENCH_MEM_FLOOR_GB, BENCH_MEM_LIMIT_GB, BENCH_SWAP_GROWTH_GB  see heavy_scene_watchdog.sh
        BENCH_RESULT     JSON-lines file the results are appended to
                         (default $NATRON_GUI_TEST_OUT/heavy_scene_bench.jsonl)
        NATRON_DRAW_STATE_TIMING  1 makes the viewer publish a timing record per presented draw state
                         change (natronSceneDrawTimings), which time_to_picture_ms is judged on and
                         draw_budget breaks down; without it time_to_picture_ms is not_valid

workflow chains the whole user workflow in one process, each step timed and judged apart (see the
workflow action); it reopens the project at its end, so it has to be the last scenario of its run.

Each scenario runs one action under a 10 ms heartbeat on the main thread and prints one JSON line:
the longest gap between heartbeats, the layers loaded by the action, and the action's own numbers,
each judged against the design's budgets as pass, fail, not_valid (measured, but not a number the
display can vouch for) or not_applicable. Actions are the ACTIONS table.

Every record carries gl_renderer and gl_vendor, and the memory the process has used: peak_rss_mb and
peak_rss_gb (its high-water mark) and mem_available_min_gb (the lowest MemAvailable during the
scenario). result is pass or fail, fail whenever a check or a budget of the scenario failed
(failed_checks lists each as {"kind": "check" or "budget", "name", "reason"}), or one of: not_applicable (the action had nothing to measure in this process, with a reason),
skipped (with a reason; nothing ran, so its peak_rss_mb
is the process's so far, or null when the watchdog wrote it), timed_out (past its wall-clock limit),
out_of_memory (the memory guard ended the process), aborted (the process ended on its own during the
scenario). heavy_scene_watchdog.sh writes those last three from outside Natron, so a hung run leaves
a record. Every record has started_at and ended_at (epoch seconds), and the log a "SCENARIO start" and
"SCENARIO end" line for each, so that stack samples can be lined up with them.

Full + Load keeps loading payloads after the first frame, and queues the payloads a landed payload
reveals only once it lands. Loads count as over when nothing is pending or composing, every posted draw
change is presented, no reveal runs, and the loaded layers, load edits, prims added and posted changes
have not moved for BENCH_SETTLE_S; *_settle fields hold the counts before and after and the wait. After
an open with it load_all waits for that (load_ms, up to the start of the still window), and the payloads
it loads were all requested. The reveal, orbit and draw_budget scenarios first wait for it
(loads_settle_wait_ms; action_started_at is when the action itself began), and reveal_load_all and the
workflow's block and reopen wait for it after their first commit.

With NATRON_DRAW_STATE_TIMING=1 the paced reveals (reveal_wide, reveal_load_all, reveal_cancel_paced and
the workflow's block) add <name>_draw_steps: the viewer's timing record of every frame presented during
the reveal, as percentiles of each part of the critical path and the 20 worst frames by picture_ms.

The draw state scenarios drive SceneViewport's setDrawState, clearDrawState and setDrawRevealBatch, and
wait for natronSceneDrawPresented, which a paint sets only once its image has taken in every change so
far. populate_full and populate_pruned open the file themselves, once per process, in place of open;
load_all then loads everything, with draw states on or off. A record's apply_ms is the render thread's
time sending the notices, total_ms runs from the call to the image that shows it, and frame_ms is the
one less the other; total_ms is polled every 50 ms, so it only bounds the time.

With NATRON_DRAW_STATE_TIMING=1 the viewer times each change itself, from the slot call to the paint
that presents it, and its record adds to the change's timings: picture_ms (call to presented image),
and the parts that sum to it: queue_ms (call to the render thread's apply), apply_ms (the filter's
notices, which reach the render index directly), wait_frame_ms (apply to the frame's render calls),
render_ms (the engine's render calls: Hydra sync, draw item regather, batch rebuild, draw submission),
gpu_tail_ms (the GPU finishing after the last call; the render thread waits for it only in this mode),
handoff_ms (to the main thread taking the frame) and present_ms (to the paint that shows it).
draw_budget sets BBox on sibling_row and on the wide row and clears it, BENCH_BUDGET_REPEATS times
untraced, then once traced: a traced change runs TraceCollector and HdPerfLog from its apply to its
frame's end and adds stages_ms (wall time on the render thread per Hydra and Storm scope), stage_counts
and counter_deltas (HdPerfLog). Each change is one entry of draw_changes; traced ones are never judged
and their stall is not counted, as tracing slows them. Without the variable time_to_picture_ms comes
from total_ms and is not_valid.

sibling_shot compares screen grabs around one state change on sibling_row, branch and root; the root, which the viewer frames, is the control: if it changes no pixels the state does
nothing, and if it does, a row that did not is out of frame or ineffective (shot_reading says which,
from its coverage). It needs geometry on screen, so it skips with payloads unloaded. orbit10k and
boxes_shot need a viewer that publishes natronSceneDrawBoxes, and skip without one; boxes_shot's
first_boxes_ms runs from the change to the paint that first drew a box (natronSceneFirstBoxesMs).

orbit10k sets the Preferences' sceneStandInMode to Off for its duration and restores the previous value
afterwards, also on failure, so orbit_fps measures Storm drawing the boxes rather than the stand-in
(orbit_full covers that); stand_in_mode records the mode used and paint_count the natronScenePaintCount
delta over the orbit, for diagnosis. orbit and orbit10k count as frames the Storm frames new to the
screen over the orbit (natronScenePresentedFrames), each made for some camera of the orbit.

orbit_full clears every draw state, waits for the full scene on screen, and orbits it continuously for
BENCH_ORBIT_S under the Preferences' scene stand-in settings. orbit_fps counts every main-thread paint
(natronScenePaintCount), stand-in or full frame; stand_in_shown says whether any paint showed the
stand-in boxes, last_full_frame_ms is the cost of the last full frame before the orbit
(natronSceneLastFullFrameMs), and refill_ms runs from the release to the paint that shows a full frame
of the view again. It needs a viewer that publishes natronScenePaintCount, and skips without one.

open records first_pass_ms, from the open to the first Storm frame of the scene on screen
(natronScenePresentedFrames), and complete_ms, to the paint that finds the fill complete
(natronSceneFillComplete: no prim or texture still to come in, and the frame converged); first_frame_ms
is the finished frame, which a viewer that fills in bounded passes paints only once the fill is complete.

orbit_fill (for Island) hides the root, then clears that, so the whole scene comes into Hydra again in
bounded passes; after BENCH_FILL_ORBIT_DELAY_S it orbits for BENCH_ORBIT_S mid-fill, then waits for the
fill to complete. It records first_pass_ms and complete_ms from the clear, worst_pass_ms, the longest gap
between Storm frames new to the screen while the camera rested and prims were still coming, fill_frames,
progress_monotonic (natronSceneFillProgress never fell while the camera rested), and the stall. It needs a
viewer that publishes natronSceneFillProgress, and skips without one.

Budgets: main-thread stall under 100 ms; time from a draw state change to the image that shows it under
100 ms (time_to_picture_ms, worst untraced change of prune_restore, sibling_state and draw_budget, with
draw_budget's rows also judged apart); a paced reveal of the root with Load All (reveal_load_all,
which also waits for the loads to land) and of the wide row (reveal_wide) completes with no gap over
100 ms between presented frames (natronScenePresentedFrames; not_valid when fewer than 2 were presented,
as is the workflow's full_load_block_worst_frame_ms); a fill of the wide row boxed again as soon as its first batch is
admitted admits at most one more batch and leaves the fill complete (reveal_cancel_paced; the batch is
1024 prims or the pacer's, and a cancel that lands after the fill ended fails the record); first frame under 2 s on the payload city, the only scene the design
gives it; no payload
file opened that the action did not request; orbit at or above 30 fps; no stage edit or notice from a
draw state; the draw filter on this viewer's engine; pixels changed on the control row. On the city the
time-to-picture budgets (time_to_picture_*, full_asset_picture_ms) and the reveal frame budgets
(reveal_*_worst_frame_ms, full_load_block_worst_frame_ms) are not_applicable, and on Island those
and orbit_fps, each with the user's decision in its reason.

The twirl and load scenarios tumble the camera until the load is committed and its draw changes are
presented, let go, then wait for the converged frame (twirl_settle_ms, load_settle_ms); the twirl's stall
is over that tumble and settle.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))
sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "tools", "bench"))

STALL_BUDGET_MS = 100.0
PICTURE_BUDGET_MS = 100.0
FIRST_FRAME_BUDGET_MS = 2000.0
ORBIT_BUDGET_FPS = 30.0
HEARTBEAT_MS = 10
# Pace of the loops that drive the mouse or watch frames. Back-to-back polls would leave the main thread
# no idle time, so stack samples and stalls would be the bench's; a paint that falls between two polls
# with another is merged into it, which lengthens a measured gap by at most this.
POLL_MS = 5
REVEAL_MIN_S = 120.0
REVEAL_S_PER_PRIM = 500e-6
# Full + Load queues the payloads a landed payload reveals only after it lands, so an empty queue for
# one poll is not the end of the loads: the counts have to hold still for a window.
SETTLE_S = float(os.environ.get("BENCH_SETTLE_S", "2"))
SETTLE_POLL_MS = 250
# Reading natronSceneDrawTimings converts every kept record, so it is read at this pace, well inside
# the time the viewer takes to replace its 256 kept records.
DRAW_STEPS_POLL_S = 0.5
DRAW_STEPS_KEPT = 20
SWITCH_NODES = int(os.environ.get("BENCH_SWITCH_NODES", "4"))
TIMEOUT_S = float(os.environ.get("BENCH_TIMEOUT_S", "600"))
EDIT_POST_WAIT_S = 10.0
SCENARIO_LIMIT_S = int(os.environ.get("BENCH_SCENARIO_LIMIT_S", "300"))
OPEN_LIMIT_S = int(os.environ.get("BENCH_OPEN_LIMIT_S", "1800"))
PICK_LIMIT_S = int(os.environ.get("BENCH_PICK_LIMIT_S", "900"))
HOLD_FILE = os.environ.get("BENCH_HOLD_FILE", "")
HOLD_S = float(os.environ.get("BENCH_HOLD_S", "120"))
OPEN_SCENARIOS = ("open", "populate_full", "populate_pruned", "load_all")
DRAW_SCENARIOS = ("populate_full", "populate_pruned", "prune_restore", "reveal", "reveal_cancel", "orbit10k", "boxes_shot",
                  "sibling_state", "sibling_shot", "noedit", "draw_budget", "reveal_load_all", "reveal_wide", "reveal_cancel_paced",
                  "orbit_fill")
# A wide row's restore puts the whole scene back into Hydra, several times over.
LONG_SCENARIOS = OPEN_SCENARIOS + ("draw_budget", "reveal_load_all", "reveal_wide", "workflow", "orbit_fill")
FILL_ORBIT_DELAY_S = float(os.environ.get("BENCH_FILL_ORBIT_DELAY_S", "1"))
PICTURE_SCENARIOS = ("prune_restore", "sibling_state", "draw_budget")
# Payloads still landing from a Full + Load open would be measured along with these.
SETTLED_SCENARIOS = ("reveal", "reveal_cancel", "reveal_wide", "reveal_cancel_paced", "orbit", "orbit10k", "orbit_full", "draw_budget")
DRAW_TIMING = os.environ.get("NATRON_DRAW_STATE_TIMING") == "1"
SOFTWARE_RENDERERS = ("llvmpipe", "softpipe", "swrast")
ROW_RECORD_KEYS = ("root", "branch", "branch_gprims", "widest_row", "widest_children", "sibling_row", "sibling_kind",
                   "reveal_branch", "box_depth", "box_depth_rows", "prims", "gprims", "instances", "prototypes",
                   "payload_rows", "load_building", "load_block", "twirl_row", "pick_s")
ROW_ENV = {"root": "BENCH_ROOT", "branch": "BENCH_BRANCH", "sibling_row": "BENCH_SIBLING_ROW",
           "reveal_branch": "BENCH_REVEAL_BRANCH", "twirl_row": "BENCH_TWIRL_PATH", "widest_row": "BENCH_WIDE_ROW"}

try:
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMetaObject, QModelIndex, QPersistentModelIndex, QPoint, QPointF, QRect, Q_ARG, QTimer, Qt
    from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent, QOffscreenSurface, QOpenGLContext
    from PySide6.QtWidgets import QApplication, QComboBox, QMessageBox, QTreeView, QWidget
    import NatronEngine

    from heavy_scene_frame import frame_coverage
    from guitest import HOST_DISPLAY, OUT, check as guitest_check, finish, plugin_problem, report, require, run, shot, widgets_of

    # The failed checks of the scenario running, which fail its record whatever its budgets say.
    scenario_checks = {"failed": None}

    def check(ok, label):
        if not ok and scenario_checks["failed"] is not None:
            scenario_checks["failed"].append({"kind": "check", "name": label, "reason": "did not hold"})
        return guitest_check(ok, label)

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3
    STATE_UNLOADED, STATE_LOADED, STATE_FAILED = 1, 3, 4
    LOADING_LABEL = "Loading..."

    if os.environ.get("BENCH_PTRACER_ANY") == "1":
        # Under kernel.yama.ptrace_scope 1 only an ancestor may attach, and a stack sampler is not one.
        import ctypes
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
        if _libc.prctl(0x59616d61, ctypes.c_ulong(-1).value, 0, 0, 0) != 0:
            report("NOTE prctl(PR_SET_PTRACER_ANY) failed: %s" % os.strerror(ctypes.get_errno()))

    if os.environ.get("BENCH_PID_FILE"):
        with open(os.environ["BENCH_PID_FILE"], "w") as pid_file:
            pid_file.write("%d\n" % os.getpid())

    ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
    fixture = os.environ.get("BENCH_FIXTURE", "flat")
    require(fixture in ("flat", "city", "scene"), "BENCH_FIXTURE %r is one of flat, city, scene" % fixture)
    default_files = {"flat": "heavy_flat.usdc", "city": "heavy_city.usda", "scene": ""}
    path = os.environ.get("BENCH_FILE", os.path.join(ROOT, "build", "bench", default_files[fixture]))
    require(os.path.isfile(path), "the fixture exists: " + path)
    path = os.path.abspath(path)
    result_path = os.environ.get("BENCH_RESULT", os.path.join(OUT, "heavy_scene_bench.jsonl"))
    payload_mark = os.environ.get("BENCH_PAYLOAD_MARK", "heavy_city_payloads")

    def gl_strings():
        """Asks a context of our own, on the same display and driver the viewer gets."""
        surface = QOffscreenSurface()
        surface.create()
        context = QOpenGLContext()
        if not context.create() or not context.makeCurrent(surface):
            return None, None
        gl_vendor, gl_renderer = 0x1F00, 0x1F01
        functions = context.functions()
        strings = functions.glGetString(gl_vendor), functions.glGetString(gl_renderer)
        context.doneCurrent()
        return strings

    gl_vendor_name, gl_renderer_name = gl_strings()
    software_gl = any(name in (gl_renderer_name or "") for name in SOFTWARE_RENDERERS)
    gpu = (os.environ.get("BENCH_GPU") == "1" or (HOST_DISPLAY and os.environ.get("BENCH_GPU") != "0")) and not software_gl
    label = os.environ.get("BENCH_LABEL", "")
    orbit_s = float(os.environ.get("BENCH_ORBIT_S", "5"))
    load_descendants = os.environ.get("BENCH_LOAD_DESCENDANTS") == "1"
    load_state = "all" if os.environ.get("BENCH_LOAD_ALL") == "1" else "none"
    box_count = int(os.environ.get("BENCH_BOXES", "10000"))
    reveal_batch = int(os.environ.get("BENCH_REVEAL_BATCH", "2000"))
    reveal_max_override_s = float(os.environ["BENCH_REVEAL_MAX_S"]) if os.environ.get("BENCH_REVEAL_MAX_S") else None
    budget_repeats = max(1, int(os.environ.get("BENCH_BUDGET_REPEATS", "2")))
    file_stem = os.path.splitext(os.path.basename(path))[0]

    def picker_command():
        beside_natron = os.path.join(os.path.dirname(os.path.realpath("/proc/self/exe")), "natron-python")
        # The bundle's interpreter first: it composes with the USD build the viewer will use.
        if os.access(beside_natron, os.X_OK):
            return [beside_natron, "-I"]
        return ["python3"]

    def picked_rows():
        rows_dir = os.environ.get("BENCH_ROWS_DIR", os.path.join(OUT, "rows"))
        box_depth_override = os.environ.get("BENCH_BOX_DEPTH")
        cached = os.path.join(rows_dir, "%s-load-%s-boxes-%d-depth-%s.json" % (file_stem, load_state, box_count, box_depth_override or "nearest"))
        if not os.path.isfile(cached) or os.path.getmtime(cached) < os.path.getmtime(path):
            picker = os.path.join(ROOT, "tools", "bench", "heavy_scene_rows.py")
            depth_args = ["--box-depth", box_depth_override] if box_depth_override else []
            done = subprocess.run(picker_command() + [picker, path, "--load", load_state, "--boxes", str(box_count), "--out", cached] + depth_args,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=PICK_LIMIT_S)
            require(done.returncode == 0, "the scene's rows were picked", done.stderr.decode(errors="replace")[-2000:])
        with open(cached) as f:
            return json.load(f)

    def fixture_rows():
        if fixture == "flat":
            return {"root": "/World", "branch": "/World", "sibling_row": "/World/c5", "widest_row": "/World", "reveal_branch": "/World",
                    "box_depth": 2}
        return {"root": "/World", "load_building": "/World/Block0/B0", "load_block": "/World/Block1", "twirl_row": "/World/Block0/B1",
                "branch": "/World/Block0", "sibling_row": "/World/Block0/B1", "widest_row": "/World", "reveal_branch": "/World",
                "box_depth": 4 if load_state == "all" else 3}

    rows = picked_rows() if fixture == "scene" else fixture_rows()
    rows["source"] = "picked" if fixture == "scene" else "fixture"
    for row_name, variable in ROW_ENV.items():
        if variable in os.environ:
            rows[row_name] = os.environ[variable]
            rows["source"] += "+env"
    if "BENCH_BOX_DEPTH" in os.environ:
        rows["box_depth"] = int(os.environ["BENCH_BOX_DEPTH"])
        rows["source"] += "+env"
    requested_scenarios = [s.strip() for s in os.environ.get("BENCH_SCENARIOS", "open,orbit").split(",") if s.strip()]
    if fixture != "scene" and "orbit10k" in requested_scenarios:
        # The fixtures' other rows are fixed, but the prims above the box depth are too many to list by hand.
        picked = picked_rows()
        for key in ("box_depth", "box_depth_rows", "box_parents"):
            rows[key] = picked.get(key)
        rows["source"] += "+picked boxes"
    default_load = rows.get("load_block" if load_descendants else "load_building") or ""
    load_paths = [p.strip() for p in os.environ.get("BENCH_LOAD_PATHS", default_load).split(",") if p.strip()]

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
            self.started_at = None

        def _tick(self):
            now = time.perf_counter()
            if self.last is not None:
                self.longest = max(self.longest, (now - self.last) * 1000.0)
            self.last = now

        def start(self):
            self.started_at = time.time()
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
    # BENCH_VIEWER_GAIN (f-stops) and BENCH_VIEWER_GAMMA grade the scene view, so that frame times
    # with and without the viewer's gain and gamma can be compared.
    for knob_name, enable_name, env_name in (("gain", "enableGain", "BENCH_VIEWER_GAIN"),
                                             ("gamma", "enableGamma", "BENCH_VIEWER_GAMMA")):
        if os.environ.get(env_name):
            viewer.getParam(enable_name).setValue(True)
            viewer.getParam(knob_name).setValue(float(os.environ[env_name]))

    try:
        import shiboken6
    except ImportError:
        shiboken6 = None

    # Scanning QApplication.allWidgets() wraps every widget of the window, which on a loaded scene costs
    # the main thread more than the poll it serves; the widgets are looked up once and kept until Qt
    # destroys them or the project is reset.
    widget_cache = {}

    def widget_alive(w):
        if shiboken6 is not None and not shiboken6.isValid(w):
            return False
        try:
            w.objectName()
            return True
        except RuntimeError:
            return False

    def forget_widgets():
        widget_cache.clear()

    def cached_widget(key, find, usable=lambda w: True):
        """find()'s widget, reused while it lives and usable(widget) holds; find() runs again only when not."""
        w = widget_cache.get(key)
        if w is not None:
            if widget_alive(w) and usable(w):
                return w
            if not widget_alive(w):
                widget_cache.pop(key, None)
        found = find()
        if found is None:
            return None
        if widget_cache.get(key) is not found:
            widget_cache[key] = found

            def destroyed(*_args, key=key, found=found):
                # The wrapper of a widget made in C++ is not always invalidated with it, and a later
                # call through it would crash rather than raise.
                if widget_cache.get(key) is found:
                    widget_cache.pop(key, None)

            found.destroyed.connect(destroyed)
        return found if usable(found) else None

    def first_visible_view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def view():
        return cached_widget("view", first_visible_view, lambda w: w.isVisible())

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    def shown():
        return bool(prop("natronSceneShown", False)) and not prop("natronSceneError")

    def report_view_state(label):
        """One NOTE line of what the viewport and the screen look like, for a wait that expired."""
        parts = []
        v = view()
        parts.append("viewport=%s" % ("none" if v is None else "visible=%s size=%dx%d dpr=%s" % (
            v.isVisible(), v.width(), v.height(), v.devicePixelRatioF())))
        if v is not None:
            names = sorted(bytes(n).decode("utf-8", "replace") for n in v.dynamicPropertyNames())
            props = ["natronSceneShown", "natronSceneError", "natronSceneSyncedAtMs", "natronSceneLastPaintMs"]
            props += [n for n in names if n.startswith("natronScene") and n not in props]
            parts.append("props " + " ".join("%s=%r" % (n, v.property(n)) for n in props))
        for w in QApplication.topLevelWidgets():
            if w.metaObject().className() == "Gui":
                g = w.geometry()
                parts.append("main window %dx%d+%d+%d visible=%s" % (g.width(), g.height(), g.x(), g.y(), w.isVisible()))
                break
        screen = QGuiApplication.primaryScreen()
        if screen is not None:
            g = screen.geometry()
            parts.append("screen %dx%d+%d+%d dpr=%s dpi=%s" % (
                g.width(), g.height(), g.x(), g.y(), screen.devicePixelRatio(), screen.logicalDotsPerInch()))
        report("NOTE %s: %s" % (label, "; ".join(parts)))

    class Measure(object):
        """What an action reports: its numbers."""

        def __init__(self):
            self.values = {}
            self.requested_files = []
            self.requested_layers = set()
            self.all_payloads_requested = False
            self.failed_checks = []
            # Set by an action that finds nothing to measure in this process, with the reason.
            self.not_applicable = None
            # Times to picture of the changes judged: {"ms", "source", "row"}.
            self.pictures = []
            # The stall of a part of the action whose heartbeat was restarted after it.
            self.stall_floor_ms = 0.0
            # The workflow's steps, each {"applicable", "ms", "stall_ms", ...}, in the order they ran.
            self.steps = {}

    # The scenario running, for the timings of the changes it makes.
    current = {"name": None, "m": None, "record": None}

    def open_file(m, connected=False):
        t0 = time.time()
        t0_ms = int(t0 * 1000.0)
        marks = [time.perf_counter()]
        # Full or Full + Load; the default boxes the top-level prims, which would leave later box edits nothing to change.
        usd.getParam("initialDraw").setValue(4 if load_state == "all" else 3)
        if load_state == "all":
            m.all_payloads_requested = True
            project_state["loaded_at_open"] = True
        marks.append(time.perf_counter())
        usd.getParam("filename").setValue(path)
        marks.append(time.perf_counter())
        if not connected:
            require(viewer.connectInput(0, usd), "connect the Viewer to USDRead")
        marks.append(time.perf_counter())

        presented_before = int(prop("natronScenePresentedFrames", 0) or 0)
        first_pass = {"ms": None}

        def scene_frame_drawn():
            # A view with nothing synced yet also finishes frames, so the scene has to have reached it.
            synced_at = prop("natronSceneSyncedAtMs", 0)
            if (first_pass["ms"] is None and shown() and synced_at >= t0_ms
                    and int(prop("natronScenePresentedFrames", 0) or 0) > presented_before):
                first_pass["ms"] = time.time() * 1000.0
            return shown() and synced_at >= t0_ms and prop("natronSceneLastPaintMs", 0) >= synced_at

        ok = yield ("until", scene_frame_drawn, TIMEOUT_S)
        if not ok:
            report_view_state("open: the scene view never showed the scene")
        check(ok, "open: the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        painted = prop("natronSceneLastPaintMs")
        m.values["first_frame_ms"] = round((painted if painted is not None else time.time() * 1000.0) - t0 * 1000.0, 1)
        m.values["first_pass_ms"] = round(first_pass["ms"] - t0 * 1000.0, 1) if first_pass["ms"] is not None else None
        m.values["complete_ms"] = m.values["first_frame_ms"] if ok and prop("natronSceneFillComplete") else None
        m.values["open_substeps_ms"] = {
            "initial_draw": round((marks[1] - marks[0]) * 1000.0, 1),
            "filename": round((marks[2] - marks[1]) * 1000.0, 1),
            "connect": None if connected else round((marks[3] - marks[2]) * 1000.0, 1),
            "to_first_frame": round((painted if painted is not None else time.time() * 1000.0) - (t0 * 1000.0 + (marks[3] - marks[0]) * 1000.0), 1),
        }
        return t0_ms

    def action_open(m):
        yield from open_file(m)

    def action_edit(m):
        knob = usd.getParam("frameOffset")
        require(knob is not None, "USDRead has a frameOffset parameter")
        require(view() is not None and shown(), "edit: a scene view is visible and showing the scene")
        t0 = time.time() * 1000.0
        knob.setValue(float(prop("natronSceneSyncRequests", 0) + 1))
        ok = yield ("until", lambda: (prop("natronSceneSyncedAtMs", 0) >= t0 and prop("natronSceneLastPaintMs", 0) >= prop("natronSceneSyncedAtMs", 0)), TIMEOUT_S)
        check(ok, "edit: the edit reached the screen")
        m.values["edit_ms"] = round(prop("natronSceneLastPaintMs", time.time() * 1000.0) - t0, 1)

    def action_orbit(m):
        navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        require(view() is not None and shown(), "orbit: a scene view is visible and showing the scene")
        original = navigation.getValue()
        maya = 1
        navigation.setValue(maya)
        v = view()
        require(v is not None, "orbit: a scene view is visible")
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier)
            QApplication.sendEvent(v, event)

        # natronSceneLastPaintMs only advances on the finished frame of the view, which a moving
        # camera never has on the render thread: each frame shown was made for an earlier camera.
        # natronScenePresentedFrames counts every Storm frame new to the screen.
        presented_before = int(prop("natronScenePresentedFrames", 0))
        deadline = time.time() + orbit_s
        started = time.time()
        step = 0
        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        while time.time() < deadline:
            step += 1
            offset = QPoint(int((step % 40) * 4 - 80), int((step % 20) * 2 - 20))
            mouse(QEvent.MouseMove, centre + offset, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", POLL_MS)
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        elapsed = time.time() - started
        frames = int(prop("natronScenePresentedFrames", 0)) - presented_before
        navigation.setValue(original)
        m.values["orbit_s"] = round(elapsed, 2)
        m.values["orbit_frames"] = frames
        m.values["orbit_fps"] = round(frames / elapsed, 1) if elapsed > 0 else None

    def tumbling_until(done):
        """Tumbles the view until done() holds, or the wait times out, then lets go; returns (moves, longest
        gap in ms, the epoch ms it let go at)."""
        navigation = NatronEngine.natron.getSettings().getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        navigation.setValue(1)
        v = view()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier)
            QApplication.sendEvent(v, event)

        # The view has to stay usable through the whole load, not only for its first moments.
        deadline = time.time() + TIMEOUT_S
        moves = 0
        longest_gap = 0.0
        last_move = time.perf_counter()
        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        while not done() and time.time() < deadline:
            moves += 1
            offset = QPoint(int((moves % 40) * 4 - 80), int((moves % 20) * 2 - 20))
            mouse(QEvent.MouseMove, centre + offset, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", POLL_MS)
            now = time.perf_counter()
            longest_gap = max(longest_gap, (now - last_move) * 1000.0)
            last_move = now
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        released_ms = time.time() * 1000.0
        navigation.setValue(original)
        return moves, round(longest_gap, 1), released_ms

    def load_watch(v):
        """A predicate that holds once a load asked for from now on has been committed and its draw changes
        presented. natronSceneLastPaintMs is left out: it only advances on a converged frame, which a camera
        still tumbling never gets."""
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        return lambda: (not v.property("natronSceneLoadsPending")
                        and int(v.property("natronSceneLoadEdits") or 0) > edits_before
                        and int(v.property("natronSceneDrawPresented") or 0) >= int(draw_stats().get("posted", 0))
                        and not v.property("natronSceneComposing"))

    def commit_watch(v):
        """A predicate that holds once a load asked for from now on has been committed and drawn."""
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        t0 = time.time() * 1000.0
        return lambda: (not v.property("natronSceneLoadsPending")
                        and int(v.property("natronSceneLoadEdits") or 0) > edits_before
                        and (v.property("natronSceneLastPaintMs") or 0) >= t0
                        and not v.property("natronSceneComposing"))

    def settle_after_tumble(released_ms):
        """Waits for the converged frame after the mouse let go at released_ms; returns its ms after the release."""
        ok = yield ("until", lambda: (prop("natronSceneLastPaintMs", 0) or 0) >= released_ms, TIMEOUT_S)
        return round(prop("natronSceneLastPaintMs", released_ms) - released_ms, 1) if ok else None

    def requested_files(paths):
        """The file-name prefixes of the city's payloads below paths: b<block>_<building>.usdc."""
        prefixes = []
        for prim in paths:
            found = re.match(r"^/World/Block(\d+)(?:/B(\d+))?$", prim)
            if found:
                prefixes.append("b%s_%s.usdc" % found.groups() if found.group(2) else "b%s_" % found.group(1))
        return prefixes

    def requested_layers(paths, descendants):
        """A scene's payload files that loading paths may open: theirs, their ancestors', and with
        descendants those below them."""
        found = set()
        for row, assets in rows.get("payload_row_assets", {}).items():
            for prim in paths:
                if row == prim or prim.startswith(row + "/") or (descendants and row.startswith(prim + "/")):
                    found.update(assets)
        return found

    def request(m, paths, descendants):
        m.requested_files = requested_files(paths)
        m.requested_layers = requested_layers(paths, descendants)

    def load_results(m, v, name, edits_before):
        m.values[name + "_hold_ms"] = v.property("natronSceneLoadHoldMs")
        m.values[name + "_edits"] = int(v.property("natronSceneLoadEdits") or 0) - edits_before
        m.values[name + "_failures"] = v.property("natronSceneLoadFailures")

    def action_load(m):
        require(view() is not None and shown(), "load: a scene view is visible and showing the scene")
        v = view()
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        request(m, load_paths, load_descendants)
        loaded = load_watch(v)
        committed = commit_watch(v)
        t0 = time.time() * 1000.0
        require(QMetaObject.invokeMethod(v, "requestLoad", Q_ARG("QStringList", load_paths), Q_ARG(bool, load_descendants)),
                "load: the scene view takes load requests")
        moves, longest_gap, released_ms = yield from tumbling_until(loaded)
        m.values["load_settle_ms"] = yield from settle_after_tumble(released_ms)
        check(committed(), "load: the loaded prims reached the screen (failures %r)" % (v.property("natronSceneLoadFailures"),))
        m.values["load_ms"] = round(time.time() * 1000.0 - t0, 1)
        m.values["load_paths"] = len(load_paths)
        m.values["load_mouse_moves"] = moves
        m.values["load_move_gap_ms"] = longest_gap
        load_results(m, v, "load", edits_before)

    held_widgets = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in held_widgets):
            held_widgets.append(w)
        return w

    def first_panel():
        panels = widgets_of("SceneGraphPanel")
        return panels[0] if panels else None

    def panel():
        return hold(cached_widget("panel", first_panel))

    def panel_tree():
        p = panel()
        return p.findChild(QTreeView, "sceneGraphTree") if p is not None else None

    def tree():
        return hold(cached_widget("tree", panel_tree))

    def rows_under(parent=QModelIndex()):
        model = hold(tree().model())
        while model.canFetchMore(parent):
            model.fetchMore(parent)
        return [model.index(r, 0, parent) for r in range(model.rowCount(parent))]

    def find_row(prim):
        """The index of prim's row, reading the tree down to it; an invalid index when absent."""
        found = QModelIndex()
        names = prim.strip("/").split("/")
        for depth in range(1, len(names) + 1):
            prefix = "/" + "/".join(names[:depth])
            parent = found
            found = QModelIndex()
            for index in rows_under(parent):
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if not found.isValid():
                break
        return found

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def show_panel():
        """Puts the Scene Graph panel beside the viewer, where a user twirls with the view in sight."""
        require(panel() is not None, "twirl: the Scene Graph panel exists")
        if panel().isVisible():
            return
        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "twirl: the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "twirl: split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "twirl: move the Scene Graph panel beside the viewer")

    def action_twirl(m):
        require(view() is not None and shown(), "twirl: a scene view is visible and showing the scene")
        show_panel()
        ok = yield ("until", lambda: panel().isVisible() and rows_under(), TIMEOUT_S)
        require(ok, "twirl: the panel lists the scene")
        row = rows["twirl_row"]
        names = row.strip("/").split("/")
        for depth in range(1, len(names)):
            ancestor = find_row("/" + "/".join(names[:depth]))
            require(ancestor.isValid(), "twirl: the panel lists the row's ancestor %s" % ("/".join(names[:depth]),))
            tree().expand(ancestor)
        target = find_row(row)
        require(target.isValid(), "twirl: the panel lists " + row)
        require(target.data(STATE_ROLE) == STATE_UNLOADED, "twirl: %s is unloaded (state %r)" % (row, target.data(STATE_ROLE)))
        persistent = QPersistentModelIndex(target)

        def state():
            return persistent.data(STATE_ROLE)

        v = view()
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        request(m, [row], False)
        loaded = load_watch(v)
        committed = commit_watch(v)
        # Docking the panel and reading down to the row are not the twirl the stall budget is about.
        heartbeat.start()
        t0 = time.time() * 1000.0
        tree().expand(target)
        moves, longest_gap, released_ms = yield from tumbling_until(
            lambda: state() == STATE_FAILED or (state() == STATE_LOADED and loaded()))
        m.values["twirl_tumble_ms"] = round(released_ms - t0, 1)
        m.values["twirl_settle_ms"] = yield from settle_after_tumble(released_ms)
        m.values["twirl_ms"] = round(time.time() * 1000.0 - t0, 1)
        check(state() == STATE_LOADED and committed(), "twirl: %s loaded and reached the screen (state %r, failures %r)"
              % (row, state(), v.property("natronSceneLoadFailures")))
        below = rows_under(persistent.sibling(persistent.row(), persistent.column()))
        names_below = [str(i.data()) for i in below]
        check(LOADING_LABEL not in names_below, "twirl: the loading row has gone (got %r)" % (names_below[:5],))
        m.values["twirl_rows"] = len(below)
        m.values["twirl_mouse_moves"] = moves
        m.values["twirl_move_gap_ms"] = longest_gap
        load_results(m, v, "load", edits_before)

    def load_snapshot():
        """(counts, idle): the counts that grow while loads land, and whether every queue the viewer
        publishes is empty; (None, False) without a view."""
        v = view()
        if v is None:
            return None, False
        stats = draw_stats()
        posted = int(stats.get("posted", 0))
        counts = {
            "layers": len(Sdf.Layer.GetLoadedLayers()) if Sdf is not None else None,
            "load_edits": int(v.property("natronSceneLoadEdits") or 0),
            "stage_load_edits": int(stats.get("stageLoadEdits", 0)),
            "prims_added": int(stats.get("inputAdded", 0)),
            "posted": posted,
        }
        idle = (not v.property("natronSceneLoadsPending") and not v.property("natronSceneComposing")
                and not stats.get("revealing") and int(v.property("natronSceneDrawPresented") or 0) >= posted)
        return counts, idle

    def wait_loads_settled(label):
        """Waits until no load is pending or composing, no draw change is unpresented, and the loaded
        layers, load edits, prims added and posted changes have held still for SETTLE_S; returns
        {"ms": time to the start of that window, "wait_ms": the whole wait, "stable_at": its
        perf_counter, the counts before and at the end, the polls}."""
        t0 = time.perf_counter()
        deadline = time.time() + TIMEOUT_S
        first, last, stable_since, polls, ok = None, None, None, 0, False
        while True:
            counts, idle = load_snapshot()
            polls += 1
            now = time.perf_counter()
            if first is None:
                first = counts
            if not idle or counts is None:
                stable_since = None
            elif stable_since is None or counts != last:
                stable_since = now
            last = counts
            if stable_since is not None and now - stable_since >= SETTLE_S:
                ok = True
                break
            if time.time() >= deadline:
                break
            yield ("sleep", SETTLE_POLL_MS)
        check(ok, "%s: the scene's loads are over (pending %r, composing %r, counts %r)"
              % (label, prop("natronSceneLoadsPending"), prop("natronSceneComposing"), last))
        stable_at = stable_since if ok else time.perf_counter()
        return {"ms": round((stable_at - t0) * 1000.0, 1), "wait_ms": round((time.perf_counter() - t0) * 1000.0, 1),
                "stable_at": stable_at, "settled": ok, "window_s": SETTLE_S, "polls": polls, "before": first, "after": last}

    def settle_fields(info):
        return {k: v for k, v in info.items() if k != "stable_at"}

    def action_load_all(m):
        require(view() is not None and shown(), "load_all: a scene view is visible and showing the scene")
        v = view()
        if project_state["loaded_at_open"]:
            # Full + Load keeps loading payloads after the first frame; what is left is what this waits for.
            edits_before = int(v.property("natronSceneLoadEdits") or 0)
            m.all_payloads_requested = True
            m.values["loaded_at_open"] = True
            settle = yield from wait_loads_settled("load_all")
            m.values["load_ms"] = settle["ms"]
            m.values["load_settle"] = settle_fields(settle)
            m.values["prims_added_upstream"] = draw_stats().get("inputAdded")
            m.values["prims_hidden_rules"] = draw_stats().get("ruleCount")
            load_results(m, v, "load", edits_before)
            return
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        m.all_payloads_requested = True
        committed = commit_watch(v)
        t0 = time.time() * 1000.0
        require(QMetaObject.invokeMethod(v, "requestLoad", Q_ARG("QStringList", [rows["root"]]), Q_ARG(bool, True)),
                "load_all: the scene view takes load requests")
        started = time.perf_counter()
        ok = yield ("until", committed, TIMEOUT_S)
        check(ok, "load_all: the loaded prims reached the screen (failures %r)" % (v.property("natronSceneLoadFailures"),))
        m.values["load_first_commit_ms"] = round(time.time() * 1000.0 - t0, 1)
        settle = yield from wait_loads_settled("load_all")
        m.values["load_ms"] = round((settle["stable_at"] - started) * 1000.0, 1)
        m.values["load_settle"] = settle_fields(settle)
        m.values["prims_added_upstream"] = draw_stats().get("inputAdded")
        m.values["prims_hidden_rules"] = draw_stats().get("ruleCount")
        load_results(m, v, "load", edits_before)

    def draw_stats():
        stats = prop("natronSceneDrawStats")
        return dict(stats) if stats else {}

    def draw_call(v, method, *args):
        require(QMetaObject.invokeMethod(v, method, *args), "the scene view has %s" % method)

    def timing_record(v, posted):
        """The viewer's timing record of the frame that took in change number posted, or None."""
        for record in reversed(list(v.property("natronSceneDrawTimings") or [])):
            record = dict(record)
            if int(record.get("firstPosted", 0)) <= posted <= int(record.get("lastPosted", 0)):
                return record
        return None

    def ms(value):
        return None if value is None else round(float(value), 2)

    def critical_path(record):
        """The parts of a record's time to picture, each from where the one before ends; they sum to picture_ms."""
        queue = float(record["queueMs"])
        applied = float(record["applyMs"])
        render_start = float(record["renderStartMs"])
        render = float(record["renderMs"])
        gpu_tail = float(record["gpuTailMs"]) if record.get("gpuTailMs") is not None else 0.0
        frame_end = render_start + render + gpu_tail
        presented = float(record["presentedMs"])
        frame_done = record.get("frameDoneMs")
        parts = {
            "picture_ms": ms(presented),
            "queue_ms": ms(queue),
            "apply_ms": ms(applied),
            "wait_frame_ms": ms(render_start - queue - applied),
            "render_ms": ms(render),
            "gpu_tail_ms": ms(record.get("gpuTailMs")),
            "handoff_ms": ms(float(frame_done) - frame_end) if frame_done is not None else None,
            "present_ms": ms(presented - (float(frame_done) if frame_done is not None else frame_end)),
            "render_passes": record.get("renderPasses"),
            "rendered_on_paint": bool(record.get("onPaint")),
            "coalesced_changes": int(record["lastPosted"]) - int(record["firstPosted"]) + 1,
            "hidden_sent": record.get("hiddenSent"),
            "shown_sent": record.get("shownSent"),
            "traced": bool(record.get("traced")),
        }
        if record.get("traced"):
            parts["trace_pending"] = bool(record.get("tracePending"))
            parts["trace_thread_found"] = bool(record.get("traceThreadFound"))
            parts["trace_collect_ms"] = ms(record.get("traceCollectMs"))
            parts["stages_ms"] = {k: ms(v) for k, v in dict(record.get("stagesMs") or {}).items()}
            parts["stage_counts"] = {k: int(v) for k, v in dict(record.get("stageCounts") or {}).items()}
            parts["counter_deltas"] = {k: float(v) for k, v in dict(record.get("counterDeltas") or {}).items()}
            parts["counter_values"] = {k: float(v) for k, v in dict(record.get("counterValues") or {}).items()}
        elif record.get("traceSkipped"):
            parts["trace_skipped"] = True
        return parts

    def presented_after_call(v, label, method, *args, judged=True, row_tag=None, may_be_noop=False):
        """Calls method and waits for the image that shows it; returns its timings, empty if nothing changed."""
        posted_before = int(draw_stats().get("posted", 0))
        started = time.perf_counter()
        draw_call(v, method, *args)
        posted = int(draw_stats().get("posted", 0))
        current["record"] = None
        current["post_wait_ms"] = None
        wait_ms = 0.0
        if posted == posted_before and may_be_noop:
            return {}
        if posted == posted_before:
            # An edit queued behind another, or on a large scene, runs on a worker and posts later.
            ok = yield ("until", lambda: int(draw_stats().get("posted", 0)) > posted_before, EDIT_POST_WAIT_S)
            wait_ms = (time.perf_counter() - started) * 1000.0
            posted = int(draw_stats().get("posted", 0))
            if not ok or posted == posted_before:
                current["post_wait_ms"] = round(wait_ms, 1)
                return {}
        ok = yield ("until", lambda: int(v.property("natronSceneDrawPresented") or 0) >= posted, TIMEOUT_S)
        check(ok, "%s: reached the screen" % label)
        total = (time.perf_counter() - started) * 1000.0
        apply_ms = float(draw_stats().get("lastApplyMs", 0.0))
        timings = {"apply_ms": round(apply_ms, 2), "frame_ms": round(max(total - apply_ms, 0.0), 2), "total_ms": round(total, 2)}
        if wait_ms > 0.0:
            timings["post_wait_ms"] = round(wait_ms, 1)
        record = timing_record(v, posted) if DRAW_TIMING else None
        current["record"] = record
        if record is not None:
            timings.update(critical_path(record))
            timings["picture_source"] = "viewer"
        else:
            timings["picture_ms"] = timings["total_ms"]
            timings["picture_source"] = "polled" if not DRAW_TIMING else "polled: no viewer record"
        m = current["m"]
        if judged and m is not None and current["name"] in PICTURE_SCENARIOS and not (record or {}).get("traced"):
            m.pictures.append({"ms": timings["picture_ms"], "source": timings["picture_source"], "row": row_tag})
        return timings

    def set_draw_state(row, state, row_tag=None):
        v = view()
        require(v is not None, "%s %s: a scene view is visible" % (state, row))
        timings = yield from presented_after_call(v, "%s %s" % (state, row), "setDrawState", Q_ARG("QString", row), Q_ARG("QString", state),
                                                  row_tag=row_tag)
        require(bool(timings), "%s %s: the draw state changed something" % (state, row),
                "no change posted within %.0f s; waited %s ms" % (EDIT_POST_WAIT_S, current.get("post_wait_ms")))
        return timings

    def clear_draw_state(row, row_tag=None):
        v = view()
        timings = yield from presented_after_call(v, "clear " + row, "clearDrawState", Q_ARG("QString", row), row_tag=row_tag)
        require(bool(timings), "clear %s: the draw state changed something" % row,
                "no change posted within %.0f s; waited %s ms" % (EDIT_POST_WAIT_S, current.get("post_wait_ms")))
        return timings

    def clear_draw_states():
        """Every scenario's first step, so that scenarios in one process do not depend on their order."""
        v = view()
        require(v is not None, "a scene view is visible")
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, 0))
        yield from presented_after_call(v, "clear all", "clearDrawStates", judged=False, may_be_noop=True)

    def populate(m, hidden_root):
        require(viewer.connectInput(0, usd), "connect the Viewer to USDRead")
        # The first render must already find the state, and a scene view exists only with the scene connected.
        ok = yield ("until", lambda: bool(widgets_of("SceneViewport")), 30)
        require(ok, "populate: connecting USDRead made a scene view")
        if hidden_root:
            target = widgets_of("SceneViewport")[0]
            draw_call(target, "setDrawState", Q_ARG("QString", rows["root"]), Q_ARG("QString", "bbox"))
        if HOLD_FILE:
            report("BEGIN populate")
            yield ("until", lambda: os.path.exists(HOLD_FILE), HOLD_S)
            if os.path.exists(HOLD_FILE):
                os.remove(HOLD_FILE)
            else:
                report("NOTE wait expired: %s was not created within %.0f s; populating without a sampler" % (HOLD_FILE, HOLD_S))
        t0_ms = yield from open_file(m, connected=True)
        stats = draw_stats()
        added_at = stats.get("lastInputAddedEpochMs")
        m.values["prims_added_upstream"] = stats.get("inputAdded")
        m.values["prims_added_ms"] = round(added_at - t0_ms, 1) if added_at else None
        m.values["rule_count"] = stats.get("ruleCount")
        m.values["hidden_sent"] = stats.get("hiddenSent")
        if DRAW_TIMING:
            posted = int(stats.get("posted", 0))
            if posted:
                yield ("until", lambda: int(v_presented()) >= posted, TIMEOUT_S)
                record = timing_record(view(), posted)
                m.values["first_change_timing"] = critical_path(record) if record is not None else None
        m.values["sync_to_first_paint_ms"] = (
            round(prop("natronSceneLastPaintMs", 0) - prop("natronSceneSyncedAtMs", 0), 1) if prop("natronSceneSyncedAtMs", 0) else None)

    def v_presented():
        v = view()
        return (v.property("natronSceneDrawPresented") or 0) if v is not None else 0

    def action_populate_pruned(m):
        yield from populate(m, True)

    def action_populate_full(m):
        yield from populate(m, False)

    def action_prune_restore(m):
        yield from clear_draw_states()
        heartbeat.start()
        before = draw_stats()
        pruned = yield from set_draw_state(rows["branch"], "bbox")
        restored = yield from clear_draw_state(rows["branch"])
        after = draw_stats()
        m.values["branch"] = rows["branch"]
        m.values["branch_hidden_sent"] = after.get("hiddenSent", 0) - before.get("hiddenSent", 0)
        m.values["branch_shown_sent"] = after.get("shownSent", 0) - before.get("shownSent", 0)
        for name, timings in (("prune", pruned), ("restore", restored)):
            for key, value in timings.items():
                m.values["%s_%s" % (name, key)] = value

    def reveal_limit_s():
        """How long a reveal may run before it is cancelled: BENCH_REVEAL_MAX_S, or REVEAL_S_PER_PRIM for each
        prim of the scene (the picked count, or the viewer's, which grows as payloads land), at least
        REVEAL_MIN_S and at most half the scenario's wall-clock limit, which has to leave room for the rest."""
        if reveal_max_override_s is not None:
            return reveal_max_override_s
        prims = max(int(rows.get("prims") or 0), int(draw_stats().get("inputAdded", 0)))
        outer = OPEN_LIMIT_S if current["name"] in LONG_SCENARIOS else SCENARIO_LIMIT_S
        return min(max(REVEAL_MIN_S, prims * REVEAL_S_PER_PRIM), max(REVEAL_MIN_S, 0.5 * outer))

    TIMING_KEY_FIELDS = ("firstPosted", "lastPosted", "applies", "queueMs", "applyMs", "renderStartMs", "renderMs", "presentedMs")
    STEP_PERCENTILE_FIELDS = ("picture_ms", "queue_ms", "apply_ms", "wait_frame_ms", "render_ms", "gpu_tail_ms", "handoff_ms", "present_ms")

    class DrawSteps(object):
        """The viewer's timing records of the frames presented from its creation on, one per frame,
        reveal steps included; collects nothing without NATRON_DRAW_STATE_TIMING=1."""

        def __init__(self):
            self.seen = set()
            self.entries = []
            self.read_at = 0.0
            self.possibly_dropped = False
            if DRAW_TIMING:
                self.seen.update(self._key(r) for r in self._records())

        @staticmethod
        def _key(record):
            return tuple(record.get(k) for k in TIMING_KEY_FIELDS)

        @staticmethod
        def _records():
            v = view()
            return [dict(r) for r in (v.property("natronSceneDrawTimings") or [])] if v is not None else []

        def poll(self, force=False):
            if not DRAW_TIMING:
                return
            now = time.perf_counter()
            if not force and now - self.read_at < DRAW_STEPS_POLL_S:
                return
            self.read_at = now
            records = self._records()
            # The viewer keeps its last 256; a full list whose oldest is new may have lost some between reads.
            if len(records) >= 256 and records and self._key(records[0]) not in self.seen:
                self.possibly_dropped = True
            for record in records:
                key = self._key(record)
                if key not in self.seen:
                    self.seen.add(key)
                    self.entries.append(record)

        def summary(self):
            if not DRAW_TIMING:
                return {"unavailable": "NATRON_DRAW_STATE_TIMING is not 1 in this run"}
            self.poll(force=True)
            steps = []
            for record in self.entries:
                try:
                    parts = critical_path(record)
                except (KeyError, TypeError, ValueError):
                    continue
                parts["reveal_step"] = bool(record.get("revealStep"))
                parts["applies"] = record.get("applies")
                steps.append(parts)

            def percentiles(key):
                values = sorted(s[key] for s in steps if s.get(key) is not None)
                if not values:
                    return None

                def at(q):
                    return values[min(len(values) - 1, int(round(q * (len(values) - 1))))]

                return {"p50": at(0.5), "p90": at(0.9), "p99": at(0.99), "max": values[-1], "mean": round(sum(values) / len(values), 2)}

            return {
                "frames": len(steps),
                "reveal_steps": sum(1 for s in steps if s["reveal_step"]),
                "possibly_dropped": self.possibly_dropped,
                "percentiles": {key: percentiles(key) for key in STEP_PERCENTILE_FIELDS},
                "worst": sorted(steps, key=lambda s: s.get("picture_ms") or 0.0, reverse=True)[:DRAW_STEPS_KEPT],
            }

    MIN_TIMED_FRAMES = 2

    def watch_reveal(done, steps=None):
        """Waits up to reveal_limit_s() for done; returns whether it held, the frames presented, the longest gap
        from the start or a frame to the next frame, and the limit it ended with. Frames are counted from
        natronScenePresentedFrames, every frame new to the screen: natronSceneLastPaintMs only advances on a
        converged frame, so a reveal's intermediate frames would not count. A gap is timed between the polls
        that saw the count move, so it is at most POLL_MS long."""
        frames = 0
        worst = 0.0
        presented = int(prop("natronScenePresentedFrames", 0) or 0)
        started = time.time()
        last = time.perf_counter()
        limit_s = reveal_limit_s()
        limit_checked = started
        while not done() and time.time() < started + limit_s:
            yield ("sleep", POLL_MS)
            if steps is not None:
                steps.poll()
            now_presented = int(prop("natronScenePresentedFrames", 0) or 0)
            if now_presented > presented:
                now = time.perf_counter()
                frames += now_presented - presented
                presented = now_presented
                worst = max(worst, (now - last) * 1000.0)
                last = now
            if time.time() - limit_checked >= 1.0:
                limit_checked = time.time()
                limit_s = reveal_limit_s()
        return done(), frames, worst, round(limit_s, 1)

    def paced_reveal(m, name, row, start_state, state, done=None, keep_states=False):
        """Reveals row from start_state to state at the viewer's own pace and records how the frames held up."""
        if not keep_states:
            yield from clear_draw_states()
        yield from set_draw_state(row, start_state)
        v = view()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, -1))
        heartbeat.start()
        before = draw_stats()
        steps = DrawSteps()
        started = time.perf_counter()
        yield from set_draw_state(row, state)
        complete, frames, worst, limit_s = yield from watch_reveal(lambda: not draw_stats().get("revealing") and (done is None or done()),
                                                                   steps)
        elapsed = (time.perf_counter() - started) * 1000.0
        after = draw_stats()
        if not complete:
            yield from set_draw_state(row, start_state)
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, 0))
        m.values[name + "_row"] = row
        m.values[name + "_complete"] = complete
        m.values[name + "_max_s"] = limit_s
        m.values[name + "_prims"] = after.get("shownSent", 0) - before.get("shownSent", 0)
        m.values[name + "_steps"] = after.get("revealSteps", 0) - before.get("revealSteps", 0)
        m.values[name + "_last_batch"] = after.get("revealBatch")
        m.values[name + "_fixed_ms"] = ms(after.get("revealFixedMs"))
        m.values[name + "_per_prim_us"] = after.get("revealPerPrimUs")
        m.values[name + "_frames"] = frames
        m.values[name + "_ms"] = round(elapsed, 1)
        m.values[name + "_worst_frame_ms"] = round(worst, 1)
        m.values[name + "_draw_steps"] = steps.summary()

    def action_reveal_load_all(m):
        v = view()
        require(v is not None and shown(), "reveal_load_all: a scene view is visible and showing the scene")
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        m.all_payloads_requested = True
        committed = commit_watch(v)
        yield from paced_reveal(m, "reveal_load_all", rows["root"], "bbox", "loadAll", committed)
        # The reveal is judged up to the first commit; the payloads that commit reveals keep landing after it.
        settle = yield from wait_loads_settled("reveal_load_all")
        m.values["reveal_load_all_settle"] = settle_fields(settle)
        load_results(m, v, "load", edits_before)

    def action_reveal_wide(m):
        yield from paced_reveal(m, "reveal_wide", rows["widest_row"], "bbox", "full")

    FILL_FIRST_BATCH = 1024
    FILL_CANCEL_SETTLE_S = 10.0

    def fill_state():
        return float(prop("natronSceneFillProgress", 1.0) or 0.0), bool(prop("natronSceneFillComplete", True))

    def action_reveal_cancel_paced(m):
        row = rows["widest_row"]
        yield from clear_draw_states()
        yield from set_draw_state(row, "bbox")
        v = view()
        steps = DrawSteps()
        heartbeat.start()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, -1))
        before = draw_stats()
        # The first admit batch is in the apply that posts the change; the rest of the row waits in the fill's queue.
        yield from set_draw_state(row, "full")
        at_cancel = draw_stats()
        progress_at_cancel, complete_at_cancel = fill_state()
        cancelled = yield from set_draw_state(row, "bbox")
        yield ("sleep", 200)
        settled = draw_stats()
        deadline = time.time() + FILL_CANCEL_SETTLE_S
        while not fill_state()[1] and time.time() < deadline:
            yield ("sleep", POLL_MS)
        progress_after, complete_after = fill_state()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, 0))
        active = not complete_at_cancel
        batch = max(FILL_FIRST_BATCH, int(at_cancel.get("revealBatch") or 0))
        admitted = at_cancel.get("shownSent", 0) - before.get("shownSent", 0)
        check(active, "reveal_cancel_paced: the cancel landed while the fill was under way (%r prims admitted, progress %.3f)"
              % (admitted, progress_at_cancel))
        m.values["cancel_paced_row"] = row
        m.values["cancel_paced_was_active"] = active
        m.values["cancel_paced_batch"] = batch
        m.values["cancel_paced_admitted_before_cancel"] = admitted
        m.values["cancel_paced_progress_at_cancel"] = round(progress_at_cancel, 4)
        m.values["cancel_paced_progress_after"] = round(progress_after, 4)
        m.values["cancel_paced_total_ms"] = cancelled["total_ms"]
        # A batch under way when the change is posted may still finish; nothing after it may be admitted.
        m.values["cancel_paced_overshoot_prims"] = settled.get("shownSent", 0) - at_cancel.get("shownSent", 0)
        m.values["cancel_paced_stopped"] = bool(complete_after)
        m.values["cancel_paced_draw_steps"] = steps.summary()

    def action_reveal(m):
        row = rows["reveal_branch"]
        yield from clear_draw_states()
        yield from set_draw_state(row, "bbox")
        v = view()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, reveal_batch))
        heartbeat.start()
        before = draw_stats()
        started = time.perf_counter()
        yield from set_draw_state(row, "full")
        complete, frames, worst, limit_s = yield from watch_reveal(lambda: not draw_stats().get("revealing"))
        elapsed = (time.perf_counter() - started) * 1000.0
        after = draw_stats()
        if not complete:
            yield from set_draw_state(row, "bbox")
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, 0))
        m.values["reveal_branch"] = row
        m.values["reveal_batch"] = reveal_batch
        m.values["reveal_complete"] = complete
        m.values["reveal_max_s"] = limit_s
        m.values["reveal_prims"] = after.get("shownSent", 0) - before.get("shownSent", 0)
        m.values["reveal_frames"] = frames
        m.values["reveal_ms"] = round(elapsed, 1)
        m.values["reveal_fps"] = round(frames * 1000.0 / elapsed, 1) if elapsed > 0 else None
        m.values["reveal_worst_frame_ms"] = round(worst, 1)

    def action_reveal_cancel(m):
        row = rows["branch"]
        cancel_batch = 50
        yield from clear_draw_states()
        yield from set_draw_state(row, "bbox")
        v = view()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, cancel_batch))
        heartbeat.start()
        before = draw_stats().get("shownSent", 0)
        started = yield from set_draw_state(row, "full")

        def advanced():
            stats = draw_stats()
            return stats.get("shownSent", 0) - before >= 3 * cancel_batch or not stats.get("revealing")

        ok = yield ("until", advanced, TIMEOUT_S)
        check(ok, "reveal_cancel: the reveal advanced")
        active = draw_stats().get("revealing")
        prims_before = draw_stats().get("shownSent", 0) - before
        cancelled = yield from set_draw_state(row, "bbox")
        settled_at = draw_stats().get("shownSent", 0)
        yield ("sleep", 200)
        settled = draw_stats()
        draw_call(v, "setDrawRevealBatch", Q_ARG(int, 0))
        m.values["cancel_branch"] = row
        m.values["cancel_was_active"] = bool(active)
        m.values["cancel_total_ms"] = cancelled["total_ms"]
        m.values["cancel_prims_before"] = prims_before
        m.values["cancel_stopped"] = not settled.get("revealing") and settled.get("shownSent", 0) == settled_at
        m.values["reveal_first_frame_ms"] = started["frame_ms"]

    def boxes_drawn():
        return int(prop("natronSceneDrawBoxes", 0))

    def draw_settled(v):
        """Whether every posted draw-state change is on screen and no reveal is still adding prims."""
        def settled():
            stats = draw_stats()
            return not stats.get("revealing") and int(v.property("natronSceneDrawPresented") or 0) >= int(stats.get("posted", 0))
        return settled

    def action_orbit10k(m):
        # orbit_fps counts natronScenePresentedFrames, which only a Storm frame advances; the
        # stand-in would suppress those while the camera moves, so the stand-in is Off here (orbit_full
        # covers it) and the previous mode is restored on every exit.
        mode = NatronEngine.natron.getSettings().getParam("sceneStandInMode")
        previous = mode.getValue() if mode is not None else None
        off = 0
        m.values["stand_in_mode"] = list(mode.getOptions())[off] if mode is not None else None
        if mode is not None:
            mode.setValue(off)
        try:
            yield from orbit10k_body(m)
        finally:
            if mode is not None and previous is not None:
                mode.setValue(previous)

    def orbit10k_body(m):
        yield from clear_draw_states()
        v = view()
        depth = rows.get("box_depth")
        parents = list(rows.get("box_parents") or [])
        require(bool(parents), "orbit10k: the scene has prims to box at depth %r" % (depth,))
        sent_ms = time.time() * 1000.0
        # Children on every prim above the box depth draws each prim at that depth as one box; BBox on the
        # root would draw a single box.
        timings = yield from presented_after_call(v, "orbit10k: Children above depth %r" % (depth,), "setDrawStateOnRows",
                                                  Q_ARG("QStringList", parents), Q_ARG("QString", "children"), judged=False)
        require(bool(timings), "orbit10k: the draw state changed something")
        # A large change can post more than once; the orbit is only meaningful on the picture it asked for.
        while True:
            ok = yield ("until", draw_settled(v), TIMEOUT_S)
            require(ok, "orbit10k: the draw-state change was presented")
            yield ("sleep", 250)
            if draw_settled(v)():
                break
        check(boxes_drawn() > 0, "orbit10k: boxes were drawn")
        m.values["boxes_requested"] = box_count
        m.values["box_depth"] = depth
        m.values["box_depth_rows"] = rows.get("box_depth_rows")
        m.values["box_parents"] = len(parents)
        m.values["boxes_drawn"] = boxes_drawn()
        m.values["boxes_presented_ms"] = round(time.time() * 1000.0 - sent_ms, 1)
        heartbeat.start()
        paints_before = prop("natronScenePaintCount", 0)
        yield from action_orbit(m)
        m.values["paint_count"] = prop("natronScenePaintCount", 0) - paints_before

    def action_orbit_full(m):
        yield from clear_draw_states()
        v = view()
        require(v is not None and shown(), "orbit_full: a scene view is visible and showing the scene")
        ok = yield ("until", draw_settled(v), TIMEOUT_S)
        require(ok, "orbit_full: the full scene was presented")
        settings = NatronEngine.natron.getSettings()
        mode = settings.getParam("sceneStandInMode")
        m.values["stand_in_mode"] = list(mode.getOptions())[mode.getValue()] if mode is not None else None
        # Always never shows a full frame.
        if m.values["stand_in_mode"] != "always":
            ok = yield ("until", lambda: not v.property("natronSceneStandIn") and v.property("natronSceneSynced"), TIMEOUT_S)
            check(ok, "orbit_full: a full frame of the scene is on screen before the orbit")
        full_frame = v.property("natronSceneLastFullFrameMs")
        m.values["last_full_frame_ms"] = round(full_frame, 1) if full_frame is not None else None
        navigation = settings.getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        navigation.setValue(1)
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            event = QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier)
            QApplication.sendEvent(v, event)

        heartbeat.start()
        stand_in_paints = 0
        paints_before = int(v.property("natronScenePaintCount") or 0)
        last_count = paints_before
        started = time.time()
        deadline = started + orbit_s
        step = 0
        mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
        while time.time() < deadline:
            step += 1
            offset = QPoint(int((step % 40) * 4 - 80), int((step % 20) * 2 - 20))
            mouse(QEvent.MouseMove, centre + offset, Qt.NoButton, Qt.LeftButton)
            yield ("sleep", POLL_MS)
            count = int(v.property("natronScenePaintCount") or 0)
            if count != last_count and v.property("natronSceneStandIn"):
                stand_in_paints += 1
            last_count = count
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        elapsed = time.time() - started
        paints = int(v.property("natronScenePaintCount") or 0) - paints_before
        released_ms = time.time() * 1000.0
        navigation.setValue(original)
        m.values["orbit_s"] = round(elapsed, 2)
        m.values["orbit_frames"] = paints
        m.values["orbit_fps"] = round(paints / elapsed, 1) if elapsed > 0 else None
        m.values["stand_in_paints"] = stand_in_paints
        m.values["stand_in_shown"] = stand_in_paints > 0
        if m.values["stand_in_mode"] == "always":
            m.values["refill_ms"] = None
            return
        ok = yield ("until", lambda: not v.property("natronSceneStandIn") and prop("natronSceneLastPaintMs", 0) >= released_ms, TIMEOUT_S)
        check(ok, "orbit_full: a full frame replaced the stand-in after the orbit")
        m.values["refill_ms"] = round(prop("natronSceneLastPaintMs", released_ms) - released_ms, 1) if ok else None
        full_frame = v.property("natronSceneLastFullFrameMs")
        m.values["refill_full_frame_ms"] = round(full_frame, 1) if full_frame is not None else None

    def action_orbit_fill(m):
        yield from clear_draw_states()
        v = view()
        require(v is not None and shown(), "orbit_fill: a scene view is visible and showing the scene")
        if v.property("natronSceneFillProgress") is None:
            m.not_applicable = "the viewer publishes no natronSceneFillProgress"
            return
        ok = yield ("until", lambda: draw_settled(v)() and bool(v.property("natronSceneFillComplete")), TIMEOUT_S)
        require(ok, "orbit_fill: the full scene was presented before the change")
        yield from set_draw_state(rows["root"], "hidden")
        settings = NatronEngine.natron.getSettings()
        navigation = settings.getParam("viewer3DNavigation")
        require(navigation is not None, "Preferences have a viewer3DNavigation parameter")
        original = navigation.getValue()
        v.activateWindow()
        v.setFocus()
        QApplication.processEvents()
        centre = v.rect().center()

        def mouse(kind, pos, button, buttons):
            QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons, Qt.AltModifier))

        heartbeat.start()
        presented_before = int(v.property("natronScenePresentedFrames") or 0)
        t0_ms = time.time() * 1000.0
        draw_call(v, "clearDrawState", Q_ARG("QString", rows["root"]))
        state = {"first": None, "complete": None, "last_frame": None, "worst": 0.0, "frames": 0, "presented": presented_before,
                 "progress": None, "monotonic": True, "resting": True}

        def watch():
            now = time.time() * 1000.0
            presented = int(v.property("natronScenePresentedFrames") or 0)
            filling = not v.property("natronSceneFillComplete")
            if presented != state["presented"]:
                state["presented"] = presented
                state["frames"] += 1
                if state["first"] is None:
                    state["first"] = now
                if state["resting"] and state["last_frame"] is not None and filling:
                    state["worst"] = max(state["worst"], now - state["last_frame"])
                state["last_frame"] = now
            progress = v.property("natronSceneFillProgress")
            if state["resting"] and progress is not None:
                if state["progress"] is not None and float(progress) < state["progress"] - 1e-9:
                    state["monotonic"] = False
                state["progress"] = float(progress)
            if state["complete"] is None and state["first"] is not None and not filling:
                state["complete"] = now
            return state["complete"] is not None

        deadline = time.time() + FILL_ORBIT_DELAY_S
        while time.time() < deadline and not watch():
            yield ("sleep", POLL_MS)
        if state["complete"] is None:
            navigation.setValue(1)
            state["resting"] = False
            step = 0
            mouse(QEvent.MouseButtonPress, centre, Qt.LeftButton, Qt.LeftButton)
            deadline = time.time() + orbit_s
            while time.time() < deadline:
                step += 1
                mouse(QEvent.MouseMove, centre + QPoint(int((step % 40) * 4 - 80), int((step % 20) * 2 - 20)), Qt.NoButton, Qt.LeftButton)
                yield ("sleep", POLL_MS)
                watch()
            mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
            navigation.setValue(original)
            # The fill resumes once the camera has rested; the gap until then is the camera's.
            state["last_frame"] = None
            state["progress"] = None
            state["resting"] = True
        ok = yield ("until", watch, TIMEOUT_S)
        check(ok, "orbit_fill: the fill completed")
        check(state["monotonic"], "orbit_fill: the fill's progress never fell while the camera rested")
        m.values["first_pass_ms"] = round(state["first"] - t0_ms, 1) if state["first"] is not None else None
        m.values["complete_ms"] = round(state["complete"] - t0_ms, 1) if state["complete"] is not None else None
        m.values["worst_pass_ms"] = round(state["worst"], 1)
        m.values["fill_frames"] = state["frames"]
        m.values["progress_monotonic"] = state["monotonic"]

    def action_boxes_shot(m):
        yield from clear_draw_states()
        ok = yield ("until", lambda: boxes_drawn() == 0, TIMEOUT_S)
        check(ok, "boxes_shot: no box is drawn before the change")
        sent_ms = time.time() * 1000.0
        yield from set_draw_state(rows["root"], "bbox")
        ok = yield ("until", lambda: boxes_drawn() > 0, TIMEOUT_S)
        check(ok, "boxes_shot: boxes were drawn")
        # Stamped by the paint that first drew a box, so the poll's interval is not in it.
        m.values["first_boxes_ms"] = round(prop("natronSceneFirstBoxesMs", 0) - sent_ms, 1) if ok else None
        yield ("sleep", 500)
        m.values["boxes_drawn"] = boxes_drawn()
        m.values["screenshot"] = shot("boxes_%s.png" % fixture)

    def action_sibling_state(m):
        row = rows["sibling_row"]
        yield from clear_draw_states()
        before = draw_stats()
        heartbeat.start()
        for pass_name in ("first", "second"):
            pruned = yield from set_draw_state(row, "bbox")
            restored = yield from clear_draw_state(row)
            for name, timings in (("set", pruned), ("clear", restored)):
                for key, value in timings.items():
                    m.values["sibling_%s_%s_%s" % (pass_name, name, key)] = value
        after = draw_stats()
        m.values["sibling_row"] = row
        m.values["sibling_resynced_paths"] = after.get("resyncedPaths", 0) - before.get("resyncedPaths", 0)
        m.values["sibling_info_only_paths"] = after.get("infoOnlyPaths", 0) - before.get("infoOnlyPaths", 0)

    def trace_read(v, timings):
        """Waits for the viewer to read the trace of the change timings came from, which it does after
        the picture and outside every measured span, and returns the timings with the trace in them."""
        record = current["record"]
        if not record or not record.get("traced"):
            return timings
        posted = int(record["lastPosted"])
        ok = yield ("until", lambda: not (timing_record(v, posted) or {}).get("tracePending", True), TIMEOUT_S)
        check(ok, "the trace of change %d was read" % posted)
        record = timing_record(v, posted)
        if record is None:
            return timings
        return dict(timings, **critical_path(record))

    def action_draw_budget(m):
        """Where a draw state change's time goes, on one sibling and on the widest row."""
        v = view()
        require(v is not None, "draw_budget: a scene view is visible")
        targets = [("sibling", rows["sibling_row"])]
        if rows.get("widest_row"):
            targets.append(("wide", rows["widest_row"]))
        m.values["budget_rows"] = {tag: row for tag, row in targets}
        m.values["draw_timing"] = DRAW_TIMING
        changes = []
        yield from clear_draw_states()
        heartbeat.start()
        for tag, row in targets:
            for repeat in range(budget_repeats):
                for kind, timings in (("set", (yield from set_draw_state(row, "bbox", row_tag=tag))),
                                      ("clear", (yield from clear_draw_state(row, row_tag=tag)))):
                    changes.append(dict(timings, row=tag, path=row, change=kind, repeat=repeat))
        m.stall_floor_ms = heartbeat.stop()
        if DRAW_TIMING:
            # Tracing slows the main thread's Python too, so its stall is kept apart from the judged one.
            heartbeat.start()
            for tag, row in targets:
                for kind in ("set", "clear"):
                    draw_call(v, "setDrawTimingTrace", Q_ARG(bool, True))
                    if kind == "set":
                        timings = yield from set_draw_state(row, "bbox", row_tag=tag)
                    else:
                        timings = yield from clear_draw_state(row, row_tag=tag)
                    draw_call(v, "setDrawTimingTrace", Q_ARG(bool, False))
                    timings = yield from trace_read(v, timings)
                    changes.append(dict(timings, row=tag, path=row, change=kind, repeat="traced"))
            m.values["traced_stall_ms"] = round(heartbeat.stop(), 1)
        heartbeat.start()
        m.values["draw_changes"] = changes
        for tag, _ in targets:
            untraced = [c["picture_ms"] for c in changes if c["row"] == tag and c["repeat"] != "traced"]
            m.values["%s_picture_ms_max" % tag] = max(untraced) if untraced else None
            m.values["%s_picture_ms_min" % tag] = min(untraced) if untraced else None
        m.values["picture_source"] = "viewer" if all(c.get("picture_source") == "viewer" for c in changes) else "polled"

    def viewport_image(file_name, area):
        return QImage(file_name).copy(area).convertToFormat(QImage.Format_RGBX8888)

    def changed_pixels(first, second):
        if first.isNull() or first.size() != second.size():
            return None
        a = bytes(first.constBits())[:first.sizeInBytes()]
        b = bytes(second.constBits())[:second.sizeInBytes()]
        if a == b:
            return 0
        return sum(1 for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3])

    def row_bounds(paths):
        """{"up_axis", "bounds"} for the rows, read from the stage in a picker process; None, with the reason, if it fails."""
        rows_dir = os.environ.get("BENCH_ROWS_DIR", os.path.join(OUT, "rows"))
        key = hashlib.sha1("\n".join(sorted(set(paths))).encode()).hexdigest()[:10]
        cached = os.path.join(rows_dir, "%s-load-%s-bounds-%s.json" % (file_stem, load_state, key))
        if not os.path.isfile(cached) or os.path.getmtime(cached) < os.path.getmtime(path):
            picker = os.path.join(ROOT, "tools", "bench", "heavy_scene_rows.py")
            try:
                done = subprocess.run(picker_command() + [picker, path, "--load", load_state, "--out", cached, "--bounds"] + sorted(set(paths)),
                                      stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=PICK_LIMIT_S)
            except subprocess.TimeoutExpired:
                return None, "the bounds query passed %d s" % PICK_LIMIT_S
            if done.returncode != 0:
                return None, "the bounds query failed: " + done.stderr.decode(errors="replace")[-300:]
        with open(cached) as f:
            return json.load(f), None

    def row_coverage(info, row, v):
        """The share of the viewport the row's world bound projects onto, and why it is None if it is."""
        if info is None:
            return None, "no bounds for the rows"
        if prop("natronSceneLookThrough"):
            return None, "the view looks through a camera prim, whose lens is not published"
        camera = prop("natronSceneFreeCamera")
        return frame_coverage(info["bounds"].get(row), camera, prop("natronSceneViewMode", "perspective"),
                              info.get("up_axis") == "Z", v.width() / float(max(1, v.height())))

    def shoot_state_change(m, tag, row, area, info):
        yield from clear_draw_states()
        yield ("sleep", 1000)
        stem = "%s_%s_load-%s" % (tag, file_stem, load_state)
        before_path = shot(stem + "_0_before.png")
        before = viewport_image(before_path, area)
        coverage, why = row_coverage(info, row, view())
        pruned = yield from set_draw_state(row, "bbox")
        immediate_path = shot(stem + "_1_set_immediate.png")
        yield ("sleep", 1000)
        settled_path = shot(stem + "_2_set_settled.png")
        restored = yield from clear_draw_state(row)
        yield ("sleep", 1000)
        restored_path = shot(stem + "_3_restored.png")
        m.values[tag + "_shot_row"] = row
        m.values[tag + "_shot_files"] = [os.path.basename(p) for p in (before_path, immediate_path, settled_path, restored_path)]
        m.values[tag + "_shot_frame_coverage"] = None if coverage is None else round(coverage, 5)
        if why:
            m.values[tag + "_shot_frame_coverage_unavailable"] = why
        m.values[tag + "_shot_changed_immediate"] = changed_pixels(before, viewport_image(immediate_path, area))
        m.values[tag + "_shot_changed_settled"] = changed_pixels(before, viewport_image(settled_path, area))
        m.values[tag + "_shot_changed_restored"] = changed_pixels(before, viewport_image(restored_path, area))
        m.values[tag + "_shot_set_frame_ms"] = pruned["frame_ms"]
        m.values[tag + "_shot_clear_frame_ms"] = restored["frame_ms"]

    def read_shots(m, tags):
        """Whether a state change drew nothing because the state does nothing or because the row is not in frame."""
        def changed(tag):
            return max(m.values.get(tag + "_shot_changed_immediate") or 0, m.values.get(tag + "_shot_changed_settled") or 0)

        control = tags["control"]
        m.values["control_changed_pixels"] = changed(control)
        if changed(control) == 0:
            m.values["shot_reading"] = "state_ineffective: the control row %s covering %s of the frame changed no pixels either" % (
                m.values[control + "_shot_row"], m.values.get(control + "_shot_frame_coverage"))
            return
        for tag in tags["rows"]:
            if tag == control:
                continue
            if changed(tag) > 0:
                reading = "state_visible"
            else:
                coverage = m.values.get(tag + "_shot_frame_coverage")
                if coverage is None:
                    reading = "no_pixels_changed_but_the_control_did: coverage unknown, so the row may be out of frame"
                elif coverage < 0.001:
                    reading = "row_out_of_frame_or_tiny: it projects onto %.5f of the frame and the control changed" % coverage
                else:
                    reading = "state_ineffective_on_this_row: it projects onto %.5f of the frame, the control changed and it did not" % coverage
            m.values[tag + "_shot_reading"] = reading
        m.values["shot_reading"] = "control_changed"

    def action_sibling_shot(m):
        v = view()
        require(v is not None, "sibling_shot: a scene view is visible")
        corner = v.mapToGlobal(v.rect().topLeft()) - QGuiApplication.primaryScreen().geometry().topLeft()
        area = QRect(corner.x(), corner.y(), v.width(), v.height())
        m.values["shot_viewport_pixels"] = v.width() * v.height()
        # One row of a large scene can cover a few pixels, which cannot tell "drew nothing" from "too
        # small to see". The branch holds more of the scene, and the root, which the viewer frames,
        # covers most of the frame; the same state change on the root says what the state can do.
        shot_rows = [("sibling", rows["sibling_row"])]
        if rows["branch"] != rows["sibling_row"]:
            shot_rows.append(("branch", rows["branch"]))
        control = next((tag for tag, row in shot_rows if row == rows["root"]), None)
        if control is None:
            control = "control"
            shot_rows.append((control, rows["root"]))
        info, why = row_bounds([row for _, row in shot_rows])
        if why:
            m.values["shot_bounds_unavailable"] = why
        for tag, row in shot_rows:
            yield from shoot_state_change(m, tag, row, area, info)
        read_shots(m, {"control": control, "rows": [tag for tag, _ in shot_rows]})
        # The grabs and the comparison are this harness's cost, not the viewer's.
        heartbeat.start()

    switch_nodes = []

    def panel_shows(node):
        p = panel()
        return (str(p.property("natronSceneGraphNode") or "") == node.getScriptName()
                and not p.property("natronSceneGraphSwitching") and hold(tree().model()).rowCount(QModelIndex()) > 0)

    def action_node_switch(m):
        require(view() is not None and shown(), "node_switch: a scene view is visible and showing the scene")
        show_panel()
        ok = yield ("until", lambda: panel().isVisible() and panel_shows(usd), TIMEOUT_S)
        require(ok, "node_switch: the panel shows the viewed scene")
        if not switch_nodes:
            for i in range(SWITCH_NODES):
                node = create("USDEditTransform", 200 + 150 * i, 150)
                require(node.connectInput(0, usd), "node_switch: connect a USDEditTransform to USDRead")
                switch_nodes.append(node)
        follow = hold(panel().findChild(QComboBox, "sceneGraphFollow"))
        require(follow is not None, "node_switch: the panel has a Follow chooser")
        app.selectNode(usd, True)
        follow.setCurrentIndex(1)
        ok = yield ("until", lambda: panel_shows(usd), TIMEOUT_S)
        require(ok, "node_switch: following the selection, the panel shows USDRead")
        # Creating the nodes is not the switching the stall budget is about.
        heartbeat.start()
        times = []
        composes = []
        # The last switch returns to the first node, whose stage is kept.
        for node in switch_nodes + switch_nodes[:1]:
            composed = int(panel().property("natronSceneGraphComposeCount") or 0)
            t0 = time.time() * 1000.0
            app.selectNode(node, True)
            ok = yield ("until", lambda n=node: panel_shows(n), TIMEOUT_S)
            check(ok, "node_switch: the panel shows " + node.getScriptName())
            times.append(round(time.time() * 1000.0 - t0, 1))
            composes.append(int(panel().property("natronSceneGraphComposeCount") or 0) - composed)
        m.values["switch_ms"] = times[:-1]
        m.values["switch_max_ms"] = max(times[:-1])
        m.values["cached_switch_ms"] = times[-1]
        m.values["cached_switch_composes"] = composes[-1]
        follow.setCurrentIndex(0)

    project_state = {"reopened": False, "loaded_at_open": False}

    def scene_has_payloads():
        return fixture == "city" or bool(rows.get("payload_rows"))

    def add_request(m, paths, descendants):
        """request() that keeps what earlier requests of the scenario asked for."""
        files, layers = list(m.requested_files), set(m.requested_layers)
        request(m, paths, descendants)
        m.requested_files = files + m.requested_files
        m.requested_layers = m.requested_layers | layers

    def scene_painted_since(t0_ms):
        def drawn():
            synced_at = prop("natronSceneSyncedAtMs", 0)
            return shown() and synced_at >= t0_ms and prop("natronSceneLastPaintMs", 0) >= synced_at
        return drawn

    def action_workflow(m):
        """Open, twirl into a payload, Full on one asset, Full + Load on a block, add a pattern node, switch
        to it, select a prim in the inspector, save, reopen. A step's stall is its own: the setup before it
        (docking the panel, reading a row's index) is not the work the budget is about."""

        def finish_step(name, ms, **extra):
            # The window stall_ms was measured over, in epoch seconds, to find the step in stack samples.
            started_at = heartbeat.started_at
            stall = heartbeat.stop()
            m.steps[name] = dict(extra, applicable=True, ms=round(ms, 1), stall_ms=round(stall, 1),
                                 started_at=round(started_at, 3) if started_at is not None else None,
                                 ended_at=round(time.time(), 3))
            m.stall_floor_ms = max(m.stall_floor_ms, stall)
            heartbeat.start()

        def skip_step(name, why):
            m.steps[name] = {"applicable": False, "reason": why}

        asset = rows.get("sibling_row")

        t0 = time.perf_counter()
        yield from open_file(m)
        finish_step("open", (time.perf_counter() - t0) * 1000.0, first_frame_ms=m.values["first_frame_ms"],
                    substeps_ms=m.values["open_substeps_ms"])

        if rows.get("twirl_row"):
            yield from action_twirl(m)
            finish_step("twirl", m.values["twirl_ms"], path=rows["twirl_row"], rows=m.values["twirl_rows"])
        else:
            skip_step("twirl", "the scene has no payload row to twirl")

        if asset:
            yield from clear_draw_states()
            yield from set_draw_state(asset, "bbox")
            heartbeat.start()
            timings = yield from set_draw_state(asset, "full")
            finish_step("full_asset", timings["total_ms"], path=asset, picture_ms=timings["picture_ms"],
                        picture_source=timings["picture_source"], apply_ms=timings["apply_ms"])
        else:
            skip_step("full_asset", "the scene has no row for an asset")

        block = rows.get("load_block")
        if scene_has_payloads() and block:
            v = view()
            edits_before = int(v.property("natronSceneLoadEdits") or 0)
            add_request(m, [block], True)
            yield from paced_reveal(m, "workflow_block", block, "bbox", "loadAll", commit_watch(v), keep_states=True)
            # The block's later payloads land after its first commit; the next step must not measure them.
            settle = yield from wait_loads_settled("workflow: full_load_block")
            load_results(m, v, "workflow_block", edits_before)
            finish_step("full_load_block", m.values["workflow_block_ms"], path=block, complete=m.values["workflow_block_complete"],
                        worst_frame_ms=m.values["workflow_block_worst_frame_ms"], frames=m.values["workflow_block_frames"],
                        prims=m.values["workflow_block_prims"],
                        settle=settle_fields(settle))
        else:
            skip_step("full_load_block", "the scene has no payloads to load")

        branch = rows.get("branch") or rows["root"]
        pattern = os.environ.get("BENCH_PATTERN") or ((asset if fixture == "flat" and asset else branch) + "//*")
        t0_ms = int(time.time() * 1000.0)
        t0 = time.perf_counter()
        heartbeat.start()
        pattern_node = create("USDEditTransform", 200, 150)
        pattern_node.getParam("primPattern").setValue(pattern)
        pattern_node.getParam("translate").set(1.0, 0.0, 0.0)
        require(pattern_node.connectInput(0, usd), "workflow: connect the pattern node to USDRead")
        created = time.perf_counter()
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, pattern_node), "workflow: view the pattern node")
        reconnected = time.perf_counter()
        ok = yield ("until", scene_painted_since(t0_ms), TIMEOUT_S)
        check(ok, "workflow: the pattern node's scene reached the screen (%r)" % (prop("natronSceneError"),))
        finish_step("pattern_node", (time.perf_counter() - t0) * 1000.0, pattern=pattern,
                    create_ms=round((created - t0) * 1000.0, 1), reconnect_ms=round((reconnected - created) * 1000.0, 1),
                    to_picture_ms=round((time.perf_counter() - reconnected) * 1000.0, 1))

        show_panel()
        ok = yield ("until", lambda: panel().isVisible() and hold(tree().model()).rowCount(QModelIndex()) > 0, TIMEOUT_S)
        require(ok, "workflow: the panel lists a scene")
        follow = hold(panel().findChild(QComboBox, "sceneGraphFollow"))
        require(follow is not None, "workflow: the panel has a Follow chooser")
        app.selectNode(usd, True)
        follow.setCurrentIndex(1)
        ok = yield ("until", lambda: panel_shows(usd), TIMEOUT_S)
        require(ok, "workflow: following the selection, the panel shows USDRead")
        heartbeat.start()
        t0 = time.perf_counter()
        app.selectNode(pattern_node, True)
        ok = yield ("until", lambda: panel_shows(pattern_node), TIMEOUT_S)
        check(ok, "workflow: the panel shows the pattern node")
        finish_step("node_switch", (time.perf_counter() - t0) * 1000.0)

        inspected = asset or rows["root"]
        target = find_row(inspected)
        require(target.isValid(), "workflow: the panel lists " + inspected)
        inspector = hold(panel().findChild(QWidget, "sceneGraphInspector"))
        require(inspector is not None, "workflow: the panel has an inspector")
        selection = tree().selectionModel()
        heartbeat.start()
        t0 = time.perf_counter()
        selection.select(target, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        selection.setCurrentIndex(target, QItemSelectionModel.NoUpdate)
        ok = yield ("until", lambda: str(inspector.property("natronSceneGraphInspectorPath")) == inspected
                    and not inspector.property("natronSceneGraphInspectorBusy"), TIMEOUT_S)
        check(ok, "workflow: the inspector shows " + inspected)
        finish_step("inspect", (time.perf_counter() - t0) * 1000.0, path=inspected)

        project_path = os.path.join(OUT, "heavy_scene_workflow_%s.ntpz" % file_stem)
        heartbeat.start()
        t0 = time.perf_counter()
        saved = app.saveProjectAs(project_path)
        finish_step("save", (time.perf_counter() - t0) * 1000.0, project=os.path.basename(project_path))
        require(saved, "workflow: save the project to " + project_path)

        # The viewer and panel changes above are project edits, so the reset asks whether to save them;
        # the saved file is the one under test.
        def discard_save_question():
            box = QApplication.activeModalWidget()
            if isinstance(box, QMessageBox) and box.button(QMessageBox.StandardButton.Discard) is not None:
                box.button(QMessageBox.StandardButton.Discard).click()

        answer = QTimer()
        answer.timeout.connect(discard_save_question)
        answer.start(200)
        # The reopen is judged from the reset on: closing the old project is part of what the user waits for.
        heartbeat.start()
        t0 = time.perf_counter()
        try:
            reset = app.resetProject()
        finally:
            answer.stop()
            answer = None
        reset_ms = (time.perf_counter() - t0) * 1000.0
        require(reset, "workflow: reset the project")
        # The nodes and widgets of the closed project are gone.
        project_state["reopened"] = True
        forget_widgets()
        del held_widgets[:]
        # The pause stands for the user picking the file, so it is left out of the step's times; the
        # heartbeat keeps running through it, as an event loop left idle stalls nothing.
        paused = time.perf_counter()
        yield ("sleep", 400)
        pause_ms = (time.perf_counter() - paused) * 1000.0
        load_ms_epoch = int(time.time() * 1000.0)
        load_t0 = time.perf_counter()
        require(app.loadProject(project_path) is not None, "workflow: reopen " + project_path)
        load_call_ms = (time.perf_counter() - load_t0) * 1000.0
        ok = yield ("until", scene_painted_since(load_ms_epoch), TIMEOUT_S)
        check(ok, "workflow: the reopened project shows the scene (%r)" % (prop("natronSceneError"),))
        painted = prop("natronSceneLastPaintMs")
        after_load_ms = (painted if painted is not None else time.time() * 1000.0) - load_ms_epoch
        settle = yield from wait_loads_settled("workflow: reopen")
        finish_step("reopen", reset_ms + (settle["stable_at"] - load_t0) * 1000.0, first_frame_ms=round(reset_ms + after_load_ms, 1),
                    reset_ms=round(reset_ms, 1), pause_ms=round(pause_ms, 1), load_call_ms=round(load_call_ms, 1),
                    first_frame_after_load_ms=round(after_load_ms, 1), settle=settle_fields(settle))
        m.values["workflow_steps"] = m.steps

    def action_noedit(m):
        def edits():
            stats = draw_stats()
            return stats.get("stageNotices", 0), stats.get("stageEdits", 0) + stats.get("stageLoadEdits", 0)

        row = rows["branch"]
        cancel_batch = 50
        yield from clear_draw_states()
        heartbeat.start()
        before = edits()
        yield from set_draw_state(row, "bbox")
        yield from clear_draw_state(row)
        yield from set_draw_state(row, "bbox")
        draw_call(view(), "setDrawRevealBatch", Q_ARG(int, cancel_batch))
        yield from set_draw_state(row, "full")
        yield from set_draw_state(row, "bbox")
        draw_call(view(), "setDrawRevealBatch", Q_ARG(int, 0))
        yield from clear_draw_state(row)
        yield ("sleep", 200)
        after = edits()
        m.values["stage_notices"] = after[0] - before[0]
        m.values["stage_edit_count"] = after[1] - before[1]
        m.values["stage_edits"] = m.values["stage_notices"] + m.values["stage_edit_count"]

    ACTIONS = {"open": action_open, "edit": action_edit, "orbit": action_orbit, "load": action_load, "twirl": action_twirl,
               "load_all": action_load_all, "populate_pruned": action_populate_pruned, "populate_full": action_populate_full,
               "prune_restore": action_prune_restore, "reveal": action_reveal, "reveal_cancel": action_reveal_cancel,
               "orbit10k": action_orbit10k, "orbit_full": action_orbit_full, "orbit_fill": action_orbit_fill, "boxes_shot": action_boxes_shot, "sibling_state": action_sibling_state,
               "sibling_shot": action_sibling_shot, "noedit": action_noedit, "draw_budget": action_draw_budget,
               "reveal_load_all": action_reveal_load_all, "reveal_wide": action_reveal_wide,
               "reveal_cancel_paced": action_reveal_cancel_paced, "node_switch": action_node_switch,
               "workflow": action_workflow}

    def verdict(value, limit, below_is_good, valid=True, applicable=True, reason=None):
        if not applicable:
            status = "not_applicable"
        elif value is None:
            status = "not_valid"
        elif not valid:
            status = "not_valid"
        else:
            status = "pass" if ((value <= limit) if below_is_good else (value >= limit)) else "fail"
        judged = {"value": value, "limit": limit, "status": status}
        if not applicable and reason:
            judged["reason"] = reason
        elif status == "fail":
            judged["reason"] = "%s is %s the limit %s" % (value, "above" if below_is_good else "below", limit)
        return judged

    is_island = file_stem.lower() == "island"
    CITY_UNJUDGED_REASON = ("not judged on the city for now (user, 2026-11-17: \"Ok for now. Will revisit later with "
                            "different techniques.\"); the city is judged on UI stalls")
    ISLAND_UNJUDGED_REASON = ("not judged on Island (user, 2026-11-17: Island's ~2 s GPU frame is accepted; "
                              "Island is judged on UI stalls only)")
    picture_unjudged_reason = CITY_UNJUDGED_REASON if fixture == "city" else ISLAND_UNJUDGED_REASON if is_island else None
    reveal_frames_unjudged_reason = CITY_UNJUDGED_REASON if fixture == "city" else ISLAND_UNJUDGED_REASON if is_island else None

    orbit_unjudged_reason = ISLAND_UNJUDGED_REASON if is_island else None

    def frame_gap_verdict(value, frames, reason_unjudged):
        judged = verdict(value, STALL_BUDGET_MS, True, valid=gpu and frames is not None and frames >= MIN_TIMED_FRAMES,
                         applicable=reason_unjudged is None, reason=reason_unjudged)
        judged["frames"] = frames
        if judged["status"] == "not_valid" and gpu and value is not None:
            judged["reason"] = "%r frames presented, fewer than the %d a gap between frames needs" % (frames, MIN_TIMED_FRAMES)
        return judged

    first_frame_judged = fixture == "city"
    first_frame_unjudged_reason = "the design sets the first-frame budget on the payload city alone"

    def skip_reason(name):
        if project_state["reopened"]:
            return "the workflow reopened the project, so this process's nodes are gone: run it last"
        if name == "load" and not load_paths:
            return "the scene has no payloads to load"
        if name == "twirl" and not rows.get("twirl_row"):
            return "the scene has no payload row to twirl"
        has_payloads = scene_has_payloads()
        if name in ("load_all", "reveal_load_all") and not has_payloads:
            return "the scene has no payloads to load"
        needs_row = {"populate_pruned": "root", "prune_restore": "branch", "noedit": "branch", "reveal_cancel": "branch",
                     "reveal": "reveal_branch", "sibling_state": "sibling_row", "sibling_shot": "sibling_row", "draw_budget": "sibling_row",
                     "orbit10k": "box_depth", "boxes_shot": "box_depth",
                     "reveal_load_all": "root", "reveal_wide": "widest_row", "reveal_cancel_paced": "widest_row"}
        if name in needs_row and rows.get(needs_row[name]) is None:
            return "the scene has no such row: no prim here holds the geometry %s needs" % name
        if name == "sibling_shot" and has_payloads and load_state == "none":
            return "needs BENCH_LOAD_ALL=1 and load_all: with payloads unloaded no row draws anything to compare"
        if name in ("orbit10k", "boxes_shot") and prop("natronSceneDrawBoxes") is None:
            return "this viewer does not publish natronSceneDrawBoxes: it draws no boxes"
        if name == "orbit_full" and prop("natronScenePaintCount") is None:
            return "this viewer does not publish natronScenePaintCount: its paints cannot be counted"
        return None

    scene_payload_layers = set(rows.get("payload_assets", ()))

    def is_payload_layer(identifier):
        if fixture == "scene":
            return identifier in scene_payload_layers
        return payload_mark in identifier

    def judge(m, stall_ms, new_layers):
        budgets = {"stall_ms": verdict(round(max(stall_ms, m.stall_floor_ms), 1), STALL_BUDGET_MS, True)}
        if m.pictures:
            # Polled times are rounded up to the poll interval and so judge nothing.
            viewer_timed = all(p["source"] == "viewer" for p in m.pictures)
            budgets["time_to_picture_ms"] = verdict(max(p["ms"] for p in m.pictures), PICTURE_BUDGET_MS, True,
                                                    valid=gpu and viewer_timed, applicable=picture_unjudged_reason is None,
                                                    reason=picture_unjudged_reason)
            budgets["time_to_picture_ms"]["source"] = "viewer" if viewer_timed else "polled"
            for tag in sorted(set(p["row"] for p in m.pictures if p["row"])):
                worst = max(p["ms"] for p in m.pictures if p["row"] == tag)
                budgets["time_to_picture_%s_ms" % tag] = verdict(worst, PICTURE_BUDGET_MS, True, valid=gpu and viewer_timed,
                                                                 applicable=picture_unjudged_reason is None,
                                                                 reason=picture_unjudged_reason)
        if "first_frame_ms" in m.values:
            budgets["first_frame_ms"] = verdict(m.values["first_frame_ms"], FIRST_FRAME_BUDGET_MS, True, valid=gpu,
                                                applicable=first_frame_judged, reason=first_frame_unjudged_reason)
        if project_state["loaded_at_open"]:
            budgets["unrequested_payloads"] = {"value": None, "limit": 0, "status": "not_applicable",
                                               "reason": "the scene was opened with Full + Load, which requests every payload"}
        elif m.all_payloads_requested:
            budgets["unrequested_payloads"] = {"value": None, "limit": 0, "status": "not_applicable"}
        elif new_layers is None:
            budgets["unrequested_payloads"] = {"value": None, "limit": 0, "status": "not_valid"}
        else:
            unrequested = sorted(i for i in new_layers if is_payload_layer(i) and i not in m.requested_layers
                                 and not any(os.path.basename(i).startswith(f) for f in m.requested_files))
            budgets["unrequested_payloads"] = verdict(len(unrequested), 0, True)
            budgets["unrequested_payloads"]["sample"] = [os.path.basename(i) for i in unrequested[:5]]
        if "orbit_fps" in m.values:
            budgets["orbit_fps"] = verdict(m.values["orbit_fps"], ORBIT_BUDGET_FPS, False, valid=gpu,
                                           applicable=orbit_unjudged_reason is None, reason=orbit_unjudged_reason)
        for name in ("reveal_load_all", "reveal_wide"):
            if name + "_worst_frame_ms" in m.values:
                budgets[name + "_complete"] = verdict(1 if m.values[name + "_complete"] else 0, 1, False)
                budgets[name + "_worst_frame_ms"] = frame_gap_verdict(m.values[name + "_worst_frame_ms"], m.values.get(name + "_frames"),
                                                                      reveal_frames_unjudged_reason)
        if "cancel_paced_overshoot_prims" in m.values:
            batch = m.values["cancel_paced_batch"]
            budgets["cancel_paced_overshoot_prims"] = verdict(m.values["cancel_paced_overshoot_prims"], batch, True,
                                                              valid=batch is not None)
            if not m.values["cancel_paced_was_active"]:
                budgets["cancel_paced_overshoot_prims"]["status"] = "fail"
                budgets["cancel_paced_overshoot_prims"]["reason"] = "the fill had finished before the cancel landed"
            budgets["cancel_paced_stopped"] = verdict(1 if m.values["cancel_paced_stopped"] else 0, 1, False)
        for step_name, step in m.steps.items():
            applicable = step["applicable"]
            budgets[step_name + "_stall_ms"] = verdict(step.get("stall_ms"), STALL_BUDGET_MS, True, applicable=applicable)
            if step_name == "full_asset":
                budgets["full_asset_picture_ms"] = verdict(step.get("picture_ms"), PICTURE_BUDGET_MS, True,
                                                           valid=gpu and step.get("picture_source") == "viewer",
                                                           applicable=applicable and picture_unjudged_reason is None,
                                                           reason=picture_unjudged_reason if applicable else None)
            elif step_name == "full_load_block":
                budgets["full_load_block_complete"] = verdict(None if not applicable else (1 if step["complete"] else 0), 1, False,
                                                              applicable=applicable)
                if applicable:
                    budgets["full_load_block_worst_frame_ms"] = frame_gap_verdict(step.get("worst_frame_ms"), step.get("frames"),
                                                                                  reveal_frames_unjudged_reason)
                else:
                    budgets["full_load_block_worst_frame_ms"] = verdict(None, STALL_BUDGET_MS, True, applicable=False)
            elif step_name == "reopen":
                budgets["reopen_first_frame_ms"] = verdict(step.get("first_frame_ms"), FIRST_FRAME_BUDGET_MS, True, valid=gpu,
                                                           applicable=first_frame_judged, reason=first_frame_unjudged_reason)
        if "stage_edits" in m.values:
            budgets["stage_edits"] = verdict(m.values["stage_edits"], 0, True)
        if "sibling_resynced_paths" in m.values:
            budgets["sibling_resynced_paths"] = verdict(m.values["sibling_resynced_paths"], 0, True)
        if "filter_bound" in m.values:
            budgets["filter_bound"] = verdict(1 if m.values["filter_bound"] else 0, 1, False)
        if "control_changed_pixels" in m.values:
            budgets["control_changed_pixels"] = verdict(m.values["control_changed_pixels"], 1, False)
        return budgets

    common_fields = {
        "label": label,
        "fixture": fixture,
        "load_state": load_state,
        "file": os.path.basename(path),
        "gpu": bool(gpu),
        "gl_renderer": gl_renderer_name,
        "gl_vendor": gl_vendor_name,
        "frame_rate": "valid" if gpu else "not valid on this display",
        "stack_sampled": os.environ.get("BENCH_STACK_SAMPLED") == "1",
        "bisect": os.environ.get("BENCH_BISECT") or None,
        "shader_cache": os.environ.get("BENCH_SHADER_CACHE") or None,
        "warmup": os.environ.get("BENCH_WARMUP") == "1",
        "rows_source": rows["source"],
        "rows": {key: rows[key] for key in ROW_RECORD_KEYS if key in rows},
    }
    progress_path = os.path.join(OUT, "bench_progress")

    def write_progress(text):
        temporary = progress_path + ".tmp"
        with open(temporary, "w") as f:
            f.write(text)
        os.replace(temporary, progress_path)

    def mark_running(name, remaining, started, limit):
        write_progress("%d %d\n%s\n%s\n%s\n" % (started, limit, name, " ".join(remaining), json.dumps(common_fields)[1:-1]))

    def log_scenario(name, edge, at):
        line = "SCENARIO %s %s %.3f %s" % (edge, name, at, time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(at)))
        print(line)
        report(line)

    def write_record(record):
        line = json.dumps(record)
        print(line)
        with open(result_path, "a") as f:
            f.write(line + "\n")
        report("RESULT " + line)

    def peak_rss_mb():
        with open("/proc/self/status") as f:
            for status_line in f:
                if status_line.startswith("VmHWM:"):
                    return int(status_line.split()[1]) // 1024
        return None

    def memory_fields(started):
        peak_mb = peak_rss_mb()
        return {
            "peak_rss_mb": peak_mb,
            "peak_rss_gb": None if peak_mb is None else round(peak_mb / 1024, 2),
            "mem_available_min_gb": available_gb_min(started),
        }

    def available_gb_min(started):
        # The watchdog samples once a second; the reading here covers the instant the scenario ends.
        low = None
        try:
            with open("/proc/meminfo") as f:
                for meminfo_line in f:
                    if meminfo_line.startswith("MemAvailable:"):
                        low = int(meminfo_line.split()[1])
            with open(progress_path + ".mem") as f:
                watched_start, watched_low, _ = f.read().split()
            if int(watched_start) == int(started):
                low = min(low, int(watched_low)) if low is not None else int(watched_low)
        except (OSError, ValueError):
            pass
        return None if low is None else round(low / 1048576, 2)

    def start_watchdog():
        write_progress("idle\n")
        script = os.path.join(ROOT, "tools", "bench", "heavy_scene_watchdog.sh")
        # Natron's library and Python paths are for the bundle, not for the system's shell tools.
        env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH")}
        return subprocess.Popen(["bash", script, str(os.getpid()), progress_path, os.path.abspath(result_path)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=env, start_new_session=True)

    watchdog = start_watchdog()

    def steps():
        names = [s.strip() for s in os.environ.get("BENCH_SCENARIOS", "open,orbit").split(",") if s.strip()]
        for name in names:
            require(name in ACTIONS, "scenario %r is a known action (%s)" % (name, ", ".join(sorted(ACTIONS))))
        for index, name in enumerate(names):
            reason = skip_reason(name)
            if reason is not None:
                record = dict(common_fields, scenario=name, result="skipped", reason=reason, time=time.strftime("%Y-%m-%dT%H:%M:%S"),
                              started_at=round(time.time(), 3), ended_at=round(time.time(), 3))
                record.update(memory_fields(time.time()))
                write_record(record)
                continue
            started = time.time()
            mark_running(name, names[index + 1:], started, OPEN_LIMIT_S if name in LONG_SCENARIOS else SCENARIO_LIMIT_S)
            log_scenario(name, "start", started)
            m = Measure()
            current["name"], current["m"] = name, m
            scenario_checks["failed"] = m.failed_checks
            if name in SETTLED_SCENARIOS and view() is not None:
                settle = yield from wait_loads_settled(name)
                m.values["loads_settle_wait_ms"] = settle["wait_ms"]
                m.values["loads_settle"] = settle_fields(settle)
                m.values["action_started_at"] = round(time.time(), 3)
            before = loaded_layers()
            heartbeat.start()
            yield from ACTIONS[name](m)
            current["name"], current["m"] = None, None
            if m.not_applicable is not None:
                heartbeat.stop()
                scenario_checks["failed"] = None
                ended = time.time()
                log_scenario(name, "end", ended)
                record = dict(common_fields, scenario=name, result="not_applicable", reason=m.not_applicable,
                              wall_s=round(ended - started, 1), time=time.strftime("%Y-%m-%dT%H:%M:%S"),
                              started_at=round(started, 3), ended_at=round(ended, 3))
                record.update(memory_fields(started))
                write_record(record)
                write_progress("idle\n")
                continue
            if name in DRAW_SCENARIOS:
                m.values["filter_bound"] = bool(draw_stats().get("filterBound"))
            yield ("sleep", 200)
            stall_ms = heartbeat.stop()
            ended = time.time()
            log_scenario(name, "end", ended)
            after = loaded_layers()
            new_layers = None if before is None or after is None else after - before
            budgets = judge(m, stall_ms, new_layers)
            scenario_checks["failed"] = None
            failed_checks = list(m.failed_checks) + [{"kind": "budget", "name": budget_name, "reason": b.get("reason")}
                                                     for budget_name, b in sorted(budgets.items()) if b["status"] == "fail"]
            failed = bool(failed_checks)
            record = dict(common_fields)
            record.update(memory_fields(started))
            record.update({
                "scenario": name,
                "wall_s": round(ended - started, 1),
                "started_at": round(started, 3),
                "ended_at": round(ended, 3),
                "layers_before": None if before is None else len(before),
                "layers_after": None if after is None else len(after),
                "layers_loaded": None if new_layers is None else len(new_layers),
                "budgets": budgets,
                "result": "fail" if failed else "pass",
                "failed_checks": failed_checks,
                "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
            })
            record.update(m.values)
            write_record(record)
            write_progress("idle\n")
            check(record["result"] == "pass", "%s: within budget" % name)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
