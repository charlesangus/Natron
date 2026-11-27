#!/usr/bin/env python3
# For-each scale bench: a GeoForEachEnd over 10,000 connectivity pieces (a cube copied onto every
# point of a 100 x 100 grid), each piece moved and rotated by a GeoTransform driven by loop.t, cooked
# at 1 thread and at every hardware thread. Geometry has no headless render target, so the cook runs
# in the Tests binary: GeoForEachTest.PiecesBench pulls the GeoForEachEnd directly, bypassing the
# cache, and prints one line per thread count. No threshold: the times are reported, not judged.
#
#   tools/bench/geo/foreach_pieces.py [--tests build/release/Tests/Tests] [--out results.jsonl]
#
# Run it where the Tests binary runs (inside natron-dev for a container build).

import argparse
import json
import os
import re
import subprocess
import sys
import time

LINE = re.compile(r"^\[ foreach  \] pieces (\d+) threads (\d+): ([0-9.]+) ms$")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests", default=os.path.join("build", "release", "Tests", "Tests"),
                        help="the Tests binary (default: build/release/Tests/Tests)")
    parser.add_argument("--out", help="JSON-lines file the result is appended to")
    args = parser.parse_args()

    if not os.access(args.tests, os.X_OK):
        sys.exit("no Tests binary at %s: pass --tests" % args.tests)

    env = dict(os.environ, NATRON_GEOFOREACH_BENCH="1")
    started = time.time()
    proc = subprocess.run([args.tests, "--gtest_filter=GeoForEachTest.PiecesBench"], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    wall_s = time.time() - started

    runs = []
    for line in proc.stdout.splitlines():
        match = LINE.match(line.strip())
        if match:
            runs.append({"pieces": int(match.group(1)), "threads": int(match.group(2)), "ms": float(match.group(3))})
    if proc.returncode != 0 or not runs:
        sys.stdout.write(proc.stdout)
        sys.exit("the bench failed (exit %d, %d timings)" % (proc.returncode, len(runs)))

    for run in runs:
        print("pieces %d threads %3d: %10.1f ms" % (run["pieces"], run["threads"], run["ms"]))
    if len(runs) >= 2 and runs[-1]["ms"] > 0:
        print("speed-up at %d threads: %.2fx" % (runs[-1]["threads"], runs[0]["ms"] / runs[-1]["ms"]))

    if args.out:
        with open(args.out, "a") as f:
            f.write(json.dumps({"bench": "geo_foreach_pieces", "time": started, "wall_s": wall_s, "runs": runs}) + "\n")


if __name__ == "__main__":
    main()
