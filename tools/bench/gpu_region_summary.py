#!/usr/bin/env python3
"""Splits GPU region time from NATRON_RENDER_PROFILE files into its phases, per region and per node.

    python3 -I tools/bench/gpu_region_summary.py [--skip-frames N] [--import-rate GBPS] [--import-rate-up GBPS] PROFILE_PREFIX_OR_FILE...
    python3 -I tools/bench/gpu_region_summary.py --copy-rate PROFILE_PREFIX_OR_FILE...
    python3 -I tools/bench/gpu_region_summary.py --driver OUT_DIR_OR_DRIVER_JSONL
    python3 -I tools/bench/gpu_region_summary.py --self-test

A PROFILE argument is a file, or a prefix whose suffixed files (<prefix>.*) are all read. Each argument is
one run and is summarised on its own. gpu_region_bench.sh names a run <case>-<gpu mode>-<kind>-r<round>
and writes <tag>.marks.jsonl beside <tag>.prof: one line per render with its label (first, b, c, playback,
a-cold, a-warm), the profile frame it starts at and its in-process time. With marks the regions are grouped
by render label, the first frame is kept (it carries the one-time costs; --skip-frames overrides), and a
per-render table shows the render time, the Write task time in it, and their difference. Without marks the
first frame is skipped as before.

--driver reads driver.jsonl of a bench output directory and prints, per build (new, or base when the run had
BENCH_BASE_APPIMAGE), gpu mode and kind, the median peak RSS (vmhwm_kb) and the mean thp_fault_alloc,
thp_fault_fallback, thp_split_pmd and pgfault deltas of the sessions, then the new/base ratio of the
in-process render time (the sum of the marks' wall_s) per kind and mode as the median over same-round
pairs. The vmstat deltas are system-wide.

prepareWaitMs is the time the region waited for its transfer destinations to be prepared. The pipelined
floor is max(0, T - K*(n-1)/n) with T the exposed plus hidden transfer ms, K the kernel ms and n the strips
(1 when the region ran unstripped): the transfer time left once every strip but one overlaps a kernel.
With --import-rate GBPS (a measured GpuBench host-import download rate in GB/s) T is instead
uploadBytes / up rate + downloadBytes / rate, so the floor shows what the transfers would cost at those
rates. --import-rate-up GBPS sets the upload rate; without it uploads are priced at --import-rate.

Which fields lie inside wallMs. wallMs runs from the end of planning to the end of the region:
  inside   kernelMs, transferExposedMs, recordMs, acquireMs, submitWaitMs
  outside  planMs (it precedes the interval, so a region costs planMs + wallMs)
  overlaps transferHiddenMs (by definition concurrent with kernels); uploadMs and downloadMs are the
           transfer totals, of which only the exposed part is in wallMs
other = wallMs - (kernelMs + transferExposedMs + recordMs + acquireMs + submitWaitMs). Pipelined
regions record the next strip while the device runs and wait on strips the device is already
computing, so recordMs and submitWaitMs overlap kernelMs there and other can be negative; the sign
tells the overlap, it is not an error. Imports have no duration of their own (their time is inside
the acquire and transfer phases), so they are shown as a count and a size.

A member's kernel time is the wallNs of its task record (device timestamps, the cost-weight split
only on devices without them). pred xfer is predictedTransferMs: the transfer time placement's cost
model predicts for the region's uploadBytes and downloadBytes, to compare with uploadMs + downloadMs.
It reads n/a when the model knows no bandwidth (the record holds null) or the record has no such field.

The transfers table lists, per region, the copies its record's uploads and downloads arrays hold, one line
per direction: their count, size (min/median/max, MB), the device rate (bytes over the summed device time of
the timestamped copies, GB/s), the timestamped share of the bytes, the share of the timestamped device time
that the last strip's copies spent, how many were staged through the transfer ring instead of imported in
place, how many downloads were of another region's device image (pinned; the frame store makes those, so they
are never timestamped), and the queue. deviceMs is null for a copy that was not timestamped (a pinned
download, or a device without timestamps); the rate and the last-strip share then read n/a. The device time
is the transfer queue's own: it does not replace transferExposedMs, which the host measures.

--copy-rate appends, after each render label's tables, the copy contention table: the device rate of the
timestamped copies (GB/s, bytes over summed deviceMs) grouped by direction, by whether the copy overlapped a
kernel, and by queue (family.index). A copy overlaps a kernel when more than half of its [beginNs, endNs]
lies inside the union of the region's memberIntervals; both are timestamps from the region's earliest one.
A copy with no timestamps is counted but has no rate, and a region with no memberIntervals counts its copies
as not overlapped. The run's tag names the comp and size, so each run's table is that comp at that size.
"""
import collections
import glob
import json
import os
import re
import sys
import tempfile

TRANSFERS = ("uploads", "downloads")
PHASES = ("kernel", "exposed", "record", "acquire", "submitWait")
FIELDS = ("wallMs", "planMs", "kernelMs", "transferExposedMs", "transferHiddenMs", "uploadMs", "downloadMs",
          "recordMs", "acquireMs", "submitWaitMs", "prepareWaitMs")
KINDS = ("a-cold", "a-warm", "bc", "playback")
TAG_RE = re.compile(r"^(?P<case>.+)-(?P<gpu>auto|off)-(?P<kind>%s)-r(?P<round>\d+)(?P<base>-base)?$" % "|".join(KINDS))
PROFILE_RE = re.compile(r"^(?P<stem>.*)\.prof(\.\d+)?$")


def read_records(paths):
    files = []
    for p in paths:
        if os.path.isfile(p):
            files.append(p)
        else:
            files += sorted(f for f in glob.glob(glob.escape(p) + ".*") if os.path.isfile(f))
    records = []
    for f in files:
        with open(f) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                except ValueError:
                    continue
    return records


def other_ms(region):
    named = sum(region.get(k, 0.0) for k in ("kernelMs", "transferExposedMs", "recordMs", "acquireMs", "submitWaitMs"))
    return region.get("wallMs", 0.0) - named


def pipelined_floor_ms(region, import_rate_gbps=None, import_rate_up_gbps=None):
    strips = max(region.get("strips", 0), 1)
    if import_rate_gbps:
        up_rate = import_rate_up_gbps or import_rate_gbps
        transfer = (region.get("uploadBytes", 0) / (up_rate * 1e9) + region.get("downloadBytes", 0) / (import_rate_gbps * 1e9)) * 1e3
    else:
        transfer = region.get("transferExposedMs", 0.0) + region.get("transferHiddenMs", 0.0)
    return max(0.0, transfer - region.get("kernelMs", 0.0) * (strips - 1) / strips)


def marks_path(arg):
    m = PROFILE_RE.match(arg)
    return m.group("stem") + ".marks.jsonl" if m else None


def read_marks(arg):
    path = marks_path(arg)
    if not path or not os.path.isfile(path):
        return []
    marks = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                marks.append(json.loads(line))
    return marks


def label_for(marks, frame):
    for m in marks:
        if m["profileFrame"] <= frame < m["profileFrame"] + m["frames"]:
            return m["label"]
    return "-"


def parse_tag(arg):
    m = PROFILE_RE.match(os.path.basename(arg))
    stem = m.group("stem") if m else os.path.basename(arg)
    t = TAG_RE.match(stem)
    return t.groupdict() if t else {"case": stem, "gpu": "?", "kind": "?", "round": "?"}


def read_jsonl(path):
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def median(values):
    v = sorted(values)
    n = len(v)
    if not n:
        return 0.0
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def render_s(out_dir, tag):
    path = os.path.join(out_dir, tag + ".marks.jsonl")
    if not os.path.isfile(path):
        return None
    marks = read_jsonl(path)
    return sum(m["wall_s"] for m in marks) if marks else None


THP_FIELDS = ("thp_fault_alloc", "thp_fault_fallback", "thp_split_pmd", "pgfault")


def summarise_driver(out_dir, rows):
    """rows are driver.jsonl records; returns (per-group entries, ratio entries)."""
    sessions = []
    for r in rows:
        t = TAG_RE.match(r["tag"])
        if not t or r.get("exit") != 0:
            continue
        sessions.append(dict(r, case=t.group("case"), mode=t.group("gpu"), kind=t.group("kind"),
                             round=t.group("round"), build="base" if t.group("base") else "new",
                             render_s=render_s(out_dir, r["tag"])))
    groups = collections.OrderedDict()
    for s in sessions:
        groups.setdefault((s["build"], s["kind"], s["mode"]), []).append(s)
    entries = []
    for (build, kind, mode), g in groups.items():
        e = {"build": build, "kind": kind, "mode": mode, "n": len(g),
             "vmhwmMB": median([s.get("vmhwm_kb", 0) for s in g]) / 1024.0}
        for f in THP_FIELDS:
            e[f] = mean([s.get(f, 0) for s in g])
        e["thp"] = ",".join(sorted({"%s/%s/%s" % (s.get("thp_enabled", "?"), s.get("thp_defrag", "?"),
                                                  s.get("thp_use_zero_page", "?")) for s in g}))
        entries.append(e)
    pairs = collections.defaultdict(dict)
    for s in sessions:
        if s["render_s"]:
            pairs[(s["kind"], s["mode"], s["case"], s["round"])][s["build"]] = s["render_s"]
    ratios = collections.defaultdict(list)
    for (kind, mode, _, _), d in pairs.items():
        if "new" in d and "base" in d:
            ratios[(kind, mode)].append(d["new"] / d["base"])
    return entries, [(k, m, len(v), median(v)) for (k, m), v in sorted(ratios.items())]


def format_driver(entries, ratios):
    lines = ["Per build, kind and gpu mode (peak RSS median MB; THP and page-fault deltas mean per session)"]
    lines.append(" | ".join(("build", "kind", "gpu", "runs", "peak RSS MB", "thp_fault_alloc", "thp_fault_fallback",
                             "thp_split_pmd", "pgfault", "thp enabled/defrag/zero_page")))
    for e in entries:
        lines.append(" | ".join((e["build"], e["kind"], e["mode"], str(e["n"]), "%.0f" % e["vmhwmMB"],
                                 "%.0f" % e["thp_fault_alloc"], "%.0f" % e["thp_fault_fallback"],
                                 "%.0f" % e["thp_split_pmd"], "%.0f" % e["pgfault"], e["thp"])))
    if ratios:
        lines += ["", "new/base in-process render time (median over same-round pairs)",
                  " | ".join(("kind", "gpu", "pairs", "new/base"))]
        for kind, mode, n, r in ratios:
            lines.append(" | ".join((kind, mode, str(n), "%.3f" % r)))
    return "\n".join(lines)


def build(records, skip_frames, marks=()):
    frames = sorted({r["frame"] for r in records if "frame" in r})
    kept = set(frames[skip_frames:])
    tasks = {}
    regions = []
    for r in records:
        if r.get("frame") not in kept:
            continue
        if r.get("kind") == "region":
            regions.append(r)
        elif "task" in r:
            tasks[(r["frame"], r["task"])] = r
    rows = []
    for g in regions:
        members = []
        for t in g.get("tasks", []):
            task = tasks.get((g["frame"], t))
            if task is None:
                continue
            on_gpu = task.get("device") == "gpu" and not g.get("fallback")
            members.append({"node": task.get("node", "?"), "plugin": task.get("plugin", "?"),
                            "kernelMs": task["wallNs"] / 1e6 if on_gpu else 0.0})
        rows.append({"region": g, "members": members, "signature": "+".join(m["node"] for m in members) or "?",
                     "label": label_for(marks, g["frame"]) if marks else "-"})
    return rows, len(kept)


def transfer_stats(region, key):
    """The figures of one region's uploads or downloads array, or None when it holds no copy."""
    copies = region.get(key) or []
    if not copies:
        return None
    sizes = sorted(d["bytes"] for d in copies)
    timed = [d for d in copies if d.get("deviceMs") is not None]
    time_ms = sum(d["deviceMs"] for d in timed)
    timed_bytes = sum(d["bytes"] for d in timed)
    rate = timed_bytes / (time_ms * 1e6) if time_ms > 0 else None
    total = sum(sizes)
    last_share = None
    strips = [d["strip"] for d in timed if d.get("strip", -1) >= 0]
    if strips and time_ms > 0:
        last = max(strips)
        last_share = sum(d["deviceMs"] for d in timed if d.get("strip") == last) / time_ms
    return {"count": len(copies), "min": sizes[0], "median": median(sizes), "max": sizes[-1], "rate": rate,
            "timedShare": timed_bytes / total if total > 0 else 0.0, "lastShare": last_share,
            "staged": sum(1 for d in copies if d.get("staged")), "pinned": sum(1 for d in copies if d.get("pinned")),
            "queue": "%d.%d" % (copies[0].get("queueFamily", 0), copies[0].get("queueIndex", 0))}


def overlap_ns(begin, end, intervals):
    """The length of [begin, end] covered by the union of the intervals, which are sorted by begin."""
    covered = 0
    cursor = begin
    for b, e in intervals:
        b = max(b, cursor)
        e = min(e, end)
        if e > b:
            covered += e - b
            cursor = e
    return covered


def copy_rate_groups(regions):
    """Per (direction, overlapped, queue): copy count, timed bytes, timed device ms and untimed count."""
    groups = collections.OrderedDict()
    for g in regions:
        intervals = sorted((m["beginNs"], m["endNs"]) for m in g.get("memberIntervals", []))
        for key in TRANSFERS:
            for d in g.get(key) or []:
                timed = d.get("deviceMs") is not None
                overlapped = False
                if timed and d.get("beginNs") is not None and d.get("endNs") is not None and d["endNs"] > d["beginNs"]:
                    covered = overlap_ns(d["beginNs"], d["endNs"], intervals)
                    overlapped = 2 * covered > d["endNs"] - d["beginNs"]
                queue = "%d.%d" % (d.get("queueFamily", 0), d.get("queueIndex", 0))
                entry = groups.setdefault((key[:-1], overlapped, queue), {"copies": 0, "bytes": 0, "ms": 0.0, "untimed": 0})
                entry["copies"] += 1
                if timed:
                    entry["bytes"] += d["bytes"]
                    entry["ms"] += d["deviceMs"]
                else:
                    entry["untimed"] += 1
    return groups


def format_copy_rate(rows):
    regions = [r["region"] for r in rows]
    groups = copy_rate_groups(regions)
    lines = ["Copy rate by overlap and queue (all regions of this render label; GB/s over timestamped copies)"]
    lines.append(" | ".join(("dir", "overlapped kernel", "queue", "copies", "untimed", "MB", "device ms", "rate GB/s")))
    for (direction, overlapped, queue), e in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][2], kv[0][1])):
        rate = ("%.2f" % (e["bytes"] / (e["ms"] * 1e6))) if e["ms"] > 0 else "n/a"
        lines.append(" | ".join((direction, "yes" if overlapped else "no", queue, str(e["copies"]), str(e["untimed"]),
                                 "%.1f" % (e["bytes"] / 1e6), "%.2f" % e["ms"], rate)))
    if not groups:
        lines.append("(no copies)")
    return "\n".join(lines)


def mean(values):
    return sum(values) / len(values) if values else 0.0


def summarise(rows, import_rate_gbps=None, import_rate_up_gbps=None):
    groups = collections.OrderedDict()
    for row in rows:
        groups.setdefault(row["signature"], []).append(row)
    out = []
    for sig, rs in groups.items():
        regs = [r["region"] for r in rs]
        entry = {"signature": sig, "n": len(rs)}
        for k in FIELDS:
            entry[k] = mean([g.get(k, 0.0) for g in regs])
        entry["other"] = mean([other_ms(g) for g in regs])
        entry["floor"] = mean([pipelined_floor_ms(g, import_rate_gbps, import_rate_up_gbps) for g in regs])
        preds = [g["predictedTransferMs"] for g in regs if g.get("predictedTransferMs") is not None]
        entry["predicted"] = mean(preds) if preds else None
        entry["imports"] = mean([g.get("importCount", 0) for g in regs])
        entry["importMB"] = mean([g.get("newImportBytes", 0) for g in regs]) / 1e6
        entry["poolMisses"] = mean([g.get("poolMisses", 0) for g in regs])
        entry["fallbacks"] = collections.Counter(g["fallback"] for g in regs if g.get("fallback"))
        entry["overlapped"] = sum(g.get("overlapStrips", 0) for g in regs)
        entry["retries"] = sum(g.get("retries", 0) for g in regs)
        nodes = collections.OrderedDict()
        for r in rs:
            for m in r["members"]:
                nodes.setdefault(m["node"], []).append(m["kernelMs"])
        entry["nodes"] = [(n, mean(v)) for n, v in nodes.items()]
        entry["transferStats"] = {key: [st for st in (transfer_stats(g, key) for g in regs) if st] for key in TRANSFERS}
        out.append(entry)
    return out


def format_summary(entries):
    lines = []
    lines.append("Per region (ms per region run, mean over runs; planMs is outside wallMs)")
    head = ("region", "runs", "plan", "wall", "kernel", "xfer exp", "xfer hid", "upload", "download", "record", "acquire",
            "submit/wait", "prepare wait", "other", "pipelined floor", "pred xfer", "imports (MB)", "pool miss", "overlap strips", "fallback/retries")
    lines.append(" | ".join(head))
    for e in entries:
        pred = "n/a" if e["predicted"] is None else "%.2f" % e["predicted"]
        fb = ",".join("%s:%d" % kv for kv in e["fallbacks"].items()) or "-"
        lines.append(" | ".join((e["signature"], str(e["n"]), "%.2f" % e["planMs"], "%.2f" % e["wallMs"], "%.2f" % e["kernelMs"],
                                 "%.2f" % e["transferExposedMs"], "%.2f" % e["transferHiddenMs"], "%.2f" % e["uploadMs"],
                                 "%.2f" % e["downloadMs"], "%.2f" % e["recordMs"], "%.2f" % e["acquireMs"],
                                 "%.2f" % e["submitWaitMs"], "%.2f" % e["prepareWaitMs"], "%.2f" % e["other"],
                                 "%.2f" % e["floor"], pred,
                                 "%.1f (%.1f)" % (e["imports"], e["importMB"]), "%.1f" % e["poolMisses"],
                                 str(e["overlapped"]),
                                 "%s / %d" % (fb, e["retries"]))))
    lines.append("")
    lines.append("Per node (member kernel ms, mean over runs; share of the region's wallMs)")
    lines.append(" | ".join(("region", "node", "kernel", "share of wall")))
    for e in entries:
        for node, k in e["nodes"]:
            share = 100.0 * k / e["wallMs"] if e["wallMs"] > 0 else 0.0
            lines.append(" | ".join((e["signature"], node, "%.2f" % k, "%.0f%%" % share)))
    lines.append("")
    lines.append("Transfers (per region run, mean over runs; rate and last strip need timestamped copies)")
    lines.append(" | ".join(("region", "dir", "copies", "min MB", "median MB", "max MB", "rate GB/s", "timed bytes",
                             "last strip share", "staged", "pinned", "queue")))
    for e in entries:
        for key in TRANSFERS:
            stats = e["transferStats"][key]
            if not stats:
                continue
            rates = [st["rate"] for st in stats if st["rate"] is not None]
            shares = [st["lastShare"] for st in stats if st["lastShare"] is not None]
            lines.append(" | ".join((e["signature"], key[:-1], "%.1f" % mean([st["count"] for st in stats]),
                                     "%.2f" % (mean([st["min"] for st in stats]) / 1e6),
                                     "%.2f" % (mean([st["median"] for st in stats]) / 1e6),
                                     "%.2f" % (mean([st["max"] for st in stats]) / 1e6),
                                     ("%.2f" % mean(rates)) if rates else "n/a",
                                     "%.0f%%" % (100.0 * mean([st["timedShare"] for st in stats])),
                                     ("%.0f%%" % (100.0 * mean(shares))) if shares else "n/a",
                                     "%.1f" % mean([st["staged"] for st in stats]),
                                     "%.1f" % mean([st["pinned"] for st in stats]), stats[0]["queue"])))
    return "\n".join(lines)


def write_ms_by_frame(records):
    totals = collections.defaultdict(float)
    for r in records:
        if "task" in r and "frame" in r and (str(r.get("node", "")).startswith("Write") or "Write" in str(r.get("plugin", ""))):
            totals[r["frame"]] += r["wallNs"] / 1e6
    return totals


def format_renders(marks, records):
    writes = write_ms_by_frame(records)
    lines = ["Per render (ms; in-process time around the render call, Write task time inside it, and the difference)"]
    lines.append(" | ".join(("#", "label", "frames", "render", "write", "render - write")))
    for m in marks:
        render = m["wall_s"] * 1e3
        write = sum(writes.get(f, 0.0) for f in range(m["profileFrame"], m["profileFrame"] + m["frames"]))
        lines.append(" | ".join((str(m["index"]), m["label"], "%d@%d" % (m["frames"], m["frame"]), "%.1f" % render,
                                 "%.1f" % write, "%.1f" % (render - write))))
    return "\n".join(lines)


def summarise_run(arg, skip, import_rate_gbps, import_rate_up_gbps=None, copy_rate=False):
    records = read_records([arg])
    marks = read_marks(arg)
    if skip is None:
        skip = 0 if marks else 1
    rows, kept = build(records, skip, marks)
    tag = parse_tag(arg)
    out = ["run %s: case %s, gpu %s, kind %s, round %s" % (os.path.basename(arg), tag["case"], tag["gpu"], tag["kind"], tag["round"]),
           "frames summarised: %d (skipped the first %d)" % (kept, skip)]
    if marks:
        out += [format_renders(marks, records), ""]
    groups = collections.OrderedDict()
    for row in rows:
        groups.setdefault(row["label"], []).append(row)
    for label, group in groups.items():
        if label != "-":
            out.append("== render label %s ==" % label)
        out.append(format_summary(summarise(group, import_rate_gbps, import_rate_up_gbps)))
        out.append("")
        if copy_rate:
            out.append(format_copy_rate(group))
            out.append("")
    return "\n".join(out), bool(rows)


def self_test():
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gpu-region-profile-sample.jsonl")
    records = read_records([sample])
    rows, kept = build(records, 0)
    assert kept == 2, kept
    assert len(rows) == 4, len(rows)
    by_sig = {e["signature"]: e for e in summarise(rows)}
    assert set(by_sig) == {"Grade1+Blur1", "Erode1", "Blur2"}, sorted(by_sig)

    pair = by_sig["Grade1+Blur1"]
    assert pair["n"] == 2
    assert abs(pair["wallMs"] - 29.0) < 1e-9
    assert abs(pair["planMs"] - 1.0) < 1e-9
    assert abs(pair["other"] - 4.0) < 1e-9, pair["other"]
    kernels = dict(pair["nodes"])
    assert abs(kernels["Grade1"] - 2.1) < 1e-9 and abs(kernels["Blur1"] - 9.9) < 1e-9, kernels
    assert abs(pair["predicted"] - 8.5) < 1e-9, pair["predicted"]

    erode = by_sig["Erode1"]
    assert abs(other_ms(rows[2]["region"]) - 5.0) < 1e-9
    assert abs(erode["other"] - 5.0) < 1e-9
    assert erode["predicted"] is None

    fallback = by_sig["Blur2"]
    assert fallback["nodes"] == [("Blur2", 0.0)], fallback["nodes"]
    assert fallback["overlapped"] == 0 and by_sig["Erode1"]["overlapped"] == 1
    assert dict(fallback["fallbacks"]) == {"no timestamps": 1} and fallback["retries"] == 1

    steady_rows, steady_kept = build(records, 1)
    assert steady_kept == 1 and len(steady_rows) == 3
    assert abs(summarise(steady_rows)[0]["wallMs"] - 28.0) < 1e-9

    predicted = [dict(rows[0]["region"], predictedTransferMs=7.5)]
    assert summarise([{"region": predicted[0], "members": [], "signature": "x"}])[0]["predicted"] == 7.5
    text = format_summary(summarise(rows))
    assert "n/a" in text and "Grade1+Blur1" in text and "prepare wait" in text and "pipelined floor" in text
    assert abs(pair["prepareWaitMs"] - 1.0) < 1e-9 and abs(erode["prepareWaitMs"] - 3.0) < 1e-9
    assert fallback["prepareWaitMs"] == 0.0

    stripped = transfer_stats(rows[2]["region"], "downloads")
    assert stripped["count"] == 4 and stripped["max"] == 33177600 and stripped["staged"] == 4
    assert abs(stripped["rate"] - 132710400 / 30e6) < 1e-9, stripped["rate"]
    assert abs(stripped["lastShare"] - 0.3) < 1e-9, stripped["lastShare"]
    assert stripped["timedShare"] == 1.0 and stripped["pinned"] == 0
    stripped_up = transfer_stats(rows[2]["region"], "uploads")
    assert stripped_up["count"] == 4 and stripped_up["queue"] == "2.0" and stripped_up["staged"] == 0
    assert abs(stripped_up["rate"] - 132710400 / 24e6) < 1e-9, stripped_up["rate"]
    assert abs(stripped_up["lastShare"] - 0.25) < 1e-9, stripped_up["lastShare"]
    whole = transfer_stats(rows[0]["region"], "downloads")
    assert whole["count"] == 1 and whole["rate"] is None and whole["lastShare"] is None and whole["timedShare"] == 0.0
    whole_up = transfer_stats(rows[0]["region"], "uploads")
    assert abs(whole_up["rate"] - 132710400 / 4e6) < 1e-9 and whole_up["timedShare"] == 1.0
    assert whole_up["lastShare"] is None
    with_pinned = transfer_stats(rows[1]["region"], "downloads")
    assert with_pinned["count"] == 2 and with_pinned["pinned"] == 1
    assert abs(with_pinned["timedShare"] - 0.8) < 1e-9, with_pinned["timedShare"]
    assert abs(with_pinned["rate"] - 132710400 / 5e6) < 1e-9, with_pinned["rate"]
    assert sum(d["bytes"] for d in rows[1]["region"]["downloads"]) == rows[1]["region"]["downloadBytes"]
    assert transfer_stats(rows[3]["region"], "downloads") is None and transfer_stats(rows[3]["region"], "uploads") is None
    assert len(pair["transferStats"]["downloads"]) == 2 and len(pair["transferStats"]["uploads"]) == 2
    assert "last strip share" in text and "30%" in text and "timed bytes" in text
    assert "Grade1+Blur1 | download | 1.5 |" in text and "| 40% |" in text, text
    assert "Erode1 | upload | 4.0 | 33.18 | 33.18 | 33.18 | 5.53 | 100% | 25% | 0.0 | 0.0 | 2.0" in text, text

    assert abs(pipelined_floor_ms(rows[2]["region"]) - 0.0) < 1e-9
    assert abs(pipelined_floor_ms(rows[0]["region"]) - 9.0) < 1e-9
    rate = pipelined_floor_ms(rows[2]["region"], 1.0)
    assert abs(rate - (2 * 132710400 / 1e9 * 1e3 - 80.0 * 3 / 4)) < 1e-6, rate
    assert abs(summarise(rows, 1.0)[1]["floor"] - rate) < 1e-6
    split = {"uploadBytes": 1e9, "downloadBytes": 1e9, "kernelMs": 0.0, "strips": 1}
    assert abs(pipelined_floor_ms(split, 16.0, 10.0) - (1e3 / 10.0 + 1e3 / 16.0)) < 1e-9
    assert abs(pipelined_floor_ms(split, 16.0) - 2e3 / 16.0) < 1e-9
    assert abs(summarise(rows, 1.0, 1.0)[1]["floor"] - rate) < 1e-6

    marks = [{"label": "first", "index": 0, "frame": 2, "frames": 1, "profileFrame": 0, "wall_s": 0.2},
             {"label": "b", "index": 1, "frame": 2, "frames": 1, "profileFrame": 1, "wall_s": 0.15}]
    labelled, _ = build(records, 0, marks)
    assert [r["label"] for r in labelled] == ["first", "b", "b", "b"], [r["label"] for r in labelled]
    assert label_for(marks, 7) == "-"
    assert abs(write_ms_by_frame(records)[1] - 41.0) < 1e-9
    assert "render - write" in format_renders(marks, records)
    assert marks_path("run/x-auto-bc-r1.prof.123") == "run/x-auto-bc-r1.marks.jsonl"
    assert parse_tag("o/gxg-erode40-8k-auto-a-cold-r2.prof")["kind"] == "a-cold"
    assert parse_tag("o/gxg-erode40-8k-off-bc-r1.prof.77")["case"] == "gxg-erode40-8k"
    assert parse_tag("o/n-erode40-uhd-auto-a-warm-r1-base.prof")["base"] == "-base"
    assert median([3, 1, 2]) == 2 and median([1, 2, 3, 10]) == 2.5
    with tempfile.TemporaryDirectory() as d:
        drv = []
        for tag, hwm, rt in (("n-e-uhd-auto-a-warm-r1", 2048 * 1024, 1.0), ("n-e-uhd-auto-a-warm-r1-base", 4096 * 1024, 2.0),
                             ("n-e-uhd-auto-a-warm-r2", 2048 * 1024, 3.0), ("n-e-uhd-auto-a-warm-r2-base", 4096 * 1024, 4.0)):
            drv.append({"tag": tag, "exit": 0, "vmhwm_kb": hwm, "thp_fault_alloc": 10, "thp_fault_fallback": 1,
                        "thp_split_pmd": 2, "pgfault": 1000, "thp_enabled": "madvise", "thp_defrag": "madvise",
                        "thp_use_zero_page": "1"})
            with open(os.path.join(d, tag + ".marks.jsonl"), "w") as fh:
                fh.write(json.dumps({"wall_s": rt}) + "\n")
        drv.append({"tag": "prime-x", "exit": 0})
        drv.append({"tag": "n-e-uhd-off-a-warm-r1", "exit": 1})
        entries, ratios = summarise_driver(d, drv)
        by = {(e["build"], e["mode"]): e for e in entries}
        assert set(by) == {("new", "auto"), ("base", "auto")}, sorted(by)
        assert by[("new", "auto")]["vmhwmMB"] == 2048 and by[("base", "auto")]["vmhwmMB"] == 4096
        assert by[("new", "auto")]["thp_fault_alloc"] == 10 and by[("new", "auto")]["thp"] == "madvise/madvise/1"
        assert ratios == [("a-warm", "auto", 2, 0.625)], ratios
        assert "new/base" in format_driver(entries, ratios) and "peak RSS MB" in format_driver(entries, ratios)
    a = {"strip": 0, "bytes": 1000000, "deviceMs": 1.0, "queueFamily": 2, "queueIndex": 0, "beginNs": 0, "endNs": 1000000}
    b = dict(a, bytes=2000000, deviceMs=1.0, beginNs=5000000, endNs=6000000, queueFamily=1)
    u = {"strip": 0, "bytes": 500000, "deviceMs": None, "queueFamily": 2, "queueIndex": 0}
    region = {"uploads": [a, u], "downloads": [b], "memberIntervals": [
        {"beginNs": 400000, "endNs": 900000}, {"beginNs": 800000, "endNs": 1200000}]}
    cg = copy_rate_groups([region])
    assert cg[("upload", True, "2.0")]["copies"] == 1 and cg[("upload", False, "2.0")]["untimed"] == 1
    assert cg[("download", False, "1.0")]["bytes"] == 2000000
    assert overlap_ns(0, 1000000, [(400000, 900000), (800000, 1200000)]) == 600000
    ctext = format_copy_rate([{"region": region}])
    assert "upload | yes | 2.0 | 1 | 0 | 1.0 | 1.00 | 1.00" in ctext, ctext
    assert "download | no | 1.0 | 1 | 0 | 2.0 | 1.00 | 2.00" in ctext, ctext
    print(text)
    print(ctext)
    print("self-test ok")


def main(argv):
    if "--self-test" in argv:
        self_test()
        return 0
    if "--driver" in argv:
        target = argv[argv.index("--driver") + 1]
        driver = os.path.join(target, "driver.jsonl") if os.path.isdir(target) else target
        entries, ratios = summarise_driver(os.path.dirname(os.path.abspath(driver)), read_jsonl(driver))
        print(format_driver(entries, ratios))
        return 0
    skip = None
    rate = None
    rate_up = None
    copy_rate = False
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--skip-frames":
            skip = int(argv[i + 1])
            i += 2
        elif argv[i] == "--import-rate":
            rate = float(argv[i + 1])
            i += 2
        elif argv[i] == "--copy-rate":
            copy_rate = True
            i += 1
        elif argv[i] == "--import-rate-up":
            rate_up = float(argv[i + 1])
            i += 2
        else:
            paths.append(argv[i])
            i += 1
    if not paths:
        sys.stderr.write(__doc__)
        return 2
    status = 0
    for path in paths:
        text, any_rows = summarise_run(path, skip, rate, rate_up, copy_rate)
        print(text)
        if not any_rows:
            sys.stderr.write("no region records in %s after skipping\n" % path)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
