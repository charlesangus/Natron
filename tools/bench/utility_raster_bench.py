# Natron Utility, Ray mode against Raster mode, on a card carrying a projected plate that slides and turns over an
# open shutter. Each sample count renders in both modes on the GPU; a render is a Write of the USDRender to a PNG,
# so the time is the whole frame as an artist's render sees it. Run inside NatronRenderer; utility_raster_bench.sh
# runs it from an AppImage:
#
#   BENCH_OUT=results.jsonl NatronRenderer -b tools/bench/utility_raster_bench.py
#
# Graph: USDRead of a generated stage (camera with an open shutter, a quad whose translation and rotation are
# time-sampled) into USDAssignMaterial, which binds a USDProject3D (a CheckerBoard seen through a USDCamera) to the
# quad, into USDRenderSettings (Natron Utility, Device GPU) into USDRender into a Write.
#
# Environment:
#   BENCH_RENDER_DIR   absolute existing directory for the stage and the PNGs, one per mode and sample count, from
#                      each pair's last timed render (default /tmp)
#   BENCH_OUT          JSON-lines file the result is appended to: one "result" record per mode and sample count, then
#                      one "ratio" record per sample count with Ray over Raster (default none)
#   BENCH_SIDES        pixel samples per side, space separated (default "3 6"); Ray draws side*side samples a pixel
#                      and Raster side*side passes, so both modes take the same number of samples
#   BENCH_REPS         timed renders per mode and sample count (default 12)
#   BENCH_WARMUPS      untimed renders per mode and sample count first, for pipelines and device tables (default 2)
#   BENCH_SIZE         "W H" of the frame (default "1920 1080")
#   BENCH_DRY_RUN      1 writes the stage, prints the plan and renders nothing; needs no Natron
#
# A render that comes back with the "Render Mode is Raster, but the frame was rendered in Ray mode" warning, or
# that errors, is recorded with its message and makes the script exit 3, so a Ray time is never read as Raster.
# Every render changes Seed, which the settings hash includes, so none is served from the node cache. The modes
# alternate render by render, so clock and thermal drift lands on both.

import json
import math
import os
import sys
import time

DRY_RUN = os.environ.get("BENCH_DRY_RUN") == "1"
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "/tmp")
OUT = os.environ.get("BENCH_OUT", "")
SIDES = [int(s) for s in os.environ.get("BENCH_SIDES", "3 6").split()]
REPS = int(os.environ.get("BENCH_REPS", "12"))
WARMUPS = int(os.environ.get("BENCH_WARMUPS", "2"))
SIZE = tuple(int(s) for s in os.environ.get("BENCH_SIZE", "1920 1080").split())
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
MODES = ("Ray", "Raster")

STAGE = """#usda 1.0
(
    defaultPrim = "World"
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
        double shutter:open = -0.5
        double shutter:close = 0.5
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, -4, 0), (6, -4, 0), (6, 4, 0), (-6, 4, 0)]
        double3 xformOp:translate.timeSamples = {0: (-0.6, 0, 0), 2: (0.6, 0, 0)}
        float xformOp:rotateY.timeSamples = {0: -8, 2: 8}
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
    }
}
"""


def log(msg):
    print("[utility-raster] " + msg)
    sys.stdout.flush()


def median(values):
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def percentile(values, p):
    s = sorted(values)
    return s[max(0, int(math.ceil(p * len(s))) - 1)]


def summary(values):
    return {"median_s": round(median(values), 4), "p95_s": round(percentile(values, 0.95), 4), "min_s": round(min(values), 4),
            "mean_s": round(sum(values) / len(values), 4)}


def write_stage():
    path = os.path.join(RENDER_DIR, "utility_raster_bench.usda")
    with open(path, "w") as f:
        f.write(STAGE)
    return path


def plan():
    return ["%s side=%d (%d samples)" % (m, side, side * side) for side in SIDES for m in MODES]


def emit(record):
    if OUT:
        with open(OUT, "a") as f:
            f.write(json.dumps(record) + "\n")


def make(plugin_id):
    node = app.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def build(stage):
    read = make("USDRead")
    read.getParam("filename").setValue(stage)
    checker = make("CheckerBoard")
    checker.getParam("boxSize").set(120.0, 120.0)
    checker.getParam("lineWidth").set(0.0)
    camera = make("USDCamera")
    camera.getParam("translate").set(0.0, 0.0, 10.0)
    project = make("USDProject3D")
    project.connectInput(0, checker)
    project.connectInput(1, camera)
    assign = make("USDAssignMaterial")
    assign.getParam("primPattern").set("/World/Card")
    assign.connectInput(0, read)
    assign.connectInput(1, project)
    settings = make("USDRenderSettings")
    settings.connectInput(0, assign)
    render = make("USDRender")
    render.connectInput(0, settings)
    writer = app.createWriter(os.path.join(RENDER_DIR, "utility_raster_bench.png"))
    writer.connectInput(0, render)

    camera_param = settings.getParam("camera")
    deadline = time.time() + 60.0
    while time.time() < deadline and "/World/Cam" not in [str(o) for o in camera_param.getOptions()]:
        time.sleep(0.2)
    camera_param.set("/World/Cam")
    settings.getParam("defaultRenderer").set(UTILITY_ID)
    if SIZE != (1920, 1080):
        settings.getParam("NatronParamFormatSize").set(SIZE[0], SIZE[1])
    return settings, render, writer


def utility_knobs(settings):
    names = [str(p.getScriptName()) for p in settings.getParams()]
    mode = next((n for n in names if n.lower().startswith("natronutility") and n.endswith("RenderMode")), None)
    if mode is None:
        raise RuntimeError("the settings node has no Natron Utility Render Mode parameter; this build has no Raster mode")
    prefix = mode[:-len("RenderMode")]
    knobs = {}
    for suffix in ("Device", "RenderMode", "RasterSamples", "PixelSamples", "Seed"):
        param = settings.getParam(prefix + suffix)
        if param is None:
            raise RuntimeError("the settings node has no %s parameter" % (prefix + suffix))
        knobs[suffix] = param
    return knobs


def run():
    stage = write_stage()
    settings, render, writer = build(stage)
    knobs = utility_knobs(settings)
    knobs["Device"].set("GPU")
    state = {"seed": 0}
    failures = []

    def render_once(mode, side, out_path):
        state["seed"] += 1
        knobs["RenderMode"].set(mode)
        knobs["PixelSamples"].set(side)
        knobs["RasterSamples"].set(side * side)
        knobs["Seed"].set(state["seed"])
        writer.getParam("filename").setValue(out_path)
        t0 = time.time()
        app.render(writer, 1, 1)
        elapsed = time.time() - t0
        message = str(render.getPersistentMessage() or "")
        ok = os.path.isfile(out_path)
        if not ok or (mode == "Raster" and FALLBACK_TEXT in message):
            failures.append("%s side=%d: %s" % (mode, side, message or "no image written"))
        return elapsed, message

    for side in SIDES:
        paths = {m: os.path.join(RENDER_DIR, "utility_raster_%s_%d.png" % (m.lower(), side * side)) for m in MODES}
        for _ in range(WARMUPS):
            for m in MODES:
                render_once(m, side, paths[m])
        times = {m: [] for m in MODES}
        messages = {m: "" for m in MODES}
        for _ in range(REPS):
            for m in MODES:
                elapsed, messages[m] = render_once(m, side, paths[m])
                times[m].append(elapsed)
        for m in MODES:
            record = {"record": "result", "mode": m, "pixelSamples": side, "samplesPerPixel": side * side,
                      "rasterSamples": side * side if m == "Raster" else None, "width": SIZE[0], "height": SIZE[1],
                      "reps": REPS, "warmups": WARMUPS, "device": "GPU", "message": messages[m], "times_s": [round(t, 4) for t in times[m]]}
            record.update(summary(times[m]))
            emit(record)
            log("%-6s %2d samples: median %.3fs p95 %.3fs min %.3fs" % (m, side * side, record["median_s"], record["p95_s"], record["min_s"]))
        ray, raster = summary(times["Ray"]), summary(times["Raster"])
        ratio = {"record": "ratio", "samplesPerPixel": side * side, "median_ray_over_raster": round(ray["median_s"] / raster["median_s"], 3),
                 "p95_ray_over_raster": round(ray["p95_s"] / raster["p95_s"], 3)}
        emit(ratio)
        log("%2d samples: Ray/Raster median %.2fx p95 %.2fx" % (side * side, ratio["median_ray_over_raster"], ratio["p95_ray_over_raster"]))
    for failure in failures:
        log("FAILED " + failure)
    return 3 if failures else 0


def main():
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    if DRY_RUN:
        log("stage " + write_stage())
        for line in plan():
            log("would render " + line + " x %d after %d warmups at %dx%d" % (REPS, WARMUPS, SIZE[0], SIZE[1]))
        return 0
    return run()


code = main()
if code:
    sys.exit(code)
