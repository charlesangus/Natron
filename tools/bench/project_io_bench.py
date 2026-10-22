# Project save and load timing on the heavy roto/tracker project. Run inside NatronRenderer, after
# make_heavy_project.py has written the project:
#
#   BENCH_OUT=build/bench/results-project-io.jsonl NatronRenderer -b tools/bench/project_io_bench.py
#
# Environment:
#   BENCH_HEAVY    .ntpz to measure (default build/bench/heavy.ntpz under the working directory)
#   BENCH_OUT      JSON-lines file the result is appended to (default build/bench/results-project-io.jsonl)
#   BENCH_RUNS     timed runs per measurement, reported as the median (default 7); one untimed warm-up precedes them
#   BENCH_WORK     scratch directory for the files written (default build/bench/project-io)
#
# Three timings need App methods that are not part of the scripting API. Each is looked up by name
# and recorded as null, with a message, when the build does not expose it:
#   saveLegacyXmlForBenchmark(filename)  Project::saveLegacyXmlForBenchmark
#   autoSaveForBenchmark(filename)       Project::saveProject_imp(path, name, autoSave=true, ...), which
#                                        appends .autosave to the file name; the
#                                        scripting path cannot reach it because Project::autoSave()
#                                        returns at once in background mode

import json
import os
import re
import statistics
import sys
import time

HEAVY = os.environ.get("BENCH_HEAVY", os.path.join(os.getcwd(), "build", "bench", "heavy.ntpz"))
OUT = os.environ.get("BENCH_OUT", os.path.join(os.getcwd(), "build", "bench", "results-project-io.jsonl"))
WORK = os.environ.get("BENCH_WORK", os.path.join(os.getcwd(), "build", "bench", "project-io"))
RUNS = int(os.environ.get("BENCH_RUNS", "7"))


def log(msg):
    print("[project-io] " + msg)
    sys.stdout.flush()


def pressure_some10(name):
    try:
        with open("/proc/pressure/" + name) as f:
            for line in f:
                m = re.match(r"some .*avg10=([0-9.]+)", line)
                if m:
                    return float(m.group(1))
    except (OSError, ValueError):
        pass
    return None


def load1():
    with open("/proc/loadavg") as f:
        return float(f.read().split()[0])


def median_time(fn):
    fn()
    samples = []
    for _ in range(RUNS):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)
    return statistics.median(samples), samples


def size_of(path):
    return os.path.getsize(path) if os.path.exists(path) else None


def measure(name, method, path, written=None):
    if method is None:
        log("%s unavailable: the build does not expose it" % name)
        return {"median_s": None, "runs_s": None, "bytes": None}
    median, samples = median_time(lambda: method(path))
    return {"median_s": round(median, 6), "runs_s": [round(s, 6) for s in samples], "bytes": size_of(written or path)}


os.makedirs(WORK, exist_ok=True)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
if not os.path.exists(HEAVY):
    raise RuntimeError("%s does not exist; run make_heavy_project.py first" % HEAVY)

pressure_start = {"load1": load1(), "psi_cpu": pressure_some10("cpu"), "psi_io": pressure_some10("io")}

session = app
load_median, load_samples = median_time(lambda: app.loadProject(HEAVY))
loaded = app.loadProject(HEAVY)
if loaded is not None:
    session = loaded
log("load median %.4fs" % load_median)

ntpz_path = os.path.join(WORK, "save.ntpz")
if not session.saveProjectAs(ntpz_path):
    raise RuntimeError("could not save " + ntpz_path)
results = {
    "ntpz_save": measure("ntpz_save", session.saveProject, ntpz_path),
    "legacy_xml_save": measure("legacy_xml_save", getattr(session, "saveLegacyXmlForBenchmark", None),
                               os.path.join(WORK, "legacy.xml")),
    "autosave": measure("autosave", getattr(session, "autoSaveForBenchmark", None),
                        os.path.join(WORK, "autosave.ntpz"),
                        written=os.path.join(WORK, "autosave.ntpz.autosave")),
}

record = {
    "bench": "project_io", "heavy": os.path.basename(HEAVY), "heavy_bytes": size_of(HEAVY), "runs": RUNS,
    "load_median_s": round(load_median, 6), "load_runs_s": [round(s, 6) for s in load_samples],
    "results": results, "epoch": int(time.time()),
    "load1_start": pressure_start["load1"], "psi_cpu_start": pressure_start["psi_cpu"],
    "psi_io_start": pressure_start["psi_io"],
}
with open(OUT, "a") as f:
    f.write(json.dumps(record) + "\n")


def cell(value, scale=1000.0, fmt="%.1f"):
    return "n/a" if value is None else fmt % (value * scale)


log("%-18s %12s %14s" % ("measurement", "median ms", "bytes"))
log("%-18s %12s %14s" % ("ntpz load", cell(load_median), size_of(HEAVY)))
for key, label in (("ntpz_save", "ntpz save"), ("legacy_xml_save", "legacy xml save"), ("autosave", "autosave")):
    r = results[key]
    log("%-18s %12s %14s" % (label, cell(r["median_s"]), "n/a" if r["bytes"] is None else r["bytes"]))
log("appended to " + OUT)
# A script run with -b renders the project's writers when it ends; the exit skips that.
os._exit(0)
