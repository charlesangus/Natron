"""Marks a run's records with what its natron.log says about the scene's files and with its shader cache.

  heavy_scene_fixture_check.py NATRON_LOG RESULTS LABEL [--shader-cache warm|cold] [--warmup]

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
    ("ptex", re.compile(r"Unable to open ptex (.*?) : ")),
    ("texture", re.compile(r"Unable to read Texture '([^']*)'")),
    ("image", re.compile(r"Cannot open image (\S+) for reading")),
)
MARKERS = ("Could not load sublayer", "Could not open asset", "Unable to open ptex", "Unable to read Texture", "Cannot open image")
SAMPLE_PATHS = 5
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


def main():
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
