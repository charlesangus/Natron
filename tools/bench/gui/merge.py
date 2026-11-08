#!/usr/bin/env python3
"""Merges repeated GUI benchmark runs into one median-of-runs baseline.

    merge.py --out BASELINE.jsonl RUN1.jsonl RUN2.jsonl [RUN3.jsonl ...] [--spread]

Each op's median_ms, p95_ms, min_ms and max_ms become the median across the runs, and its stall
maximum the largest across the runs, so every contributing run compares clean against the result.
An op present in only some of the runs is merged from those runs (the `runs` field counts them).
An op that is not ok in any run that has it is written with that run's status and no timings. --spread prints
the run-to-run spread of each op, (max - min) / median of the per-run medians, worst first.
"""
import argparse
import json
import statistics
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import compare


def merge(runs):
    keys = set()
    for ops in runs:
        keys |= set(ops)
    merged, spreads = {}, {}
    for key in sorted(keys):
        recs = [ops[key] for ops in runs if key in ops]
        bad = [r for r in recs if r.get("status") != "ok"]
        if bad:
            merged[key] = dict(bad[0])
            continue
        rec = dict(recs[0])
        for field in ("median_ms", "p95_ms", "min_ms", "max_ms", "per_event_max_ms", "cpu_ms"):
            vals = [r[field] for r in recs if r.get(field) is not None]
            if vals:
                rec[field] = round(statistics.median(vals), 3)
        stalls = [r.get("stalls", {}) for r in recs]
        rec["stalls"] = {"count": max(s.get("count", 0) for s in stalls),
                         "max_ms": max(s.get("max_ms", 0.0) for s in stalls),
                         "total_ms": max(s.get("total_ms", 0.0) for s in stalls)}
        rec["runs"] = len(recs)
        medians = [r["median_ms"] for r in recs]
        mid = statistics.median(medians)
        if len(recs) > 1:
            spreads[key] = (max(medians) - min(medians)) / mid if mid > 0 else 0.0
        merged[key] = rec
    return merged, spreads


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--spread", action="store_true")
    args = ap.parse_args()
    loaded = [compare.load(p) for p in args.files]
    merged, spreads = merge([ops for _, ops in loaded])
    graphs = {}
    for p in args.files:
        with open(p) as f:
            for line in f:
                rec = json.loads(line) if line.strip() else {}
                if rec.get("type") == "graph":
                    graphs[(rec["topology"], rec["n"])] = rec
    with open(args.out, "w") as out:
        host = dict(loaded[0][0] or {})
        host["merged_runs"] = len(args.files)
        out.write(json.dumps(host, sort_keys=True) + "\n")
        for key in sorted(graphs):
            out.write(json.dumps(graphs[key], sort_keys=True) + "\n")
        for key in sorted(merged):
            out.write(json.dumps(merged[key], sort_keys=True) + "\n")
    if args.spread:
        for key, s in sorted(spreads.items(), key=lambda kv: -kv[1]):
            print("%-20s %-7s %6d  spread %6.1f%%  median %10.2f ms" % (key[0], key[1], key[2], 100 * s, merged[key]["median_ms"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
