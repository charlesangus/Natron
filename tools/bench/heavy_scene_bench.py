"""Budget harness for the scene viewer on the heavy fixtures.

  Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py [natron-binary]
      Runs inside Natron (GUI, under Xvfb, or on a real display through heavy_scene_gpu_host.sh) and is
      configured by the environment:
        BENCH_FIXTURE    flat (default), city, or scene: which file USDRead reads. flat and city are
                         tools/bench/heavy_scene_make.py outputs; scene is any file named by BENCH_FILE,
                         whose rows tools/bench/heavy_scene_rows.py picks
        BENCH_FILE       overrides the fixture path (the fixture kind still selects the budgets)
        BENCH_SCENARIOS  comma-separated actions run in order (default open,orbit)
        BENCH_ORBIT_S    seconds the orbit action runs (default 5)
        BENCH_LOAD_PATHS comma-separated prims the load action asks to load (default the fixture's
                         load_building row, or load_block with BENCH_LOAD_DESCENDANTS=1); on the city,
                         /World/BlockN with BENCH_LOAD_DESCENDANTS=1 is a larger load
        BENCH_LOAD_DESCENDANTS  1 to load the payloads below those prims too
        BENCH_TWIRL_PATH the payload row the twirl action opens in the Scene Graph panel (default the
                         fixture's twirl_row row)
        BENCH_PAYLOAD_MARK  substring of a layer identifier that marks a payload file on flat and city
                         (default heavy_city_payloads)
        BENCH_ROWS_DIR   where a scene's picked rows are cached (default $NATRON_GUI_TEST_OUT/rows)
        BENCH_PICK_LIMIT_S  seconds picking a scene's rows may take (default 900)
        BENCH_GPU        1 when the display is a real GPU; otherwise frame-dependent budgets are
                         reported as not valid (llvmpipe and GitHub runners do not count, whatever
                         this says)
        BENCH_LABEL      free text stored with each result
        BENCH_TIMEOUT_S  per-wait timeout (default 600)
        BENCH_SCENARIO_LIMIT_S  wall-clock limit of a scenario, from outside Natron (default 300)
        BENCH_OPEN_LIMIT_S      the same for one that opens the file (default 1800)
        BENCH_MEM_FLOOR_GB, BENCH_MEM_LIMIT_GB, BENCH_SWAP_GROWTH_GB  see heavy_scene_watchdog.sh
        BENCH_RESULT     JSON-lines file the results are appended to
                         (default $NATRON_GUI_TEST_OUT/heavy_scene_bench.jsonl)

Each scenario runs one action under a 10 ms heartbeat on the main thread and prints one JSON line:
the longest gap between heartbeats, the layers loaded by the action, and the action's own numbers,
each judged against the design's budgets as pass, fail, not_valid (measured, but not a number the
display can vouch for) or not_applicable. Actions are the ACTIONS table.

Every record carries gl_renderer and gl_vendor, and the memory the process has used: peak_rss_mb and
peak_rss_gb (its high-water mark) and mem_available_min_gb (the lowest MemAvailable during the
scenario). result is pass or fail, or one of: skipped (with a reason; nothing ran, so its peak_rss_mb
is the process's so far, or null when the watchdog wrote it), timed_out (past its wall-clock limit),
out_of_memory (the memory guard ended the process), aborted (the process ended on its own during the
scenario). heavy_scene_watchdog.sh writes those last three from outside Natron, so a hung run leaves
a record.

Budgets: main-thread stall under 100 ms; first frame under 2 s on a scene with payloads; no payload
file opened that the action did not request; orbit at or above 30 fps.
"""
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STALL_BUDGET_MS = 100.0
FIRST_FRAME_BUDGET_MS = 2000.0
ORBIT_BUDGET_FPS = 30.0
HEARTBEAT_MS = 10
TIMEOUT_S = float(os.environ.get("BENCH_TIMEOUT_S", "600"))
SCENARIO_LIMIT_S = int(os.environ.get("BENCH_SCENARIO_LIMIT_S", "300"))
OPEN_LIMIT_S = int(os.environ.get("BENCH_OPEN_LIMIT_S", "1800"))
PICK_LIMIT_S = int(os.environ.get("BENCH_PICK_LIMIT_S", "900"))
OPEN_SCENARIOS = ("open",)
SOFTWARE_RENDERERS = ("llvmpipe", "softpipe", "swrast")
ROW_RECORD_KEYS = ("root", "prims", "gprims", "instances", "prototypes", "payload_rows", "load_building",
                   "load_block", "twirl_row", "pick_s")

try:
    from PySide6.QtCore import QEvent, QMetaObject, QModelIndex, QPersistentModelIndex, QPoint, QPointF, Q_ARG, QTimer, Qt
    from PySide6.QtGui import QMouseEvent, QOffscreenSurface, QOpenGLContext
    from PySide6.QtWidgets import QApplication, QTreeView
    import NatronEngine

    from guitest import HOST_DISPLAY, OUT, check, finish, plugin_problem, report, require, run, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3
    STATE_UNLOADED, STATE_LOADED, STATE_FAILED = 1, 3, 4
    LOADING_LABEL = "Loading..."

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

    def picker_command():
        beside_natron = os.path.join(os.path.dirname(os.path.realpath("/proc/self/exe")), "natron-python")
        # The bundle's interpreter first: it composes with the USD build the viewer will use.
        if os.access(beside_natron, os.X_OK):
            return [beside_natron, "-I"]
        return ["python3"]

    def picked_rows():
        rows_dir = os.environ.get("BENCH_ROWS_DIR", os.path.join(OUT, "rows"))
        cached = os.path.join(rows_dir, os.path.splitext(os.path.basename(path))[0] + ".json")
        if not os.path.isfile(cached) or os.path.getmtime(cached) < os.path.getmtime(path):
            picker = os.path.join(ROOT, "tools", "bench", "heavy_scene_rows.py")
            done = subprocess.run(picker_command() + [picker, path, "--out", cached],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=PICK_LIMIT_S)
            require(done.returncode == 0, "the scene's rows were picked", done.stderr.decode(errors="replace")[-2000:])
        with open(cached) as f:
            return json.load(f)

    def fixture_rows():
        if fixture == "flat":
            return {"root": "/World"}
        return {"root": "/World", "load_building": "/World/Block0/B0", "load_block": "/World/Block1", "twirl_row": "/World/Block0/B1"}

    rows = picked_rows() if fixture == "scene" else fixture_rows()
    rows["source"] = "picked" if fixture == "scene" else "fixture"
    if "BENCH_TWIRL_PATH" in os.environ:
        rows["twirl_row"] = os.environ["BENCH_TWIRL_PATH"]
        rows["source"] += "+env"
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

    class Measure(object):
        """What an action reports: its numbers."""

        def __init__(self):
            self.values = {}
            self.requested_files = []
            self.requested_layers = set()

    def action_open(m):
        t0 = time.time()
        t0_ms = int(t0 * 1000.0)
        usd.getParam("filename").setValue(path)
        require(viewer.connectInput(0, usd), "connect the Viewer to USDRead")

        def scene_frame_drawn():
            # A view with nothing synced yet also finishes frames, so the scene has to have reached it.
            synced_at = prop("natronSceneSyncedAtMs", 0)
            return shown() and synced_at >= t0_ms and prop("natronSceneLastPaintMs", 0) >= synced_at

        ok = yield ("until", scene_frame_drawn, TIMEOUT_S)
        check(ok, "open: the scene view shows the scene (%r)" % (prop("natronSceneError"),))
        painted = prop("natronSceneLastPaintMs")
        m.values["first_frame_ms"] = round((painted if painted is not None else time.time() * 1000.0) - t0 * 1000.0, 1)

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
        navigation.setValue(original)
        m.values["orbit_s"] = round(elapsed, 2)
        m.values["orbit_frames"] = len(paints)
        m.values["orbit_fps"] = round(len(paints) / elapsed, 1) if elapsed > 0 else None

    def tumbling_until(done):
        """Tumbles the view until done() holds, or the wait times out; returns (moves, longest gap in ms)."""
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
            yield ("sleep", 0)
            now = time.perf_counter()
            longest_gap = max(longest_gap, (now - last_move) * 1000.0)
            last_move = now
        mouse(QEvent.MouseButtonRelease, centre, Qt.LeftButton, Qt.NoButton)
        navigation.setValue(original)
        return moves, round(longest_gap, 1)

    def commit_watch(v):
        """A predicate that holds once a load asked for from now on has been committed and drawn."""
        edits_before = int(v.property("natronSceneLoadEdits") or 0)
        t0 = time.time() * 1000.0
        return lambda: (not v.property("natronSceneLoadsPending")
                        and int(v.property("natronSceneLoadEdits") or 0) > edits_before
                        and (v.property("natronSceneLastPaintMs") or 0) >= t0
                        and not v.property("natronSceneComposing"))

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
        committed = commit_watch(v)
        t0 = time.time() * 1000.0
        require(QMetaObject.invokeMethod(v, "requestLoad", Q_ARG("QStringList", load_paths), Q_ARG(bool, load_descendants)),
                "load: the scene view takes load requests")
        moves, longest_gap = yield from tumbling_until(committed)
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

    def panel():
        panels = widgets_of("SceneGraphPanel")
        return hold(panels[0]) if panels else None

    def tree():
        p = panel()
        return hold(p.findChild(QTreeView, "sceneGraphTree")) if p is not None else None

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
        committed = commit_watch(v)
        # Docking the panel and reading down to the row are not the twirl the stall budget is about.
        heartbeat.start()
        t0 = time.time() * 1000.0
        tree().expand(target)
        moves, longest_gap = yield from tumbling_until(lambda: state() == STATE_FAILED or (state() == STATE_LOADED and committed()))
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

    ACTIONS = {"open": action_open, "edit": action_edit, "orbit": action_orbit, "load": action_load, "twirl": action_twirl}

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

    def skip_reason(name):
        if name == "load" and not load_paths:
            return "the scene has no payloads to load"
        if name == "twirl" and not rows.get("twirl_row"):
            return "the scene has no payload row to twirl"
        return None

    scene_payload_layers = set(rows.get("payload_assets", ()))

    def is_payload_layer(identifier):
        if fixture == "scene":
            return identifier in scene_payload_layers
        return payload_mark in identifier

    def judge(m, stall_ms, new_layers):
        budgets = {"stall_ms": verdict(round(stall_ms, 1), STALL_BUDGET_MS, True)}
        if "first_frame_ms" in m.values:
            budgets["first_frame_ms"] = verdict(m.values["first_frame_ms"], FIRST_FRAME_BUDGET_MS, True, valid=gpu,
                                                applicable=(fixture == "city" or bool(rows.get("payload_rows"))))
        if new_layers is None:
            budgets["unrequested_payloads"] = {"value": None, "limit": 0, "status": "not_valid"}
        else:
            unrequested = sorted(i for i in new_layers if is_payload_layer(i) and i not in m.requested_layers
                                 and not any(os.path.basename(i).startswith(f) for f in m.requested_files))
            budgets["unrequested_payloads"] = verdict(len(unrequested), 0, True)
            budgets["unrequested_payloads"]["sample"] = [os.path.basename(i) for i in unrequested[:5]]
        if "orbit_fps" in m.values:
            budgets["orbit_fps"] = verdict(m.values["orbit_fps"], ORBIT_BUDGET_FPS, False, valid=gpu)
        return budgets

    common_fields = {
        "label": label,
        "fixture": fixture,
        "file": os.path.basename(path),
        "gpu": bool(gpu),
        "gl_renderer": gl_renderer_name,
        "gl_vendor": gl_vendor_name,
        "frame_rate": "valid" if gpu else "not valid on this display",
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
                record = dict(common_fields, scenario=name, result="skipped", reason=reason, time=time.strftime("%Y-%m-%dT%H:%M:%S"))
                record.update(memory_fields(time.time()))
                write_record(record)
                continue
            started = time.time()
            mark_running(name, names[index + 1:], started, OPEN_LIMIT_S if name in OPEN_SCENARIOS else SCENARIO_LIMIT_S)
            m = Measure()
            before = loaded_layers()
            heartbeat.start()
            yield from ACTIONS[name](m)
            yield ("sleep", 200)
            stall_ms = heartbeat.stop()
            after = loaded_layers()
            new_layers = None if before is None or after is None else after - before
            budgets = judge(m, stall_ms, new_layers)
            record = dict(common_fields)
            record.update(memory_fields(started))
            record.update({
                "scenario": name,
                "wall_s": round(time.time() - started, 1),
                "layers_before": None if before is None else len(before),
                "layers_after": None if after is None else len(after),
                "layers_loaded": None if new_layers is None else len(new_layers),
                "budgets": budgets,
                "result": "fail" if any(b["status"] == "fail" for b in budgets.values()) else "pass",
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
