#!/usr/bin/env python3
"""Splits GPU region time from NATRON_RENDER_PROFILE files into its phases, per region and per node.

    python3 -I tools/bench/gpu_region_summary.py [--skip-frames N] PROFILE_PREFIX_OR_FILE...
    python3 -I tools/bench/gpu_region_summary.py --self-test

A PROFILE argument is a file, or a prefix whose suffixed files (<prefix>.*) are all read.

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
import sys

PHASES = ("kernel", "exposed", "record", "acquire", "submitWait")
FIELDS = ("wallMs", "planMs", "kernelMs", "transferExposedMs", "transferHiddenMs", "uploadMs", "downloadMs",
          "recordMs", "acquireMs", "submitWaitMs")


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


def build(records, skip_frames):
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
        rows.append({"region": g, "members": members, "signature": "+".join(m["node"] for m in members) or "?"})
    return rows, len(kept)


def mean(values):
    return sum(values) / len(values) if values else 0.0


def summarise(rows):
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
        preds = [g["predictedTransferMs"] for g in regs if g.get("predictedTransferMs") is not None]
        entry["predicted"] = mean(preds) if preds else None
        entry["imports"] = mean([g.get("importCount", 0) for g in regs])
        entry["importMB"] = mean([g.get("newImportBytes", 0) for g in regs]) / 1e6
        entry["poolMisses"] = mean([g.get("poolMisses", 0) for g in regs])
        entry["fallbacks"] = collections.Counter(g["fallback"] for g in regs if g.get("fallback"))
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
            "submit/wait", "other", "pred xfer", "imports (MB)", "pool miss", "fallback/retries")
    lines.append(" | ".join(head))
    for e in entries:
        pred = "n/a" if e["predicted"] is None else "%.2f" % e["predicted"]
        fb = ",".join("%s:%d" % kv for kv in e["fallbacks"].items()) or "-"
        lines.append(" | ".join((e["signature"], str(e["n"]), "%.2f" % e["planMs"], "%.2f" % e["wallMs"], "%.2f" % e["kernelMs"],
                                 "%.2f" % e["transferExposedMs"], "%.2f" % e["transferHiddenMs"], "%.2f" % e["uploadMs"],
                                 "%.2f" % e["downloadMs"], "%.2f" % e["recordMs"], "%.2f" % e["acquireMs"],
                                 "%.2f" % e["submitWaitMs"], "%.2f" % e["other"], pred,
                                 "%.1f (%.1f)" % (e["imports"], e["importMB"]), "%.1f" % e["poolMisses"],
                                 "%s / %d" % (fb, e["retries"]))))
    lines.append("")
    lines.append("Per node (member kernel ms, mean over runs; share of the region's wallMs)")
    lines.append(" | ".join(("region", "node", "kernel", "share of wall")))
    for e in entries:
        for node, k in e["nodes"]:
            share = 100.0 * k / e["wallMs"] if e["wallMs"] > 0 else 0.0
            lines.append(" | ".join((e["signature"], node, "%.2f" % k, "%.0f%%" % share)))
    return "\n".join(lines)


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
    assert dict(fallback["fallbacks"]) == {"no timestamps": 1} and fallback["retries"] == 1

    steady_rows, steady_kept = build(records, 1)
    assert steady_kept == 1 and len(steady_rows) == 3
    assert abs(summarise(steady_rows)[0]["wallMs"] - 28.0) < 1e-9

    predicted = [dict(rows[0]["region"], predictedTransferMs=7.5)]
    assert summarise([{"region": predicted[0], "members": [], "signature": "x"}])[0]["predicted"] == 7.5
    text = format_summary(summarise(rows))
    assert "n/a" in text and "Grade1+Blur1" in text
    print(text)
    print("self-test ok")


def main(argv):
    if "--self-test" in argv:
        self_test()
        return 0
    skip = 1
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == "--skip-frames":
            skip = int(argv[i + 1])
            i += 2
        else:
            paths.append(argv[i])
            i += 1
    if not paths:
        sys.stderr.write(__doc__)
        return 2
    rows, kept = build(read_records(paths), skip)
    if not rows:
        sys.stderr.write("no region records after skipping %d frame(s)\n" % skip)
        return 1
    print("frames summarised: %d (skipped the first %d)" % (kept, skip))
    print(format_summary(summarise(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
