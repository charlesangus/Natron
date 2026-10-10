#!/usr/bin/env python3
"""Merges the GpuBench runs and the CPU baselines into CSVs and a short summary.

usage: summarize.py <results dir> <gpu runs>

Writes <dir>/gpu-bench.csv (every run, with a run column) and <dir>/cpu-baseline.csv, and prints
the summary: GPU against CPU per size and workload, the break-even round trip, the H and V blur
pass times, and the run-to-run spread.
"""
import csv
import json
import os
import statistics
import sys

SIZES = ["UHD", "8K", "16k", "24k"]
WORKLOADS = ["Grade", "Blur3", "Blur25", "Blur100", "GradeBlur25"]
KEY = ("size", "mode", "workload", "path", "overlap")
METRICS = ["moved_mb_per_image", "kernel_ms", "grade_ms", "h_ms", "v_ms", "host_up_ms", "gpu_up_ms",
           "gpu_comp_ms", "gpu_down_ms", "host_down_ms", "e2e_ms"]


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def load_gpu(out, runs):
    rows = []
    for r in range(1, runs + 1):
        path = os.path.join(out, "gpu-run%d.csv" % r)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            for row in csv.DictReader(f):
                row["run"] = str(r)
                rows.append(row)
    return rows


def mean_over_runs(rows):
    groups = {}
    for row in rows:
        groups.setdefault(tuple(row[k] for k in KEY), []).append(row)
    merged = {}
    for key, rs in groups.items():
        m = {k: key[i] for i, k in enumerate(KEY)}
        for name in METRICS:
            vals = [num(r[name]) for r in rs if num(r[name]) is not None]
            m[name] = statistics.mean(vals) if vals else None
        m["note"] = "; ".join(sorted({r["note"] for r in rs if r["note"] != "-"}))
        m["context"] = "; ".join(sorted({r["context"] for r in rs if r["context"]}))
        if "MISMATCH" in m["note"]:
            for name in METRICS:
                m[name] = None
        merged[key] = m
    return merged


def cpu_baseline(out):
    path = os.path.join(out, "cpu-results.jsonl")
    if not os.path.exists(path):
        return {}, {}
    # A record has no label for its blur size, so records pair with the context rows in run order.
    ctx = {}
    ctx_path = os.path.join(out, "cpu-context.tsv")
    tags = []
    if os.path.exists(ctx_path):
        for line in open(ctx_path):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                tags.append(parts)
    lines = [json.loads(l) for l in open(path) if l.strip()]
    result = {}
    for rec, tag in zip(lines, tags):
        result[tag[0]] = rec
        ctx[tag[0]] = tag
    return result, ctx


def wall_ms(rec):
    return statistics.median(rec["frame_wall_s"]) * 1000.0


def cpu_costs(recs):
    """Marginal CPU ms per workload and size, keyed (size, workload)."""
    costs = {}
    for size in SIZES:
        base = recs.get(size + "-chain0")
        if base is None:
            continue
        grade_tags = [t for t in recs if t.startswith(size + "-chain") and not t.endswith("chain0")]
        entry = {}
        if grade_tags:
            rec = recs[grade_tags[0]]
            entry["Grade"] = (wall_ms(rec) - wall_ms(base)) / rec["n"]
        for sigma in (3, 25, 100):
            rec = recs.get("%s-blur%d" % (size, sigma))
            if rec:
                entry["Blur%d" % sigma] = wall_ms(rec) - wall_ms(base)
        if "Grade" in entry and "Blur25" in entry:
            entry["GradeBlur25"] = entry["Grade"] + entry["Blur25"]
        for name, val in entry.items():
            costs[(size, name)] = val
        costs[(size, "_base")] = wall_ms(base)
        costs[(size, "_par")] = base.get("parallelism")
    return costs


def fmt(v, width=9, digits=1):
    return ("%*.*f" % (width, digits, v)) if v is not None else "%*s" % (width, "-")


def main():
    out, runs = sys.argv[1], int(sys.argv[2])
    rows = load_gpu(out, runs)
    if not rows:
        print("no GPU results in", out)
        return 1
    for row in rows:
        row["note"] = row["note"] or "-"
    with open(os.path.join(out, "gpu-bench.csv"), "w", newline="") as f:
        cols = ["run"] + [c for c in rows[0].keys() if c != "run"]
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    empty = sum(1 for r in rows for v in r.values() if v == "")
    na = sum(1 for r in rows for v in r.values() if v == "NA")
    merged = mean_over_runs(rows)

    recs, ctx = cpu_baseline(out)
    costs = cpu_costs(recs)
    status = open(os.path.join(out, "cpu-status.txt")).read().strip() if os.path.exists(
        os.path.join(out, "cpu-status.txt")) else "not run"
    with open(os.path.join(out, "cpu-baseline.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["size", "workload", "cpu_ms", "base_graph_ms", "parallelism_cpu_s_per_wall_s", "threads"])
        for (size, name), v in sorted(costs.items()):
            if name.startswith("_"):
                continue
            w.writerow([size, name, "%.1f" % v, "%.1f" % costs[(size, "_base")],
                        costs[(size, "_par")], recs[size + "-chain0"].get("cpus", "")])

    def get(size, wl, path, overlap, metric="e2e_ms", mode=None):
        for key, m in merged.items():
            if key[0] == size and key[2] == wl and key[3] == path and key[4] == overlap and \
                    (mode is None or key[1] == mode):
                return m[metric]
        return None

    def kernel(size, wl, metric="kernel_ms"):
        return get(size, wl, "staging", "on", metric, mode=None)

    print("GPU: %s | CPU: Natron native Grade and Blur (IIR Gaussian), %s threads" % (
        "RADV (see device line in gpu-run*.log)", next(iter(recs.values())).get("cpus", "?") if recs else "16"))
    print("Times are ms per image, mean of %d run(s); medians of the iterations inside each run." % runs)
    print("e2e = upload + compute + download, pipelined across frames/strips (on) or one at a time (off).")
    print()
    for size in SIZES:
        modes = sorted({k[1] for k in merged if k[0] == size})
        for mode in modes:
            tag = size if mode != "single" else size + " single frame"
            print("%s (%s)" % (tag, mode))
            print("  %-12s %9s %9s | %9s %9s | %9s %9s | %9s %9s | %7s" % (
                "workload", "CPU", "kernel", "stg off", "stg on", "imp off", "imp on", "H pass", "V pass", "GPU/CPU"))
            for wl in ["Copy"] + WORKLOADS:
                def g(path, ov, metric="e2e_ms"):
                    return get(size, wl, path, ov, metric, mode)
                cpu = costs.get((size, wl))
                best = g("host-import", "on")
                ratio = (cpu / best) if (cpu and best) else None
                print("  %-12s %s %s | %s %s | %s %s | %s %s | %s" % (
                    wl, fmt(cpu), fmt(g("staging", "on", "kernel_ms")), fmt(g("staging", "off")),
                    fmt(g("staging", "on")), fmt(g("host-import", "off")), fmt(g("host-import", "on")),
                    fmt(g("staging", "on", "h_ms"), digits=2), fmt(g("staging", "on", "v_ms"), digits=2),
                    ("%6.1fx" % ratio) if ratio else "      -"))
            print()
    print("GPU/CPU = CPU ms / host-import pipelined e2e ms (above 1 means the GPU wins end to end).")
    for key, m in sorted(merged.items()):
        if key[1] == "single" and key[3] == "staging" and key[4] == "on" and m["note"]:
            print("  %s single frame, %s: %s" % (key[0], key[2], m["note"].replace("; one frame: overlap on and off are the same run", "")))
    print()

    print("Break-even: CPU time a region must save to pay for one round trip (Copy workload, no compute)")
    print("  %-14s %-12s %12s %12s %10s" % ("size", "path", "serial rt ms", "pipelined", "GB/s rt"))
    breakeven = {}
    for size in SIZES:
        for mode in sorted({k[1] for k in merged if k[0] == size}):
            for path in ("staging", "host-import"):
                serial = get(size, "Copy", path, "off", "e2e_ms", mode)
                piped = get(size, "Copy", path, "on", "e2e_ms", mode)
                mb = get(size, "Copy", path, "off", "moved_mb_per_image", mode)
                if serial is None:
                    continue
                gbps = (mb / 1000.0) / (serial / 1000.0) if mb else None
                breakeven[(size, mode, path)] = serial
                print("  %-14s %-12s %12.1f %12s %10s" % (
                    size + ("/" + mode if mode == "single" else ""), path, serial, fmt(piped, 0),
                    ("%.1f" % gbps) if gbps else "-"))
    print("  rt = upload + download of one image. Add the workload's GPU kernel time when comparing a")
    print("  specific region; pipelining hides part of the round trip behind other frames' compute.")
    print()

    print("Blur kernel passes (ms per image, resident data): H = horizontal, V = vertical")
    for size in SIZES:
        for wl in ("Blur3", "Blur25", "Blur100"):
            h, v = kernel(size, wl, "h_ms"), kernel(size, wl, "v_ms")
            if h is not None and v is not None:
                print("  %-4s %-8s H %8.2f  V %8.2f  V/H %5.2f" % (size, wl, h, v, v / h if h else float("nan")))
    print()

    spread = []
    by_key = {}
    for r in rows:
        by_key.setdefault(tuple(r[k] for k in KEY), {})[r["run"]] = r
    for key, rs in by_key.items():
        if "1" not in rs or "2" not in rs:
            continue
        for metric in ("e2e_ms", "kernel_ms"):
            a, b = num(rs["1"][metric]), num(rs["2"][metric])
            if a and b:
                spread.append(((b - a) / ((a + b) / 2.0), key, metric))
    if spread:
        devs = [abs(s[0]) for s in spread]
        outside = [s for s in spread if abs(s[0]) > 0.10]
        print("Run-to-run (run 2 vs run 1, %d comparisons): median |diff| %.1f%%, max %.1f%%, outside +-10%%: %d" % (
            len(spread), 100 * statistics.median(devs), 100 * max(devs), len(outside)))
        for d, key, metric in sorted(outside, key=lambda s: -abs(s[0]))[:12]:
            print("    %+.1f%%  %s %s" % (100 * d, "/".join(key), metric))
        print()

    contexts = sorted({(r["run"], r["load1"], r["context"]) for r in rows if r["workload"] == "Copy"
                       and r["size"] == "UHD" and r["path"] == "staging" and r["overlap"] == "on"})
    print("Context per GPU run:", "; ".join("run %s: %s, load1 %s" % (c[0], c[2], c[1]) for c in contexts))
    print("CPU baselines:", status)
    if ctx:
        busy = [t for t, v in ctx.items() if v[2] != "idle"]
        print("  every CPU run started with no build running" + ("" if not busy else "; build seen after: " + ", ".join(busy)))
        print("  render threads: all 16 (Natron default); other load: load1 at start in cpu-context.tsv")
    print("Cells: %d empty, %d NA (not applicable or not run; the note column says why)" % (empty, na))
    return 0


if __name__ == "__main__":
    sys.exit(main())
