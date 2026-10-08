#!/usr/bin/env bash
# Interleaved native-vs-OFX Grade chain benchmark and the four gate ratios.
#
#   tools/bench/native_vs_ofx.sh <tag> <rounds>
#
# Each round renders, first with BENCH_IMPL=ofx and then with BENCH_IMPL=native, a tiny chain of 0 and
# 1000 Grades (5 frames) and an HD chain of 30 and 100 Grades (3 frames). Results go to
# build/bench/results-<tag>-r<round>-<impl>.jsonl. Ratios are only meaningful within one invocation,
# because the host is shared and its load changes between runs. Absolute numbers are called
# references only when every configuration started quiet (load1 < 0.5, cpu and io `some avg10` < 5%).
# BENCH_RENDERER_DIR and BENCH_PLUGIN_PATH are forwarded to run_matrix.sh.
# shellcheck source-path=SCRIPTDIR
set -u
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ $# -ne 2 ]; then
    echo "usage: $0 <tag> <rounds>" >&2
    exit 2
fi
tag=$1
rounds=$2
case $rounds in
    '' | *[!0-9]* | 0) echo "rounds must be a positive integer" >&2; exit 2 ;;
esac

renderer_dir=${BENCH_RENDERER_DIR:-build/release}
case $renderer_dir in /*) ;; *) renderer_dir=$repo/$renderer_dir ;; esac
renderer=$renderer_dir/Renderer/NatronRenderer
if [ ! -x "$renderer" ]; then
    echo "no renderer at $renderer; build release first" >&2
    exit 1
fi
if [ -d "$repo/Engine/Nodes/Image" ]; then
    newer=$(find "$repo/Engine/Nodes/Image" -type f -newer "$renderer" -print -quit)
    if [ -n "$newer" ]; then
        echo "$renderer is older than $newer; rebuild before benchmarking" >&2
        exit 1
    fi
fi

results() {
    echo "$repo/build/bench/results-$tag-r$1-$2.jsonl"
}
for r in $(seq 1 "$rounds"); do
    for impl in ofx native; do
        if [ -e "$(results "$r" "$impl")" ]; then
            echo "$(results "$r" "$impl") exists; pick a new tag" >&2
            exit 1
        fi
    done
done

for r in $(seq 1 "$rounds"); do
    for impl in ofx native; do
        export BENCH_IMPL=$impl
        echo "== round $r, $impl =="
        "$repo/tools/bench/run_matrix.sh" "$tag-r$r-$impl" tiny 5 0 chain:0,1000 || exit 1
        "$repo/tools/bench/run_matrix.sh" "$tag-r$r-$impl" hd 3 0 chain:30,100 || exit 1
    done
done

files=()
for r in $(seq 1 "$rounds"); do
    files+=("$(results "$r" ofx)" "$(results "$r" native)")
done
python3 - "${files[@]}" <<'PY'
import json
import statistics
import sys

paths = sys.argv[1:]
rounds = [(paths[i], paths[i + 1]) for i in range(0, len(paths), 2)]


def load(path):
    recs = {}
    for line in open(path):
        line = line.strip()
        if line:
            r = json.loads(line)
            recs[(r["res"], r["n"])] = r
    return recs


def frame(recs, res, n):
    return recs[(res, n)]["frame_wall_med_s"]


def rss(recs, n):
    return recs[("tiny", n)]["rss_before_mb"]


quiet = True
rows = []
for r, (ofx_path, nat_path) in enumerate(rounds, 1):
    o, n = load(ofx_path), load(nat_path)
    for recs in (o, n):
        for rec in recs.values():
            vals = (rec.get("load1_start"), rec.get("psi_cpu_some10"), rec.get("psi_io_some10"))
            if None in vals or vals[0] >= 0.5 or vals[1] >= 5 or vals[2] >= 5:
                quiet = False
    try:
        ratios = {
            "mem": (rss(n, 1000) - rss(n, 0)) / (rss(o, 1000) - rss(o, 0)),
            "build": n[("tiny", 1000)]["build_s"] / o[("tiny", 1000)]["build_s"],
            "tiny": frame(n, "tiny", 1000) / frame(o, "tiny", 1000),
            "hd": (frame(n, "hd", 100) - frame(n, "hd", 30)) / (frame(o, "hd", 100) - frame(o, "hd", 30)),
        }
        per_node = {
            "ofx_kb": (rss(o, 1000) - rss(o, 0)) * 1024 / 1000,
            "native_kb": (rss(n, 1000) - rss(n, 0)) * 1024 / 1000,
            "ofx_ms": (frame(o, "hd", 100) - frame(o, "hd", 30)) * 1000 / 70,
            "native_ms": (frame(n, "hd", 100) - frame(n, "hd", 30)) * 1000 / 70,
        }
    except (KeyError, ZeroDivisionError) as e:
        sys.exit("round %d is missing a configuration or has a zero delta: %r" % (r, e))
    rows.append((ratios, per_node))

names = ("mem", "build", "tiny", "hd")
print("round  " + "  ".join("%7s" % k for k in names) + "   KB/node ofx->native   HD ms/node ofx->native")
for r, (ratios, pn) in enumerate(rows, 1):
    print("%5d  " % r + "  ".join("%7.3f" % ratios[k] for k in names)
          + "   %7.0f->%-7.0f      %7.2f->%-7.2f" % (pn["ofx_kb"], pn["native_kb"], pn["ofx_ms"], pn["native_ms"]))
print("median " + "  ".join("%7.3f" % statistics.median(x[0][k] for x in rows) for k in names))
spread = {k: max(x[0][k] for x in rows) - min(x[0][k] for x in rows) for k in names}
print("spread " + "  ".join("%7.3f" % spread[k] for k in names))
wide = [k for k in names if spread[k] > 0.15]
if wide:
    print("spread above 0.15 for %s: run two more rounds and take the median of all" % ", ".join(wide))
print("host: " + ("quiet at every configuration start" if quiet
                  else "contended (load1 >= 0.5 or cpu/io some avg10 >= 5% or unreadable); ratios only, no absolute reference"))
PY
