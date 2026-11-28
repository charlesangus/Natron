# Roto render throughput: one scenario per run, inside NatronRenderer from the repo root.
#
#   BENCH_ROTO_NODE=roto BENCH_N=100 BENCH_RES=uhd NatronRenderer -b tools/bench/roto_render_bench.py
#
# tools/bench/roto_render_bench.sh runs the whole matrix against an AppImage.
#
# The graph is Read (a looping checkerboard plate) -> Grade -> Roto or RotoPaint -> Write. Shape
# scenarios build N closed Beziers of 24 points through the Python roto API, scattered over the
# frame with a radius of 12% of its height, so the covered area grows with N. Keyed shapes get a
# keyframe on every rendered frame and move between them. Stroke scenarios load a RotoPaint project
# that tools/bench/gui/make_roto_strokes.py painted; their feather is 0 and they are still.
#
# Frames 1..BENCH_FRAMES render one at a time, each timed around app.render; the written EXR is
# deleted outside the timing. Then frame BENCH_FRAMES renders three more times: unchanged, after
# one item's colour changes, and after the Grade's multiply changes. One JSON line per run goes to
# stdout and BENCH_OUT. Its topo, n, res, named and settings fields are the keys tools/bench/compare.py
# pairs on, and frame_wall_med_s (the median of frames 2..BENCH_FRAMES) is its frame_s metric.
#
# Environment:
#   BENCH_ROTO_NODE         roto | rotopaint (default roto); stroke scenarios are always rotopaint
#   BENCH_ROTO_SOURCE       shapes | strokes (default shapes)
#   BENCH_N                 shapes, or the stroke count of the project to load (default 30)
#   BENCH_RES               2k (2K_DCP, 2048x1080) | uhd (UHD_4K, 3840x2160) (default uhd)
#   BENCH_ROTO_FEATHER      feather distance of every shape in pixels (default 0)
#   BENCH_ROTO_MOTION       still | keyed (default still)
#   BENCH_FRAMES            frames rendered, at least 2 (default 12)
#   BENCH_PLATE_FRAMES      frames in the plate the Read loops over (default 8)
#   BENCH_PLATES_DIR        where roto_plate_<res>.####.exr lives (default build/bench/fixtures)
#   BENCH_ROTO_STROKES_DIR  where roto_strokes_<n>_<res>.ntp lives (default build/bench/fixtures)
#   BENCH_ROTO_PLATES_ONLY  1 to write the missing plates for BENCH_RES and exit
#   BENCH_RENDER_DIR        directory the Write renders into (default build/bench/roto-render/render)
#   BENCH_SETTINGS          free text recorded as "settings", the --setting flags the run was given
#   BENCH_LABEL             free text recorded as "label"
#   BENCH_OUT               JSON-lines file the record is appended to
#   NATRON_ROTO_RENDER_PROFILE  path of the per-stage profile; a profiled run is slower, so do not
#                           compare its times with an unprofiled one
#
# NatronRenderer takes no script arguments, so the scenario is environment variables.

import glob
import json
import math
import os
import random
import statistics
import sys
import time

ROOT = os.getcwd()
NODE = os.environ.get("BENCH_ROTO_NODE", "roto")
SOURCE = os.environ.get("BENCH_ROTO_SOURCE", "shapes")
N = int(os.environ.get("BENCH_N", "30"))
RES = os.environ.get("BENCH_RES", "uhd")
FEATHER = float(os.environ.get("BENCH_ROTO_FEATHER", "0"))
MOTION = os.environ.get("BENCH_ROTO_MOTION", "still")
FRAMES = int(os.environ.get("BENCH_FRAMES", "12"))
PLATE_FRAMES = int(os.environ.get("BENCH_PLATE_FRAMES", "8"))
PLATES = os.environ.get("BENCH_PLATES_DIR") or os.path.join(ROOT, "build", "bench", "fixtures")
STROKES_DIR = os.environ.get("BENCH_ROTO_STROKES_DIR") or os.path.join(ROOT, "build", "bench", "fixtures")
PLATES_ONLY = os.environ.get("BENCH_ROTO_PLATES_ONLY", "0") == "1"
RENDER_DIR = os.path.abspath(os.environ.get("BENCH_RENDER_DIR") or os.path.join(ROOT, "build", "bench", "roto-render", "render"))
SETTINGS = os.environ.get("BENCH_SETTINGS", "")
LABEL = os.environ.get("BENCH_LABEL", "")
OUT = os.environ.get("BENCH_OUT", "")

FORMATS = {"2k": ("2K_DCP", 2048, 1080), "uhd": ("UHD_4K", 3840, 2160)}
POINTS = 24
SEED = 299


def log(msg):
    print("[roto_render_bench] " + msg)
    sys.stdout.flush()


def make(target, plugin_id):
    node = target.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def param(node, name):
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return p


def set_choice(node, name, value):
    p = param(node, name)
    p.set(value)
    shown = p.getOption(p.get())
    if shown != value:
        raise RuntimeError("%s.%s is %s after setting %s" % (node.getPluginID(), name, shown, value))


def set_format(target):
    name = FORMATS[RES][0]
    fmt = target.getProjectParam("outputFormat")
    fmt.set(name)
    if fmt.getOption(fmt.get()) != name:
        raise RuntimeError("output format is %s after setting %s" % (fmt.getOption(fmt.get()), name))


def pin_output_space(writer):
    working = param(writer, "ocioWorkingSpace").get()
    if not working:
        raise RuntimeError("%s has no working colourspace to pin the output to" % writer.getScriptName())
    param(writer, "colorspace").set(working)


def plate_files():
    return [os.path.join(PLATES, "roto_plate_%s.%04d.exr" % (RES, f)) for f in range(1, PLATE_FRAMES + 1)]


def make_plate():
    # A checkerboard compresses to almost nothing, so decoding the plate stays a small share of a frame.
    files = plate_files()
    if all(os.path.isfile(p) and os.path.getsize(p) > 0 for p in files):
        return
    os.makedirs(PLATES, exist_ok=True)
    set_format(app)
    app.getProjectParam("frameRange").set(1, PLATE_FRAMES)
    cb = make(app, "CheckerBoard")
    set_choice(cb, "extent", "project")
    color = param(cb, "color0")
    for f in range(1, PLATE_FRAMES + 1):
        color.setValueAtTime(0.1 + 0.03 * f, f, 0)
    writer = app.createWriter(os.path.join(PLATES, "roto_plate_%s.####.exr" % RES))
    writer.connectInput(0, cb)
    pin_output_space(writer)
    set_choice(writer, "bitDepth", "16f")
    app.render(writer, 1, PLATE_FRAMES)
    missing = [p for p in files if not os.path.isfile(p)]
    if missing:
        raise RuntimeError("plate render left %d frames missing, first %s" % (len(missing), missing[0]))
    writer.destroy()
    cb.destroy()
    log("wrote %d plate frames at %s" % (PLATE_FRAMES, os.path.join(PLATES, "roto_plate_%s.####.exr" % RES)))


def read_plate(target):
    if not all(os.path.isfile(p) for p in plate_files()):
        raise RuntimeError("no plate roto_plate_%s in %s; run with BENCH_ROTO_PLATES_ONLY=1 first" % (RES, PLATES))
    node = target.createReader(os.path.join(PLATES, "roto_plate_%s.####.exr" % RES))
    if node is None:
        raise RuntimeError("could not create a reader for the plate")
    for name in ("beforeFirstFrame", "afterLastFrame"):
        set_choice(node, name, "loop")
    return node


def shape_point(s, i, t, centres, radius):
    cx, cy, phase = centres[s]
    if MOTION == "keyed":
        cx += 6.0 * (t - 1)
        cy += 4.0 * math.sin(0.5 * t + phase)
    a = 2.0 * math.pi * i / POINTS
    r = radius * (1.0 + 0.15 * math.sin(3.0 * a + phase + (0.2 * t if MOTION == "keyed" else 0.0)))
    x, y = cx + r * math.cos(a), cy + r * math.sin(a)
    tx, ty = -0.1 * r * math.sin(a), 0.1 * r * math.cos(a)
    return x, y, x - tx, y - ty, x + tx, y + ty


def build_shapes(roto_node):
    ctx = roto_node.getRotoContext()
    if ctx is None:
        raise RuntimeError("%s has no Python roto context" % roto_node.getScriptName())
    _, width, height = FORMATS[RES]
    radius = 0.12 * height
    rng = random.Random(SEED + N)
    centres = [(rng.uniform(radius, width - radius), rng.uniform(radius, height - radius), rng.uniform(0.0, 2.0 * math.pi))
               for _ in range(N)]
    times = range(1, FRAMES + 1) if MOTION == "keyed" else (1,)
    shapes = []
    for s in range(N):
        x, y = shape_point(s, 0, 1, centres, radius)[:2]
        bez = ctx.createBezier(x, y, 1)
        if bez is None:
            raise RuntimeError("could not create a Bezier")
        for i in range(1, POINTS):
            bez.addControlPoint(*shape_point(s, i, 1, centres, radius)[:2])
        bez.setCurveFinished(True)
        for t in times:
            for i in range(POINTS):
                pt = shape_point(s, i, t, centres, radius)
                bez.setPointAtIndex(i, t, *pt)
                bez.setFeatherPointAtIndex(i, t, *pt)
        if FEATHER > 0:
            bez.setFeatherDistance(FEATHER, 1)
        shapes.append(bez)
    return shapes


def load_strokes():
    path = os.path.join(STROKES_DIR, "roto_strokes_%d_%s.ntp" % (N, RES))
    if not os.path.isfile(path):
        raise RuntimeError("no stroke project %s; paint it with tools/bench/gui/make_roto_strokes.py" % path)
    loaded = app.loadProject(path)
    target = loaded if loaded is not None else app
    paint = target.getNode("RotoPaint1")
    if paint is None or paint.getRotoContext() is None:
        raise RuntimeError("%s has no RotoPaint1 node" % path)
    items = list(paint.getRotoContext().getBaseLayer().getChildren())
    if len(items) != N:
        raise RuntimeError("%s holds %d strokes, not %d" % (path, len(items), N))
    return target, paint, items


def vm(field):
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(field + ":"):
                return int(line.split()[1]) // 1024
    return -1


def p95(values):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


class Renderer(object):
    def __init__(self, target, writer):
        self.target = target
        self.writer = writer
        self.written = os.path.join(RENDER_DIR, "roto_render.[0-9][0-9][0-9][0-9].exr")

    def frame(self, t):
        t0 = time.perf_counter()
        self.target.render(self.writer, t, t)
        elapsed = time.perf_counter() - t0
        produced = glob.glob(self.written)
        if not produced:
            raise RuntimeError("frame %d wrote nothing to %s" % (t, RENDER_DIR))
        for p in produced:
            os.remove(p)
        return elapsed


def set_color(item, value):
    p = item.getParam("color")
    if p is None:
        raise RuntimeError("%s has no color param" % item.getScriptName())
    for d in range(p.getNumDimensions()):
        p.setValue(value, d)


def main():
    if RES not in FORMATS:
        raise RuntimeError("BENCH_RES must be 2k or uhd, not %s" % RES)
    if PLATES_ONLY:
        make_plate()
        return
    if FRAMES < 2:
        raise RuntimeError("BENCH_FRAMES must be at least 2")
    if SOURCE not in ("shapes", "strokes") or MOTION not in ("still", "keyed") or NODE not in ("roto", "rotopaint"):
        raise RuntimeError("unknown scenario: node=%s source=%s motion=%s" % (NODE, SOURCE, MOTION))
    os.makedirs(RENDER_DIR, exist_ok=True)
    for p in glob.glob(os.path.join(RENDER_DIR, "roto_render.[0-9][0-9][0-9][0-9].exr")):
        os.remove(p)

    t0 = time.perf_counter()
    if SOURCE == "strokes":
        node, motion, feather = "rotopaint", "still", 0.0
        target, roto, items = load_strokes()
    else:
        node, motion, feather = NODE, MOTION, FEATHER
        target = app
        set_format(target)
        roto = make(target, "RotoPaint" if node == "rotopaint" else "Roto")
        items = build_shapes(roto)
    target.getProjectParam("frameRange").set(1, FRAMES)
    grade = make(target, "Grade")
    grade.connectInput(0, read_plate(target))
    if not roto.connectInput(0, grade):
        raise RuntimeError("could not connect the Grade to %s" % roto.getScriptName())
    writer = target.createWriter(os.path.join(RENDER_DIR, "roto_render.####.exr"))
    writer.connectInput(0, roto)
    pin_output_space(writer)
    compression = writer.getParam("compression")
    if compression is not None:
        compression.set("none")
    build_s = time.perf_counter() - t0

    topo = "roto_%s_%s_%s_f%g" % (node, SOURCE, motion, feather)
    log("%s n=%d res=%s: scene built in %.3fs" % (topo, N, RES, build_s))
    rss_before = vm("VmRSS")
    render = Renderer(target, writer)
    frames = [render.frame(t) for t in range(1, FRAMES + 1)]
    later = frames[1:]
    rerender = render.frame(FRAMES)
    set_color(items[0], 0.25)
    edit_color = render.frame(FRAMES)
    param(grade, "multiply").set(1.25, 1.0, 1.0, 1.0)
    edit_grade = render.frame(FRAMES)

    result = {
        "bench": "roto_render", "topo": topo, "n": N, "res": RES, "named": None, "settings": SETTINGS,
        "label": LABEL, "node": node, "source": SOURCE, "motion": motion, "feather": feather,
        "points": POINTS if SOURCE == "shapes" else None, "frames": FRAMES,
        "build_s": round(build_s, 4),
        "frame_cold_s": round(frames[0], 5),
        "frame_wall_med_s": round(statistics.median(later), 5),
        "frame_p95_s": round(p95(later), 5),
        "rerender_s": round(rerender, 5),
        "edit_color_s": round(edit_color, 5),
        "edit_grade_s": round(edit_grade, 5),
        "frames_s": [round(v, 5) for v in frames],
        "rss_before_mb": rss_before, "rss_peak_mb": vm("VmHWM"), "cpus": os.cpu_count(),
        "profiled": bool(os.environ.get("NATRON_ROTO_RENDER_PROFILE")),
    }
    line = json.dumps(result)
    print("[roto_render_bench] RESULT " + line)
    if OUT:
        with open(OUT, "a") as f:
            f.write(line + "\n")


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stdout.flush()
    os._exit(3)
sys.stdout.flush()
os._exit(0)
