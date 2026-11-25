"""Marks a run's records with what its natron.log says about the scene's files and with its shader cache.

  heavy_scene_fixture_check.py NATRON_LOG RESULTS LABEL [--shader-cache warm|cold] [--warmup]
  heavy_scene_fixture_check.py --rejudge RESULTS OUT.jsonl [--logs-dir DIR]

NATRON_LOG is scanned for the scene's files failing to open: "Could not load sublayer", "Could not open
asset" (a reference or payload that did not resolve or open), "Unable to open ptex", "Unable to read
Texture" and "Cannot open image". Every record of LABEL in RESULTS gets fixture_check: the count of each
kind, and the first few paths that failed. When any matched, a record whose result is pass or fail gets
result fixture_incomplete instead, its own result kept as judged_result: the scene under test was not
the whole scene, so its numbers judge nothing.

Every record of LABEL without shader_cache gets the one given, as the watchdog's and the shell's records
are written without it. With --warmup each record gets warmup true, and a pass, fail or
fixture_incomplete becomes result warmup (judged_result keeps the judged one): the run only fills the
shader cache. RESULTS is rewritten in place; the other lines are kept as they are. Prints one line.
--rejudge re-scans, for every label in RESULTS, DIR/<label>/natron.log (DIR defaults to runs/ beside RESULTS)
and writes the records to OUT.jsonl, leaving RESULTS alone: a fixture_incomplete record whose log no longer
matches gets its judged_result back as result, and a pass or fail record whose log now matches becomes
fixture_incomplete; warmup records keep their result and only get a fresh fixture_check.
Standard library only, so it runs on the host's python3.
"""

import argparse
import json
import os
import re
import sys

PATTERNS = (
    ("sublayer", re.compile(r"Could not load sublayer @([^@]*)@")),
    ("asset", re.compile(r"Could not open asset @([^@]*)@")),
    ("ptex", re.compile(r"Unable to open ptex (\S*(?:/|\.ptx)\S*) : ")),
    ("texture", re.compile(r"Unable to read Texture '([^']*)'")),
    ("image", re.compile(r"Cannot open image (\S+) for reading")),
)
MARKERS = ("Could not load sublayer", "Could not open asset", "Unable to open ptex", "Unable to read Texture", "Cannot open image")
SAMPLE_PATHS = 5
# Storm also names a material node's texture "ptex" or "HwPtexTexture_1" when it has no file; only a
# value that is a path counts as a missing file.
JUDGED = ("pass", "fail")


def scan(path):
    counts = {kind: 0 for kind, _ in PATTERNS}
    samples = []
    if not os.path.exists(path):
        return None
    with open(path, errors="replace") as f:
        for line in f:
            if not any(marker in line for marker in MARKERS):
                continue
            for kind, pattern in PATTERNS:
                found = pattern.search(line)
                if found:
                    counts[kind] += 1
                    entry = {"kind": kind, "path": found.group(1)}
                    if len(samples) < SAMPLE_PATHS and entry not in samples:
                        samples.append(entry)
                    break
    return {"counts": counts, "failed": sum(counts.values()), "paths": samples, "log": os.path.basename(path)}


def rejudge(results, out, logs_dir):
    logs_dir = logs_dir or os.path.join(os.path.dirname(os.path.abspath(results)), "runs")
    cache = {}
    changed = 0
    out_lines = []
    with open(results) as f:
        lines = f.read().splitlines()
    for line in lines:
        try:
            record = json.loads(line)
        except ValueError:
            out_lines.append(line)
            continue
        label = record.get("label")
        if label not in cache:
            cache[label] = scan(os.path.join(logs_dir, str(label), "natron.log"))
        found = cache[label]
        if found is None or record.get("warmup") or record.get("result") == "warmup":
            if found is not None:
                record["fixture_check"] = found
            out_lines.append(json.dumps(record))
            continue
        before = record.get("result")
        record["fixture_check"] = found
        if found["failed"]:
            if before in JUDGED:
                record["judged_result"] = before
                record["result"] = "fixture_incomplete"
        elif before == "fixture_incomplete" and record.get("judged_result"):
            record["result"] = record.pop("judged_result")
        changed += before != record.get("result")
        out_lines.append(json.dumps(record))
    with open(out, "w") as f:
        f.write("".join(l + "\n" for l in out_lines))
    print("    rejudged %d records, %d results changed -> %s" % (len(out_lines), changed, out))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--rejudge":
        parser = argparse.ArgumentParser()
        parser.add_argument("--rejudge")
        parser.add_argument("out")
        parser.add_argument("--logs-dir")
        args = parser.parse_args()
        rejudge(args.rejudge, args.out, args.logs_dir)
        return 0
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("results")
    parser.add_argument("label")
    parser.add_argument("--shader-cache", choices=("warm", "cold"))
    parser.add_argument("--warmup", action="store_true")
    args = parser.parse_args()

    found = scan(args.log)
    incomplete = bool(found and found["failed"])
    if found is None:
        print("    fixture: no log at %s" % args.log)
    elif incomplete:
        print("    fixture: INCOMPLETE, %s; first: %s" % (
            ", ".join("%d %s" % (n, kind) for kind, n in found["counts"].items() if n),
            "; ".join(p["path"] for p in found["paths"])))
    else:
        print("    fixture: complete (no sublayer, asset, ptex or texture open failures in the log)")

    with open(args.results) as f:
        lines = f.read().splitlines()
    out_lines = []
    for line in lines:
        try:
            record = json.loads(line)
        except ValueError:
            out_lines.append(line)
            continue
        if record.get("label") != args.label:
            out_lines.append(line)
            continue
        record["fixture_check"] = found
        if incomplete and record.get("result") in JUDGED:
            record["judged_result"] = record["result"]
            record["result"] = "fixture_incomplete"
        if not record.get("shader_cache"):
            record["shader_cache"] = args.shader_cache
        if args.warmup:
            record["warmup"] = True
            if record.get("result") in JUDGED + ("fixture_incomplete",):
                record.setdefault("judged_result", record["result"])
                record["result"] = "warmup"
        out_lines.append(json.dumps(record))

    temporary = args.results + ".fixture.tmp"
    with open(temporary, "w") as f:
        f.write("".join(l + "\n" for l in out_lines))
    os.replace(temporary, args.results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
