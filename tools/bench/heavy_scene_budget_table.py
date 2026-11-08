"""Prints where draw state changes spend their time, from heavy_scene_bench.py's draw_budget records.

  heavy_scene_budget_table.py <results.jsonl>

One block per draw_budget record (one scene): the judged budgets, then one line per change with its
time to picture and the parts it sums from, then the traced changes' render-thread time per Hydra and
Storm scope and the HdPerfLog counters that moved. Times are milliseconds. Standard library only, so
it runs on the host's python3.
"""

import json
import sys

PARTS = ("picture_ms", "queue_ms", "apply_ms", "wait_frame_ms", "render_ms", "gpu_tail_ms", "handoff_ms", "present_ms")
HEADS = ("picture", "queue", "apply", "wait", "render", "gpu", "handoff", "present")


def cell(value):
    if value is None:
        return "-"
    if isinstance(value, float):
        return "%.1f" % value
    return str(value)


def budgets_line(record):
    out = []
    for name in sorted(record.get("budgets", {})):
        if name == "stall_ms" or name.startswith("time_to_picture"):
            b = record["budgets"][name]
            out.append("%s %s/%s %s" % (name, cell(b.get("value")), cell(b.get("limit")), b.get("status")))
    return "; ".join(out)


def print_record(record):
    print("== %s (%s, %s): %s" % (record.get("label"), record.get("file"), record.get("gl_renderer"), record.get("result")))
    print("   %s" % budgets_line(record))
    rows = record.get("budget_rows") or {}
    print("   rows: %s" % ", ".join("%s=%s" % (k, v) for k, v in sorted(rows.items())))
    if record.get("traced_stall_ms") is not None:
        print("   traced stall (not judged): %s" % cell(record["traced_stall_ms"]))
    print("   %-8s %-6s %-7s %s  %8s %8s" % ("row", "change", "repeat", " ".join("%8s" % h for h in HEADS), "hidden", "shown"))
    for change in record.get("draw_changes", []):
        print("   %-8s %-6s %-7s %s  %8s %8s" % (change.get("row"), change.get("change"), change.get("repeat"),
                                                " ".join("%8s" % cell(change.get(p)) for p in PARTS),
                                                cell(change.get("hidden_sent")), cell(change.get("shown_sent"))))
    for change in record.get("draw_changes", []):
        if not change.get("traced"):
            continue
        print("   traced %s %s: picture %s, trace collected in %s, render thread found: %s" % (
            change.get("row"), change.get("change"), cell(change.get("picture_ms")), cell(change.get("trace_collect_ms")),
            change.get("trace_thread_found")))
        stages = change.get("stages_ms") or {}
        counts = change.get("stage_counts") or {}
        for stage in sorted(stages, key=lambda s: -(stages[s] or 0.0)):
            print("     %-48s %10s ms  x%s" % (stage, cell(stages[stage]), counts.get(stage, "-")))
        deltas = {k: v for k, v in (change.get("counter_deltas") or {}).items() if v}
        if deltas:
            print("     counters: %s" % ", ".join("%s +%g" % (k, v) for k, v in sorted(deltas.items())))


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    found = False
    with open(sys.argv[1]) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            budget_run = str(record.get("label", "")).startswith("budget-")
            if record.get("scenario") == "draw_budget" and record.get("draw_changes") is not None:
                found = True
                print_record(record)
            elif (record.get("scenario") == "draw_budget" or budget_run) and record.get("result") != "pass":
                # What kept a scene from its budget record: a skip, a time out, memory, or an earlier step.
                found = True
                print("== %s %s: %s %s" % (record.get("label"), record.get("scenario"), record.get("result"),
                                           record.get("reason", "")))
    if not found:
        print("no draw_budget records in %s" % sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
