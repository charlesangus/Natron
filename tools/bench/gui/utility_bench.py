"""Utility bench: edit-to-viewer frame time of Natron Utility in Raster mode, interactive in the viewer.

Runs inside the Natron GUI, normally through tools/bench/gui/run.sh --driver utility, which wraps
Tests/gui/run-gui-test.sh (Xvfb) or runs an AppImage on the host display. The numbers only mean
something on a hardware GPU under a real X server (run.sh --host on a GPU VM): under Xvfb the
viewer has no device hand-off and the device is lavapipe.

    Tests/gui/run-gui-test.sh tools/bench/gui/utility_bench.py [natron-binary]

Graph: USDRead of a generated stage (a camera at the origin facing a card ten units away) into
USDEditTransform on /World/Cam, into USDAssignMaterial binding a USDProject3D (a CheckerBoard seen
through a USDCamera at the origin) to the card, into USDRenderSettings (Natron Utility, Raster)
into USDRender into Viewer1, whose Full Frame is on so every frame renders the whole 1920x1080
image at full resolution whatever the zoom.

Per pass count, UTILITY_BENCH_WARMUPS untimed edits and then UTILITY_BENCH_EDITS timed ones. An
edit moves the camera sideways through the USDEditTransform to a value it has never held (or, with
UTILITY_BENCH_NUDGE=seed, changes the Seed), so no frame comes from a cache. Each edit is timed
from the knob change to the first and to the last viewer frame it brings (the viewer_frame records
of NATRON_UTILITY_PROFILE, stamped with the steady clock this script reads), the last one being
the frame the viewer settles on; the next edit waits for UTILITY_BENCH_QUIET_MS without a new frame.

Configured by the environment:
    GUI_BENCH_OUT (default $NATRON_GUI_TEST_OUT/utility_bench.jsonl), GUI_BENCH_MODE,
    GUI_BENCH_BUILD_TYPE, GUI_BENCH_GIT_SHA, GUI_BENCH_APPIMAGE, GUI_BENCH_WATCHDOG_S,
    GUI_BENCH_OP_TIMEOUT_S: as for gui_bench.py
    UTILITY_BENCH_PASSES     Raster Samples counts, comma separated (default 1,16)
    UTILITY_BENCH_EDITS      timed edits per pass count (default 30)
    UTILITY_BENCH_WARMUPS    untimed edits first (default 3)
    UTILITY_BENCH_QUIET_MS   how long without a new viewer frame ends an edit (default 400)
    UTILITY_BENCH_NUDGE      camera (default) or seed
    UTILITY_BENCH_SIZE       "W,H" of the render (default 1920,1080)
    UTILITY_BENCH_DEVICE     Natron Utility Device (default GPU)
    UTILITY_BENCH_TARGET_MS  the per-frame target (default 50) and UTILITY_BENCH_STRETCH_MS (default 33)
    NATRON_UTILITY_PROFILE   required: the renderer's profile, read back for viewer_frame and hydra_profile
    NATRON_RENDER_PROFILE    optional: the scheduler's task log, read back for the USDRender's placement

Output: one host record, then per edit a "frame" record and per pass count one "summary" record:
    first_ms, last_ms   edit to the first and to the last viewer frame of the edit
    frames              viewer frames the edit brought
    rung                the viewer's hand-off rung on the last frame
    device_color        the last frame came to the viewer as a device root
    fallback            a device root drawn from a download instead (the legacy path)
    downloads, pick_readbacks  how many device downloads and pick readbacks the edit cost
    hydra_ms, hydra     the HydraRender calls in the edit's window: total and per kind and stage
    gpu                 per kind and stage, the device time the stage's submissions took, from GPU timestamps
    placement           (summary) the USDRender tasks the scheduler ran on the gpu and on the cpu
    segments            the edit's steps from the profile's stamp and span records: to_<step> is the ms from the
                        step before to that step's end (knob, viewer_request, task_graph_build, viewer_prepare,
                        usdrender_region_start, prepare_render, hydra, usdrender_region, viewer_complete,
                        viewer_frame, viewer_paint), <span>_ms each span's total in the edit
    segment_median_ms   (summary) the median of each segment over the edits
    raster_fallback     the render warned that Raster fell back to Ray
With nvidia-smi on the PATH, nvidia_smi.csv beside the output holds the GPU's clocks and power state every 100 ms.
The summary's median_ms and p95_ms are of last_ms, the headline compare.py diffs, against the target.
Exit status is non-zero when any edit brought no viewer frame or Raster fell back to Ray.
"""
import atexit
import glob
import json
import math
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 500
POLL_MS = 5
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
NUDGE_STEP = 0.02


def env_list(name, default):
    return [v.strip() for v in os.environ.get(name, default).split(",") if v.strip()]


PASSES = [int(v) for v in env_list("UTILITY_BENCH_PASSES", "1,16")]
EDITS = int(os.environ.get("UTILITY_BENCH_EDITS", "30"))
WARMUPS = int(os.environ.get("UTILITY_BENCH_WARMUPS", "3"))
QUIET_MS = float(os.environ.get("UTILITY_BENCH_QUIET_MS", "400"))
NUDGE = os.environ.get("UTILITY_BENCH_NUDGE", "camera")
SIZE = tuple(int(v) for v in env_list("UTILITY_BENCH_SIZE", "1920,1080"))
DEVICE = os.environ.get("UTILITY_BENCH_DEVICE", "GPU")
TARGET_MS = float(os.environ.get("UTILITY_BENCH_TARGET_MS", "50"))
STRETCH_MS = float(os.environ.get("UTILITY_BENCH_STRETCH_MS", "33"))
OP_TIMEOUT_S = float(os.environ.get("GUI_BENCH_OP_TIMEOUT_S", "30"))
WATCHDOG_S = int(os.environ.get("GUI_BENCH_WATCHDOG_S", str(2 * 3600)))
PROFILE = os.environ.get("NATRON_UTILITY_PROFILE", "")
RENDER_PROFILE = os.environ.get("NATRON_RENDER_PROFILE", "")

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{
    def Camera "Cam"
    {
        float focalLength = 24
        float horizontalAperture = 36
        float verticalAperture = 20.25
        float2 clippingRange = (0.1, 200)
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, -4, -10), (6, -4, -10), (6, 4, -10), (-6, 4, -10)]
    }
}
"""

try:
    import NatronEngine
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, plugin_problem, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    OUT_PATH = os.environ.get("GUI_BENCH_OUT", os.path.join(gt.OUT, "utility_bench.jsonl"))
    _out_dir = os.path.dirname(os.path.abspath(OUT_PATH))
    if not os.path.isdir(_out_dir):
        os.makedirs(_out_dir)
    _out = open(OUT_PATH, "a")

    def emit(record):
        _out.write(json.dumps(record, sort_keys=True) + "\n")
        _out.flush()

    def mono_ms():
        # The profile stamps std::chrono::steady_clock, which is this clock on Linux.
        return time.monotonic() * 1000.0

    def cpu_model():
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
        return platform.processor() or "unknown"

    def host_record():
        return {
            "type": "host",
            "driver": "utility",
            "hostname": os.environ.get("GUI_BENCH_HOSTNAME") or socket.gethostname(),
            "cpu_model": cpu_model(),
            "cores": os.cpu_count(),
            "qt_platform": QGuiApplication.platformName(),
            "mode": os.environ.get("GUI_BENCH_MODE", "unknown"),
            "build_type": os.environ.get("GUI_BENCH_BUILD_TYPE", "unknown"),
            "git_sha": os.environ.get("GUI_BENCH_GIT_SHA", "unknown"),
            "appimage": bool(os.environ.get("GUI_BENCH_APPIMAGE")),
            "natron_version": NatronEngine.natron.getNatronVersionString(),
            "natron_gpu": os.environ.get("NATRON_GPU", ""),
            "viewer_handoff": os.environ.get("NATRON_VIEWER_HANDOFF", ""),
            "passes": PASSES,
            "edits": EDITS,
            "warmups": WARMUPS,
            "quiet_ms": QUIET_MS,
            "nudge": NUDGE,
            "size": list(SIZE),
            "device": DEVICE,
            "target_ms": TARGET_MS,
            "stretch_ms": STRETCH_MS,
            "render_profile": bool(RENDER_PROFILE),
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

    class Tail(object):
        """The JSON lines appended to the files matching a pattern since the last read."""

        def __init__(self, pattern):
            self.pattern = pattern
            self.offsets = {}

        def read_new(self):
            out = []
            for path in sorted(glob.glob(self.pattern)):
                try:
                    with open(path) as f:
                        f.seek(self.offsets.get(path, 0))
                        text = f.read()
                except OSError:
                    continue
                complete = text.rfind("\n") + 1
                self.offsets[path] = self.offsets.get(path, 0) + len(text[:complete].encode("utf-8"))
                for line in text[:complete].splitlines():
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        continue
            return out

    def median(values):
        ordered = sorted(values)
        n = len(ordered)
        if n == 0:
            return None
        return ordered[n // 2] if n % 2 else 0.5 * (ordered[n // 2 - 1] + ordered[n // 2])

    def p95(values):
        ordered = sorted(values)
        return ordered[max(0, int(math.ceil(0.95 * len(ordered))) - 1)] if ordered else None

    def rounded(value):
        return None if value is None else round(value, 3)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def make(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        return node

    def utility_knobs(settings):
        names = [str(p.getScriptName()) for p in settings.getParams()]
        mode = next((n for n in names if n.lower().startswith("natronutility") and n.endswith("RenderMode")), None)
        require(mode is not None, "the settings node has a Natron Utility Render Mode parameter")
        prefix = mode[:-len("RenderMode")]
        knobs = {}
        for suffix in ("Device", "RenderMode", "RasterSamples", "PixelSamples", "Seed"):
            knobs[suffix] = settings.getParam(prefix + suffix)
            require(knobs[suffix] is not None, "the settings node has a %s parameter" % (prefix + suffix))
        return knobs

    def build():
        stage_path = os.path.join(gt.OUT, "utility_bench.usda")
        with open(stage_path, "w") as f:
            f.write(STAGE)
        read = make("USDRead")
        # USDXformPrim would define an Xform at /World/Cam and graft the stage under it, leaving no
        # camera at that path; USDEditTransform moves the existing prim.
        rig = make("USDEditTransform")
        rig.getParam("primPattern").set("/World/Cam")
        require(rig.connectInput(0, read), "connect the USDEditTransform to the USDRead")
        checker = make("CheckerBoard")
        checker.getParam("boxSize").set(120.0, 120.0)
        projector = make("USDCamera")
        projector.getParam("translate").set(0.0, 0.0, 0.0)
        projector.getParam("focalLength").set(24.0)
        projector.getParam("horizontalAperture").set(36.0)
        projector.getParam("verticalAperture").set(20.25)
        project = make("USDProject3D")
        project.connectInput(0, checker)
        project.connectInput(1, projector)
        assign = make("USDAssignMaterial")
        assign.getParam("primPattern").set("/World/Card")
        require(assign.connectInput(0, rig), "connect the USDAssignMaterial to the USDEditTransform")
        require(assign.connectInput(1, project), "connect the USDAssignMaterial to the USDProject3D")
        settings = make("USDRenderSettings")
        require(settings.connectInput(0, assign), "connect the USDRenderSettings to the USDAssignMaterial")
        render = make("USDRender")
        require(render.connectInput(0, settings), "connect the USDRender to the USDRenderSettings")
        read.getParam("filename").setValue(stage_path)
        return rig, settings, render

    def segments_of(t0, seen, hydra, marks):
        """The edit's steps in ms, from the profile's stamps and spans: each step from the end of the one before
        it to its own end, up to the last viewer frame and the paint that shows it, plus each span's total."""
        if not seen:
            return {}
        last_frame = seen[-1]["mono_ms"]
        spans = {}
        stamps = {}
        for r in marks:
            name = r.get("name", "?")
            if r.get("record") == "stamp":
                stamps.setdefault(name, r["mono_ms"])
                continue
            if r.get("start_mono_ms", 0.0) < t0:
                continue
            spans.setdefault(name, []).append((r["start_mono_ms"], r["mono_ms"]))
        out = {}
        for name, items in spans.items():
            out[name + "_ms"] = rounded(sum(end - start for start, end in items))
        for r in hydra:
            start = r.get("start_mono_ms")
            if start is not None and start >= t0:
                spans.setdefault("hydra", []).append((start, r["mono_ms"]))
        # Each step's mark: the first one of the edit for the steps before the render, the last one before the
        # last frame for the render's own, and the first paint after it.
        points = [("knob", t0)]
        if "viewer_request" in stamps:
            points.append(("viewer_request", stamps["viewer_request"]))
        for name in ("task_graph_build", "viewer_prepare"):
            if name in spans:
                points.append((name, min(end for _, end in spans[name])))
        for name in ("usdrender_region_start", "prepare_render", "hydra", "usdrender_region", "viewer_complete"):
            key = "usdrender_region" if name == "usdrender_region_start" else name
            ended = [(start, end) for start, end in spans.get(key, []) if end <= last_frame]
            if ended:
                points.append((name, max(start for start, _ in ended) if name == "usdrender_region_start" else max(end for _, end in ended)))
        points.append(("viewer_frame", last_frame))
        painted = [end for start, end in spans.get("viewer_paint", []) if start >= last_frame]
        if painted:
            points.append(("viewer_paint", min(painted)))
        points.sort(key=lambda p: p[1])
        for (_, before), (name, at) in zip(points, points[1:]):
            out["to_" + name] = rounded(at - before)
        return out

    def summary(passes, frames, placement, failures):
        last = [f["last_ms"] for f in frames if f["last_ms"] is not None]
        first = [f["first_ms"] for f in frames if f["first_ms"] is not None]
        hydra = [f["hydra_ms"] for f in frames if f["hydra_ms"] is not None]
        stages = {}
        gpu_stages = {}
        for f in frames:
            for key, ms in f["hydra"].items():
                stages.setdefault(key, []).append(ms)
            for key, ms in f.get("gpu", {}).items():
                gpu_stages.setdefault(key, []).append(ms)
        segments = {}
        for f in frames:
            for key, ms in f.get("segments", {}).items():
                if ms is not None:
                    segments.setdefault(key, []).append(ms)
        med = median(last)
        return {
            "type": "summary",
            "op": "utility_raster_%dp" % passes,
            "topology": "utility-%dx%d" % SIZE,
            "n": passes,
            "passes": passes,
            "edits": len(frames),
            "headline": "last_ms",
            "median_ms": rounded(med),
            "p95_ms": rounded(p95(last)),
            "min_ms": rounded(min(last)) if last else None,
            "max_ms": rounded(max(last)) if last else None,
            "first_median_ms": rounded(median(first)),
            "hydra_median_ms": rounded(median(hydra)),
            "hydra_mean_ms": dict((k, round(sum(v) / len(frames), 3)) for k, v in sorted(stages.items())),
            "gpu_mean_ms": dict((k, round(sum(v) / len(frames), 3)) for k, v in sorted(gpu_stages.items())),
            "segment_median_ms": dict((k, rounded(median(v))) for k, v in sorted(segments.items())),
            "frames_per_edit": rounded(sum(f["frames"] for f in frames) / float(len(frames))) if frames else None,
            "rungs": sorted(set(f["rung"] for f in frames if f["rung"])),
            "device_color_edits": sum(1 for f in frames if f["device_color"]),
            "fallback_edits": sum(1 for f in frames if f["fallback"]),
            "downloads": sum(f["downloads"] for f in frames),
            "pick_readbacks": sum(f["pick_readbacks"] for f in frames),
            "raster_fallback_edits": sum(1 for f in frames if f["raster_fallback"]),
            "placement": placement,
            "target_ms": TARGET_MS,
            "stretch_ms": STRETCH_MS,
            "meets_target": med is not None and med <= TARGET_MS,
            "meets_stretch": med is not None and med <= STRETCH_MS,
            "failures": failures,
        }

    def sample_gpu_clocks():
        """Samples the GPU's clocks and power state every 100 ms into nvidia_smi.csv beside the output, where
        nvidia-smi exists: a frame time that moves with the clocks is the driver's, not the renderer's."""
        smi = shutil.which("nvidia-smi")
        if not smi:
            return None
        csv = open(os.path.join(_out_dir, "nvidia_smi.csv"), "w")
        sampler = subprocess.Popen([smi, "--query-gpu=clocks.gr,clocks.mem,pstate", "--format=csv", "-lms", "100"],
                                   stdout=csv, stderr=subprocess.DEVNULL)
        csv.close()

        def stop():
            if sampler.poll() is None:
                sampler.terminate()
                try:
                    sampler.wait(5)
                except subprocess.TimeoutExpired:
                    sampler.kill()

        atexit.register(stop)
        return stop

    def steps():
        emit(host_record())
        stop_sampling = sample_gpu_clocks()
        report("NOTE utility bench started; results in %s" % OUT_PATH)
        require(bool(PROFILE), "NATRON_UTILITY_PROFILE is set (run.sh --driver utility sets it)")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        # Natron launched with a script makes no default Viewer, so on run.sh --host there is none.
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: viewer_gl() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gt.close_all_panels()
        full_frame = viewer.getParam("fullFrame")
        require(full_frame is not None, "the Viewer has a Full Frame parameter")
        full_frame.set(True)

        rig, settings, render = build()
        camera = settings.getParam("camera")
        ok = yield ("until", lambda: "/World/Cam" in [str(o) for o in camera.getOptions()], 60.0)
        require(ok, "the settings' Camera menu lists /World/Cam")
        camera.set("/World/Cam")
        settings.getParam("defaultRenderer").set(UTILITY_ID)
        settings.getParam("NatronParamFormatSize").set(SIZE[0], SIZE[1])
        knobs = utility_knobs(settings)
        knobs["Device"].set(DEVICE)
        knobs["RenderMode"].set("Raster")
        knobs["PixelSamples"].set(1)

        utility = Tail(PROFILE)
        tasks = Tail(RENDER_PROFILE + ".*") if RENDER_PROFILE else None
        state = {"seq": 0}

        def nudge():
            state["seq"] += 1
            if NUDGE == "seed":
                knobs["Seed"].set(state["seq"])
            else:
                rig.getParam("translate").set(NUDGE_STEP * math.sin(state["seq"]), 0.0, 0.0)

        def edit():
            """Steps of one edit; returns its frame record without the config fields."""
            utility.read_new()
            t0 = mono_ms()
            nudge()
            seen = []
            hydra = []
            marks = []

            def drain():
                for r in utility.read_new():
                    if r.get("mono_ms", 0.0) < t0:
                        continue
                    if r.get("record") == "viewer_frame":
                        seen.append(r)
                    elif r.get("record") == "hydra_profile":
                        hydra.append(r)
                    elif r.get("record") in ("stamp", "span"):
                        marks.append(r)
                return bool(seen)

            got = yield ("until", drain, OP_TIMEOUT_S)
            while got:
                count = len(seen)
                quiet_until = mono_ms() + QUIET_MS

                def more_or_quiet():
                    drain()
                    return len(seen) > count or mono_ms() >= quiet_until

                yield ("until", more_or_quiet, QUIET_MS / 1000.0 + 1.0)
                if len(seen) == count:
                    break
            message = str(render.getPersistentMessage() or "")
            stages = {}
            gpu_stages = {}
            hydra_ms = 0.0
            for r in hydra:
                hydra_ms += r.get("total_ms", 0.0)
                for name, stage in (r.get("stages") or {}).items():
                    key = "%s.%s" % (r.get("kind", "?"), name)
                    stages[key] = stages.get(key, 0.0) + stage.get("ms", 0.0)
                    if stage.get("gpu_ms"):
                        gpu_stages[key] = gpu_stages.get(key, 0.0) + stage["gpu_ms"]
            last = seen[-1] if seen else {}
            first = seen[0] if seen else {}
            previous = state.get("counters")
            counters = (last.get("device_downloads", 0), last.get("pick_readbacks", 0)) if seen else previous
            state["counters"] = counters
            return {
                "first_ms": rounded(first["mono_ms"] - t0) if seen else None,
                "last_ms": rounded(last["mono_ms"] - t0) if seen else None,
                "frames": len(seen),
                "rung": last.get("rung"),
                "device_color": bool(last.get("device_color")),
                "fallback": any(bool(r.get("fallback")) for r in seen),
                "downloads": (counters[0] - previous[0]) if previous and counters else 0,
                "pick_readbacks": (counters[1] - previous[1]) if previous and counters else 0,
                "hydra_ms": rounded(hydra_ms) if hydra else None,
                "hydra_calls": dict((k, sum(1 for r in hydra if r.get("kind") == k)) for k in set(r.get("kind") for r in hydra)),
                "hydra": dict((k, round(v, 3)) for k, v in stages.items()),
                "gpu": dict((k, round(v, 3)) for k, v in gpu_stages.items()),
                "segments": segments_of(t0, seen, hydra, marks),
                "raster_fallback": FALLBACK_TEXT in message,
                "message": message,
            }

        require(viewer.connectInput(0, render), "connect the Viewer to the USDRender")
        for passes in PASSES:
            gt.section("raster-%dp" % passes)
            knobs["RasterSamples"].set(passes)
            yield ("sleep", SETTLE_MS)
            for _ in range(WARMUPS):
                yield from edit()
            if tasks:
                tasks.read_new()
            frames = []
            failures = []
            for i in range(EDITS):
                record = yield from edit()
                record.update({"type": "frame", "passes": passes, "edit": i, "size": list(SIZE)})
                emit(record)
                frames.append(record)
                if record["last_ms"] is None:
                    failures.append("edit %d brought no viewer frame within %g s" % (i, OP_TIMEOUT_S))
                if record["raster_fallback"]:
                    failures.append("edit %d: %s" % (i, record["message"]))
            placement = None
            if tasks:
                placement = {"gpu": 0, "cpu": 0}
                label = str(render.getScriptName())
                for t in tasks.read_new():
                    if t.get("node") in (label, str(render.getLabel())):
                        placement[t.get("device", "cpu")] = placement.get(t.get("device", "cpu"), 0) + 1
            record = summary(passes, frames, placement, failures)
            emit(record)
            report("NOTE utility raster %d passes at %dx%d: median %s ms, p95 %s ms (target %g, stretch %g), "
                   "first frame median %s ms, hydra median %s ms, rungs %s, device colour %d/%d, fallback %d, "
                   "downloads %d, placement %s" % (
                       passes, SIZE[0], SIZE[1], record["median_ms"], record["p95_ms"], TARGET_MS, STRETCH_MS,
                       record["first_median_ms"], record["hydra_median_ms"], ",".join(record["rungs"]) or "-",
                       record["device_color_edits"], len(frames), record["fallback_edits"], record["downloads"],
                       placement))
            check(not failures, "every edit at %d passes brought a Raster viewer frame (%s)" % (passes, "; ".join(failures[:3])))
        viewer.disconnectInput(0)
        if stop_sampling:
            stop_sampling()
        _out.close()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
