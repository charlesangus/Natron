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
    Writer/encoder tasks (internalEncoderNode, Write plugins) never join one;
    they bound it, so their input counts as region output.
  * Boundary bytes of a region are the estimatedBytes of every outside task it
    reads plus every region task whose result is read outside the region or
    is a final output. Each is counted once.
  * Net gain = cpu_ms / parallelism * (1 - 1 / gpu_speedup) - boundary_bytes
    / bandwidth, in wall-clock ms, and is reported as a share of frame wall
    time. parallelism is task time per frame divided by --wall-ms-per-frame
    (1 when that is not given, which is right for a one-thread profile).
    Only regions with a positive gain count as offloaded.
  * The unified-memory scenario repeats this with --unified-bandwidth-gbps
    (0 means transfers are free).

Frames are keyed on the record's "frame" field (the render index); "time" can
hold sentinels such as -1073741824 when time-offset nodes are in the graph.

wallNs is thread time, inflated by contention when many render threads run, so
the heavy/cheap split of a multi-thread profile is unreliable. Profile with
"--setting noRenderThreads=1" and pass that file as --serial-profile (or
analyse the one-thread file directly); the multi-thread run then only supplies
the task graph and --wall-ms-per-frame.
"""

import argparse
import collections
import json
import os
import sys

WRITER_NODES = ("internalEncoderNode",)


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


def is_writer(rec):
    return rec["node"] in WRITER_NODES or "write" in rec["plugin"].lower()


def apply_serial_costs(frames, serial_frames):
    """Replaces each task's wallNs by the one-thread cost per pixel of its node."""
    ns = collections.defaultdict(float)
    px = collections.defaultdict(float)
    for tasks in serial_frames.values():
        for rec in tasks.values():
            ns[rec["node"]] += rec["wallNs"]
            px[rec["node"]] += rec["roiPixels"]
    for tasks in frames.values():
        for rec in tasks.values():
            if px.get(rec["node"], 0) > 0:
                rec["wallNs"] = ns[rec["node"]] / px[rec["node"]] * rec["roiPixels"]


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
    writers = {t for t in ids if is_writer(tasks[t])}

    def reach(start_nodes, edges):
        seen = set()
        stack = list(start_nodes)
        while stack:
            for nxt in edges[stack.pop()]:
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
        return seen

    seeds = heavy - writers
    below = reach(seeds, users)
    above = reach(seeds, deps)
    candidates = seeds | {t for t in ids - heavy - writers if t in below and t in above}

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
    """Per-input-file analysis; returns a list of dicts, one per file.

    Gains are wall-clock: region task time is divided by the parallelism."""
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
        total_ms = sum(rec["wallNs"] for _, tasks in plist for rec in tasks.values()) / 1e6
        wall_ms = args.wall_ms_per_frame * len(plist) if args.wall_ms_per_frame > 0 else total_ms
        parallelism = total_ms / wall_ms if wall_ms else 1.0
        total_ms = 0.0
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
                saved = cpu / parallelism * (1 - 1 / args.gpu_speedup)
                g1 = saved - transfer_ms(nbytes, args.bandwidth_gbps)
                g2 = saved - transfer_ms(nbytes, args.unified_bandwidth_gbps)
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
            "parallelism": parallelism,
            "wall_ms": wall_ms,
            "heavy_task_ms": heavy_ms,
            "heavy_share": heavy_ms / total_ms if total_ms else 0.0,
            "region_instances": sum(r["frames"] for r in regions_out),
            "offloaded_region_instances": offloaded_regions,
            "region_signatures": regions_out,
            "net_gain_ms": gain_pcie,
            "net_gain_unified_ms": gain_unified,
            "net_gain_share": gain_pcie / wall_ms if wall_ms else 0.0,
            "net_gain_unified_share": gain_unified / wall_ms if wall_ms else 0.0,
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
        print("total task time %.2f ms; heavy %.2f ms (%.1f%%); wall %.2f ms, parallelism %.1f"
              % (res["total_task_ms"], res["heavy_task_ms"], 100 * res["heavy_share"],
                 res["wall_ms"], res["parallelism"]))
        print("%-28s %12s %12s" % ("plugin", "ms", "heavy ms"))
        for p in res["plugins"]:
            print("%-28s %12.2f %12.2f" % (p["plugin"][:28], p["ms"], p["heavy_ms"]))
        sizes = [len(r["nodes"]) for r in res["region_signatures"] for _ in range(r["frames"])]
        print("regions: %d (sizes %s), offloaded over PCIe-like link: %d"
              % (len(sizes), sorted(sizes), res["offloaded_region_instances"]))
        for r in res["region_signatures"]:
            print("  [%s] x%d  cpu %.2f ms  boundary %.1f MB  transfer %.2f ms  "
                  "wall gain %.2f ms  wall gain(unified) %.2f ms"
                  % (", ".join(r["nodes"]), r["frames"], r["cpu_ms"],
                     r["boundary_bytes"] / 1e6, r["transfer_ms"],
                     r["net_gain_ms"], r["net_gain_unified_ms"]))
        print("net GPU gain: %.2f ms (%.1f%% of wall time); unified memory: %.2f ms (%.1f%%)"
              % (res["net_gain_ms"], 100 * res["net_gain_share"],
                 res["net_gain_unified_ms"], 100 * res["net_gain_unified_share"]))
        n = res["frames"]
        print("per frame: task %.0f ms, wall %.0f ms, parallelism %.1f, heavy share %.0f%%, "
              "%.1f region(s); saves %.0f ms (%.0f%% of wall), unified %.0f ms (%.0f%%)"
              % (res["total_task_ms"] / n, res["wall_ms"] / n, res["parallelism"],
                 100 * res["heavy_share"], res["region_instances"] / n,
                 res["net_gain_ms"] / n, 100 * res["net_gain_share"],
                 res["net_gain_unified_ms"] / n, 100 * res["net_gain_unified_share"]))


def self_test(args):
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "render-profile-sample.jsonl")
    args.heavy_ms_per_mpx = 20.0
    args.gpu_speedup = 10.0
    args.bandwidth_gbps = 25.0
    args.unified_bandwidth_gbps = 0.0
    args.wall_ms_per_frame = 0.0
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

    frames = load_records([sample])
    args.wall_ms_per_frame = 427.0 / 2
    half = analyse(frames, args)[0]
    assert close(half["parallelism"], 2.0), half["parallelism"]
    assert close(half["net_gain_ms"], (205 + 200) / 2 * 0.9 - 2 * 1.28), half["net_gain_ms"]
    assert close(half["net_gain_share"], half["net_gain_ms"] / (427.0 / 2)), half["net_gain_share"]

    def task(node, plugin, deps, ns):
        return {"plugin": plugin, "node": node, "deps": deps, "roiPixels": 1000000,
                "estimatedBytes": 1000, "wallNs": ns}
    tasks = {0: task("A", "Blur", [], 100000000),
             1: task("internalEncoderNode", "fr.inria.openfx.WriteOIIO", [0], 100000000),
             2: task("B", "Blur", [1], 100000000)}
    heavy, regions, _, _ = find_regions(tasks, 20.0)
    assert regions == [{0}, {2}] or regions == [{2}, {0}], regions
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
    ap.add_argument("--wall-ms-per-frame", type=float, default=0.0,
                    help="measured wall-clock ms per frame of the same comp; parallelism is task time "
                         "per frame divided by this (default: 1, for a one-thread profile)")
    ap.add_argument("--serial-profile", metavar="FILE", action="append", default=[],
                    help="one-thread profile (noRenderThreads=1) giving each node's own cost per pixel, "
                         "replacing the contended wallNs of the input files for classification and "
                         "region cost; may be repeated for several pids")
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
    frames = load_records(args.files)
    if args.serial_profile:
        apply_serial_costs(frames, load_records(args.serial_profile))
    results = analyse(frames, args)
    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print_report(results, args)


if __name__ == "__main__":
    main()
