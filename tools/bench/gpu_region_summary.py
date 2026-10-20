#!/usr/bin/env python3
"""Splits GPU region time from NATRON_RENDER_PROFILE files into its phases, per region and per node.

    python3 -I tools/bench/gpu_region_summary.py [--skip-frames N] [--import-rate GBPS] PROFILE_PREFIX_OR_FILE...
    python3 -I tools/bench/gpu_region_summary.py --self-test

A PROFILE argument is a file, or a prefix whose suffixed files (<prefix>.*) are all read. Each argument is
one run and is summarised on its own. gpu_region_bench.sh names a run <case>-<gpu mode>-<kind>-r<round>
and writes <tag>.marks.jsonl beside <tag>.prof: one line per render with its label (first, b, c, playback,
a-cold, a-warm), the profile frame it starts at and its in-process time. With marks the regions are grouped
by render label, the first frame is kept (it carries the one-time costs; --skip-frames overrides), and a
per-render table shows the render time, the Write task time in it, and their difference. Without marks the
first frame is skipped as before.

prepareWaitMs is the time the region waited for its transfer destinations to be prepared. The pipelined
floor is max(0, T - K*(n-1)/n) with T the exposed plus hidden transfer ms, K the kernel ms and n the strips
(1 when the region ran unstripped): the transfer time left once every strip but one overlaps a kernel.
With --import-rate GBPS (a measured GpuBench host-import rate in GB/s) T is instead
(uploadBytes + downloadBytes) / rate, so the floor shows what the transfers would cost at that rate.

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
"""
import collections
import glob
import json
import os
import re
import sys

PHASES = ("kernel", "exposed", "record", "acquire", "submitWait")
FIELDS = ("wallMs", "planMs", "kernelMs", "transferExposedMs", "transferHiddenMs", "uploadMs", "downloadMs",
          "recordMs", "acquireMs", "submitWaitMs", "prepareWaitMs")
KINDS = ("a-cold", "a-warm", "bc", "playback")
TAG_RE = re.compile(r"^(?P<case>.+)-(?P<gpu>auto|off)-(?P<kind>%s)-r(?P<round>\d+)$" % "|".join(KINDS))
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


def pipelined_floor_ms(region, import_rate_gbps=None):
    strips = max(region.get("strips", 0), 1)
    if import_rate_gbps:
        transfer = (region.get("uploadBytes", 0) + region.get("downloadBytes", 0)) / (import_rate_gbps * 1e9) * 1e3
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


def mean(values):
    return sum(values) / len(values) if values else 0.0


def summarise(rows, import_rate_gbps=None):
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
        entry["floor"] = mean([pipelined_floor_ms(g, import_rate_gbps) for g in regs])
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


def summarise_run(arg, skip, import_rate_gbps):
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
        out.append(format_summary(summarise(group, import_rate_gbps)))
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

    assert abs(pipelined_floor_ms(rows[2]["region"]) - 0.0) < 1e-9
    assert abs(pipelined_floor_ms(rows[0]["region"]) - 9.0) < 1e-9
    rate = pipelined_floor_ms(rows[2]["region"], 1.0)
    assert abs(rate - (2 * 132710400 / 1e9 * 1e3 - 80.0 * 3 / 4)) < 1e-6, rate
    assert abs(summarise(rows, 1.0)[1]["floor"] - rate) < 1e-6

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
    print(text)
    print("self-test ok")


def main(argv):
    if "--self-test" in argv:
        self_test()
        return 0
    skip = None
    rate = None
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--skip-frames":
            skip = int(argv[i + 1])
            i += 2
        elif argv[i] == "--import-rate":
            rate = float(argv[i + 1])
            i += 2
        else:
            paths.append(argv[i])
            i += 1
    if not paths:
        sys.stderr.write(__doc__)
        return 2
    status = 0
    for path in paths:
        text, any_rows = summarise_run(path, skip, rate)
        print(text)
        if not any_rows:
            sys.stderr.write("no region records in %s after skipping\n" % path)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
