#!/usr/bin/env python3
"""Estimate how much of a comp's render time could move to a GPU.

Reads the JSON-lines files written by the render scheduler when
NATRON_RENDER_PROFILE=<path> is set (the files are named <path>.<pid>).
Each line is one executed task with fields: frame, task, plugin, node, time,
view, mipmapLevel, roiPixels, components, bitDepth, deps, estimatedBytes,
wallNs, pointOp, glSupport, ok.

Method:
  * A task is heavy when wallNs / roiPixels exceeds --heavy-ms-per-mpx.
  * A cheap task joins the GPU candidate set when it lies on a dependency path
    between two heavy tasks; connected candidate tasks form a region.
  * Boundary bytes of a region are the estimatedBytes of every outside task it
    reads plus every region task whose result is read outside the region or
    is a final output. Each is counted once.
  * Net gain = cpu_ms - cpu_ms / gpu_speedup - boundary_bytes / bandwidth.
    Only regions with a positive gain count as offloaded.
  * The unified-memory scenario repeats this with --unified-bandwidth-gbps
    (0 means transfers are free).

Task time is summed over all pool threads, so shares are shares of total task
time, not of wall-clock time.
"""

import argparse
import collections
import json
import os
import sys


def load_records(paths):
    """Returns {(path, frame): {task: record}} keeping only successful tasks."""
    frames = collections.OrderedDict()
    for path in paths:
        with open(path, "r", encoding="utf-8") as handle:
            for number, line in enumerate(handle, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    key = (path, rec["frame"])
                    task = rec["task"]
                    rec["plugin"], rec["node"], rec["deps"]
                    rec["roiPixels"], rec["estimatedBytes"], rec["wallNs"]
                except (ValueError, KeyError) as err:
                    sys.exit("%s:%d: bad record (%s)" % (path, number, err))
                if not rec.get("ok", True):
                    continue
                frames.setdefault(key, {})[task] = rec
    return frames


def ms_per_mpx(rec):
    if rec["roiPixels"] <= 0:
        return 0.0
    return (rec["wallNs"] / 1e6) / (rec["roiPixels"] / 1e6)


def find_regions(tasks, threshold):
    """Returns (heavy ids, list of region id-sets) for one frame."""
    ids = set(tasks)
    deps = {t: [d for d in tasks[t]["deps"] if d in ids] for t in ids}
    users = {t: [] for t in ids}
    for t, ds in deps.items():
        for d in ds:
            users[d].append(t)
    heavy = {t for t in ids if ms_per_mpx(tasks[t]) > threshold}

    def reach(start_nodes, edges):
        seen = set()
        stack = list(start_nodes)
        while stack:
            for nxt in edges[stack.pop()]:
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
        return seen

    below = reach(heavy, users)
    above = reach(heavy, deps)
    candidates = heavy | {t for t in ids - heavy if t in below and t in above}

    regions = []
    remaining = set(candidates)
    while remaining:
        seed = remaining.pop()
        region = {seed}
        stack = [seed]
        while stack:
            cur = stack.pop()
            for nb in deps[cur] + users[cur]:
                if nb in remaining:
                    remaining.discard(nb)
                    region.add(nb)
                    stack.append(nb)
        regions.append(region)
    regions.sort(key=min)
    return heavy, regions, deps, users


def boundary_bytes(region, tasks, deps, users):
    inbound = {d for t in region for d in deps[t] if d not in region}
    outbound = {t for t in region
                if not users[t] or any(u not in region for u in users[t])}
    total = sum(tasks[t]["estimatedBytes"] for t in inbound | outbound)
    return total, len(inbound), len(outbound)


def transfer_ms(nbytes, gbps):
    if gbps <= 0:
        return 0.0
    return nbytes / (gbps * 1e9) * 1000.0


def analyse(frames, args):
    """Per-input-file analysis; returns a list of dicts, one per file."""
    by_file = collections.OrderedDict()
    for (path, frame), tasks in frames.items():
        by_file.setdefault(path, []).append((frame, tasks))

    out = []
    for path, plist in by_file.items():
        plugin_ms = collections.defaultdict(float)
        plugin_heavy_ms = collections.defaultdict(float)
        total_ms = heavy_ms = 0.0
        sigs = collections.OrderedDict()
        gain_pcie = gain_unified = 0.0
        offloaded_regions = 0
        for frame, tasks in plist:
            heavy, regions, deps, users = find_regions(tasks, args.heavy_ms_per_mpx)
            for t, rec in tasks.items():
                ms = rec["wallNs"] / 1e6
                total_ms += ms
                plugin_ms[rec["plugin"]] += ms
                if t in heavy:
                    heavy_ms += ms
                    plugin_heavy_ms[rec["plugin"]] += ms
            for region in regions:
                cpu = sum(tasks[t]["wallNs"] for t in region) / 1e6
                nbytes, n_in, n_out = boundary_bytes(region, tasks, deps, users)
                gpu = cpu / args.gpu_speedup
                g1 = cpu - gpu - transfer_ms(nbytes, args.bandwidth_gbps)
                g2 = cpu - gpu - transfer_ms(nbytes, args.unified_bandwidth_gbps)
                if g1 > 0:
                    gain_pcie += g1
                    offloaded_regions += 1
                if g2 > 0:
                    gain_unified += g2
                sig = tuple(sorted(tasks[t]["node"] for t in region))
                agg = sigs.setdefault(sig, {
                    "nodes": list(sig), "frames": 0, "cpu_ms": 0.0,
                    "boundary_bytes": 0, "transfer_ms": 0.0,
                    "net_gain_ms": 0.0, "net_gain_unified_ms": 0.0})
                agg["frames"] += 1
                agg["cpu_ms"] += cpu
                agg["boundary_bytes"] += nbytes
                agg["transfer_ms"] += transfer_ms(nbytes, args.bandwidth_gbps)
                agg["net_gain_ms"] += g1
                agg["net_gain_unified_ms"] += g2
        regions_out = sorted(sigs.values(), key=lambda r: -r["cpu_ms"])
        out.append({
            "file": path,
            "frames": len(plist),
            "total_task_ms": total_ms,
            "heavy_task_ms": heavy_ms,
            "heavy_share": heavy_ms / total_ms if total_ms else 0.0,
            "region_instances": sum(r["frames"] for r in regions_out),
            "offloaded_region_instances": offloaded_regions,
            "region_signatures": regions_out,
            "net_gain_ms": gain_pcie,
            "net_gain_unified_ms": gain_unified,
            "net_gain_share": gain_pcie / total_ms if total_ms else 0.0,
            "net_gain_unified_share": gain_unified / total_ms if total_ms else 0.0,
            "plugins": sorted(
                ({"plugin": p, "ms": ms, "heavy_ms": plugin_heavy_ms.get(p, 0.0)}
                 for p, ms in plugin_ms.items()), key=lambda r: -r["ms"]),
        })
    return out


def print_report(results, args):
    print("heavy > %g ms/Mpx, GPU speedup %gx, bandwidth %g GB/s, unified %s"
          % (args.heavy_ms_per_mpx, args.gpu_speedup, args.bandwidth_gbps,
             "free" if args.unified_bandwidth_gbps <= 0
             else "%g GB/s" % args.unified_bandwidth_gbps))
    for res in results:
        print("\n== %s (%d frame(s))" % (res["file"], res["frames"]))
        print("total task time %.2f ms; heavy %.2f ms (%.1f%%)"
              % (res["total_task_ms"], res["heavy_task_ms"], 100 * res["heavy_share"]))
        print("%-28s %12s %12s" % ("plugin", "ms", "heavy ms"))
        for p in res["plugins"]:
            print("%-28s %12.2f %12.2f" % (p["plugin"][:28], p["ms"], p["heavy_ms"]))
        sizes = [len(r["nodes"]) for r in res["region_signatures"] for _ in range(r["frames"])]
        print("regions: %d (sizes %s), offloaded over PCIe-like link: %d"
              % (len(sizes), sorted(sizes), res["offloaded_region_instances"]))
        for r in res["region_signatures"]:
            print("  [%s] x%d  cpu %.2f ms  boundary %.1f MB  transfer %.2f ms  "
                  "gain %.2f ms  gain(unified) %.2f ms"
                  % (", ".join(r["nodes"]), r["frames"], r["cpu_ms"],
                     r["boundary_bytes"] / 1e6, r["transfer_ms"],
                     r["net_gain_ms"], r["net_gain_unified_ms"]))
        print("net GPU gain: %.2f ms (%.1f%% of task time); unified memory: %.2f ms (%.1f%%)"
              % (res["net_gain_ms"], 100 * res["net_gain_share"],
                 res["net_gain_unified_ms"], 100 * res["net_gain_unified_share"]))


def self_test(args):
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "render-profile-sample.jsonl")
    args.heavy_ms_per_mpx = 20.0
    args.gpu_speedup = 10.0
    args.bandwidth_gbps = 25.0
    args.unified_bandwidth_gbps = 0.0
    res = analyse(load_records([sample]), args)[0]

    def close(a, b):
        return abs(a - b) < 1e-6

    assert close(res["total_task_ms"], 427.0), res["total_task_ms"]
    assert close(res["heavy_task_ms"], 400.0), res["heavy_task_ms"]
    regions = {tuple(r["nodes"]): r for r in res["region_signatures"]}
    assert set(regions) == {("Blur1", "Blur2", "Grade1"), ("Defocus1",)}, regions.keys()
    a = regions[("Blur1", "Blur2", "Grade1")]
    b = regions[("Defocus1",)]
    assert close(a["cpu_ms"], 205.0) and a["boundary_bytes"] == 32000000
    assert close(a["net_gain_ms"], 205 - 20.5 - 1.28), a["net_gain_ms"]
    assert close(b["net_gain_ms"], 200 - 20 - 1.28), b["net_gain_ms"]
    assert close(res["net_gain_unified_ms"], 184.5 + 180.0), res["net_gain_unified_ms"]
    assert close(res["net_gain_ms"], 183.22 + 178.72), res["net_gain_ms"]
    print("self-test passed")


def main():
    ap = argparse.ArgumentParser(
        description="Find GPU-candidate regions in render profile JSON-lines "
                    "files and estimate the net gain of offloading them.")
    ap.add_argument("files", nargs="*",
                    help="profile files (NATRON_RENDER_PROFILE=<path> writes <path>.<pid>)")
    ap.add_argument("--heavy-ms-per-mpx", type=float, default=20.0,
                    help="a task slower than this many ms per megapixel of RoI is heavy (default 20)")
    ap.add_argument("--bandwidth-gbps", type=float, default=25.0,
                    help="host<->GPU transfer bandwidth in GB/s (default 25)")
    ap.add_argument("--unified-bandwidth-gbps", type=float, default=0.0,
                    help="bandwidth for the unified-memory scenario in GB/s; 0 means free (default 0)")
    ap.add_argument("--gpu-speedup", type=float, default=10.0,
                    help="assumed GPU speedup over the CPU for the nodes inside a region (default 10)")
    ap.add_argument("--json", action="store_true", help="print machine-readable JSON instead of text")
    ap.add_argument("--self-test", action="store_true",
                    help="check the analysis against render-profile-sample.jsonl and exit")
    args = ap.parse_args()

    if args.self_test:
        self_test(args)
        return
    if not args.files:
        ap.error("no input files")
    if args.gpu_speedup <= 0 or args.bandwidth_gbps <= 0:
        ap.error("--gpu-speedup and --bandwidth-gbps must be positive")
    results = analyse(load_records(args.files), args)
    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print_report(results, args)


if __name__ == "__main__":
    main()
