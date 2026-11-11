#!/usr/bin/env python3
"""Compares GUI benchmark runs written by tools/bench/gui/run.sh.

    compare.py BASELINE.jsonl RUN.jsonl [--tol 0.2]   per-op change; exit 1 on a regression
    compare.py --scaling RUN.jsonl                     how each op grows with graph size
    compare.py --budgets RUN.jsonl                     a host run's per-event times against the budgets
    compare.py --self-test

A regression is an op whose rep median, per-event p50 or per-event p95 (judged only with 20 or more
events on both sides) grew by more than --tol (a fraction) and by more than --min-ms milliseconds, an op whose status fell from ok (newly over budget,
timed out or errored), an op missing from the run or not run for a reason of its own, or an op with a
stall longer than --long-stall-ms (default 250) where the baseline had none. Values within --min-ms of
each other are too close to timer noise to judge.

over_budget (and the older timeout) are censored: the rep was abandoned at its timeout, so the value is
only a lower bound, printed with censored_ms, how far the rep got. Censored on both sides is "still
over" and judged neither way; censored only in the baseline is an improvement. An op "not run" because
the step before it in the delete/undo/redo cycle failed is not a regression of its own; the failed step
is. "not run here" marks an op carried from a smaller n where it wedged Natron. An op not run because
an earlier op wedged Natron with no restart left ("Natron wedged on <op>") is "not measured" when the
baseline has no ok record of it either, and "REGRESSED: missing after wedge" when it does.

A slowdown, or a fall to over_budget without a wedge, that rests on fewer than 5 samples on either side
is "CONFIRM" rather than a regression: it does not fail the comparison, and the output ends with the
run.sh --rerun command that re-runs those ops with 5 reps (--appimage PATH puts the AppImage's path in
it; otherwise it names the run's AppImage file).

--budgets judges each budgeted op at the largest n of each topology on its per-event p50: 16 ms per
event for interaction ops, 100 ms for edits (OP_BUDGET_MS in gui_bench.py). Over budget, censored or not
run fails; exit 1 on any failure. Budgets are for host runs; Xvfb numbers are only for regressions.

The header names each side's build (AppImage file name, size, mtime and sha256) and the harness
commit, and warns when both sides are the same AppImage. When a (topology, n, seed) graph that both
sides generated has different hashes, the two runs timed different graphs: a warning on stderr names
each mismatch, and no row for those sizes is a like-for-like comparison.

When both runs carry the per-event split (step_wall_*, step_cpu_*, loop_pass_*), a line under
each op shows the medians of the step's wall time, its main-thread CPU and the loop pass after it.

--scaling fits log(median) against log(n) per (op, topology) over the sizes present and marks
slopes above --superlinear (default 1.2) as superlinear, steepest first: those are the ops whose cost grows faster
than the graph.
"""
import argparse
import ast
import json
import math
import os
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


def load_graphs(path):
    """{(topology, n, seed): {hash}} of the graph records in path."""
    graphs = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if rec.get("type") == "graph":
                graphs.setdefault((rec["topology"], rec["n"], rec.get("seed")), set()).add(rec.get("hash"))
    return graphs


def graph_mismatches(base_graphs, run_graphs):
    """[(topology, n, seed, base hashes, run hashes)] for each graph both sides built that differs."""
    out = []
    for key in sorted(set(base_graphs) & set(run_graphs), key=lambda k: (k[0], k[1], str(k[2]))):
        if base_graphs[key] != run_graphs[key]:
            out.append(key + (sorted(map(str, base_graphs[key])), sorted(map(str, run_graphs[key]))))
    return out


CENSORED = ("timeout", "over_budget")
CONFIRM_SAMPLES = 5
PER_EVENT_FIELDS = (("step_wall_p50", "p50"), ("step_wall_p95", "p95"))
# Below this many events a p95 is the slowest event or next to it, which is noise rather than a tail.
P95_MIN_SAMPLES = 20


def censored(rec):
    return rec.get("status") in CENSORED


def samples(rec):
    """How many timings rec's numbers rest on: its timed events, else its reps; None when it says neither."""
    count = rec.get("samples_total") or len(rec.get("samples_ms") or ())
    return count if count else rec.get("reps")


def too_few(*recs):
    counts = [samples(rec) for rec in recs]
    return any(c is not None and c < CONFIRM_SAMPLES for c in counts)


def fewest(*recs):
    return min(c for c in (samples(rec) for rec in recs) if c is not None)


WEDGE_REASON = "Natron wedged on "


def after_wedge(rec):
    return rec.get("status") == "not_run" and (rec.get("reason") or "").startswith(WEDGE_REASON)


def excused_not_run(key, rec, run_ops):
    """True when rec is not run because the step it names, at the same topology and n, was not ok."""
    if rec.get("status") != "not_run":
        return False
    words = (rec.get("reason") or "").split()
    if not words or words[0] == key[0]:
        return False
    before = run_ops.get((words[0], key[1], key[2]))
    return before is not None and before.get("status") != "ok"


def compare(base_ops, run_ops, tol, min_ms, long_stall_ms=250.0):
    """(rows, regressions, confirms) where rows are printable lines, regressions the keys that fail and
    confirms the keys that look regressed on fewer than CONFIRM_SAMPLES timings."""
    rows, regressions, confirms = [], [], []
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
        bst, rst = b.get("status"), r.get("status")
        few = too_few(b, r)
        if rst == "not_run":
            if after_wedge(r):
                if bst == "ok":
                    rows.append("%s  REGRESSED: missing after wedge (%s)" % (label, r.get("reason")))
                    regressions.append(key)
                else:
                    rows.append("%s  not measured (%s)" % (label, r.get("reason")))
            elif excused_not_run(key, r, run_ops):
                rows.append("%s  not run: %s" % (label, r.get("reason")))
            elif bst == "not_run":
                rows.append("%s  not run in either" % label)
            else:
                rows.append("%s  REGRESSED: not run (%s)" % (label, r.get("reason")))
                regressions.append(key)
            continue
        if bst == "not_run":
            rows.append("%s  baseline not run (%s) -> %s" % (label, b.get("reason"), outcome(r)))
            continue
        if censored(b) and censored(r):
            rows.append("%s  still over:%s /%s" % (label, timed_out(b), timed_out(r)))
            continue
        if bst == "ok" and censored(r):
            if r.get("wedged") or not few:
                rows.append("%s  REGRESSED status ok -> %s%s" % (label, rst, timed_out(r)))
                regressions.append(key)
            else:
                rows.append("%s  CONFIRM status ok -> %s%s on %d sample(s)" % (label, rst, timed_out(r), fewest(b, r)))
                confirms.append(key)
            continue
        if bst == "ok" and rst != "ok":
            rows.append("%s  REGRESSED status %s -> %s%s" % (label, bst, rst, timed_out(r)))
            regressions.append(key)
            continue
        if bst != "ok" and rst == "ok" and r.get("median_ms") is not None:
            if censored(b):
                rows.append("%s  baseline %s%s -> %10.2f ms  improved" % (label, censored_word(b), timed_out(b),
                                                                         r["median_ms"]))
            else:
                rows.append("%s  baseline %s -> %10.2f ms" % (label, bst, r["median_ms"]))
            continue
        bm, rm = b.get("median_ms"), r.get("median_ms")
        if bm is None or rm is None:
            rows.append("%s  no median (%s / %s)" % (label, bst, rst))
            continue
        change = (rm - bm) / bm if bm > 0 else 0.0
        slower = [name for name, bv, rv in metrics(b, r) if worse(bv, rv, tol, min_ms)]
        bs, rs = b.get("stalls", {}).get("max_ms", 0.0), r.get("stalls", {}).get("max_ms", 0.0)
        if rs > long_stall_ms >= bs:
            verdict = "REGRESSED stall %.0f ms (baseline max %.0f ms)" % (rs, bs)
            regressions.append(key)
        elif slower and few:
            verdict = "CONFIRM %s slower on %d sample(s)" % ("/".join(slower), min(samples(b), samples(r)))
            confirms.append(key)
        elif slower:
            verdict = "REGRESSED" if slower == ["median"] else "REGRESSED %s" % "/".join(slower)
            regressions.append(key)
        else:
            verdict = "improved" if change < -tol and bm - rm > min_ms else ""
        rows.append("%s  %10.2f -> %10.2f ms  %+7.1f%%  %s" % (label, bm, rm, 100.0 * change, verdict))
        split = split_row(label, b, r)
        if split:
            rows.append(split)
    return rows, regressions, confirms


def metrics(b, r):
    """[(name, baseline, run)] of the rep median and the per-event percentiles both sides carry."""
    out = [("median", b.get("median_ms"), r.get("median_ms"))]
    for field, name in PER_EVENT_FIELDS:
        if b.get(field) is None or r.get(field) is None:
            continue
        if name == "p95" and min(samples(b) or 0, samples(r) or 0) < P95_MIN_SAMPLES:
            continue
        out.append(("event " + name, b[field], r[field]))
    return out


def worse(bv, rv, tol, min_ms):
    return bv is not None and rv is not None and bv > 0 and (rv - bv) / bv > tol and rv - bv > min_ms


def censored_word(rec):
    return "timed out" if rec.get("status") == "timeout" else "over budget"


def outcome(rec):
    if rec.get("status") == "ok" and rec.get("median_ms") is not None:
        return "%.2f ms" % rec["median_ms"]
    return "%s%s" % (rec.get("status"), timed_out(rec))


def timed_out(rec):
    """' (>N s)' for a censored record, with how far the rep got (censored_ms) and, for one carried from
    a smaller n where it wedged Natron, where it was measured."""
    if not censored(rec) or rec.get("timeout_s") is None:
        return ""
    if rec.get("wedged_at_n") is not None or (rec.get("not_run") and rec.get("status") == "timeout"):
        return " (>%.0f s at n=%d, not run here)" % (rec["timeout_s"], rec.get("wedged_at_n", 0))
    text = " (>%.0f s" % rec["timeout_s"]
    if rec.get("wedged"):
        text += ", wedged"
    if rec.get("censored_ms") is not None:
        text += ", reached %.0f ms" % rec["censored_ms"]
    return text + ")"


SPLIT_FIELDS = (("step_wall_median_ms", "wall"), ("step_cpu_median_ms", "cpu"), ("loop_pass_median_ms", "loop"))


def split_row(label, b, r):
    """A line comparing the per-event timing split, with the per-event p50 and p95 when both sides have
    them, or None when either side lacks the split."""
    parts = []
    for field, name in SPLIT_FIELDS:
        bv, rv = b.get(field), r.get(field)
        if bv is None or rv is None:
            return None
        parts.append("%s %.2f -> %.2f" % (name, bv, rv))
    for field, name in PER_EVENT_FIELDS:
        bv, rv = b.get(field), r.get(field)
        if bv is not None and rv is not None:
            parts.append("%s %.2f -> %.2f" % (name, bv, rv))
    return "%s    per event ms: %s" % (label, ", ".join(parts))


def rerun_command(keys, run_host, baseline, appimage=None):
    """The run.sh line that re-runs keys' ops with 5 reps on the run's build, against baseline."""
    ops = sorted(set(k[0] for k in keys))
    topologies = sorted(set(k[1] for k in keys))
    sizes = sorted(set(k[2] for k in keys))
    mode = (run_host or {}).get("mode")
    cmd = ["tools/bench/gui/run.sh", "--%s" % (mode if mode in ("xvfb", "host") else "xvfb"),
           "--rerun", ",".join(ops), "--reps", str(CONFIRM_SAMPLES),
           "--topologies", ",".join(topologies), "--sizes", ",".join(map(str, sizes))]
    image = appimage or (run_host or {}).get("appimage_name")
    if image:
        cmd += ["--appimage", image]
    cmd += ["--baseline", baseline]
    return " ".join(cmd)


def load_budgets(path=None):
    """OP_BUDGET_MS from gui_bench.py, evaluated from its source so this script needs no Natron."""
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "gui_bench.py")
    with open(path) as f:
        tree = ast.parse(f.read(), path)
    names = ("INTERACTION_BUDGET_MS", "EDIT_BUDGET_MS", "OP_BUDGET_MS")
    scope = {"dict": dict, "__builtins__": {}}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and getattr(node.targets[0], "id", None) in names:
            scope[node.targets[0].id] = eval(compile(ast.Expression(node.value), path, "eval"), scope)
    return scope["OP_BUDGET_MS"]


def per_event_ms(rec, field="step_wall_p50"):
    """rec's per-event time: the recorded percentile, else the rep median over its events."""
    if rec.get(field) is not None:
        return rec[field]
    if field == "step_wall_p50" and rec.get("median_ms") is not None:
        return rec["median_ms"] / max(1, rec.get("events_per_rep") or 1)
    return None


def budgets(run_ops, table):
    """(rows, failures): each budgeted op at its largest n per topology, judged on its per-event p50."""
    largest = {}
    for op, topology, n in run_ops:
        if op in table and n > largest.get((op, topology), -1):
            largest[(op, topology)] = n
    rows, failures = [], []
    for (op, topology), n in sorted(largest.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        rec = run_ops[(op, topology, n)]
        budget = table[op]
        label = "%-20s %-7s %6d  budget %5.0f ms/event" % (op, topology, n, budget)
        p50, p95 = per_event_ms(rec), per_event_ms(rec, "step_wall_p95")
        reached = "p50 %8.2f" % p50 if p50 is not None else "p50      n/a"
        reached += "  p95 %8.2f" % p95 if p95 is not None else "  p95      n/a"
        status = rec.get("status")
        if status == "not_run":
            rows.append("%s  FAIL not run (%s)" % (label, rec.get("reason")))
            failures.append((op, topology, n))
        elif censored(rec):
            rows.append("%s  %s  FAIL %s%s" % (label, reached, status, timed_out(rec)))
            failures.append((op, topology, n))
        elif status != "ok" or p50 is None:
            rows.append("%s  FAIL %s" % (label, status))
            failures.append((op, topology, n))
        elif p50 > budget:
            rows.append("%s  %s  FAIL" % (label, reached))
            failures.append((op, topology, n))
        else:
            rows.append("%s  %s  pass%s" % (label, reached, "  (p95 over)" if p95 is not None and p95 > budget else ""))
    return rows, failures


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


def describe_build(host):
    """The build under test: the AppImage's file identity when there is one, else the build type."""
    if not host.get("appimage_name"):
        return "%s (no AppImage identity recorded)" % host.get("build_type")
    return "appimage %s, %s bytes, modified %s, sha256 %s" % (
        host.get("appimage_name"), host.get("appimage_bytes"), host.get("appimage_mtime"),
        (host.get("appimage_sha256") or "?")[:16])


def describe_host(host):
    if not host:
        return "no host record"
    text = "%s, %s (%s cores), %s, %s; harness %s" % (host.get("hostname"), host.get("cpu_model"), host.get("cores"),
                                                     host.get("qt_platform"), describe_build(host), host.get("git_sha"))
    if host.get("mixed_builds"):
        text += "; WARNING merged from different builds: %s" % " | ".join(host["mixed_builds"])
    return text


def build_id(host):
    """A key that differs between builds: the AppImage's sha256, else its name, size and mtime."""
    if not host:
        return None
    if host.get("appimage_sha256"):
        return host["appimage_sha256"]
    if host.get("appimage_name"):
        return "%s/%s/%s" % (host.get("appimage_name"), host.get("appimage_bytes"), host.get("appimage_mtime"))
    return None


def self_test_censored(op):
    def timed(name, median, count, **extra):
        rec = dict(op(name, 300, median), reps=count, samples_ms=[median] * count)
        rec.update(extra)
        return rec

    def over(name, count=1, **extra):
        rec = dict(op(name, 300, None, "over_budget"), timeout_s=12.0, censored_ms=12100.0, reps=0,
                   samples_ms=[100.0] * count)
        rec.update(extra)
        return rec

    def k(name):
        return (name, "mixed", 300)

    base = {k("still"): over("still"), k("fixed"): over("fixed"), k("slow"): timed("slow", 50.0, 5),
            k("hang"): timed("hang", 50.0, 5), k("blip"): timed("blip", 50.0, 1), k("lone"): timed("lone", 100.0, 1),
            k("tail"): timed("tail", 50.0, 40, step_wall_p50=1.0, step_wall_p95=20.0),
            k("delete_1000"): timed("delete_1000", 900.0, 1), k("undo"): timed("undo", 90.0, 1),
            k("redo"): timed("redo", 90.0, 1), k("gone"): timed("gone", 50.0, 5)}
    run = {k("still"): over("still"), k("fixed"): timed("fixed", 40.0, 5), k("slow"): over("slow", 5),
           k("hang"): over("hang", wedged=True), k("blip"): over("blip"), k("lone"): timed("lone", 300.0, 1),
           k("tail"): timed("tail", 50.0, 40, step_wall_p50=1.0, step_wall_p95=60.0),
           k("delete_1000"): timed("delete_1000", 900.0, 1), k("undo"): over("undo", 0, wedged=True),
           k("redo"): dict(op("redo", 300, None, "not_run"), not_run=True, reason="undo wedged Natron"),
           k("gone"): dict(op("gone", 300, None, "not_run"), not_run=True, reason="gone was deselected")}
    base.update({k("lost"): timed("lost", 50.0, 5), k("unseen"): over("unseen"),
                 k("twice"): dict(op("twice", 300, None, "not_run"), not_run=True, reason="Natron wedged on hang")})
    for name in ("lost", "unseen", "twice"):
        run[k(name)] = dict(op(name, 300, None, "not_run"), not_run=True, reason="Natron wedged on hang")
    rows, regressed, confirms = compare(base, run, 0.2, 5.0)
    text = dict((row.split()[0], row) for row in rows if "per event" not in row)
    # still stays over and fixed came back; slow went over on 5 samples and hang wedged, tail's per-event
    # p95 tripled and gone was dropped for no failed step before it: those fail. blip went over and lone
    # tripled on one sample each: those want a re-run. redo follows the undo that wedged. lost, unseen and
    # twice were left unrun by hang's wedge: only lost, ok in the baseline, is missing.
    assert sorted(k[0] for k in regressed) == ["gone", "hang", "lost", "slow", "tail", "undo"], (regressed, rows)
    assert sorted(k[0] for k in confirms) == ["blip", "lone"], (confirms, rows)
    assert "still over: (>12 s, reached 12100 ms) / (>12 s, reached 12100 ms)" in text["still"], text["still"]
    assert "baseline over budget (>12 s, reached 12100 ms) ->" in text["fixed"] and "improved" in text["fixed"]
    assert "CONFIRM status ok -> over_budget (>12 s, reached 12100 ms) on 1 sample(s)" in text["blip"], text["blip"]
    assert "CONFIRM median slower on 1 sample(s)" in text["lone"], text["lone"]
    assert "REGRESSED event p95" in text["tail"], text["tail"]
    assert "not run: undo wedged Natron" in text["redo"] and "REGRESSED" not in text["redo"], text["redo"]
    assert "REGRESSED: missing after wedge (Natron wedged on hang)" in text["lost"], text["lost"]
    assert "not measured (Natron wedged on hang)" in text["unseen"] and "not measured" in text["twice"], rows
    command = rerun_command(confirms, {"mode": "host", "appimage_name": "N.AppImage"}, "/b.jsonl")
    assert command == ("tools/bench/gui/run.sh --host --rerun blip,lone --reps 5 --topologies mixed --sizes 300 "
                       "--appimage N.AppImage --baseline /b.jsonl"), command

    table = load_budgets()
    assert table["hover"] == 16.0 and table["delete_1000"] == 100.0 and "load" not in table, table
    host = {("hover", "mixed", 300): dict(op("hover", 300, 4000.0), step_wall_p50=20.0, step_wall_p95=30.0),
            ("hover", "mixed", 3000): dict(op("hover", 3000, 2000.0), step_wall_p50=10.0, step_wall_p95=30.0),
            ("pan", "mixed", 3000): dict(op("pan", 3000, None, "over_budget"), timeout_s=3.2, censored_ms=3300.0),
            ("undo", "mixed", 3000): dict(op("undo", 3000, 60.0), events_per_rep=1),
            ("redo", "mixed", 3000): dict(op("redo", 3000, None, "not_run"), reason="undo failed"),
            ("load", "mixed", 3000): op("load", 3000, 9000.0)}
    rows, failures = budgets(host, table)
    assert sorted(f[0] for f in failures) == ["pan", "redo"], (failures, rows)
    assert len(rows) == 4 and not any("load" in row for row in rows), rows
    hover = [row for row in rows if row.startswith("hover")]
    assert len(hover) == 1 and " 3000 " in hover[0] and "pass  (p95 over)" in hover[0], hover


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
    _rows, regressed, _confirms = compare(base, run, 0.2, 5.0)
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
    with_split = dict(op("a", 300, 10.0), step_wall_median_ms=4.0, step_cpu_median_ms=3.5, loop_pass_median_ms=6.0)
    rows, _, _ = compare({("a", "mixed", 300): with_split}, {("a", "mixed", 300): with_split}, 0.2, 5.0)
    assert len(rows) == 2 and "cpu 3.50 -> 3.50" in rows[1], rows
    rows, _, _ = compare({("a", "mixed", 300): op("a", 300, 10.0)}, {("a", "mixed", 300): with_split}, 0.2, 5.0)
    assert len(rows) == 1, rows
    wedged = dict(op("w", 300, None, "timeout"), timeout_s=300.0, wedged=True)
    carried = dict(op("w", 1000, None, "timeout"), timeout_s=300.0, wedged=True, not_run=True, wedged_at_n=300)
    rows, regressed, _ = compare({("w", "mixed", 300): wedged, ("w", "mixed", 1000): carried},
                              {("w", "mixed", 300): op("w", 300, 40.0), ("w", "mixed", 1000): op("w", 1000, 90.0)},
                              0.2, 5.0)
    assert not regressed, regressed
    assert "baseline timed out (>300 s, wedged) ->" in rows[0] and rows[0].endswith("improved"), rows
    assert "(>300 s at n=300, not run here)" in rows[1], rows
    rows, regressed, _ = compare({("w", "mixed", 300): op("w", 300, 40.0)}, {("w", "mixed", 300): wedged}, 0.2, 5.0)
    assert regressed and "timeout (>300 s, wedged)" in rows[0], rows
    self_test_censored(op)
    mismatched = graph_mismatches({("mixed", 300, 1): {"aa"}, ("mixed", 1000, 1): {"bb"}, ("comp", 300, 1): {"cc"}},
                                  {("mixed", 300, 1): {"aa"}, ("mixed", 1000, 1): {"bx"}, ("wide", 300, 1): {"dd"}})
    assert mismatched == [("mixed", 1000, 1, ["bb"], ["bx"])], mismatched
    same = {"appimage_name": "a.AppImage", "appimage_sha256": "ab" * 32}
    assert build_id(same) == build_id(dict(same)) and build_id({"build_type": "release"}) is None
    assert "sha256 abababababababab" in describe_host(dict(same, hostname="h")), describe_host(same)
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
    ap.add_argument("--budgets", action="store_true")
    ap.add_argument("--appimage", help="the AppImage path the printed re-run command names")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.budgets:
        if len(args.files) != 1:
            ap.error("--budgets takes one run")
        host, ops = load(args.files[0])
        print("# %s" % describe_host(host))
        if host and host.get("mode") != "host":
            print("# warning: budgets are judged on a host run; this one is %s" % host.get("mode"))
        rows, failures = budgets(ops, load_budgets())
        for row in rows:
            print(row)
        print("%d op(s) over budget" % len(failures))
        return 1 if failures else 0
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
    if build_id(base_host) is not None and build_id(base_host) == build_id(run_host):
        print("# warning: the baseline and the run are the same AppImage")
    mismatched = graph_mismatches(load_graphs(args.files[0]), load_graphs(args.files[1]))
    if mismatched:
        sys.stderr.write("WARNING: the baseline and the run timed different graphs; the rows for these sizes are "
                         "not comparable; re-record the older side with the current harness:\n")
        for topology, n, seed, bh, rh in mismatched:
            sys.stderr.write("WARNING:   %s n=%d seed=%s: baseline hash %s, run hash %s\n" % (
                topology, n, seed, ",".join(h[:12] for h in bh), ",".join(h[:12] for h in rh)))
        print("# warning: %d graph(s) differ from the baseline's; see stderr" % len(mismatched))
    rows, regressions, confirms = compare(base_ops, run_ops, args.tol, args.min_ms, args.long_stall_ms)
    for row in rows:
        print(row)
    print("%d regression(s)" % len(regressions))
    if confirms:
        print("%d op(s) to confirm; they rest on fewer than %d samples. Re-run them with:" % (
            len(confirms), CONFIRM_SAMPLES))
        print("  " + rerun_command(confirms, run_host, os.path.abspath(args.files[0]), args.appimage))
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
