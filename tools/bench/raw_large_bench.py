# Renders one camera raw file through the whole Develop RAW chain (RawPrepare, RawTemperature,
# RawHighlights, RawDemosaic, RawInputColor) to an EXR and prints the time of each render and the
# process's peak RSS. Run inside NatronRenderer; raw_large_bench.sh runs it at several thread counts:
#
#   BENCH_RAW_FILE=big.dng NatronRenderer --setting noRenderThreads=4 -b tools/bench/raw_large_bench.py
#
# Environment:
#   BENCH_RAW_FILE   the raw file (default Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2)
#   BENCH_RENDER_DIR absolute existing directory for the EXR (default /tmp); removed after each render
#   BENCH_REPS       timed renders after the untimed first one, which decodes the file (default 3)
#   BENCH_OUT        JSON-lines file the result is appended to (default none)
#   BENCH_THREADS    recorded in the result only; the thread count is the noRenderThreads setting

import glob
import json
import os
import resource
import sys
import time

import NatronEngine

RAW = os.environ.get("BENCH_RAW_FILE") or os.path.join(os.getcwd(), "Tests", "fixtures", "raw", "canon-eos-5d-mark-iii.cr2")
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "/tmp")
REPS = int(os.environ.get("BENCH_REPS", "3"))
OUT = os.environ.get("BENCH_OUT", "")
THREADS = os.environ.get("BENCH_THREADS", "")

CHAIN = (
    "fr.natron.RawPrepare",
    "fr.natron.RawTemperature",
    "fr.natron.RawHighlights",
    "fr.natron.RawDemosaic",
    "fr.natron.RawInputColor",
)


def log(msg):
    print("[raw-large] " + msg)
    sys.stdout.flush()


def peak_rss_mb():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) // 1024
    return None


def build():
    read = app.createReader(RAW)
    if read is None:
        raise RuntimeError("could not create a reader for " + RAW)
    previous = read
    nodes = {}
    for plugin_id in CHAIN:
        node = app.createNode(plugin_id)
        if node is None:
            raise RuntimeError("could not create " + plugin_id)
        node.connectInput(0, previous)
        nodes[plugin_id] = node
        previous = node
    writer = app.createWriter(os.path.join(RENDER_DIR, "raw_large_####.exr"))
    writer.connectInput(0, previous)
    working = writer.getParam("ocioWorkingSpace").get()
    writer.getParam("ocioOutputSpace").set(working)
    writer.getParam("ocioOutputSpaceSet").set(True)
    compression = writer.getParam("compression")
    if compression is not None:
        compression.set("none")
    return writer, nodes["fr.natron.RawTemperature"]


def render(writer):
    t0 = time.time()
    app.render(writer, 1, 1)
    elapsed = time.time() - t0
    for path in glob.glob(os.path.join(RENDER_DIR, "raw_large_*.exr")):
        os.remove(path)
    return elapsed


def main():
    if not os.path.isabs(RENDER_DIR) or not os.path.isdir(RENDER_DIR):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    writer, temperature = build()
    # A custom red multiplier nudged between renders changes the hash of every node below the
    # temperature, so each render redoes the chain instead of reusing the cache; the file's decode
    # stays cached, as it does while an artist tweaks a develop setting.
    temperature.getParam("mode").set("Custom")
    first = render(writer)
    log("first render (decodes the file) %.3fs" % first)
    times = []
    for k in range(1, REPS + 1):
        temperature.getParam("red").set(1.0 + 0.001 * k)
        times.append(render(writer))
        log("render %d %.3fs" % (k, times[-1]))
    best = min(times) if times else first
    log("threads=%s file=%s first_s=%.3f best_s=%.3f peak_rss_mb=%s" % (THREADS or "?", os.path.basename(RAW), first, best, peak_rss_mb()))
    if OUT:
        with open(OUT, "a") as f:
            f.write(json.dumps({"threads": THREADS, "file": RAW, "first_s": round(first, 4), "times_s": [round(t, 4) for t in times],
                                "peak_rss_mb": peak_rss_mb(), "rusage_maxrss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024}) + "\n")


main()
