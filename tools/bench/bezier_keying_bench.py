# Bezier keying benchmark: how the cost of keying a shape grows with its keyframe count.
# Run inside NatronRenderer from the repo root:
#
#   NatronRenderer -b tools/bench/bezier_keying_bench.py
#   BENCH_BEZIER_HEAVY=1 NatronRenderer -b tools/bench/bezier_keying_bench.py
#
# For each keyframe count K it builds BENCH_BEZIER_SHAPES finished Beziers of BENCH_BEZIER_POINTS
# points in one Roto node, then keys every point and its feather point at frames 1..K with
# setPointAtIndex and setFeatherPointAtIndex, with auto-keying on (the default). It prints the wall time of the keying
# per K, the median cost of one point edit while keying the last K, and the slope of
# log(time) against log(K): about 2 when each edit costs time proportional to the shape's keyframes.
#
# Environment:
#   BENCH_BEZIER_SHAPES   Beziers per K (default 1; 30 with BENCH_BEZIER_HEAVY=1)
#   BENCH_BEZIER_POINTS   control points per Bezier (default 24)
#   BENCH_BEZIER_KEYS     comma-separated keyframe counts (default 50,100,200; 200 with BENCH_BEZIER_HEAVY=1)
#   BENCH_BEZIER_HEAVY    1 for the heavy preset: 30 shapes x 24 points x 200 keys
#   BENCH_LABEL           free text stored with the result (for example "before" or "after")
#   BENCH_OUT             JSON-lines file the result is appended to
#
# NatronRenderer takes no script arguments, so the presets are environment variables.

import json
import math
import os
import sys
import time

HEAVY = os.environ.get("BENCH_BEZIER_HEAVY", "0") == "1"
SHAPES = int(os.environ.get("BENCH_BEZIER_SHAPES", "30" if HEAVY else "1"))
POINTS = int(os.environ.get("BENCH_BEZIER_POINTS", "24"))
KEYS = [int(k) for k in os.environ.get("BENCH_BEZIER_KEYS", "200" if HEAVY else "50,100,200").split(",") if k.strip()]
LABEL = os.environ.get("BENCH_LABEL", "")
OUT = os.environ.get("BENCH_OUT", "")


def point_at(s, i, k, grow):
    a = 2.0 * math.pi * i / POINTS
    cx = 300.0 + 40.0 * (s % 10)
    cy = 300.0 + 40.0 * (s // 10)
    r = (100.0 + 10.0 * math.sin(0.3 * k + i)) * grow
    x, y = cx + r * math.cos(a), cy + r * math.sin(a)
    tx, ty = -8.0 * math.sin(a), 8.0 * math.cos(a)
    return x, y, x - tx, y - ty, x + tx, y + ty


def build_shapes(ctx):
    shapes = []
    for s in range(SHAPES):
        bez = None
        for i in range(POINTS):
            x, y = point_at(s, i, 0, 1.0)[:2]
            if bez is None:
                bez = ctx.createBezier(x, y, 1)
                if bez is None:
                    raise RuntimeError("could not create a Bezier")
            else:
                bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        shapes.append(bez)
    return shapes


def key_shapes(shapes, keys, per_edit):
    for k in range(keys):
        t = 1 + k
        for s, bez in enumerate(shapes):
            for i in range(POINTS):
                start = time.perf_counter()
                bez.setPointAtIndex(i, t, *point_at(s, i, k, 1.0))
                mid = time.perf_counter()
                bez.setFeatherPointAtIndex(i, t, *point_at(s, i, k, 1.1))
                end = time.perf_counter()
                if per_edit is not None:
                    per_edit.append(mid - start)
                    per_edit.append(end - mid)


def slope(xs, ys):
    pts = [(math.log(x), math.log(y)) for x, y in zip(xs, ys) if x > 0 and y > 0]
    if len(pts) < 2:
        return None
    n = float(len(pts))
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    den = n * sxx - sx * sx
    return (n * sxy - sx * sy) / den if den else None


def main():
    rows = []
    last_edits = []
    for r, keys in enumerate(KEYS):
        node = app.createNode("Roto")
        ctx = node.getRotoContext()
        if ctx is None:
            raise RuntimeError("Roto node has no Python roto context")
        build_start = time.perf_counter()
        shapes = build_shapes(ctx)
        build_s = time.perf_counter() - build_start
        per_edit = last_edits if r == len(KEYS) - 1 else None
        start = time.perf_counter()
        key_shapes(shapes, keys, per_edit)
        key_s = time.perf_counter() - start
        rows.append({"keys": keys, "build_s": round(build_s, 4), "key_s": round(key_s, 4)})
        print("[bezier_keying_bench] %d shapes x %d points x %d keys: keying %.3f s (shapes built in %.3f s)"
              % (SHAPES, POINTS, keys, key_s, build_s))
        sys.stdout.flush()
        node.destroy()

    last_edits.sort()
    median_ms = 1e3 * last_edits[len(last_edits) // 2] if last_edits else None
    fit = slope([row["keys"] for row in rows], [row["key_s"] for row in rows])
    if median_ms is not None:
        print("[bezier_keying_bench] median point edit at %d keys: %.4f ms over %d edits" % (KEYS[-1], median_ms, len(last_edits)))
    if fit is not None:
        print("[bezier_keying_bench] log-log slope of keying time against keys: %.3f" % fit)

    result = {"bench": "bezier_keying", "label": LABEL, "shapes": SHAPES, "points": POINTS, "rows": rows,
              "median_edit_ms": None if median_ms is None else round(median_ms, 5),
              "slope": None if fit is None else round(fit, 3), "cpus": os.cpu_count()}
    line = json.dumps(result)
    print("[bezier_keying_bench] RESULT " + line)
    if OUT:
        with open(OUT, "a") as f:
            f.write(line + "\n")


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stdout.flush()
    os._exit(3)
sys.stdout.flush()
os._exit(0)
