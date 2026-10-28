#!/usr/bin/env python3
# Summarises the profiles tools/bench/node_profile.sh wrote: per node, the GPU kernel time per
# megapixel and per UHD image from the region records, and the CPU task time per megapixel at one
# thread and at the full thread count.
#
#   tools/bench/node_profile_summary.py [DIR] [--cpu-profiles-for node,...]
#
# DIR defaults to build/bench/m85-nodes. With --cpu-profiles-for the CPU task records of the named
# nodes are also copied, minus the source and the writer, to <node>.steady.jsonl and
# <node>-serial.steady.jsonl, the inputs render-profile-report.py reads for its CPU rows.
import argparse
import collections
import glob
import json
import os
import statistics

UHD_MPX = 3840 * 2160 / 1e6
SKIPPED_PLUGINS = ("Ramp",)


def records(path):
    with open(path, encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def is_support(rec):
    return rec["plugin"] in SKIPPED_PLUGINS or rec["plugin"].rsplit(".", 1)[-1].lower().startswith("write")


def gpu_ms_per_mpx(path):
    """Returns (median kernel ms per Mpx, median UHD ms per node, members per region, fallback regions)."""
    recs = records(path)
    tasks = collections.defaultdict(dict)
    regions = []
    for rec in recs:
        if rec.get("kind") == "region":
            regions.append(rec)
        else:
            tasks[rec["frame"]][rec["task"]] = rec
    samples = []
    members = 0
    fallbacks = 0
    for region in regions:
        if region["fallback"]:
            fallbacks += 1
            continue
        mpx = sum(tasks[region["frame"]][t]["roiPixels"] for t in region["tasks"] if t in tasks[region["frame"]]) / 1e6
        if mpx > 0:
            samples.append(region["kernelMs"] / mpx)
            members = len(region["tasks"])
    if not samples:
        return None
    median = statistics.median(samples)
    return median, median * UHD_MPX, members, fallbacks


def cpu_ms_per_mpx(path):
    samples = []
    for rec in records(path):
        if rec.get("kind") == "region" or is_support(rec) or rec.get("device") != "cpu":
            continue
        if rec["roiPixels"] > 0:
            samples.append((rec["wallNs"] / 1e6 / (rec["roiPixels"] / 1e6), rec["roiPixels"]))
    if not samples:
        return None
    samples.sort()
    half = sum(w for _, w in samples) / 2.0
    acc = 0.0
    for value, weight in samples:
        acc += weight
        if acc >= half:
            return value
    return None


def copy_cpu_records(src, dst):
    with open(src, encoding="utf-8") as handle, open(dst, "w", encoding="utf-8") as out:
        for line in handle:
            if line.strip():
                rec = json.loads(line)
                if rec.get("kind") != "region" and not is_support(rec):
                    out.write(line)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", nargs="?", default="build/bench/m85-nodes")
    parser.add_argument("--cpu-profiles-for", default="")
    args = parser.parse_args()
    nodes = sorted({os.path.basename(p).split("-", 1)[1].split(".")[0]
                    for tag in ("gpu1", "cpu", "serial") for p in glob.glob(os.path.join(args.dir, tag + "-*.steady.jsonl"))})
    print("%-14s %9s %9s %9s %9s %9s %9s %9s" % ("node", "gpu1/Mpx", "gpu1 UHD", "gpu8/Mpx", "gpu8 UHD", "cpu1/Mpx", "cpuN/Mpx", "speedup"))
    for node in nodes:
        cells = []
        for tag in ("gpu1", "gpu8"):
            path = os.path.join(args.dir, "%s-%s.steady.jsonl" % (tag, node))
            got = gpu_ms_per_mpx(path) if os.path.exists(path) else None
            cells += ["%9.4f %9.3f" % (got[0], got[1])] if got else ["%9s %9s" % ("-", "-")]
        serial = threaded = None
        spath = os.path.join(args.dir, "serial-%s.steady.jsonl" % node)
        tpath = os.path.join(args.dir, "cpu-%s.steady.jsonl" % node)
        if os.path.exists(spath):
            serial = cpu_ms_per_mpx(spath)
        if os.path.exists(tpath):
            threaded = cpu_ms_per_mpx(tpath)
        speed = serial / threaded if serial and threaded else None
        print("%-14s %s %s %9s %9s %9s" % (node, cells[0], cells[1], "%.3f" % serial if serial else "-",
                                           "%.3f" % threaded if threaded else "-", "%.3f" % speed if speed else "-"))
    for node in filter(None, args.cpu_profiles_for.split(",")):
        copy_cpu_records(os.path.join(args.dir, "cpu-%s.steady.jsonl" % node), os.path.join(args.dir, "%s.steady.jsonl" % node))
        copy_cpu_records(os.path.join(args.dir, "serial-%s.steady.jsonl" % node), os.path.join(args.dir, "%s-serial.steady.jsonl" % node))


if __name__ == "__main__":
    main()
