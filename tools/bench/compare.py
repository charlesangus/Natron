#!/usr/bin/env python3
"""Compare two graph_bench.py result files and flag regressions.

usage: compare.py BEFORE.jsonl AFTER.jsonl [--threshold 1.5] [--pair-settings BEFORE_SETTINGS AFTER_SETTINGS]
                  [--pair-field FIELD BEFORE_VALUE AFTER_VALUE]

--pair-field FIELD A B pairs rows by topo/n/res where FIELD equals A in the first file against rows
where it equals B in the second (the same file may be given twice) and prints B/A ratios. Ratios from
rows of one interleaved run are comparable; the printed load/PSI shows how contended each side was.
"""
import argparse
import json
import statistics
import sys

# (label, record field, True if a larger value is worse)
METRICS = [
    ("build_s", "build_s", True),
    ("frame_s", "frame_wall_med_s", True),
    ("parallelism", "parallelism", False),
    ("rss_mb", "rss_before_mb", True),
]

# Printed only when both files have the field; range_wall_s is divided by range_frames per record.
RANGE_METRICS = [
    ("range_frame_s", "range_frame_s", True),
    ("range_par", "range_parallelism", False),
]

# Block IO of the timed frames in MB; printed when both files have it and never flagged, since a
# change in IO volume is a property of the workload rather than a regression.
IO_METRICS = [
    ("io_r", "io_read_mb"),
    ("io_w", "io_write_mb"),
]


# Recorded by graph_bench.py at the start of the timed phase; printed, never flagged.
PRESSURE_FIELDS = [
    ("load1", "load1_start"),
    ("psi_cpu", "psi_cpu_some10"),
    ("psi_io", "psi_io_some10"),
    ("psi_mem", "psi_mem_some10"),
]


def load(path, only_settings=None, only_field=None):
    groups = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if rec.get("range_wall_s") and rec.get("range_frames"):
                rec["range_frame_s"] = rec["range_wall_s"] / rec["range_frames"]
            for src, dst in (("io_read_bytes", "io_read_mb"), ("io_write_bytes", "io_write_mb")):
                if isinstance(rec.get(src), (int, float)):
                    rec[dst] = rec[src] / 1e6
            settings = rec.get("settings") or ""
            if only_field is not None:
                name, value = only_field
                if str(rec.get(name)) != value:
                    continue
                settings = ""
            if only_settings is not None:
                if settings != only_settings:
                    continue
                settings = ""
            key = (rec.get("topo"), rec.get("n"), rec.get("res"), rec.get("named"), settings)
            groups.setdefault(key, []).append(rec)
    return groups


def median_of(records, field):
    vals = [r[field] for r in records if isinstance(r.get(field), (int, float))]
    return statistics.median(vals) if vals else None


def khz_note(label, before, after):
    b = median_of(before, "cpu_khz_mean")
    a = median_of(after, "cpu_khz_mean")
    if b is None and a is None:
        return None
    text = "khz_mean %s->%s" % (fmt(b), fmt(a))
    if b and a and abs(a - b) / min(a, b) > 0.10:
        print("note: %s: cpu clock differs by more than 10%% (%s kHz vs %s kHz); timings are not comparable"
              % (label, fmt(b), fmt(a)))
    return text


def fmt(v):
    return "-" if v is None else "%.4g" % v


def key_sort(key):
    return tuple("" if k is None else str(k) for k in key[:1]) + (
        key[1] if isinstance(key[1], int) else -1,
        str(key[2]),
        str(key[3]),
        key[4],
    )


def compare_metric(before, after, higher_is_worse, threshold):
    if before is None or after is None or before == 0:
        return None, False
    ratio = after / before
    if higher_is_worse:
        return ratio, ratio > threshold
    return ratio, ratio < 1.0 / threshold


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument(
        "--threshold",
        type=float,
        default=1.5,
        help="flag a ratio beyond this factor (default 1.5; run-to-run noise reaches 2x)",
    )
    ap.add_argument(
        "--pair-settings",
        nargs=2,
        metavar=("BEFORE_SETTINGS", "AFTER_SETTINGS"),
        help="match rows whose settings equal BEFORE_SETTINGS in the first file against rows whose "
        "settings equal AFTER_SETTINGS in the second; 'default' selects rows with empty settings",
    )
    ap.add_argument(
        "--pair-field",
        nargs=3,
        metavar=("FIELD", "BEFORE_VALUE", "AFTER_VALUE"),
        help="generalises --pair-settings: match rows whose FIELD equals BEFORE_VALUE in the first "
        "file against rows whose FIELD equals AFTER_VALUE in the second",
    )
    args = ap.parse_args()
    if args.pair_field and args.pair_settings:
        ap.error("--pair-field and --pair-settings are exclusive")

    only_before = only_after = None
    field_before = field_after = None
    if args.pair_field:
        field_before = (args.pair_field[0], args.pair_field[1])
        field_after = (args.pair_field[0], args.pair_field[2])
    if args.pair_settings:
        only_before, only_after = ("" if v == "default" else v for v in args.pair_settings)
    before = load(args.before, only_before, field_before)
    after = load(args.after, only_after, field_after)
    common = sorted(set(before) & set(after), key=key_sort)
    flagged_any = False

    for key in common:
        topo, n, res, named, settings = key
        cells = []
        flagged = False
        for label, field, higher_is_worse in METRICS + RANGE_METRICS:
            b = median_of(before[key], field)
            a = median_of(after[key], field)
            if (b is None or a is None) and (label, field, higher_is_worse) in RANGE_METRICS:
                continue
            ratio, bad = compare_metric(b, a, higher_is_worse, args.threshold)
            flagged = flagged or bad
            r = "-" if ratio is None else "%.2f" % ratio
            cells.append("%s %s->%s x%s" % (label, fmt(b), fmt(a), r))
        for label, field in IO_METRICS:
            b = median_of(before[key], field)
            a = median_of(after[key], field)
            if b is None or a is None:
                continue
            cells.append("%s %sMB->%sMB x%s" % (label, fmt(b), fmt(a), "-" if not b else "%.2f" % (a / b)))
        flagged_any = flagged_any or flagged
        head = "%s n=%s res=%s named=%s%s" % (topo, n, res, named, " settings=" + settings if settings else "")
        for label, field in PRESSURE_FIELDS:
            b = median_of(before[key], field)
            a = median_of(after[key], field)
            if b is not None or a is not None:
                cells.append("%s %s->%s" % (label, fmt(b), fmt(a)))
        khz = khz_note(head, before[key], after[key])
        if khz:
            cells.append(khz)
        print("%-34s %s%s" % (head, "  ".join(cells), "  FLAGGED" if flagged else ""))

    missing = sorted(set(before) - set(after), key=key_sort)
    for key in missing:
        print("FLAGGED missing from after: topo=%s n=%s res=%s named=%s settings=%r" % key)
    for key in sorted(set(after) - set(before), key=key_sort):
        print("note: only in after: topo=%s n=%s res=%s named=%s settings=%r" % key)

    if not common:
        print("FLAGGED: no matching records")
        return 1
    return 1 if flagged_any or missing else 0


if __name__ == "__main__":
    sys.exit(main())
