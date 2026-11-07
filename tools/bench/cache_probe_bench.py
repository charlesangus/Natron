#!/usr/bin/env python3
"""Probe and sampling cost of a disk tier holding many files.

Plants sparse files in a throwaway tier, drives a session in the test executable's worker mode, and
reports what a frame of missing nodes costs with and without the negative cache, and what a
maintenance tick costs. Run by hand; ctest does not.

    python3 tools/bench/cache_probe_bench.py --tests build/release/Tests/Tests
    python3 tools/bench/cache_probe_bench.py --tests ... --dir /mnt/nfs/scratch --files 300000

--dir picks the disk under test; the default is under build/bench. One JSON line goes to --out.
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

BUILD = "bench-build"
SHARD_DIGITS = 3
WORKER_FLAG = "--natron-tier-worker"
PLANTED_BYTES = 65536
AMPLE_BYTES = 1 << 50
ESTIMATE_MS_PER_FRAME = 30.0


class Worker:
    def __init__(self, tests, root, size):
        self.process = subprocess.Popen(
            [tests, WORKER_FLAG, "root=" + root, "build=" + BUILD, "size=%d" % size],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        ready = self.reply()
        if not ready.startswith("ready ") or ready == "ready none":
            sys.exit("the worker did not open the tier: " + ready)

    def reply(self):
        for line in self.process.stdout:
            if line.startswith("@ "):
                return line[2:].strip()
        sys.exit("the worker died")

    def command(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        return self.reply()

    def quit(self):
        self.process.stdin.close()
        self.process.wait()


def plant(root, count):
    auto = os.path.join(root, BUILD, "auto")
    made = set()
    rng = random.Random(1)
    for _ in range(count):
        base = "%016x" % rng.getrandbits(64)
        shard = os.path.join(auto, base[:SHARD_DIGITS])
        if shard not in made:
            os.makedirs(shard, exist_ok=True)
            made.add(shard)
        name = "%s-%016x-%016x.ntc" % (base, rng.getrandbits(64), rng.getrandbits(64))
        with open(os.path.join(shard, name), "wb") as f:
            os.ftruncate(f.fileno(), PLANTED_BYTES)


def milliseconds(nanoseconds):
    return int(nanoseconds) / 1e6


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tests", required=True, help="the Tests executable")
    parser.add_argument("--dir", default="build/bench", help="where the throwaway tier goes")
    parser.add_argument("--files", type=int, default=300000)
    parser.add_argument("--nodes", type=int, default=1000)
    parser.add_argument("--fps", type=int, default=24)
    parser.add_argument("--out", default="build/bench/cache-probe.jsonl")
    args = parser.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    root = tempfile.mkdtemp(prefix="cache-probe-", dir=args.dir)
    try:
        started = time.monotonic()
        plant(root, args.files)
        planted_s = time.monotonic() - started

        worker = Worker(os.path.abspath(args.tests), root, AMPLE_BYTES)
        scan_ms = milliseconds(worker.command("timed-maintain 1"))
        tick_ms = milliseconds(worker.command("timed-maintain 0"))

        # Hashes far from anything planted, and fresh for each measurement, so the first one misses
        # both the files and the negative cache.
        first = 1 << 40
        cold_ms = milliseconds(worker.command("bench-miss %d %d 1" % (first, args.nodes)))
        first += args.nodes
        first_frame_ms = milliseconds(worker.command("bench-miss %d %d 0" % (first, args.nodes)))
        warm_ms = milliseconds(worker.command("bench-miss %d %d 0" % (first, args.nodes)))
        worker.quit()

        # A budget below what is planted makes the tick sample and unlink.
        pressured = Worker(os.path.abspath(args.tests), root, PLANTED_BYTES * args.files // 2)
        pressured.command("timed-maintain 1")
        eviction_ms = milliseconds(pressured.command("timed-maintain 0"))
        pressured.quit()

        per_frame_ms = (first_frame_ms + warm_ms * (args.fps - 1)) / args.fps
        record = {
            "files": args.files,
            "nodes": args.nodes,
            "fps": args.fps,
            "planted_s": round(planted_s, 1),
            "scan_ms": round(scan_ms, 3),
            "tick_ms": round(tick_ms, 3),
            "tick_with_eviction_ms": round(eviction_ms, 3),
            "miss_no_negative_cache_ms": round(cold_ms, 3),
            "miss_first_frame_ms": round(first_frame_ms, 3),
            "miss_within_negative_cache_ms": round(warm_ms, 3),
            "miss_per_frame_ms": round(per_frame_ms, 3),
            "estimate_ms": ESTIMATE_MS_PER_FRAME,
        }
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "a") as f:
            f.write(json.dumps(record) + "\n")
        for key, value in record.items():
            print("%-32s %s" % (key, value))
        if per_frame_ms > ESTIMATE_MS_PER_FRAME:
            print("OVER the estimate: reconsider the negative cache's lifetime", file=sys.stderr)
            return 1
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
