#!/usr/bin/env python3
"""Print a boundedness verdict, with the numbers behind it, for each graph_bench.py record.

usage: classify.py RESULTS.jsonl [RESULTS.jsonl ...]

The thresholds are heuristics, meant to say which resource a configuration is closest to
saturating, not to prove it:

  RAM-bound        rss_peak_mb >= 0.8 x cache budget (total RAM x 50%, or BENCH_CACHE_MB), or major
                   page faults during the timed frames (memory was paged back in)
  IO-bound         block IO (io_read_bytes + io_write_bytes) / wall >= 0.5 x BENCH_DISK_GBPS
                   (default 2), or the plate and output file bytes of a frame / frame wall would
                   reach that rate if they came from disk (page-cache hits make block reads 0)
  bandwidth-bound  estimated bytes moved per frame / frame wall >= 0.5 x BENCH_STREAM_GBPS
                   (default 7); every node reads its inputs and writes one float RGBA image, so
                   bytes = pixels x 16 x (2 x nodes + merges); deep data is not counted
  host-overhead    frame wall per node < 1 ms and parallelism <= 1.3: per-node host work, not
                   pixels, sets the frame time
  CPU-bound        parallelism >= 0.7 x cores and none of the signals above
  under-occupied   none of the above: cores sit idle, so the graph's shape or the scheduler,
                   not a resource, limits the frame
"""
import json
import os
import sys

STREAM_GBPS = float(os.environ.get("BENCH_STREAM_GBPS", "7"))
DISK_GBPS = float(os.environ.get("BENCH_DISK_GBPS", "2"))
# For records written before graph_bench.py recorded "pixels".
RES_PIXELS = {"tiny": 32 * 32, "hd": 1920 * 1080, "uhd": 3840 * 2160}


def host_mem_mb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def num(v):
    return v if isinstance(v, (int, float)) else None


def classify(rec):
    signals = []
    facts = []
    walls = rec.get("frame_wall_s") or []
    wall = sum(walls)
    med = num(rec.get("frame_wall_med_s"))
    nodes = num(rec.get("nodes")) or 1
    cores = num(rec.get("cpus")) or os.cpu_count() or 1
    par = num(rec.get("parallelism"))

    mem = num(rec.get("mem_total_mb")) or host_mem_mb()
    budget = float(os.environ["BENCH_CACHE_MB"]) if os.environ.get("BENCH_CACHE_MB") else (mem * 0.5 if mem else None)
    rss = num(rec.get("rss_peak_mb"))
    if rss is not None and budget:
        facts.append("rss_peak %d MB / budget %d MB" % (rss, budget))
        if rss >= 0.8 * budget:
            signals.append("RAM-bound")
    majflt = num(rec.get("majflt"))
    if majflt:
        facts.append("major faults %d" % majflt)
        if "RAM-bound" not in signals:
            signals.append("RAM-bound")

    block = None
    if num(rec.get("io_read_bytes")) is not None and wall > 0:
        block = (rec["io_read_bytes"] + (rec.get("io_write_bytes") or 0)) / wall / 1e9
        facts.append("block IO %.3f GB/s (r %.0f MB, w %.0f MB)"
                     % (block, rec["io_read_bytes"] / 1e6, (rec.get("io_write_bytes") or 0) / 1e6))
        if rec.get("io_rchar") is not None:
            facts.append("rchar %.0f MB" % (rec["io_rchar"] / 1e6))
    files = None
    plate = num(rec.get("plate_bytes")) or 0
    out_frames = num(rec.get("output_frames")) or 0
    out_per_frame = (rec.get("output_bytes") or 0) / out_frames if out_frames else 0
    if med and (plate or out_per_frame):
        files = (plate + out_per_frame) / med / 1e9
        facts.append("file bytes/frame %.1f MB (plates %.1f, output %.1f) = %.3f GB/s"
                     % ((plate + out_per_frame) / 1e6, plate / 1e6, out_per_frame / 1e6, files))
    if (block is not None and block >= 0.5 * DISK_GBPS) or (files is not None and files >= 0.5 * DISK_GBPS):
        signals.append("IO-bound")

    pixels = num(rec.get("pixels")) or RES_PIXELS.get(rec.get("res"))
    counts = rec.get("counts") or {}
    merges = sum(v for k, v in counts.items() if "Merge" in k)
    if pixels and med:
        moved = pixels * 16.0 * (2 * nodes + merges)
        rate = moved / med / 1e9
        facts.append("est. moved %.0f MB/frame = %.2f GB/s" % (moved / 1e6, rate))
        if rate >= 0.5 * STREAM_GBPS:
            signals.append("bandwidth-bound")

    if med is not None:
        per_node_ms = med / nodes * 1000
        facts.append("%.3f ms/node" % per_node_ms)
        if per_node_ms < 1.0 and par is not None and par <= 1.3:
            signals.append("host-overhead-bound")

    if par is not None:
        facts.append("parallelism %.2f of %d cores" % (par, cores))
        if not signals and par >= 0.7 * cores:
            signals.append("CPU-bound")
    if not signals:
        signals.append("under-occupied")
    return signals, facts


def main(paths):
    if not paths:
        print(__doc__.strip().splitlines()[2])
        return 2
    for path in paths:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                signals, facts = classify(rec)
                head = "%s n=%s res=%s%s" % (rec.get("topo"), rec.get("n"), rec.get("res"),
                                              " settings=" + rec["settings"] if rec.get("settings") else "")
                print("%-44s %-28s %s" % (head, "+".join(signals), "; ".join(facts)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
