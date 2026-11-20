# Times a LoopEnd in Map Blend over a driven Blur, rendered to an uncompressed EXR. Run inside
# NatronRenderer; loop_map_blend_bench.sh runs it per Map and thread count:
#
#   BENCH_MAP=half NatronRenderer --setting noRenderThreads=8 -b tools/bench/loop_map_blend_bench.py
#
# The Map is a one-pixel CheckerBoard of 0 and 1, so every tile it covers spans the whole range and
# needs every iteration. BENCH_MAP=full covers the frame with it, so nothing is culled; half
# covers the right half only, and the left half, which reads 0, needs one iteration.
#
# Environment:
#   BENCH_MAP        full or half (default half)
#   BENCH_BUILD      recorded in the result only
#   BENCH_THREADS    recorded in the result only; the thread count is the noRenderThreads setting
#   BENCH_SIZE       frame width and height (default "1920 1080")
#   BENCH_ITERATIONS the loop's iterations (default 16)
#   BENCH_REPS       timed renders after the untimed first one (default 5); the best is reported
#   BENCH_RENDER_DIR absolute existing directory for the EXR (default /tmp); removed after each render
#   BENCH_OUT        JSON-lines file the result is appended to (default none)

import glob
import json
import os
import random
import sys
import time

import NatronEngine

MAP = os.environ.get("BENCH_MAP", "half")
BUILD = os.environ.get("BENCH_BUILD", "")
THREADS = os.environ.get("BENCH_THREADS", "")
WIDTH, HEIGHT = (int(v) for v in os.environ.get("BENCH_SIZE", "1920 1080").split())
ITERATIONS = int(os.environ.get("BENCH_ITERATIONS", "16"))
REPS = int(os.environ.get("BENCH_REPS", "5"))
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "/tmp")
OUT = os.environ.get("BENCH_OUT", "")


def log(msg):
    print("[loop-map-blend] " + msg)
    sys.stdout.flush()


def result(**fields):
    fields.update({"build": BUILD, "map": MAP, "threads": THREADS, "width": WIDTH, "height": HEIGHT, "iterations": ITERATIONS})
    if OUT:
        with open(OUT, "a") as f:
            f.write(json.dumps(fields) + "\n")


def render(writer):
    t0 = time.time()
    app.render(writer, 1, 1)
    elapsed = time.time() - t0
    for path in glob.glob(os.path.join(RENDER_DIR, "loop_map_blend_*.exr")):
        os.remove(path)
    return elapsed


def checker_map():
    checker = app.createNode("CheckerBoard")
    checker.getParam("extent").set("size")
    left = WIDTH // 2 if MAP == "half" else 0
    checker.getParam("bottomLeft").set(left, 0)
    checker.getParam("size").set(WIDTH - left, HEIGHT)
    checker.getParam("boxSize").set(1, 1)
    for name, value in (("color0", 0.), ("color1", 1.), ("color2", 0.), ("color3", 1.)):
        checker.getParam(name).set(value, value, value, value)
    for name in ("lineWidth", "centerlineWidth"):
        param = checker.getParam(name)
        if param is not None:
            param.set(0)
    return checker


def main():
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    if MAP not in ("full", "half"):
        raise RuntimeError("BENCH_MAP must be full or half, got %r" % MAP)
    source = app.createNode("CheckerBoard")
    source.getParam("extent").set("size")
    source.getParam("bottomLeft").set(0, 0)
    source.getParam("size").set(WIDTH, HEIGHT)
    source.getParam("boxSize").set(32, 32)
    begin = app.createNode("LoopBegin")
    begin.connectInput(0, source)
    blur = app.createNode("Blur")
    blur.connectInput(0, begin)
    if not blur.getParam("size").setDriver("loop.t", 0., 20.):
        raise RuntimeError("could not drive the Blur's size")
    end = app.createNode("LoopEnd")
    end.connectInput(0, blur)
    end.connectInput(1, checker_map())
    end.getParam("mode").set("mapBlend")
    end.getParam("iterations").set(ITERATIONS)
    # The renderer renders the project's range once the script returns; one frame keeps that short.
    app.getProjectParam("frameRange").set(1, 1)
    writer = app.createWriter(os.path.join(RENDER_DIR, "loop_map_blend_####.exr"))
    writer.connectInput(0, end)
    writer.getParam("colorspace").set(writer.getParam("ocioWorkingSpace").get())
    compression = writer.getParam("compression")
    if compression is not None:
        compression.set("none")
    # The node cache's disk tier outlives the process, so colours repeated from an earlier run would be
    # served from it: every run draws its own.
    base = random.SystemRandom().uniform(0.1, 0.9)
    color = source.getParam("color0")
    color.set(base, base, base, 1.)
    first = render(writer)
    log("first render %.3fs" % first)
    times = []
    for k in range(1, REPS + 1):
        # A nudged colour changes the hash of everything below, so each render redoes every iteration.
        value = base + 1e-4 * k
        color.set(value, value, value, 1.)
        times.append(render(writer))
        log("render %d %.3fs" % (k, times[-1]))
    result(first_s=round(first, 4), times_s=[round(t, 4) for t in times])


main()
