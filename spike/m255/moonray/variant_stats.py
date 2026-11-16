"""Prints one table row per variant: deep sample counts and flatten(deep) vs flat beauty error.

    variant_stats.py <variants dir> <tag>...

Each <tag> needs <tag>_deep.exr and <tag>_main.exr in the variants dir.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import flatten, read_deep, read_flat  # noqa: E402


def stats(d, tag):
    got = read_deep(os.path.join(d, f"{tag}_deep.exr"))
    flat = read_flat(os.path.join(d, f"{tag}_main.exr"))
    if got is None or flat is None:
        return f"{tag:24s} missing output"
    spec, dd, names = got
    w, h = spec.width, spec.height
    rgba, counts = flatten(spec, dd, names)
    pixels = flat[1]
    total = sum(sum(r) for r in counts)
    covered = sum(1 for r in counts for n in r if n)
    multi = sum(1 for r in counts for n in r if n > 1)
    se = [0.0] * 4
    less = more = 0
    for y in range(h):
        for x in range(w):
            ref = pixels[y][x]
            for c in range(4):
                se[c] += (rgba[y][x][c] - float(ref[c])) ** 2
            da = rgba[y][x][3] - float(ref[3])
            less += da < -1e-3
            more += da > 1e-3
    n = w * h
    rgb = math.sqrt(sum(se[:3]) / (3 * n))
    alpha = math.sqrt(se[3] / n)
    rgba_e = math.sqrt(sum(se) / (4 * n))
    mean_cov = total / covered if covered else 0.0
    return (f"{tag:24s} samples={total:6d} per-covered-px={mean_cov:.3f} max={max(max(r) for r in counts)} "
            f"px>1={multi:5d} rmse rgba={rgba_e:.5f} rgb={rgb:.5f} a={alpha:.5f} "
            f"px deepA<flatA={less:5d} deepA>flatA={more:5d}")


def main(d, tags):
    for tag in tags:
        print(stats(d, tag))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
