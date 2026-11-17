"""Adds a run's GPU memory to its records, from sample_vram.sh's samples.

  heavy_scene_vram.py SAMPLES RESULTS LABEL [--bisect VARIANT]

Each record of LABEL in RESULTS that has started_at and ended_at gets, in MB: vram_peak_mb and
gtt_peak_mb (the highest sample between the two), vram_start_mb, vram_end_mb, vram_delta_mb and
gtt_delta_mb (end less start, each the last sample at or before the time), and vram_total_mb and
gtt_total_mb. Samples whose GTT column is "-" (NVIDIA, which has no GTT) leave the gtt fields null. Every record of LABEL without a bisect field gets VARIANT (null without one), as the
watchdog's and the shell's records are written without it. Prints one line per scenario. RESULTS is
rewritten in place; the other lines are kept as they are. Standard library only, so it runs on the
host's python3.
"""

import argparse
import bisect
import json
import os
import sys

MB = 1048576.0


def read_samples(path):
    header = {}
    times, vram, gtt = [], [], []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                words = line[1:].split()
                header.update(zip(words[0::2], words[1::2]))
                continue
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                t, v = float(parts[0]), int(parts[1])
                g = None if parts[2] == "-" else int(parts[2])
            except ValueError:
                continue
            times.append(t)
            vram.append(v)
            gtt.append(g)
    return header, times, vram, gtt


def at_or_before(times, values, t):
    i = bisect.bisect_right(times, t) - 1
    return values[max(i, 0)]


def window_fields(header, times, vram, gtt, start, end):
    lo = bisect.bisect_left(times, start)
    hi = bisect.bisect_right(times, end)
    vram_start, vram_end = at_or_before(times, vram, start), at_or_before(times, vram, end)
    vram_peak = max(vram[lo:hi] + [vram_start, vram_end])
    has_gtt = all(g is not None for g in gtt)
    if has_gtt:
        gtt_start, gtt_end = at_or_before(times, gtt, start), at_or_before(times, gtt, end)
        gtt_peak = max(gtt[lo:hi] + [gtt_start, gtt_end])
    else:
        gtt_start = gtt_end = gtt_peak = None

    def mb(value):
        return None if value is None else round(value / MB, 1)

    def total(key):
        try:
            return int(header[key])
        except (KeyError, ValueError):
            return None

    return {
        "vram_peak_mb": mb(vram_peak),
        "gtt_peak_mb": mb(gtt_peak),
        "vram_start_mb": mb(vram_start),
        "vram_end_mb": mb(vram_end),
        "vram_delta_mb": mb(vram_end - vram_start),
        "gtt_delta_mb": mb(gtt_end - gtt_start) if has_gtt else None,
        "vram_total_mb": total("vram_total_mb"),
        "gtt_total_mb": total("gtt_total_mb"),
        "vram_samples": hi - lo,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("samples")
    parser.add_argument("results")
    parser.add_argument("label")
    parser.add_argument("--bisect")
    args = parser.parse_args()

    header, times, vram, gtt = {}, [], [], []
    if os.path.exists(args.samples):
        header, times, vram, gtt = read_samples(args.samples)
    if not times:
        print("    vram: no samples in %s" % args.samples)

    out_lines = []
    with open(args.results) as f:
        lines = f.read().splitlines()
    for line in lines:
        try:
            record = json.loads(line)
        except ValueError:
            out_lines.append(line)
            continue
        if record.get("label") != args.label:
            out_lines.append(line)
            continue
        if "bisect" not in record:
            record["bisect"] = args.bisect or None
        start, end = record.get("started_at"), record.get("ended_at")
        if times and isinstance(start, (int, float)) and isinstance(end, (int, float)):
            fields = window_fields(header, times, vram, gtt, start, end)
            record.update(fields)
            gtt_text = ("GTT peak %8.1f MB (%+8.1f)" % (fields["gtt_peak_mb"], fields["gtt_delta_mb"])
                        if fields["gtt_peak_mb"] is not None else "no GTT")
            print("    vram: %-22s VRAM peak %8.1f MB (%+8.1f), %s, %d samples"
                  % (record.get("scenario"), fields["vram_peak_mb"], fields["vram_delta_mb"], gtt_text, fields["vram_samples"]))
        out_lines.append(json.dumps(record))

    temporary = args.results + ".vram.tmp"
    with open(temporary, "w") as f:
        f.write("".join(l + "\n" for l in out_lines))
    os.replace(temporary, args.results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
