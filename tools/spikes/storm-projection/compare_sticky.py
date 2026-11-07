#!/usr/bin/env python3
# Prints, per prim region of gen_sticky.py's scene, how many pixels differ between two renders.
# usage: compare_sticky.py <a.png> <b.png>
import sys

import numpy as np
import OpenImageIO as oiio


def read(path):
    return oiio.ImageInput.open(path).read_image("float32")[..., :3]


a, b = read(sys.argv[1]), read(sys.argv[2])
# The plate's colours, wherever either render shows them: white reads the same as no image here.
coloured = (a.max(axis=2) - a.min(axis=2) > 0.5) | (b.max(axis=2) - b.min(axis=2) > 0.5)
different = coloured & (np.abs(a - b).max(axis=2) > 0.25)
print("coloured %d, different %d" % (coloured.sum(), different.sum()))
h, w = different.shape
for name, x0, x1, y0, y1 in (("mesh", 0, .36, 0, .55), ("curves", .36, .62, 0, .55), ("points", .62, 1, 0, .55), ("rigid", .3, .75, .55, 1)):
    region = different[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
    print("  %s: %d" % (name, region.sum()))
