#!/usr/bin/env python3
# Scale bench of the geometry nodes on a 10M-point grid, a Tests binary built before a change
# against one built after it, run back to back in one sitting:
#
#   python3 tools/bench/geo_scale.py <before-Tests> <after-Tests> [--grid 3162] [--reps 5] [--selection 1000000]
#
# Each binary runs the DISABLED_GeoBenchTest suite of Tests/GeoBench_Test.cpp, which times
# GeoTransform, GeoEdit (5 points), GeoDelete (half the points, by a point group pattern),
# GeoToUSD (with the USDWrite that flattens it, so the time includes the file write) and, over a
# selection of --selection points, geoEncodePattern and geoExpandPoints. Every stage reports its
# median wall time and its peak RSS (the kernel high-water mark, restarted before each run), plus
# the peak's growth over the RSS the stage started at. No thresholds: it prints a table.
#
# Run it on a quiet machine, nothing else using the build or the GPU. The working directory of a
# run is the binary's directory unless --cwd says otherwise, as ctest runs the suites there.
# Raw lines land in <out>/<label>.log (default build/bench/geo-scale).

import argparse
import json
import os
import subprocess
import sys

MARKER = "GEOBENCH "


def run(binary, label, args, out_dir):
    env = dict(os.environ)
    env["NATRON_GEO_BENCH_GRID"] = str(args.grid)
    env["NATRON_GEO_BENCH_REPS"] = str(args.reps)
    env["NATRON_GEO_BENCH_SELECTION"] = str(args.selection)
    cwd = args.cwd or os.path.dirname(os.path.abspath(binary))
    command = [os.path.abspath(binary), "--gtest_also_run_disabled_tests", "--gtest_filter=DISABLED_GeoBenchTest.*"]
    print("[geo-scale] %s: %s" % (label, " ".join(command)), flush=True)
    proc = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=args.timeout)
    log_path = os.path.join(out_dir, label + ".log")
    with open(log_path, "w") as f:
        f.write(proc.stdout)
    stages = {}
    order = []
    for line in proc.stdout.splitlines():
        if line.startswith(MARKER):
            record = json.loads(line[len(MARKER):])
            stages[record["stage"]] = record
            order.append(record["stage"])
    if proc.returncode != 0:
        print("[geo-scale] %s exited with %d, see %s" % (label, proc.returncode, log_path), file=sys.stderr)
    return stages, order, proc.returncode


def cell(record, key, scale=1.0):
    if record is None or key not in record:
        return None
    return record[key] * scale


def fmt(value, spec):
    return "n/a" if value is None else format(value, spec)


def ratio(before, after):
    if before is None or after is None or before <= 0:
        return "n/a"
    return "%.2fx" % (after / before)


def table(before, after, order):
    header = ("stage", "before s", "after s", "after/before", "before peak MB", "after peak MB", "before +MB", "after +MB")
    rows = []
    for name in order:
        b, a = before.get(name), after.get(name)
        if (b and "median_s" not in b) or (a and "median_s" not in a) or not (b or a):
            continue
        rows.append((name,
                     fmt(cell(b, "median_s"), ".3f"), fmt(cell(a, "median_s"), ".3f"),
                     ratio(cell(b, "median_s"), cell(a, "median_s")),
                     fmt(cell(b, "peak_rss_kb", 1 / 1024), ".0f"), fmt(cell(a, "peak_rss_kb", 1 / 1024), ".0f"),
                     fmt(cell(b, "delta_rss_kb", 1 / 1024), ".0f"), fmt(cell(a, "delta_rss_kb", 1 / 1024), ".0f")))
    widths = [max(len(r[i]) for r in rows + [header]) for i in range(len(header))]
    for r in [header] + rows:
        print("  ".join(c.ljust(widths[i]) if i == 0 else c.rjust(widths[i]) for i, c in enumerate(r)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before", help="Tests binary of the build before the change")
    parser.add_argument("after", help="Tests binary of the build after the change")
    parser.add_argument("--grid", type=int, default=3162, help="grid faces per side (3162 is 10,004,569 points)")
    parser.add_argument("--reps", type=int, default=5, help="timed runs per stage; the median is reported")
    parser.add_argument("--selection", type=int, default=1000000, help="elements in the encode and expand selections")
    parser.add_argument("--cwd", help="working directory of the runs (default: each binary's directory)")
    parser.add_argument("--out", default="build/bench/geo-scale", help="directory for the raw logs")
    parser.add_argument("--timeout", type=int, default=7200, help="seconds allowed per binary")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    before, before_order, before_code = run(args.before, "before", args, args.out)
    after, after_order, after_code = run(args.after, "after", args, args.out)

    order = list(before_order)
    order += [s for s in after_order if s not in order]
    print()
    for label, stages in (("before", before), ("after", after)):
        if "grid" in stages:
            print("%s grid: %s" % (label, stages["grid"]["detail"]))
    for name in order:
        for label, stages in (("before", before), ("after", after)):
            detail = stages.get(name, {}).get("detail")
            if detail and name != "grid":
                print("%s %s: %s" % (label, name, detail))
    print()
    table(before, after, [s for s in order if s != "grid"])
    return 1 if (before_code or after_code) else 0


if __name__ == "__main__":
    sys.exit(main())
