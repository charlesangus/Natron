#!/usr/bin/env python3
"""Per-thread stack summary of eu-stack samples, and the main thread's stacks during main-thread stalls.

Usage: stall_stacks.py SAMPLES [--stalls STALL_LOG] [--main-tid TID] [--top N] [--min-stall-ms MS]

SAMPLES is sample_stacks.sh's output: "=== SAMPLE i epoch-seconds" headers, "TID n:" thread headers and
"#k 0xaddr function - module" frames. STALL_LOG is the stall monitor's JSON lines (start_ms on the
steady clock, duration_ms); the steady clock is mapped to the wall clock with this host's offset, so
the log must come from the same boot. A sample covers the time from its header to the next one.
The main thread is the thread whose id is MAIN_TID (default the lowest id in the samples).
"""
import collections
import json
import re
import sys
import time

FRAME = re.compile(r"^#(\d+)\s+0x[0-9a-f]+\s+(.*?)(?:\s+-\s+(\S+))?$")


def parse(path):
    """A list of (epoch, {tid: [function, ...]})."""
    samples = []
    threads = None
    thread = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== SAMPLE"):
                parts = line.split()
                threads = {}
                samples.append((float(parts[3]) if len(parts) > 3 else 0.0, threads))
                thread = None
            elif line.startswith("TID ") and threads is not None:
                thread = []
                threads[int(line.split()[1].rstrip(":"))] = thread
            elif thread is not None:
                m = FRAME.match(line.strip())
                if m:
                    thread.append(m.group(2) or "??")
    return samples


def short(fn, width=110):
    fn = re.sub(r"\s+", " ", fn)
    return fn if len(fn) <= width else fn[:width - 3] + "..."


def stack_key(stack, depth):
    return " <- ".join(short(fn, 80) for fn in stack[:depth]) or "(no frames)"


def print_top_stacks(counter, total, top_n):
    for key, n in counter.most_common(top_n):
        print("    %5.1f%% %4d  %s" % (100.0 * n / max(total, 1), n, key))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2

    def option(name, default=None):
        return args[args.index(name) + 1] if name in args else default

    samples = [s for s in parse(args[0]) if s[1]]
    top_n = int(option("--top", "12"))
    min_stall = float(option("--min-stall-ms", "100"))
    if not samples:
        print("no samples with threads in %s" % args[0])
        return 1
    tids = sorted(set(t for _, th in samples for t in th))
    main_tid = int(option("--main-tid", tids[0]))
    span = samples[-1][0] - samples[0][0]
    print("samples: %d over %.2f s (mean interval %.0f ms), %d threads seen, main thread %d" % (
        len(samples), span, 1000.0 * span / max(len(samples) - 1, 1), len(tids), main_tid))

    print("\n== per-thread top stacks (innermost 6 frames)")
    for tid in sorted(tids, key=lambda t: (t != main_tid, t)):
        counter = collections.Counter(stack_key(th[tid], 6) for _, th in samples if tid in th)
        seen = sum(counter.values())
        print("\n  thread %d%s: in %d of %d samples" % (tid, " (main)" if tid == main_tid else "", seen, len(samples)))
        print_top_stacks(counter, seen, top_n)

    stalls_path = option("--stalls")
    if not stalls_path:
        return 0
    offset = time.time() - time.monotonic()
    stalls = []
    try:
        with open(stalls_path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if "start_ms" in r and r.get("duration_ms", 0) >= min_stall:
                    start = r["start_ms"] / 1000.0 + offset
                    stalls.append((start, start + r["duration_ms"] / 1000.0, r["duration_ms"]))
    except OSError as e:
        print("\nstall log unreadable: %s" % e)
        return 0
    first, last = samples[0][0], samples[-1][0]
    inside = [s for s in stalls if s[1] >= first and s[0] <= last]
    print("\n== main-thread stalls of at least %.0f ms: %d in the log, %d inside the sampled window" % (min_stall, len(stalls), len(inside)))
    for start, end, ms in sorted(inside, key=lambda s: -s[2]):
        picked = []
        for i, (t, th) in enumerate(samples):
            until = samples[i + 1][0] if i + 1 < len(samples) else t
            if until > start and t < end and main_tid in th:
                picked.append(th[main_tid])
        print("\n  stall of %.0f ms, %.2f s into the samples: %d main-thread samples" % (ms, start - first, len(picked)))
        if picked:
            print("  top stacks (innermost 10 frames):")
            print_top_stacks(collections.Counter(stack_key(st, 10) for st in picked), len(picked), top_n)
            incl = collections.Counter(short(fn, 100) for st in picked for fn in set(st))
            print("  frames present in the most samples:")
            for fn, n in incl.most_common(top_n):
                print("    %5.1f%%  %s" % (100.0 * n / len(picked), fn))
    return 0


sys.exit(main())
