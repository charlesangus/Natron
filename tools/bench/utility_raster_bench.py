# Natron Utility, Ray mode against Raster mode, on a card carrying a projected plate, seen by a camera that pans
# while the card slides and turns, over an open shutter. Each sample count renders frame 1 in both modes; a render
# is a one-frame Write of the USDRender to a PNG, so the time is the whole frame as an artist's render sees it.
# Run inside NatronRenderer; utility_raster_bench.sh runs it from an AppImage:
#
#   BENCH_OUT=results.jsonl NatronRenderer -b tools/bench/utility_raster_bench.py
#
# Graph: USDRead of a generated stage (a camera whose translation is time-sampled, a quad whose translation and
# rotation are time-sampled) into USDAssignMaterial, which binds a USDProject3D (a CheckerBoard seen through a
# static USDCamera) to the quad, into USDRenderSettings (Natron Utility, Override Shutter on, a one-frame shutter
# centred on the frame by the default Shutter Offset) into USDRender into a Write.
#
# The projector stands still while the render camera pans, so the plate, fixed in the world, smears across the
# frame; a projector moving with the camera, or a card moving alone under a static projector, renders a sharp plate
# whatever the shutter does. The card's own move blurs its silhouette against the empty background.
#
# Environment:
#   BENCH_RENDER_DIR   absolute existing directory for the stage and the PNGs, one per mode and sample count, from
#                      each pair's last timed render (default /tmp)
#   BENCH_OUT          JSON-lines file the result is appended to: one "result" record per mode and sample count, then
#                      one "ratio" record per sample count with Ray over Raster, one "blur" record per mode and
#                      one "difference" record per sample count (default none)
#   BENCH_SIDES        pixel samples per side, space separated (default "3 6"); Ray draws side*side samples a pixel
#                      and Raster side*side passes, so both modes take the same number of samples
#   BENCH_REPS         timed renders per mode and sample count (default 12)
#   BENCH_WARMUPS      untimed renders per mode and sample count first, for pipelines and device tables (default 2)
#   BENCH_SIZE         "W H" of the frame (default "1920 1080")
#   BENCH_DEVICE       Natron Utility Device, GPU or CPU (default GPU); on the CPU Raster falls back to Ray, which
#                      fails the bench, so CPU only checks the scene and the blur gate
#   BENCH_PIXEL_FILTER Natron Utility Pixel Filter label, e.g. Box (default: the renderer's default)
#   BENCH_DRY_RUN      1 writes the stage, prints the plan and renders nothing; needs no Natron
#   BENCH_BLUR_CHECK   space-separated PNG paths: prints each one's blur record and exits 3 if any shows no blur;
#                      needs no Natron
#
# A render that comes back with the "Render Mode is Raster, but the frame was rendered in Ray mode" warning, or
# that errors, is recorded with its message and makes the script exit 3, so a Ray time is never read as Raster.
# So does a last render of either mode whose plate edges or card silhouette are not smeared wider than the blur
# gate, or a Ray and a Raster image that are identical: one "blur" record per mode and sample count holds the
# measured edge widths. Exit 1 is an error in the script itself.
# Every render changes Seed, which the settings hash includes, so none is served from the node cache. The modes
# alternate render by render, so clock and thermal drift lands on both.

import json
import math
import os
import struct
import sys
import time
import zlib

DRY_RUN = os.environ.get("BENCH_DRY_RUN") == "1"
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "/tmp")
OUT = os.environ.get("BENCH_OUT", "")
SIDES = [int(s) for s in os.environ.get("BENCH_SIDES", "3 6").split()]
REPS = int(os.environ.get("BENCH_REPS", "12"))
WARMUPS = int(os.environ.get("BENCH_WARMUPS", "2"))
SIZE = tuple(int(s) for s in os.environ.get("BENCH_SIZE", "1920 1080").split())
DEVICE = os.environ.get("BENCH_DEVICE", "GPU")
BLUR_CHECK = os.environ.get("BENCH_BLUR_CHECK", "").split()
PIXEL_FILTER = os.environ.get("BENCH_PIXEL_FILTER", "")
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
MODES = ("Ray", "Raster")

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
        double shutter:open = -0.5
        double shutter:close = 0.5
        double3 xformOp:translate.timeSamples = {0: (0.5, 0, 10), 2: (-0.5, 0, 10)}
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, -4, 0), (6, -4, 0), (6, 4, 0), (-6, 4, 0)]
        double3 xformOp:translate.timeSamples = {0: (-0.6, 0, 0), 2: (0.6, 0, 0)}
        float xformOp:rotateY.timeSamples = {0: 8, 2: -8}
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
    }
}
"""

# Over the one-frame shutter at frame 1 the camera pans 0.5 units, so the plate at the card's depth moves 0.5 / 15
# of the frame width (a 24 mm lens on a 36 mm back, 10 units away): its edges smear over about W / 30 pixels, of
# which the 20 to 80 percent span is 60 percent. The card's slide and turn both carry its left and right edges the
# other way from the pan, so its silhouette smears further. A sharp edge spans one or two pixels.
# The rows cross the middles of checker squares (120 px boxes on the 1080-line plate, which fills the frame
# height), so no row runs along a horizontal edge, which would read as one long smear.
ROWS = (300 / 1080.0, 420 / 1080.0, 660 / 1080.0)
EXTREME = 0.2


def blur_gate_px(width):
    return max(3.0, width / 240.0)


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


def read_png_rows(path, rows):
    # Natron's Python has no imaging module, so the rows the blur gate reads are decoded here.
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(path + " is not a PNG")
    pos = 8
    idat = []
    header = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    width, height, depth, color, _, _, interlace = header
    if color not in (2, 6) or depth not in (8, 16) or interlace:
        raise RuntimeError("%s: colour type %d, depth %d, interlace %d is not read here" % (path, color, depth, interlace))
    channels = 4 if color == 6 else 3
    bpp = channels * depth // 8
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat))
    last = max(rows)
    prev = bytearray(stride)
    out = {}
    for y in range(last + 1):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        if kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 0xFF
        if y in rows:
            step = depth // 8
            rgba = []
            for x in range(width):
                px = [line[x * bpp + k * step] for k in range(channels)]
                rgba.append(tuple(px) if channels == 4 else tuple(px) + (255,))
            out[y] = rgba
        prev = line
    return width, height, out


def transitions(values, low, high):
    count = 0
    side = None
    for v in values:
        if v is None:
            side = None
            continue
        here = "low" if v <= low else ("high" if v >= high else None)
        if here is None:
            continue
        if side is not None and here != side:
            count += 1
        side = here
    return count


def smoothed(values, radius):
    out = []
    for i in range(len(values)):
        window = [v for v in values[max(0, i - radius):i + radius + 1] if v is not None]
        out.append(None if values[i] is None or not window else sum(window) / len(window))
    return out


def edge_width(values, low, high, radius):
    # Ray mode gives each sample its own shutter time, so a smear is dithered rather than a ramp: edges are counted
    # on a smoothed row, where the dither cannot flip a pixel from one side to the other, and the pixels between
    # the extremes on the row itself.
    edges = transitions(smoothed(values, radius), low, high)
    between = sum(1 for v in values if v is not None and low < v < high)
    return edges, between


def blur_record(path):
    with open(path, "rb") as f:
        f.seek(16)
        width, height = struct.unpack(">II", f.read(8))
    rows = sorted(set(int(height * r) for r in ROWS))
    width, height, decoded = read_png_rows(path, rows)
    radius = max(1, width // 160)
    plate = [0, 0]
    silhouette = [0, 0]
    for y in rows:
        line = decoded[y]
        edges, between = edge_width([p[3] for p in line], 8, 247, radius)
        silhouette[0] += edges
        silhouette[1] += between
        luma = [(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) if p[3] == 255 else None for p in line]
        covered = sorted(v for v in luma if v is not None)
        if not covered:
            continue
        lo = covered[len(covered) // 20]
        hi = covered[-1 - len(covered) // 20]
        if hi - lo < 20:
            continue
        span = hi - lo
        edges, between = edge_width(luma, lo + EXTREME * span, hi - EXTREME * span, radius)
        plate[0] += edges
        plate[1] += between
    gate = blur_gate_px(width)
    plate_px = float(plate[1]) / plate[0] if plate[0] else 0.0
    silhouette_px = float(silhouette[1]) / silhouette[0] if silhouette[0] else 0.0
    return {"record": "blur", "path": path, "width": width, "height": height, "rows": rows, "gate_px": round(gate, 2),
            "plate_edges": plate[0], "plate_edge_px": round(plate_px, 2), "silhouette_edges": silhouette[0],
            "silhouette_edge_px": round(silhouette_px, 2), "blurred": plate_px >= gate and silhouette_px >= gate,
            "luma": [[(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * p[3] / 255.0 for p in decoded[y]] for y in rows]}


def blur_failure(record):
    if record["blurred"]:
        return None
    return "%s: too little motion blur, plate edges %.1f px over %d edges, silhouette %.1f px over %d edges, gate %.1f px" % (
        record["path"], record["plate_edge_px"], record["plate_edges"], record["silhouette_edge_px"], record["silhouette_edges"], record["gate_px"])


def mean_difference(a, b):
    total, count = 0.0, 0
    for row_a, row_b in zip(a["luma"], b["luma"]):
        for va, vb in zip(row_a, row_b):
            total += abs(va - vb)
            count += 1
    return total / count if count else 0.0


def public(record):
    return dict((k, v) for k, v in record.items() if k != "luma")


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
    centerline = checker.getParam("centerlineWidth")
    if centerline is not None:
        centerline.set(0.0)
    camera = make("USDCamera")
    camera.getParam("translate").set(0.0, 0.0, 10.0)
    camera.getParam("focalLength").set(24.0)
    camera.getParam("horizontalAperture").set(36.0)
    camera.getParam("verticalAperture").set(20.25)
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
    app.getProjectParam("frameRange").set(1, 1)
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
    for suffix in ("Device", "RenderMode", "RasterSamples", "PixelSamples", "Seed", "OverrideShutter", "Shutter", "PixelFilter"):
        param = settings.getParam(prefix + suffix)
        if param is None:
            raise RuntimeError("the settings node has no %s parameter" % (prefix + suffix))
        knobs[suffix] = param
    return knobs


def run():
    stage = write_stage()
    settings, render, writer = build(stage)
    knobs = utility_knobs(settings)
    knobs["Device"].set(DEVICE)
    knobs["OverrideShutter"].set(True)
    knobs["Shutter"].set(1.0)
    if PIXEL_FILTER:
        knobs["PixelFilter"].set(PIXEL_FILTER)
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
                      "reps": REPS, "warmups": WARMUPS, "device": DEVICE, "pixelFilter": PIXEL_FILTER or None, "message": messages[m], "times_s": [round(t, 4) for t in times[m]]}
            record.update(summary(times[m]))
            emit(record)
            log("%-6s %2d samples: median %.3fs p95 %.3fs min %.3fs" % (m, side * side, record["median_s"], record["p95_s"], record["min_s"]))
        ray, raster = summary(times["Ray"]), summary(times["Raster"])
        ratio = {"record": "ratio", "samplesPerPixel": side * side, "median_ray_over_raster": round(ray["median_s"] / raster["median_s"], 3),
                 "p95_ray_over_raster": round(ray["p95_s"] / raster["p95_s"], 3)}
        emit(ratio)
        log("%2d samples: Ray/Raster median %.2fx p95 %.2fx" % (side * side, ratio["median_ray_over_raster"], ratio["p95_ray_over_raster"]))
        blurs = {}
        for m in MODES:
            if not os.path.isfile(paths[m]):
                continue
            blurs[m] = blur_record(paths[m])
            blurs[m].update({"mode": m, "samplesPerPixel": side * side})
            emit(public(blurs[m]))
            log("%-6s %2d samples: plate edges %.1f px, silhouette %.1f px, gate %.1f px" % (
                m, side * side, blurs[m]["plate_edge_px"], blurs[m]["silhouette_edge_px"], blurs[m]["gate_px"]))
            failure = blur_failure(blurs[m])
            if failure:
                failures.append("%s side=%d: %s" % (m, side, failure))
            if (blurs[m]["width"], blurs[m]["height"]) != SIZE:
                failures.append("%s side=%d: the image is %dx%d, not %dx%d" % (m, side, blurs[m]["width"], blurs[m]["height"], SIZE[0], SIZE[1]))
        if len(blurs) == 2:
            difference = mean_difference(blurs["Ray"], blurs["Raster"])
            emit({"record": "difference", "samplesPerPixel": side * side, "mean_abs_luma": round(difference, 4)})
            log("%2d samples: Ray and Raster differ by %.3f luma on average" % (side * side, difference))
            if difference == 0.0:
                failures.append("side=%d: the Ray and Raster images are identical" % side)
    for failure in failures:
        log("FAILED " + failure)
    return 3 if failures else 0


def check_blur(paths):
    failures = []
    for path in paths:
        record = blur_record(path)
        print(json.dumps(public(record)))
        failure = blur_failure(record)
        if failure:
            failures.append(failure)
    for failure in failures:
        log("FAILED " + failure)
    return 3 if failures else 0


def main():
    if BLUR_CHECK:
        return check_blur(BLUR_CHECK)
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    if DRY_RUN:
        log("stage " + write_stage())
        for line in plan():
            log("would render " + line + " x %d after %d warmups at %dx%d" % (REPS, WARMUPS, SIZE[0], SIZE[1]))
        return 0
    return run()


# A script that returns makes NatronRenderer render every Write over the project's range afterwards, which would
# overwrite the last PNG of a mode; leaving through os._exit skips that.
try:
    code = main()
except Exception:
    import traceback
    traceback.print_exc()
    code = 1
sys.stdout.flush()
sys.stderr.flush()
os._exit(code)
