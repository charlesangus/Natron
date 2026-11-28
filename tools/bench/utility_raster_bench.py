# Natron Utility, Ray mode against Raster mode, on a card carrying a projected plate, seen by a camera that pans
# while the card slides and turns, over an open shutter. Each session renders frames 1 to BENCH_FRAMES one after the
# other in one mode at one pass count, each frame its own one-frame render of a Write, so every frame is timed on
# its own while the renderer keeps its state from frame to frame as an artist's sequence render does. Ray draws
# passes samples a pixel, Raster passes passes, so both modes take the same number of samples.
# Run inside NatronRenderer; utility_raster_bench.sh runs it from an AppImage:
#
#   BENCH_OUT=results.jsonl NATRON_UTILITY_PROFILE=profile.jsonl NatronRenderer -b tools/bench/utility_raster_bench.py
#
# Graph: USDRead of a generated stage (a camera whose translation is time-sampled, a quad whose translation and
# rotation are time-sampled) into USDAssignMaterial, which binds a USDProject3D (a CheckerBoard seen through a
# static USDCamera) to the quad, into USDRenderSettings (Natron Utility, Override Shutter on, a one-frame shutter
# centred on the frame by the default Shutter Offset) into USDRender into a timing Write (an uncompressed half-float
# EXR by default) and a PNG Write the blur gate reads.
#
# The projector stands still while the render camera pans, so the plate, fixed in the world, smears across the
# frame; a projector moving with the camera, or a card moving alone under a static projector, renders a sharp plate
# whatever the shutter does. The card's own move blurs its silhouette against the empty background. The moves swing
# back and forth between half frames, so every frame's shutter sees the same amount of motion.
#
# Each mode's frame times are fitted to fixed + per_pass * passes over the pass counts, separating what a frame costs
# whatever its samples from what each pass or sample adds. With NATRON_UTILITY_PROFILE set, the renderer's stage
# timings of every frame are read back and fitted the same way, stage by stage.
#
# Environment:
#   BENCH_RENDER_DIR   absolute existing directory for the stage, the EXRs and the blur gate's PNGs (default /tmp)
#   BENCH_OUT          JSON-lines file the results are appended to: one "frame" record per timed frame, one "fit"
#                      record per mode, one "stage_fit" record per mode when profiled, one "ray_per_sample" and one
#                      "speedup" record, then one "blur" record per mode and gated pass count and one "difference"
#                      record per gated pass count (default none)
#   BENCH_PASSES       pass counts, space separated, each a square as Ray takes side * side samples (default
#                      "1 4 16 64")
#   BENCH_FRAMES       consecutive frames a session renders, from frame 1 (default 6)
#   BENCH_SESSIONS     sessions per mode and pass count (default 3); the modes and pass counts alternate session by
#                      session, so clock and thermal drift lands on all of them
#   BENCH_WARMUPS      untimed renders of frame 1 per mode and pass count first, for pipelines and device tables
#                      (default 1)
#   BENCH_OUTPUT       exr (default) or png, what the timing Write encodes; png pays a 1080p PNG encode a frame
#   BENCH_BLUR_PASSES  the smallest pass count the blur gate checks (default 16); fewer samples cannot show a smear
#   BENCH_SPEEDUP_AT   the sample count the fitted Ray over Raster speedup is given at (default 32)
#   BENCH_RAY_BASELINE_MS  the Ray per-sample cost the fitted one is compared with (default 97, the RTX 3060 figure
#                      at 1920x1080 with a PNG Write), and its target, a third of it
#   BENCH_SIZE         "W H" of the frame (default "1920 1080")
#   BENCH_DEVICE       Natron Utility Device, GPU or CPU (default GPU); on the CPU Raster falls back to Ray, which
#                      fails the bench, so CPU only checks the scene and the blur gate
#   BENCH_PIXEL_FILTER Natron Utility Pixel Filter label, e.g. Box (default: the renderer's default)
#   BENCH_VIEWER       1 also runs the viewer-equivalent sessions after the others, "only" runs nothing else: Raster,
#                      colour only, BENCH_VIEWER_FRAMES consecutive frames (default 12) a session, BENCH_SESSIONS
#                      sessions at each of BENCH_VIEWER_PASSES (default "1 4"). Each frame is reported as the Write's
#                      wall time and as the wall time of the renderer's HydraRender call from the profile, which is
#                      what a viewer waits for without the graph around it or any encode; one "viewer_frame" record a
#                      frame, one "viewer" record a pass count with the medians and the per-stage means
#   NATRON_UTILITY_PROFILE  the renderer's profile, one JSON line a frame; read back when set
#   BENCH_DRY_RUN      1 writes the stage, prints the plan and renders nothing; needs no Natron
#   BENCH_BLUR_CHECK   space-separated PNG paths: prints each one's blur record and exits 3 if any shows no blur;
#                      needs no Natron
#   BENCH_PROFILE_REPORT  a profile JSON-lines file: prints its per-stage tables and fits; needs no Natron
#
# A render that comes back with the "Render Mode is Raster, but the frame was rendered in Ray mode" warning, or
# that errors, is recorded with its message and makes the script exit 3, so a Ray time is never read as Raster.
# So does a PNG of either mode at a gated pass count whose plate edges or card silhouette are not smeared wider than
# the blur gate, or a Ray and a Raster image that are identical. Exit 1 is an error in the script itself.
# Every session changes Seed, which the settings hash includes, so no frame is served from the node cache.

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
PASSES = [int(s) for s in os.environ.get("BENCH_PASSES", "1 4 16 64").split()]
FRAMES = int(os.environ.get("BENCH_FRAMES", "6"))
SESSIONS = int(os.environ.get("BENCH_SESSIONS", "3"))
WARMUPS = int(os.environ.get("BENCH_WARMUPS", "1"))
OUTPUT = os.environ.get("BENCH_OUTPUT", "exr")
BLUR_PASSES = int(os.environ.get("BENCH_BLUR_PASSES", "16"))
SPEEDUP_AT = int(os.environ.get("BENCH_SPEEDUP_AT", "32"))
RAY_BASELINE_MS = float(os.environ.get("BENCH_RAY_BASELINE_MS", "97"))
SIZE = tuple(int(s) for s in os.environ.get("BENCH_SIZE", "1920 1080").split())
DEVICE = os.environ.get("BENCH_DEVICE", "GPU")
BLUR_CHECK = os.environ.get("BENCH_BLUR_CHECK", "").split()
PROFILE_REPORT = os.environ.get("BENCH_PROFILE_REPORT", "")
PROFILE = os.environ.get("NATRON_UTILITY_PROFILE", "")
PIXEL_FILTER = os.environ.get("BENCH_PIXEL_FILTER", "")
VIEWER = os.environ.get("BENCH_VIEWER", "0")
VIEWER_FRAMES = int(os.environ.get("BENCH_VIEWER_FRAMES", "12"))
VIEWER_PASSES = [int(s) for s in os.environ.get("BENCH_VIEWER_PASSES", "1 4").split()]
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
MODES = ("Ray", "Raster")

STAGE_HEAD = """#usda 1.0
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
        double3 xformOp:translate.timeSamples = {%(camera)s}
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, -4, 0), (6, -4, 0), (6, 4, 0), (-6, 4, 0)]
        double3 xformOp:translate.timeSamples = {%(card)s}
        float xformOp:rotateY.timeSamples = {%(turn)s}
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
    }
}
"""


def stage_text(frames):
    # Each frame's shutter runs from one half frame to the next, so the samples sit there and alternate.
    times = [k + 0.5 for k in range(-1, frames + 1)]

    def sign(k):
        return 1 if k % 2 == 0 else -1

    camera = ", ".join("%g: (%g, 0, 10)" % (t, 0.25 * sign(k)) for k, t in enumerate(times, -1))
    card = ", ".join("%g: (%g, 0, 0)" % (t, -0.3 * sign(k)) for k, t in enumerate(times, -1))
    turn = ", ".join("%g: %g" % (t, 4 * sign(k)) for k, t in enumerate(times, -1))
    return STAGE_HEAD % {"camera": camera, "card": card, "turn": turn}


# Over the one-frame shutter of any frame the camera pans 0.5 units, so the plate at the card's depth moves 0.5 / 15
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
        f.write(stage_text(FRAMES))
    return path


def side_of(passes):
    side = int(round(math.sqrt(passes)))
    if passes < 1 or side * side != passes:
        raise RuntimeError("BENCH_PASSES holds %d, which is not a square: Ray draws side * side samples a pixel" % passes)
    return side


def gated(passes):
    return passes >= BLUR_PASSES


def plan():
    lines = []
    for passes in VIEWER_PASSES if VIEWER in ("1", "only") else []:
        lines.append("viewer-equivalent Raster %d passes: %d sessions of frames 1-%d, colour only, at %dx%d" % (
            passes, SESSIONS, VIEWER_FRAMES, SIZE[0], SIZE[1]))
    if VIEWER == "only":
        return lines
    for passes in PASSES:
        for m in MODES:
            lines.append("%s %d passes (Pixel Samples %d): %d sessions of frames 1-%d to %s after %d warmups at %dx%d" % (
                m, passes, side_of(passes), SESSIONS, FRAMES, OUTPUT.upper(), WARMUPS, SIZE[0], SIZE[1]))
    for passes in PASSES:
        if gated(passes):
            lines.append("blur gate: frame 1 of both modes at %d passes to PNG" % passes)
    return lines


def emit(record):
    if OUT:
        with open(OUT, "a") as f:
            f.write(json.dumps(record) + "\n")


def fit(points):
    """Least squares of y = fixed + per_pass * x; None without two distinct x."""
    if len(set(x for x, _ in points)) < 2:
        return None
    n = float(len(points))
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    sxy = sum((x - mx) * (y - my) for x, y in points)
    slope = sxy / sxx
    fixed = my - slope * mx
    total = sum((y - my) ** 2 for _, y in points)
    residual = sum((y - fixed - slope * x) ** 2 for x, y in points)
    return {"fixed": fixed, "per_pass": slope, "r2": 1.0 - residual / total if total > 0 else 1.0, "points": len(points)}


class ProfileTail(object):
    """The profile records the renderer appended since the last read."""

    def __init__(self, path):
        self.path = path
        self.hydra = []
        self.offset = os.path.getsize(path) if os.path.isfile(path) else 0

    def read_new(self):
        if not os.path.isfile(self.path):
            return []
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            data = f.read()
        # A line still being written is left for the next read.
        cut = data.rfind(b"\n") + 1
        self.offset += cut
        text = data[:cut].decode("utf-8", "replace")
        self.hydra = parse_profile(text, "hydra_profile")
        return parse_profile(text)


def parse_profile(text, kind="utility_profile"):
    records = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if record.get("record") == kind:
            records.append(record)
    return records


def frame_profile(records):
    """The finished record of one render; a frame restarted by a cancel leaves unfinished ones before it."""
    done = [r for r in records if r.get("finished")]
    return done[-1] if done else None


def stage_report(records):
    """Prints a per-stage table per mode from finished profile records and returns each mode's stage fits."""
    fits = {}
    for mode in MODES:
        mine = [r for r in records if r.get("finished") and r.get("mode") == mode]
        if not mine:
            continue
        counts = sorted(set(int(r["passes_wanted"]) for r in mine))
        stages = []
        for r in mine:
            for name in r["stages"]:
                if name not in stages:
                    stages.append(name)
        rows = [("total", lambda r: r["total_ms"]), ("outside passes", lambda r: r["outside_passes_ms"]),
                ("pass mean", lambda r: r["pass_ms"]["mean"]), ("unstaged", lambda r: r["unstaged_ms"]),
                ("submits", lambda r: float(r["submits"]))]
        rows += [(name, (lambda n: lambda r: r["stages"].get(n, {}).get("ms", 0.0))(name)) for name in stages]
        devices = sorted(set(r.get("device", "") for r in mine))
        log("%s per-stage profile, ms per frame (mean over %d frames; device %s; fixed and per pass fitted over passes)" % (
            mode, len(mine), ", ".join(devices)))
        header = "  %-20s %10s %10s %6s" % ("stage", "fixed", "per pass", "r2") + "".join(" %10s" % ("%d p" % c) for c in counts)
        log(header)
        mode_fits = {}
        for name, value in rows:
            by_count = {}
            points = []
            for r in mine:
                v = value(r)
                c = int(r["passes_wanted"])
                by_count.setdefault(c, []).append(v)
                points.append((c, v))
            f = fit(points)
            if f is not None and name not in ("pass mean",):
                mode_fits[name] = {"fixed_ms": round(f["fixed"], 3), "per_pass_ms": round(f["per_pass"], 4), "r2": round(f["r2"], 4)}
            cells = "".join(" %10.3f" % (sum(by_count[c]) / len(by_count[c])) for c in counts)
            if f is None or name == "pass mean":
                log("  %-20s %10s %10s %6s%s" % (name, "-", "-", "-", cells))
            else:
                log("  %-20s %10.3f %10.4f %6.3f%s" % (name, f["fixed"], f["per_pass"], f["r2"], cells))
        fits[mode] = mode_fits
    return fits


def make(plugin_id):
    node = app.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def set_if_present(node, name, value):
    param = node.getParam(name)
    if param is None:
        return False
    try:
        param.set(value)
    except Exception:
        return False
    return True


def make_writer(path, source):
    writer = app.createWriter(path)
    writer.connectInput(0, source)
    working = writer.getParam("ocioWorkingSpace")
    if working is not None:
        set_if_present(writer, "colorspace", working.get())
    if path.endswith(".exr"):
        set_if_present(writer, "compression", "none")
        set_if_present(writer, "bitDepth", "16f")
    return writer


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
    app.getProjectParam("frameRange").set(1, FRAMES)
    extension = "exr" if OUTPUT == "exr" else "png"
    timing = make_writer(os.path.join(RENDER_DIR, "utility_raster_timing.####." + extension), render)
    gate = make_writer(os.path.join(RENDER_DIR, "utility_raster_gate.png"), render)

    camera_param = settings.getParam("camera")
    deadline = time.time() + 60.0
    while time.time() < deadline and "/World/Cam" not in [str(o) for o in camera_param.getOptions()]:
        time.sleep(0.2)
    camera_param.set("/World/Cam")
    settings.getParam("defaultRenderer").set(UTILITY_ID)
    if SIZE != (1920, 1080):
        settings.getParam("NatronParamFormatSize").set(SIZE[0], SIZE[1])
    return settings, render, timing, gate


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


def timing_path(frame):
    return os.path.join(RENDER_DIR, "utility_raster_timing.%04d.%s" % (frame, "exr" if OUTPUT == "exr" else "png"))


def report_times(frames):
    """Prints per mode and pass count frame times and the fits; returns the fit of each mode in seconds."""
    fits = {}
    for mode in MODES:
        mine = [r for r in frames if r["mode"] == mode]
        for passes in PASSES:
            here = [r["seconds"] for r in mine if r["passes"] == passes]
            first = [r["seconds"] for r in mine if r["passes"] == passes and r["frame"] == 1]
            later = [r["seconds"] for r in mine if r["passes"] == passes and r["frame"] > 1]
            if not here:
                continue
            s = summary(here)
            log("%-6s %3d passes: %d frames, median %.3fs p95 %.3fs min %.3fs; frame 1 median %.3fs, frames 2+ median %s" % (
                mode, passes, len(here), s["median_s"], s["p95_s"], s["min_s"], median(first) if first else float("nan"),
                ("%.3fs" % median(later)) if later else "-"))
        f = fit([(r["passes"], r["seconds"]) for r in mine])
        steady = fit([(r["passes"], r["seconds"]) for r in mine if r["frame"] > 1])
        if f is None:
            continue
        fits[mode] = f
        record = {"record": "fit", "mode": mode, "fixed_s": round(f["fixed"], 4), "per_pass_ms": round(1000 * f["per_pass"], 3),
                  "r2": round(f["r2"], 4), "frames": f["points"], "passes": PASSES, "output": OUTPUT, "width": SIZE[0], "height": SIZE[1]}
        if steady is not None:
            record.update({"steady_fixed_s": round(steady["fixed"], 4), "steady_per_pass_ms": round(1000 * steady["per_pass"], 3)})
        emit(record)
        log("%-6s fit: %.3fs fixed + %.2f ms per pass (r2 %.3f over %d frames)%s" % (
            mode, f["fixed"], 1000 * f["per_pass"], f["r2"], f["points"],
            "; frames 2+ only: %.3fs + %.2f ms" % (steady["fixed"], 1000 * steady["per_pass"]) if steady else ""))
    return fits


def report_targets(fits, frames):
    if "Ray" in fits:
        per_sample = 1000 * fits["Ray"]["per_pass"]
        target = RAY_BASELINE_MS / 3.0
        profiled = [r["profile_pass_mean_ms"] for r in frames if r["mode"] == "Ray" and r.get("profile_pass_mean_ms") is not None and r["passes"] == max(PASSES)]
        record = {"record": "ray_per_sample", "per_sample_ms": round(per_sample, 3), "baseline_ms": RAY_BASELINE_MS, "target_ms": round(target, 3),
                  "met": per_sample <= target, "profile_pass_mean_ms": round(median(profiled), 3) if profiled else None,
                  "width": SIZE[0], "height": SIZE[1]}
        emit(record)
        log("Ray per sample: %.2f ms fitted%s against %.1f ms before, target %.1f ms: %s" % (
            per_sample, (", %.2f ms profiled at %d samples" % (record["profile_pass_mean_ms"], max(PASSES))) if profiled else "",
            RAY_BASELINE_MS, target, "met" if record["met"] else "not met"))
    if "Ray" in fits and "Raster" in fits:
        ray = fits["Ray"]["fixed"] + fits["Ray"]["per_pass"] * SPEEDUP_AT
        raster = fits["Raster"]["fixed"] + fits["Raster"]["per_pass"] * SPEEDUP_AT
        per_pass = 1000 * fits["Raster"]["per_pass"]
        record = {"record": "speedup", "samples": SPEEDUP_AT, "ray_s": round(ray, 4), "raster_s": round(raster, 4),
                  "ray_over_raster": round(ray / raster, 3) if raster > 0 else None, "raster_per_pass_ms": round(per_pass, 3),
                  "raster_per_pass_single_digit": per_pass < 10.0}
        emit(record)
        log("at %d samples (fitted): Ray %.3fs, Raster %.3fs, Ray/Raster %.2fx; Raster %.2f ms per pass" % (
            SPEEDUP_AT, ray, raster, ray / raster if raster > 0 else float("nan"), per_pass))


def run_viewer(configure, render_frame, tail, failures):
    """The viewer-equivalent sessions: Raster frames whose wall time is also read off the HydraRender call."""
    for passes in VIEWER_PASSES:
        side_of(passes)
        for _ in range(WARMUPS):
            configure("Raster", passes)
            render_frame("Raster", passes, 1)
        walls = []
        renders = []
        stages = {}
        for session in range(SESSIONS):
            configure("Raster", passes)
            for frame in range(1, VIEWER_FRAMES + 1):
                elapsed, message, _ = render_frame("Raster", passes, frame)
                # USDRender renders through a whole-frame call on the CPU path, a device window on the GPU one.
                calls = list(tail.hydra) if tail else []
                call = calls[-1] if calls else None
                walls.append(elapsed)
                if call:
                    renders.append(call["total_ms"])
                    for name, stage in call.get("stages", {}).items():
                        stages.setdefault(name, []).append(stage.get("ms", 0.0))
                emit({"record": "viewer_frame", "passes": passes, "session": session, "frame": frame, "write_wall_ms": round(1000 * elapsed, 3),
                      "render_wall_ms": call["total_ms"] if call else None, "hydra_calls": len(calls), "width": SIZE[0], "height": SIZE[1],
                      "device": DEVICE, "message": message, "hydra": call})
            log("viewer Raster %3d passes session %d: Write %s ms" % (
                passes, session, " ".join("%.1f" % (1000 * w) for w in walls[-VIEWER_FRAMES:])))
        means = dict((name, round(sum(v) / len(walls), 3)) for name, v in stages.items())
        record = {"record": "viewer", "passes": passes, "frames": len(walls), "write_wall_median_ms": round(1000 * median(walls), 3),
                  "write_wall_p95_ms": round(1000 * percentile(walls, 95), 3),
                  "render_wall_median_ms": round(median(renders), 3) if renders else None,
                  "render_wall_p95_ms": round(percentile(renders, 95), 3) if renders else None,
                  "stage_mean_ms": means, "width": SIZE[0], "height": SIZE[1]}
        emit(record)
        log("viewer Raster %3d passes: Write median %.1f ms (p95 %.1f); HydraRender call median %s ms (p95 %s) over %d frames" % (
            passes, record["write_wall_median_ms"], record["write_wall_p95_ms"],
            "%.1f" % record["render_wall_median_ms"] if renders else "-", "%.1f" % record["render_wall_p95_ms"] if renders else "-", len(walls)))
        for name in sorted(means, key=lambda n: -means[n]):
            log("  %-20s %10.3f ms a frame" % (name, means[name]))
        if tail and not renders:
            failures.append("viewer %d passes: no hydra_profile record came back; this build does not time its HydraRender calls" % passes)


def run():
    for passes in PASSES:
        side_of(passes)
    stage = write_stage()
    settings, render, timing, gate = build(stage)
    knobs = utility_knobs(settings)
    knobs["Device"].set(DEVICE)
    knobs["OverrideShutter"].set(True)
    knobs["Shutter"].set(1.0)
    if PIXEL_FILTER:
        knobs["PixelFilter"].set(PIXEL_FILTER)
    tail = ProfileTail(PROFILE) if PROFILE else None
    state = {"seed": 0}
    failures = []

    def configure(mode, passes):
        state["seed"] += 1
        knobs["RenderMode"].set(mode)
        knobs["PixelSamples"].set(side_of(passes))
        knobs["RasterSamples"].set(passes)
        knobs["Seed"].set(state["seed"])

    def render_frame(writer, out_path, mode, passes, frame):
        if os.path.isfile(out_path):
            os.remove(out_path)
        t0 = time.time()
        app.render(writer, frame, frame)
        elapsed = time.time() - t0
        message = str(render.getPersistentMessage() or "")
        if not os.path.isfile(out_path) or (mode == "Raster" and FALLBACK_TEXT in message):
            failures.append("%s %d passes frame %d: %s" % (mode, passes, frame, message or "no image written"))
        return elapsed, message, tail.read_new() if tail else []

    if VIEWER == "only":
        run_viewer(configure, lambda m, passes, frame: render_frame(timing, timing_path(frame), m, passes, frame), tail, failures)
        for failure in failures:
            log("FAILED " + failure)
        return 3 if failures else 0

    for passes in PASSES:
        for m in MODES:
            for _ in range(WARMUPS):
                configure(m, passes)
                render_frame(timing, timing_path(1), m, passes, 1)

    frames = []
    timed_profiles = []
    for session in range(SESSIONS):
        order = PASSES if session % 2 == 0 else list(reversed(PASSES))
        for passes in order:
            for m in (MODES if session % 2 == 0 else tuple(reversed(MODES))):
                configure(m, passes)
                for frame in range(1, FRAMES + 1):
                    elapsed, message, profiles = render_frame(timing, timing_path(frame), m, passes, frame)
                    profile = frame_profile(profiles)
                    record = {"record": "frame", "mode": m, "passes": passes, "session": session, "frame": frame, "seconds": round(elapsed, 4),
                              "seed": state["seed"], "output": OUTPUT, "width": SIZE[0], "height": SIZE[1], "device": DEVICE,
                              "pixelFilter": PIXEL_FILTER or None, "message": message, "profiles": len(profiles),
                              "profile_total_ms": profile["total_ms"] if profile else None,
                              "profile_pass_mean_ms": profile["pass_ms"]["mean"] if profile else None,
                              "profile_device": profile.get("device") if profile else None}
                    frames.append(record)
                    record = dict(record)
                    if profile:
                        record["profile"] = profile
                        timed_profiles.append(profile)
                    emit(record)
                log("%-6s %3d passes session %d: %s" % (m, passes, session, " ".join("%.3f" % r["seconds"] for r in frames[-FRAMES:])))
                profiled = [r for r in frames[-FRAMES:] if r["profile_total_ms"] is not None]
                if tail and len(profiled) < FRAMES:
                    log("%-6s %3d passes session %d: %d of %d frames came back without a profile record" % (m, passes, session, FRAMES - len(profiled), FRAMES))

    fits = report_times(frames)
    report_targets(fits, frames)
    if tail:
        if timed_profiles:
            stage_fits = stage_report(timed_profiles)
            for mode, by_stage in stage_fits.items():
                emit({"record": "stage_fit", "mode": mode, "stages": by_stage})
        else:
            log("no profile records came back: this build has no NATRON_UTILITY_PROFILE support")

    if VIEWER == "1":
        run_viewer(configure, lambda m, passes, frame: render_frame(timing, timing_path(frame), m, passes, frame), tail, failures)

    for passes in PASSES:
        if not gated(passes):
            continue
        blurs = {}
        for m in MODES:
            path = os.path.join(RENDER_DIR, "utility_raster_%s_%d.png" % (m.lower(), passes))
            configure(m, passes)
            gate.getParam("filename").setValue(path)
            render_frame(gate, path, m, passes, 1)
            if not os.path.isfile(path):
                continue
            blurs[m] = blur_record(path)
            blurs[m].update({"mode": m, "passes": passes})
            emit(public(blurs[m]))
            log("%-6s %3d passes: plate edges %.1f px, silhouette %.1f px, gate %.1f px" % (
                m, passes, blurs[m]["plate_edge_px"], blurs[m]["silhouette_edge_px"], blurs[m]["gate_px"]))
            failure = blur_failure(blurs[m])
            if failure:
                failures.append("%s %d passes: %s" % (m, passes, failure))
            if (blurs[m]["width"], blurs[m]["height"]) != SIZE:
                failures.append("%s %d passes: the image is %dx%d, not %dx%d" % (m, passes, blurs[m]["width"], blurs[m]["height"], SIZE[0], SIZE[1]))
        if len(blurs) == 2:
            difference = mean_difference(blurs["Ray"], blurs["Raster"])
            emit({"record": "difference", "passes": passes, "mean_abs_luma": round(difference, 4)})
            log("%3d passes: Ray and Raster differ by %.3f luma on average" % (passes, difference))
            if difference == 0.0:
                failures.append("%d passes: the Ray and Raster images are identical" % passes)
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


def report_profile(path):
    with open(path) as f:
        records = parse_profile(f.read())
    if not records:
        log("%s holds no profile records" % path)
        return 1
    stage_report(records)
    for mode in MODES:
        mine = [r for r in records if r.get("finished") and r.get("mode") == mode]
        f = fit([(int(r["passes_wanted"]), r["total_ms"]) for r in mine])
        if f is not None:
            log("%-6s profiled fit: %.1f ms fixed + %.2f ms per pass (r2 %.3f over %d frames)" % (mode, f["fixed"], f["per_pass"], f["r2"], f["points"]))
    return 0


def main():
    if BLUR_CHECK:
        return check_blur(BLUR_CHECK)
    if PROFILE_REPORT:
        return report_profile(PROFILE_REPORT)
    if VIEWER not in ("0", "1", "only"):
        raise RuntimeError("BENCH_VIEWER must be 0, 1 or only, got %r" % VIEWER)
    if OUTPUT not in ("exr", "png"):
        raise RuntimeError("BENCH_OUTPUT must be exr or png, got %r" % OUTPUT)
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    if DRY_RUN:
        log("stage " + write_stage())
        for line in plan():
            log("would render " + line)
        others = 0 if VIEWER == "only" else 1
        log("%d timed frames, %d warmup and %d blur-gate renders" % (
            others * len(PASSES) * len(MODES) * SESSIONS * FRAMES + (len(VIEWER_PASSES) * SESSIONS * VIEWER_FRAMES if VIEWER != "0" else 0),
            others * len(PASSES) * len(MODES) * WARMUPS + (len(VIEWER_PASSES) * WARMUPS if VIEWER != "0" else 0),
            others * len(MODES) * len([p for p in PASSES if gated(p)])))
        return 0
    return run()


# A script that returns makes NatronRenderer render every Write over the project's range afterwards, which would
# overwrite the last images; leaving through os._exit skips that.
try:
    code = main()
except Exception:
    import traceback
    traceback.print_exc()
    code = 1
sys.stdout.flush()
sys.stderr.flush()
os._exit(code)
