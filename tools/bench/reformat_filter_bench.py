# Times one 4x Reformat downscale of a CheckerBoard to an uncompressed EXR. Run inside NatronRenderer;
# reformat_filter_bench.sh runs it per build, filter and thread count:
#
#   BENCH_FILTER=lanczos3 NatronRenderer --setting noRenderThreads=8 -b tools/bench/reformat_filter_bench.py
#
# Environment:
#   BENCH_FILTER     the Reformat filter ID (default cubic)
#   BENCH_BUILD      recorded in the result only ("main", "branch")
#   BENCH_THREADS    recorded in the result only; the thread count is the noRenderThreads setting
#   BENCH_SIZE       source width and height (default "3840 2160")
#   BENCH_REPS       timed renders after the untimed first one (default 5); the best is reported
#   BENCH_RENDER_DIR absolute existing directory for the EXR (default /tmp); removed after each render
#   BENCH_OUT        JSON-lines file the result is appended to (default none)

import glob
import json
import os
import sys
import time

import NatronEngine

FILTER = os.environ.get("BENCH_FILTER", "cubic")
BUILD = os.environ.get("BENCH_BUILD", "")
THREADS = os.environ.get("BENCH_THREADS", "")
WIDTH, HEIGHT = (int(v) for v in os.environ.get("BENCH_SIZE", "3840 2160").split())
REPS = int(os.environ.get("BENCH_REPS", "5"))
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "/tmp")
OUT = os.environ.get("BENCH_OUT", "")


def log(msg):
    print("[reformat-filter] " + msg)
    sys.stdout.flush()


def result(**fields):
    fields.update({"build": BUILD, "filter": FILTER, "threads": THREADS, "width": WIDTH, "height": HEIGHT})
    if OUT:
        with open(OUT, "a") as f:
            f.write(json.dumps(fields) + "\n")


def render(writer):
    t0 = time.time()
    app.render(writer, 1, 1)
    elapsed = time.time() - t0
    for path in glob.glob(os.path.join(RENDER_DIR, "reformat_filter_*.exr")):
        os.remove(path)
    return elapsed


def main():
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    source = app.createNode("CheckerBoard")
    source.getParam("extent").set("size")
    source.getParam("bottomLeft").set(0, 0)
    source.getParam("size").set(WIDTH, HEIGHT)
    source.getParam("boxSize").set(9, 9)
    reformat = app.createNode("Reformat")
    reformat.connectInput(0, source)
    reformat.getParam("reformatType").set("scale")
    reformat.getParam("reformatScale").set(0.25, 0.25)
    choice = reformat.getParam("filter")
    if FILTER not in choice.getOptions():
        log("build=%s has no filter %s" % (BUILD, FILTER))
        result(unavailable=True)
        return
    choice.set(FILTER)
    writer = app.createWriter(os.path.join(RENDER_DIR, "reformat_filter_####.exr"))
    writer.connectInput(0, reformat)
    writer.getParam("colorspace").set(writer.getParam("ocioWorkingSpace").get())
    compression = writer.getParam("compression")
    if compression is not None:
        compression.set("none")
    first = render(writer)
    log("first render %.3fs" % first)
    color = source.getParam("color0")
    times = []
    for k in range(1, REPS + 1):
        # A nudged colour changes the hash of everything below, so each render redoes the resample.
        color.set(0.001 * k, 0.001 * k, 0.001 * k, 1.)
        times.append(render(writer))
        log("render %d %.3fs" % (k, times[-1]))
    result(first_s=round(first, 4), times_s=[round(t, 4) for t in times])


main()
