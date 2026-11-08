#!/usr/bin/env python3
"""Compares GUI benchmark runs written by tools/bench/gui/run.sh.

    compare.py BASELINE.jsonl RUN.jsonl [--tol 0.2]   per-op median change; exit 1 on a regression
    compare.py --scaling RUN.jsonl                     how each op grows with graph size
    compare.py --self-test

A regression is an op whose median grew by more than --tol (a fraction) and by more than
--min-ms milliseconds, an op whose status fell from ok (a new timeout or error), an op missing
from the run, or an op with a stall longer than --long-stall-ms (default 250) where the baseline had none. Medians within --min-ms of each other are too close to timer noise to judge.

--scaling fits log(median) against log(n) per (op, topology) over the sizes present and marks
slopes above --superlinear (default 1.2) as superlinear, steepest first: those are the ops whose cost grows faster
than the graph.
"""
import argparse
import json
import math
import sys


def load(path):
    host = None
    ops = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            kind = rec.get("type")
            if kind == "host":
                host = rec
            elif kind == "op":
                ops[(rec["op"], rec["topology"], rec["n"])] = rec
    return host, ops


def compare(base_ops, run_ops, tol, min_ms, long_stall_ms=250.0):
    """(rows, regressions) where rows are printable lines."""
    rows, regressions = [], []
    for key in sorted(set(base_ops) | set(run_ops)):
        op, topology, n = key
        label = "%-20s %-7s %6d" % (op, topology, n)
        b, r = base_ops.get(key), run_ops.get(key)
        if b is None:
            rows.append("%s  only in the run" % label)
            continue
        if r is None:
            rows.append("%s  REGRESSED: missing from the run" % label)
            regressions.append(key)
            continue
        if b.get("status") == "ok" and r.get("status") != "ok":
            rows.append("%s  REGRESSED status %s -> %s" % (label, b.get("status"), r.get("status")))
            regressions.append(key)
            continue
        bm, rm = b.get("median_ms"), r.get("median_ms")
        if bm is None or rm is None:
            rows.append("%s  no median (%s / %s)" % (label, b.get("status"), r.get("status")))
            continue
        change = (rm - bm) / bm if bm > 0 else 0.0
        bs, rs = b.get("stalls", {}).get("max_ms", 0.0), r.get("stalls", {}).get("max_ms", 0.0)
        regressed = change > tol and rm - bm > min_ms
        verdict = "REGRESSED" if regressed else ("improved" if change < -tol and bm - rm > min_ms else "")
        if rs > long_stall_ms >= bs:
            regressed = True
            verdict = "REGRESSED stall %.0f ms (baseline max %.0f ms)" % (rs, bs)
        if regressed:
            regressions.append(key)
        rows.append("%s  %10.2f -> %10.2f ms  %+7.1f%%  %s" % (label, bm, rm, 100.0 * change, verdict))
    return rows, regressions


def slope(points):
    """Least-squares slope of log(y) on log(x)."""
    xs = [math.log(x) for x, _ in points]
    ys = [math.log(y) for _, y in points]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def scaling(run_ops, superlinear):
    """[(op, topology, sizes, slope, flagged)] for each (op, topology) with two or more sizes."""
    groups = {}
    for (op, topology, n), rec in run_ops.items():
        m = rec.get("median_ms")
        if rec.get("status") == "ok" and m is not None and m > 0 and n > 0:
            groups.setdefault((op, topology), []).append((n, m))
    out = []
    for (op, topology), points in groups.items():
        if len(set(n for n, _ in points)) < 2:
            continue
        s = slope(sorted(points))
        out.append((op, topology, sorted(n for n, _ in points), s, s is not None and s > superlinear))
    out.sort(key=lambda row: (-(row[3] if row[3] is not None else float("-inf")), row[0], row[1]))
    return out


def describe_host(host):
    if not host:
        return "no host record"
    return "%s, %s (%s cores), %s, %s %s" % (host.get("hostname"), host.get("cpu_model"), host.get("cores"),
                                              host.get("qt_platform"), host.get("build_type"), host.get("git_sha"))


def self_test():
    def op(name, n, median, status="ok", stall=0.0):
        return {"type": "op", "op": name, "topology": "mixed", "n": n, "status": status,
                "median_ms": median, "stalls": {"count": 1 if stall else 0, "max_ms": stall}}

    base = {("a", "mixed", 300): op("a", 300, 100.0), ("b", "mixed", 300): op("b", 300, 100.0),
            ("c", "mixed", 300): op("c", 300, 1.0), ("d", "mixed", 300): op("d", 300, 50.0),
            ("e", "mixed", 300): op("e", 300, 50.0), ("f", "mixed", 300): op("f", 300, 50.0),
            ("h", "mixed", 300): op("h", 300, 50.0, stall=300.0)}
    run = {("a", "mixed", 300): op("a", 300, 110.0),
           ("b", "mixed", 300): op("b", 300, 150.0),
           ("c", "mixed", 300): op("c", 300, 2.0),
           ("d", "mixed", 300): op("d", 300, None, "timeout"),
           ("e", "mixed", 300): op("e", 300, 50.0, stall=400.0),
           ("h", "mixed", 300): op("h", 300, 50.0, stall=320.0),
           ("g", "mixed", 300): op("g", 300, 5.0)}
    # a stays within tolerance, c doubles below --min-ms, g has no baseline and h's long stall is
    # already in the baseline; b slowed, d timed out, e stalled and f vanished.
    _rows, regressed = compare(base, run, 0.2, 5.0)
    got = sorted(k[0] for k in regressed)
    assert got == ["b", "d", "e", "f"], got

    linear = {("lin", "mixed", n): op("lin", n, 0.1 * n) for n in (300, 3000, 30000)}
    quad = {("quad", "mixed", n): op("quad", n, 1e-4 * n * n) for n in (300, 3000, 30000)}
    single = {("one", "mixed", 300): op("one", 300, 4.0)}
    runs = dict(linear)
    runs.update(quad)
    runs.update(single)
    ranked = scaling(runs, 1.2)
    assert [row[0] for row in ranked] == ["quad", "lin"], ranked
    result = dict(((o, t), (s, flag)) for o, t, _sizes, s, flag in ranked)
    assert set(result) == {("lin", "mixed"), ("quad", "mixed")}, result
    assert abs(result[("lin", "mixed")][0] - 1.0) < 1e-9 and not result[("lin", "mixed")][1], result
    assert abs(result[("quad", "mixed")][0] - 2.0) < 1e-9 and result[("quad", "mixed")][1], result
    print("compare.py self-test passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--tol", type=float, default=0.2)
    ap.add_argument("--min-ms", type=float, default=5.0)
    ap.add_argument("--long-stall-ms", type=float, default=250.0)
    ap.add_argument("--scaling", action="store_true")
    ap.add_argument("--superlinear", type=float, default=1.2)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.scaling:
        if len(args.files) != 1:
            ap.error("--scaling takes one run")
        host, ops = load(args.files[0])
        print("# %s" % describe_host(host))
        flagged = 0
        for op, topology, sizes, s, flag in scaling(ops, args.superlinear):
            flagged += flag
            print("%-20s %-7s n=%-24s slope %s%s" % (op, topology, ",".join(map(str, sizes)),
                                                   "n/a" if s is None else "%.2f" % s, "  SUPERLINEAR" if flag else ""))
        print("%d superlinear op(s)" % flagged)
        return 0
    if len(args.files) != 2:
        ap.error("give a baseline and a run")
    base_host, base_ops = load(args.files[0])
    run_host, run_ops = load(args.files[1])
    print("# baseline: %s" % describe_host(base_host))
    print("# run:      %s" % describe_host(run_host))
    if base_host and run_host and base_host.get("cpu_model") != run_host.get("cpu_model"):
        print("# warning: the runs come from different CPUs")
    rows, regressions = compare(base_ops, run_ops, args.tol, args.min_ms, args.long_stall_ms)
    for row in rows:
        print(row)
    print("%d regression(s)" % len(regressions))
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
