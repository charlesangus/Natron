#!/usr/bin/env python3
"""Estimate how much of a comp's render time could move to a GPU.

Reads the JSON-lines files written by the render scheduler when
NATRON_RENDER_PROFILE=<path> is set (the files are named <path>.<pid>), one
executed task per line. Times are milliseconds of thread time; gains are
wall-clock milliseconds, also given as a share of the frame wall time. Frames
are keyed on the record's "frame" field (the render index).

wallNs is thread time, inflated by contention when many render threads run, so
the heavy/cheap split of a multi-thread profile is unreliable. Profile with
"--setting noRenderThreads=1" and pass that file as the serial half of
--serial-profile INPUT=SERIAL (or analyse the one-thread file directly); the
multi-thread run then only supplies the task graph and --wall-ms-per-frame.
"""

import argparse
import collections
import json
import os
import sys
import tempfile
import time


REQUIRED_FIELDS = ("frame", "task", "plugin", "node", "deps", "roiPixels", "estimatedBytes", "wallNs")


def load_records(paths):
    """Returns {(path, frame): {task: record}} keeping only successful tasks.

    A malformed final line is what a crashed render leaves behind; it is skipped with a warning."""
    frames = collections.OrderedDict()
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            lines = [(n, line.strip()) for n, line in enumerate(handle, 1) if line.strip()]
        for index, (number, line) in enumerate(lines):
            try:
                rec = json.loads(line)
                if isinstance(rec, dict) and rec.get("kind") == "region":
                    continue
                missing = [f for f in REQUIRED_FIELDS if f not in rec]
                if missing:
                    raise KeyError(", ".join(missing))
            except (ValueError, KeyError, TypeError) as err:
                if index == len(lines) - 1:
                    sys.stderr.write("%s:%d: skipping truncated last line (%s)\n" % (path, number, err))
                    continue
                sys.exit("%s:%d: bad record (%s)" % (path, number, err))
            if not rec.get("ok", True):
                continue
            rec.setdefault("device", "cpu")
            rec.setdefault("regionId", -1)
            frames.setdefault((path, rec["frame"]), {})[rec["task"]] = rec
    return frames


def is_fallback(rec):
    """A member of a GPU region that was rendered on the CPU instead."""
    return rec.get("device") == "cpu" and rec.get("regionId", -1) >= 0


def is_writer(rec):
    return rec["plugin"].rsplit(".", 1)[-1].lower().startswith("write")


def apply_serial_costs(frames, serial_frames, pairs):
    """Replaces each task's wallNs by the one-thread cost per pixel of its node.

    pairs maps an input file to its serial profile; costs are never shared between files."""
    for path, serial_path in pairs.items():
        ns = collections.defaultdict(float)
        px = collections.defaultdict(float)
        for (spath, _), tasks in serial_frames.items():
            if spath != serial_path:
                continue
            for rec in tasks.values():
                ns[rec["node"]] += rec["wallNs"]
                px[rec["node"]] += rec["roiPixels"]
        missing = set()
        for (fpath, _), tasks in frames.items():
            if fpath != path:
                continue
            for rec in tasks.values():
                if px.get(rec["node"], 0) > 0:
                    rec["wallNs"] = ns[rec["node"]] / px[rec["node"]] * rec["roiPixels"]
                else:
                    missing.add(rec["node"])
        if missing:
            sys.stderr.write("warning: %s: no cost in %s for node(s) %s; keeping their contended wallNs\n"
                             % (path, serial_path, ", ".join(sorted(missing))))


def ms_per_mpx(rec):
    if rec["roiPixels"] <= 0:
        return 0.0
    return (rec["wallNs"] / 1e6) / (rec["roiPixels"] / 1e6)


def find_regions(tasks, threshold):
    """Returns (heavy ids, region id-sets, deps, users) for one frame."""
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
        fallbacks = collections.Counter()
        heavy_ms = 0.0
        sigs = collections.OrderedDict()
        gain_pcie = gain_unified = 0.0
        offloaded_regions = 0
        total_ms = sum(rec["wallNs"] for _, tasks in plist for rec in tasks.values()) / 1e6
        per_frame = args.wall_ms_per_frame.get(path, 0.0)
        wall_ms = per_frame * len(plist) if per_frame > 0 else total_ms
        parallelism = max(total_ms / wall_ms, 1.0) if wall_ms else 1.0
        for frame, tasks in plist:
            heavy, regions, deps, users = find_regions(tasks, args.heavy_ms_per_mpx)
            for t, rec in tasks.items():
                ms = rec["wallNs"] / 1e6
                plugin_ms[rec["plugin"]] += ms
                if is_fallback(rec):
                    fallbacks[rec["plugin"]] += 1
                if t in heavy:
                    heavy_ms += ms
                    plugin_heavy_ms[rec["plugin"]] += ms
            for region in regions:
                cpu = sum(tasks[t]["wallNs"] for t in region) / 1e6
                nbytes, n_in, n_out = boundary_bytes(region, tasks, deps, users)
                saved = cpu / parallelism * (1 - 1 / args.gpu_speedup)
                g1 = saved - transfer_ms(nbytes, args.bandwidth_gbps)
                g2 = saved - transfer_ms(nbytes, args.unified_bandwidth_gbps)
                g1 = max(g1, 0.0)
                g2 = max(g2, 0.0)
                gain_pcie += g1
                gain_unified += g2
                if g1 > 0:
                    offloaded_regions += 1
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
            "gpu_fallbacks": dict(sorted(fallbacks.items())),
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
        if res["gpu_fallbacks"]:
            print("GPU region members that fell back to the CPU: %s"
                  % ", ".join("%s x%d" % (p, n) for p, n in res["gpu_fallbacks"].items()))
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


BLUR_PLUGIN_ID = "net.sf.cimg.CImgBlur"
GRADE_PLUGIN_ID = "net.sf.openfx.GradePlugin"
TRANSFORM_PLUGIN_ID = "net.sf.openfx.TransformPlugin"
COLOR_CORRECT_PLUGIN_ID = "net.sf.openfx.ColorCorrectPlugin"
INVERT_PLUGIN_ID = "net.sf.openfx.Invert"
CLAMP_PLUGIN_ID = "net.sf.openfx.Clamp"
ADD_PLUGIN_ID = "net.sf.openfx.AddPlugin"
MULTIPLY_PLUGIN_ID = "net.sf.openfx.MultiplyPlugin"
GAMMA_PLUGIN_ID = "net.sf.openfx.GammaPlugin"
SATURATION_PLUGIN_ID = "net.sf.openfx.SaturationPlugin"
COLOR_LOOKUP_PLUGIN_ID = "net.sf.openfx.ColorLookupPlugin"
KEYER_PLUGIN_ID = "net.sf.openfx.KeyerPlugin"
CHROMA_KEYER_PLUGIN_ID = "net.sf.openfx.ChromaKeyerPlugin"
ERODE_PLUGIN_ID = "net.sf.cimg.CImgErode"
DILATE_PLUGIN_ID = "net.sf.cimg.CImgDilate"
CONSTANT_PLUGIN_ID = "net.sf.openfx.ConstantPlugin"
SOLID_PLUGIN_ID = "net.sf.openfx.Solid"
CHECKER_BOARD_PLUGIN_ID = "net.sf.openfx.CheckerBoardPlugin"

# Profile records carry no knob values. The bench comps render every one of these with motion
# blur (tools/bench/comps/build_comps.py), so their rows price only a task with motion blur.
MOTION_BLUR_PLUGIN_IDS = frozenset([TRANSFORM_PLUGIN_ID])
UHD_MPX = 3840 * 2160 / 1e6
OPEN_SIGMA = 1e9
OPEN_SIZE = 1000000

# Profile records carry no sigma, so these come from in-situ Blur measurements (UHD float,
# native node, GPU off, taskgraph per-task ms/Mpx, 16 threads vs noRenderThreads=1) of the
# four Blur comps. Rows are (algorithm, largest sigma covered, serial, threaded); the last FIR
# bucket is open-ended and was measured at sigma 100. The recursive filter costs the same at
# any sigma, so its one measurement at sigma 3 covers every sigma.
BLUR_SIGMA_MEASURED = (
    ("fir", 3.0, 30.13, 4.70),
    ("fir", 25.0, 84.6, 12.18),
    ("fir", OPEN_SIGMA, 252.8, 34.66),
    ("iir", OPEN_SIGMA, 52.84, 6.83),
)

# Grade's CPU cost (serial, threaded ms/Mpx) from the same in-situ measurement, in place of
# the comp profiles' median, which predates the render path's serial-overhead cuts.
GRADE_CPU_MEASURED = (11.48, 1.46)

# The nodes with a GPU kernel. Each needs a CPU and a GPU seed row.
PORTED_PLUGIN_IDS = (
    GRADE_PLUGIN_ID, COLOR_CORRECT_PLUGIN_ID, INVERT_PLUGIN_ID, CLAMP_PLUGIN_ID, ADD_PLUGIN_ID, MULTIPLY_PLUGIN_ID,
    GAMMA_PLUGIN_ID, SATURATION_PLUGIN_ID, COLOR_LOOKUP_PLUGIN_ID, KEYER_PLUGIN_ID, CHROMA_KEYER_PLUGIN_ID,
    ERODE_PLUGIN_ID, DILATE_PLUGIN_ID, CONSTANT_PLUGIN_ID, SOLID_PLUGIN_ID, CHECKER_BOARD_PLUGIN_ID,
    TRANSFORM_PLUGIN_ID, BLUR_PLUGIN_ID,
)

# Plain Transform CPU cost (serial, threaded ms/Mpx), in situ at UHD float. The profile records
# carry no motion blur flag, so the comps' Transform records all go to the motion blur row and a
# plain one is measured apart (tools/bench/node_profile.sh serial|cpu, node "transform").
TRANSFORM_PLAIN_MEASURED = (26.126, 6.096)

# Kernel-only GPU time per UHD image in ms on an RX 7900 XTX: the median kernelMs of the region
# record of a lone node whose source is uploaded and whose result is downloaded
# (tools/bench/node_profile.sh gpu 1). Rows are (plugin id, kind, largest sigma covered, ms); kind
# is "plain", "iir" for the recursive Blur filters or "motion_blur" for a Transform that renders
# with motion blur (quality 1, as the CPU row). The Blur rows after the first are FIR Gaussians
# (size 7.2, 60 and 240); the IIR row is the slower of Quasi-Gaussian and Gaussian at size 240.
# A row may end with two more fields, the Blur filter it was measured with (quasi_gaussian,
# gaussian, box, triangle, quadratic or fir_gaussian) and the largest Erode or Dilate size it
# covers; a row without them holds for any filter and any size. The first Erode row is size 6 and
# the second is size 40: the median kernelMs, 4.720 ms, of the steady-state region records of
# main/node-erode40-uhd-auto-r1 and r2 under .local/m109/p2t1, less the 0.550 ms of the
# CheckerBoard that shares the region. The Blur rows that name a filter are the median steady-state
# kernelMs at size 60 under .local/m109/p4t4, less the same 0.550 ms: box 22.85, triangle 32.79,
# quadratic 40.20, Gaussian 29.46 and Quasi-Gaussian 35.42 ms. Each runs as running sums or a
# recursion, which cost the same at any size, so it covers every sigma.
GPU_KERNEL_UHD_MS = (
    (GRADE_PLUGIN_ID, "plain", 0.0, 0.497),
    (COLOR_CORRECT_PLUGIN_ID, "plain", 0.0, 0.514),
    (INVERT_PLUGIN_ID, "plain", 0.0, 0.477),
    (CLAMP_PLUGIN_ID, "plain", 0.0, 0.494),
    (ADD_PLUGIN_ID, "plain", 0.0, 0.480),
    (MULTIPLY_PLUGIN_ID, "plain", 0.0, 0.482),
    (GAMMA_PLUGIN_ID, "plain", 0.0, 0.487),
    (SATURATION_PLUGIN_ID, "plain", 0.0, 0.482),
    (COLOR_LOOKUP_PLUGIN_ID, "plain", 0.0, 0.504),
    (KEYER_PLUGIN_ID, "plain", 0.0, 0.441),
    (CHROMA_KEYER_PLUGIN_ID, "plain", 0.0, 0.514),
    (ERODE_PLUGIN_ID, "plain", 0.0, 6.827, None, 6),
    (ERODE_PLUGIN_ID, "plain", 0.0, 4.170, None, OPEN_SIZE),
    (DILATE_PLUGIN_ID, "plain", 0.0, 6.915),
    (CONSTANT_PLUGIN_ID, "plain", 0.0, 0.520),
    (SOLID_PLUGIN_ID, "plain", 0.0, 0.510),
    (CHECKER_BOARD_PLUGIN_ID, "plain", 0.0, 0.550),
    (TRANSFORM_PLUGIN_ID, "plain", 0.0, 0.670),
    (TRANSFORM_PLUGIN_ID, "motion_blur", 0.0, 9.790),
    (BLUR_PLUGIN_ID, "plain", 3.0, 9.514),
    (BLUR_PLUGIN_ID, "plain", 25.0, 7.073),
    (BLUR_PLUGIN_ID, "plain", OPEN_SIGMA, 18.745),
    (BLUR_PLUGIN_ID, "iir", OPEN_SIGMA, 20.014),
    (BLUR_PLUGIN_ID, "plain", OPEN_SIGMA, 22.300, "box", 0),
    (BLUR_PLUGIN_ID, "plain", OPEN_SIGMA, 32.240, "triangle", 0),
    (BLUR_PLUGIN_ID, "plain", OPEN_SIGMA, 39.650, "quadratic", 0),
    (BLUR_PLUGIN_ID, "iir", OPEN_SIGMA, 28.910, "gaussian", 0),
    (BLUR_PLUGIN_ID, "iir", OPEN_SIGMA, 34.870, "quasi_gaussian", 0),
)


def weighted_median(pairs):
    """pairs is [(value, weight)]; returns the first value whose cumulative weight reaches half."""
    pairs = sorted(pairs)
    half = sum(w for _, w in pairs) / 2.0
    acc = 0.0
    for value, weight in pairs:
        acc += weight
        if acc >= half:
            return value
    return 0.0


def plugin_costs(paths):
    """Returns {plugin id: weighted median ms/Mpx over every successful task of the files}."""
    samples = collections.defaultdict(list)
    for tasks in load_records(paths).values():
        for rec in tasks.values():
            if rec["roiPixels"] > 0:
                samples[rec["plugin"]].append((ms_per_mpx(rec), rec["roiPixels"]))
    return {plugin: weighted_median(pairs) for plugin, pairs in samples.items()}


def cost_table_rows(threaded_paths, serial_paths):
    """Returns (cpu rows, motion-blur cpu rows, sigma rows, gpu rows) for the seed table.

    The parallel speedup is the serial cost per pixel over the threaded one, 1 for a plugin
    the threaded profiles lack."""
    serial = plugin_costs(serial_paths)
    threaded = plugin_costs(threaded_paths)
    cpu = []
    motion_blur = []
    for plugin in sorted(serial):
        speedup = serial[plugin] / threaded[plugin] if threaded.get(plugin, 0) > 0 else 1.0
        rows = motion_blur if plugin in MOTION_BLUR_PLUGIN_IDS else cpu
        if plugin == GRADE_PLUGIN_ID:
            rows.append((plugin, GRADE_CPU_MEASURED[0], GRADE_CPU_MEASURED[0] / GRADE_CPU_MEASURED[1]))
        else:
            rows.append((plugin, serial[plugin], speedup))
    cpu.append((TRANSFORM_PLUGIN_ID, TRANSFORM_PLAIN_MEASURED[0], TRANSFORM_PLAIN_MEASURED[0] / TRANSFORM_PLAIN_MEASURED[1]))
    cpu.sort()
    sigma = [(BLUR_PLUGIN_ID, algo, top, ser, ser / par) for algo, top, ser, par in BLUR_SIGMA_MEASURED]
    gpu = [(row[0], row[1], row[2], row[3] / UHD_MPX) + (tuple(row[4:]) or (None, 0)) for row in GPU_KERNEL_UHD_MS]
    return cpu, motion_blur, sigma, gpu


def file_date(path):
    return time.strftime("%Y-%m-%d", time.localtime(os.path.getmtime(path)))


def sigma_text(top):
    return "1e9" if top >= OPEN_SIGMA else repr(float(top))


def gpu_row_text(row):
    """One GPU seed row; the filter and size columns are written only for a row that names either."""
    plugin, kind, top, ms = row[:4]
    blur_filter, size = (tuple(row[4:]) + (None, 0))[:2]
    text = '"%s", "%s", %s, %.4f' % (plugin, kind, sigma_text(top), ms)
    if blur_filter or size:
        text += ", %s, %d" % ('"%s"' % blur_filter if blur_filter else "nullptr", size)
    return "{ %s }" % text


def emit_cost_table(pairs, command):
    """Returns the text of Engine/GpuCostSeed.inc; pairs maps each threaded profile to its serial one."""
    threaded = list(pairs)
    serial = [pairs[path] for path in threaded]
    cpu, motion_blur, sigma, gpu = cost_table_rows(threaded, serial)

    def block(name, rows):
        if not rows:
            return "#define %s\n" % name
        lines = ["#define %s \\" % name]
        lines += ["    %s, \\" % row for row in rows]
        lines[-1] = lines[-1][:-3]
        return "\n".join(lines) + "\n"

    out = ["// Seed costs for the GPU placement cost model. Generated, do not edit.",
           "//",
           "// Serial (noRenderThreads=1) inputs, dated by file modification time:"]
    out += ["//   %s (%s)" % (path, file_date(path)) for path in serial]
    out.append("// Threaded inputs:")
    out += ["//   %s (%s)" % (path, file_date(path)) for path in threaded]
    out += ["//",
            "// Regenerate from the repository root with:",
            "//   " + command,
            "//",
            "// Each macro expands to an initialiser list; the consuming code defines the row structs.",
            "// CPU rows: { plugin id, serial ms per Mpx (median over tasks weighted by RoI pixels),",
            "//   parallel speedup (serial over threaded ms per Mpx) }.",
            "// CPU motion-blur rows: the same, for the plugins the bench comps render with motion blur;",
            "//   they price only a task that renders with motion blur.",
            "// CPU sigma rows: { plugin id, algorithm, largest sigma covered, serial ms per Mpx, parallel",
            "//   speedup }. Use the first row of the algorithm whose sigma bound is at least the sigma.",
            "//   Measured in situ on UHD float at 16 threads, so they are hard-coded here.",
            "// GPU rows: { plugin id, kind (plain, iir or motion_blur), largest sigma covered (0 when the",
            "//   kernel has no sigma), kernel-only ms per Mpx on an RX 7900 XTX, from the kernelMs of lone-node",
            "//   regions at UHD, then optionally the Blur filter (nullptr for any) and the largest Erode or",
            "//   Dilate size covered (0 for any) }. A row that names a filter or a size beats one that does not.",
            ""]
    out.append(block("NATRON_GPU_COST_SEED_CPU_ROWS",
                     ['{ "%s", %.4f, %.4f }' % row for row in cpu]))
    out.append(block("NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS",
                     ['{ "%s", %.4f, %.4f }' % row for row in motion_blur]))
    out.append(block("NATRON_GPU_COST_SEED_CPU_SIGMA_ROWS",
                     ['{ "%s", "%s", %s, %.4f, %.4f }' % (p, a, sigma_text(t), s, k)
                      for p, a, t, s, k in sigma]))
    out.append(block("NATRON_GPU_COST_SEED_GPU_ROWS", [gpu_row_text(row) for row in gpu]))
    return "\n".join(out)


def self_test(args):
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "render-profile-sample.jsonl")
    args.heavy_ms_per_mpx = 20.0
    args.gpu_speedup = 10.0
    args.bandwidth_gbps = 25.0
    args.unified_bandwidth_gbps = 0.0
    args.wall_ms_per_frame = {}
    res = analyse(load_records([sample]), args)[0]

    def close(a, b):
        return abs(a - b) < 1e-6

    loaded = load_records([sample])
    assert sum(1 for line in open(sample, encoding="utf-8") if '"kind":"region"' in line) == 1
    assert [len(tasks) for tasks in loaded.values()] == [7], "region record must not become a task"
    assert all("kind" not in rec and rec["task"] in range(7)
               for tasks in loaded.values() for rec in tasks.values())
    assert all(rec["device"] == "cpu" and rec["regionId"] == -1
               for tasks in loaded.values() for rec in tasks.values())

    with tempfile.TemporaryDirectory() as tmp:
        gpu_path = os.path.join(tmp, "gpu.jsonl")
        with open(gpu_path, "w", encoding="utf-8") as out:
            out.write('{"kind":"region","frame":0,"regionId":3,"tasks":[0]}\n')
            out.write('{"frame":0,"task":0,"plugin":"Blur","node":"B","deps":[],"roiPixels":1,'
                      '"estimatedBytes":1,"wallNs":1,"device":"gpu","regionId":3,"ok":true}\n')
        rec = load_records([gpu_path])[(gpu_path, 0)][0]
        assert rec["device"] == "gpu" and rec["regionId"] == 3, rec
        assert analyse(load_records([gpu_path]), args)[0]["gpu_fallbacks"] == {}
        fb_path = os.path.join(tmp, "fallback.jsonl")
        with open(fb_path, "w", encoding="utf-8") as out:
            out.write('{"frame":0,"task":0,"plugin":"Blur","node":"B","deps":[],"roiPixels":1,'
                      '"estimatedBytes":1,"wallNs":1,"device":"cpu","regionId":3,"ok":true}\n')
            out.write('{"frame":0,"task":1,"plugin":"Grade","node":"G","deps":[0],"roiPixels":1,'
                      '"estimatedBytes":1,"wallNs":1,"device":"cpu","regionId":3,"ok":true}\n')
            out.write('{"frame":0,"task":2,"plugin":"Blur","node":"B2","deps":[1],"roiPixels":1,'
                      '"estimatedBytes":1,"wallNs":1,"device":"cpu","regionId":-1,"ok":true}\n')
            out.write('{"kind":"region","frame":0,"regionId":3,"tasks":[0,1],"fallback":"out of device memory",'
                      '"retries":2}\n')
        assert analyse(load_records([fb_path]), args)[0]["gpu_fallbacks"] == {"Blur": 1, "Grade": 1}
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
    args.wall_ms_per_frame = {sample: 427.0 / 2}
    half = analyse(frames, args)[0]
    assert close(half["parallelism"], 2.0), half["parallelism"]
    assert close(half["net_gain_ms"], (205 + 200) / 2 * 0.9 - 2 * 1.28), half["net_gain_ms"]
    assert close(half["net_gain_share"], half["net_gain_ms"] / (427.0 / 2)), half["net_gain_share"]

    def task(node, plugin, deps, ns):
        return {"plugin": plugin, "node": node, "deps": deps, "roiPixels": 1000000,
                "estimatedBytes": 1000, "wallNs": ns}
    tasks = {0: task("A", "Blur", [], 100000000),
             1: task("Write1", "fr.inria.built-in.Write", [0], 100000000),
             2: task("B", "Blur", [1], 100000000)}
    heavy, regions, _, _ = find_regions(tasks, 20.0)
    assert regions == [{0}, {2}] or regions == [{2}, {0}], regions

    with tempfile.TemporaryDirectory() as tmp:
        fast = os.path.join(tmp, "fast.jsonl")
        with open(sample, encoding="utf-8") as src, open(fast, "w", encoding="utf-8") as dst:
            for line in src:
                rec = json.loads(line)
                if "wallNs" in rec:
                    rec["wallNs"] //= 2
                dst.write(json.dumps(rec) + "\n")
        cpu, motion_blur, sigma, gpu = cost_table_rows([fast], [sample])
        rows = {plugin: (cost, speedup) for plugin, cost, speedup in cpu}
        assert set(rows) == {"Read", "Blur", "Grade", "Write", "Defocus", "Merge", TRANSFORM_PLUGIN_ID}, rows.keys()
        assert close(rows[TRANSFORM_PLUGIN_ID][0], TRANSFORM_PLAIN_MEASURED[0]), rows[TRANSFORM_PLUGIN_ID]
        assert close(rows[TRANSFORM_PLUGIN_ID][1], TRANSFORM_PLAIN_MEASURED[0] / TRANSFORM_PLAIN_MEASURED[1])
        assert motion_blur == [], motion_blur
        assert close(rows["Blur"][0], 100.0) and close(rows["Blur"][1], 2.0), rows["Blur"]
        assert close(rows["Defocus"][0], 200.0)
        assert [r[1:3] for r in sigma] == [("fir", 3.0), ("fir", 25.0), ("fir", 1e9), ("iir", 1e9)]
        assert close(sigma[0][4], 30.13 / 4.70) and close(sigma[3][4], 52.84 / 6.83), sigma
        assert [r[:3] for r in gpu] == [(row[0], row[1], row[2]) for row in GPU_KERNEL_UHD_MS], gpu
        grade_gpu = [r for r in gpu if r[0] == GRADE_PLUGIN_ID]
        assert len(grade_gpu) == 1 and close(grade_gpu[0][3], 0.497 / UHD_MPX), grade_gpu
        for plugin in PORTED_PLUGIN_IDS:
            assert any(r[0] == plugin for r in gpu), plugin
        assert [r[1] for r in gpu if r[0] == BLUR_PLUGIN_ID] == ["plain", "plain", "plain", "iir", "plain", "plain", "plain", "iir", "iir"], gpu
        assert [r[4] for r in gpu if r[0] == BLUR_PLUGIN_ID] == [None] * 4 + ["box", "triangle", "quadratic", "gaussian", "quasi_gaussian"], gpu
        assert [r[1] for r in gpu if r[0] == TRANSFORM_PLUGIN_ID] == ["plain", "motion_blur"], gpu
        text = emit_cost_table({fast: sample}, "cmd")
        assert text == emit_cost_table({fast: sample}, "cmd"), "output must be deterministic"
        assert '    { "Blur", 100.0000, 2.0000 }, \\\n' in text, text
        assert text.count("#define NATRON_GPU_COST_SEED_") == 4
        assert "#define NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS\n" in text, text
        assert '{ "net.sf.cimg.CImgBlur", "fir", 3.0, 30.1300, 6.4106 }' in text, text
        assert '{ "net.sf.cimg.CImgBlur", "iir", 1e9, ' in text, text
        assert "(%s)" % file_date(sample) in text and "//   cmd\n" in text
        body = [line for line in text.splitlines() if not line.startswith("//")]
        assert not any(line.endswith("\\") for line in body if line.startswith("}")), body

        moving = os.path.join(tmp, "transform.jsonl")
        with open(moving, "w", encoding="utf-8") as out:
            out.write('{"frame":0,"task":0,"plugin":"%s","node":"T","deps":[],"roiPixels":1000000,'
                      '"estimatedBytes":1,"wallNs":300000000,"device":"cpu","regionId":-1,"ok":true}\n'
                      % TRANSFORM_PLUGIN_ID)
            out.write('{"frame":0,"task":1,"plugin":"Grade","node":"G","deps":[0],"roiPixels":1000000,'
                      '"estimatedBytes":1,"wallNs":10000000,"device":"cpu","regionId":-1,"ok":true}\n')
        cpu, motion_blur, _, _ = cost_table_rows([moving], [moving])
        assert [r[0] for r in cpu] == ["Grade", TRANSFORM_PLUGIN_ID], cpu
        assert [r[0] for r in motion_blur] == [TRANSFORM_PLUGIN_ID] and close(motion_blur[0][1], 300.0), motion_blur
        text = emit_cost_table({moving: moving}, "cmd")
        assert ('#define NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS \\\n    { "%s", 300.0000, 1.0000 }\n'
                % TRANSFORM_PLUGIN_ID) in text, text
    assert gpu_row_text(("p", "plain", 0.0, 1.0)) == '{ "p", "plain", 0.0, 1.0000 }'
    assert gpu_row_text(("p", "plain", 0.0, 1.0, None, 6)) == '{ "p", "plain", 0.0, 1.0000, nullptr, 6 }'
    assert gpu_row_text(("p", "plain", OPEN_SIGMA, 1.0, "box", 0)) == '{ "p", "plain", 1e9, 1.0000, "box", 0 }'
    seed_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Engine", "GpuCostSeed.inc")
    if os.path.exists(seed_path):
        with open(seed_path, encoding="utf-8") as handle:
            seed = handle.read()
        cpu_block = seed[seed.index("#define NATRON_GPU_COST_SEED_CPU_ROWS"):seed.index("#define NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS")]
        gpu_block = seed[seed.index("#define NATRON_GPU_COST_SEED_GPU_ROWS"):]
        for plugin in PORTED_PLUGIN_IDS:
            assert '"%s"' % plugin in cpu_block, "no CPU seed row for " + plugin
            assert '"%s"' % plugin in gpu_block, "no GPU seed row for " + plugin
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
    ap.add_argument("--wall-ms-per-frame", metavar="[INPUT=]MS", action="append", default=[],
                    help="measured wall-clock ms per frame of the same comp, over the same frames as the "
                         "input (e.g. the steady frames, not the cold first one); parallelism is task time "
                         "per frame divided by this, at least 1. Use INPUT=MS to give one value per input "
                         "file; a bare MS is accepted only with a single input. Default: parallelism 1, "
                         "right for a one-thread profile")
    ap.add_argument("--serial-profile", metavar="INPUT=SERIAL", action="append", default=[],
                    help="pairs an input file with the one-thread profile (noRenderThreads=1) of the same "
                         "comp; each node's own cost per pixel then replaces the contended wallNs of that "
                         "input for classification and region cost. Costs are not shared between inputs; "
                         "nodes absent from the serial profile keep their wallNs and are reported")
    ap.add_argument("--emit-cost-table", metavar="OUT",
                    help="write the per-plugin cost seed (a C++ .inc) from the one-thread profiles of "
                         "--serial-profile INPUT=SERIAL pairs, with INPUT the threaded profile, and exit")
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
    walls = {}
    for spec in args.wall_ms_per_frame:
        key, sep, value = spec.rpartition("=")
        if not sep:
            if len(args.files) != 1:
                ap.error("--wall-ms-per-frame needs INPUT=MS with several input files")
            key = args.files[0]
        elif key not in args.files:
            ap.error("--wall-ms-per-frame: %s is not an input file" % key)
        try:
            walls[key] = float(value)
        except ValueError:
            ap.error("--wall-ms-per-frame: bad value %r" % value)
    args.wall_ms_per_frame = walls
    pairs = {}
    for spec in args.serial_profile:
        key, sep, serial = spec.partition("=")
        if not sep or key not in args.files:
            ap.error("--serial-profile needs INPUT=SERIAL with INPUT one of the input files")
        pairs[key] = serial
    if args.emit_cost_table:
        if not pairs:
            ap.error("--emit-cost-table needs --serial-profile INPUT=SERIAL pairs")
        command = " ".join(["tools/bench/render-profile-report.py"] + ["--serial-profile %s=%s" % kv
                           for kv in pairs.items()] + args.files + ["--emit-cost-table", args.emit_cost_table])
        with open(args.emit_cost_table, "w", encoding="utf-8") as out:
            out.write(emit_cost_table(pairs, command))
        return
    frames = load_records(args.files)
    if pairs:
        apply_serial_costs(frames, load_records(pairs.values()), pairs)
    results = analyse(frames, args)
    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print_report(results, args)


if __name__ == "__main__":
    main()
