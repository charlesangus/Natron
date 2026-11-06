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
        BENCH_LABEL      free text stored with each result
        BENCH_TIMEOUT_S  per-wait timeout (default 600)
        BENCH_RESULT     JSON-lines file the results are appended to
                         (default $NATRON_GUI_TEST_OUT/heavy_scene_bench.jsonl)

Each scenario runs one action under a 10 ms heartbeat on the main thread and prints one JSON line:
the longest gap between heartbeats, the layers loaded by the action, and the action's own numbers,
each judged against the design's budgets as pass, fail, not_valid (measured, but not a number the
display can vouch for) or not_applicable. Actions are the ACTIONS table; a later milestone adds its
own (twirl, reveal, draw state) by adding an entry.

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

    from guitest import HOST_DISPLAY, OUT, check, finish, report, require, run, widgets_of

    ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
    fixture = os.environ.get("BENCH_FIXTURE", "flat")
    require(fixture in ("flat", "city"), "BENCH_FIXTURE is flat or city, not %r" % fixture)
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

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead")
    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

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

    def action_open(m):
        t0 = time.time()
        usd.getParam("filename").setValue(path)
        require(viewer.connectInput(0, usd), "connect the Viewer to USDRead")
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

    def action_orbit(m):
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
        m.values["boxes_drawn"] = prop("natronSceneBoxCount")

    ACTIONS = {"open": action_open, "edit": action_edit, "orbit": action_orbit}

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
            sys.stdout.flush()
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
