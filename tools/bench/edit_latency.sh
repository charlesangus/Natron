#!/usr/bin/env bash
# Edit-latency matrix: runs tools/bench/edit_latency_bench.py once per configuration under
# Tests/gui/run-gui-test.sh and collects one JSON line per edit.
#
#   tools/bench/edit_latency.sh <Natron.AppImage> [out-dir]
#
# Environment (each a space-separated list; the defaults give the full matrix):
#   BENCH_SCENARIOS   chain           chain, or offbranch: the same chain plus off-branch Grades merged with
#                                     the edited node into a second, inactive Viewer
#   BENCH_CASES       grade hsv       the edited node: Grade, or HSVKeyer after the same RAW chain
#   BENCH_ZOOMS       fit 100         viewer zoom while editing
#   BENCH_PANELS      closed open     the edited node's settings panel
#   BENCH_GPUS        off auto        NATRON_GPU
#   BENCH_AUTOPROXYS  off on          the autoProxyScrubbing preference
#   BENCH_RAMS        default         the maxRAMPercent preference; default leaves it alone
#   BENCH_POLICIES    default         NATRON_CACHE_ADMISSION (all, priorityFloor); default leaves it unset.
#                                     Innermost loop, so the policies alternate run by run
#   BENCH_REPS        repetitions of the whole matrix, outermost loop (default 1)
#   BENCH_EDITS       edits per configuration (default 20); BENCH_WARMUP untimed edits before them (default 3)
#   BENCH_RAW_FILE    the raw file (default Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2)
#   BENCH_LABEL       free text stored in every line
#   BENCH_TMPDIR      where run-gui-test.sh extracts the AppImage, and so where the throwaway HOME and its
#                     disk tier live (default: mktemp's default, usually /tmp)
#   GUI_TEST_TIMEOUT  seconds allowed per configuration (default 1800)
#   GUI_TEST_HOST_DISPLAY  e.g. :0 to run against the host's display; without it the run uses Xvfb with
#                          software GL, where NATRON_GPU=auto has no GPU to use
#
# Results: <out-dir>/edit_latency.jsonl (default out-dir build/bench/edit-latency), plus each
# configuration's render profile, results.txt and natron.log under
# <out-dir>/<scenario>-<node>-<zoom>-panel-<panel>-gpu-<gpu>-proxy-<proxy>-ram-<ram>-adm-<policy>-rep-<rep>/.
# The AppImage is extracted afresh for every configuration, so every run starts with an empty disk tier.
set -uo pipefail

APPIMAGE=${1:-}
[ -n "$APPIMAGE" ] || { echo "usage: $0 <Natron.AppImage> [out-dir]" >&2; exit 2; }
[ -f "$APPIMAGE" ] || { echo "no such AppImage: $APPIMAGE" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
APPIMAGE=$(realpath "$APPIMAGE")
OUT=$(realpath -m "${2:-$REPO/build/bench/edit-latency}")
mkdir -p "$OUT"

if [ -n "${BENCH_TMPDIR:-}" ]; then
    BENCH_TMPDIR=$(realpath -m "$BENCH_TMPDIR")
    mkdir -p "$BENCH_TMPDIR"
fi

failed=0
for rep in $(seq 0 $((${BENCH_REPS:-1} - 1))); do
for scenario in ${BENCH_SCENARIOS:-chain}; do
for node in ${BENCH_CASES:-grade hsv}; do
    for zoom in ${BENCH_ZOOMS:-fit 100}; do
        for panel in ${BENCH_PANELS:-closed open}; do
            for gpu in ${BENCH_GPUS:-off auto}; do
                for proxy in ${BENCH_AUTOPROXYS:-off on}; do
                    for ram in ${BENCH_RAMS:-default}; do
                        for policy in ${BENCH_POLICIES:-default}; do
                            tag="$scenario-$node-$zoom-panel-$panel-gpu-$gpu-proxy-$proxy-ram-$ram-adm-$policy-rep-$rep"
                            dir="$OUT/$tag"
                            mkdir -p "$dir"
                            rm -f "$dir"/render.prof.*
                            echo "== $tag"
                            run_env=(NATRON_GPU="$gpu" NATRON_RENDER_SCHEDULER=taskgraph
                                NATRON_RENDER_PROFILE="$dir/render.prof" NATRON_GUI_TEST_OUT="$dir"
                                BENCH_SCENARIO="$scenario" BENCH_CASE="$node" BENCH_ZOOM="$zoom" BENCH_PANEL="$panel"
                                BENCH_AUTOPROXY="$proxy" BENCH_RAM_PERCENT="$ram" BENCH_REP="$rep"
                                BENCH_RESULT="$OUT/edit_latency.jsonl" GUI_TEST_TIMEOUT="${GUI_TEST_TIMEOUT:-1800}")
                            if [ "$policy" = default ]; then
                                unset_policy=(-u NATRON_CACHE_ADMISSION)
                            else
                                unset_policy=()
                                run_env+=(NATRON_CACHE_ADMISSION="$policy")
                            fi
                            if [ -n "${BENCH_TMPDIR:-}" ]; then
                                run_env+=(TMPDIR="$BENCH_TMPDIR")
                            fi
                            env "${unset_policy[@]}" "${run_env[@]}" \
                                "$REPO/Tests/gui/run-gui-test.sh" "$REPO/tools/bench/edit_latency_bench.py" "$APPIMAGE" \
                                > "$dir/run.log" 2>&1
                            code=$?
                            if [ "$code" -ne 0 ]; then
                                echo "$tag exited with $code, see $dir/run.log" >&2
                                failed=$((failed + 1))
                            fi
                        done
                    done
                done
            done
        done
    done
done
done
done

python3 -I - "$OUT/edit_latency.jsonl" <<'PY'
import collections
import json
import os
import statistics
import sys

if not os.path.exists(sys.argv[1]):
    sys.exit("no results")
groups = collections.OrderedDict()
for line in open(sys.argv[1]):
    if line.strip():
        r = json.loads(line)
        key = (r.get("scenario", "chain"), r["case"], r["zoom"], r["panel"], r["gpu"], r["autoproxy"],
               r.get("ram_percent", "default"), r.get("policy", "default"))
        groups.setdefault(key, []).append(r)


def med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def p95(values):
    values = sorted(v for v in values if v is not None)
    return values[max(0, -(-95 * len(values) // 100) - 1)] if values else None


def show(v, scale=1.0):
    return "-" if v is None else "%.1f" % (v / scale)


# Repetitions are pooled; a run's peak RSS, write bytes and tier size are those of its last edit.
print("%-9s %-5s %-4s %-6s %-3s %-5s %-7s %-13s %4s %5s %9s %9s %6s %8s %9s %9s %9s %9s" % (
    "scenario", "case", "zoom", "panel", "gpu", "proxy", "ram", "policy", "runs", "edits", "median_ms", "p95_ms",
    "up_ex", "off_ex", "off_ms", "peak_mb", "write_mb", "tier_mb"))
for key, rows in groups.items():
    runs = collections.OrderedDict()
    for r in rows:
        runs.setdefault(r.get("rep", 0), []).append(r)
    last = [max(rs, key=lambda r: r.get("edit", 0)) for rs in runs.values()]
    off = [r.get("offbranch") or {} for r in rows]
    print("%-9s %-5s %-4s %-6s %-3s %-5s %-7s %-13s %4d %5d %9s %9s %6s %8s %9s %9s %9s %9s" % (key + (
        len(runs), len(rows),
        show(med(r.get("latency_ms") for r in rows)), show(p95(r.get("latency_ms") for r in rows)),
        show(med(r.get("upstream_executed") for r in rows)),
        show(med(o.get("offbranch_executed") for o in off)), show(med(o.get("latency_ms") for o in off)),
        show(med(r.get("vm_hwm_mb") for r in last)),
        show(med(r.get("io_write_bytes") for r in last), 1 << 20),
        show(med(r.get("tier_bytes") for r in last), 1 << 20))))
PY
python3 -I "$REPO/tools/bench/edit_latency_bench.py" --classify "$OUT" || failed=$((failed + 1))
exit $((failed > 0))
