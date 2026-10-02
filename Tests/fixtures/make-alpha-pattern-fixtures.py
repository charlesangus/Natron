#!/usr/bin/env python3
# Regenerates the spatially varying EXR fixtures ColorViewsRender_Test.cpp reads:
#
#   python3 Tests/fixtures/make-alpha-pattern-fixtures.py Tests/fixtures
#
# flat-alpha-pattern.exr: 16x16, alpha-only, A = ((7x + 3y) mod 16) / 16, a pattern that is
#   neither constant nor symmetric, so misplaced coverage, a flip or a stride error changes it.
# flat-rgba-pattern.exr: the same A under constant R, G, B, so an effect's alpha on it can be
#   compared with the same effect on the alpha-only stream.
# flat-rgb-a-offset-window.exr: R, G and A but no B, R = x and G = y in file coordinates, A = 1,
#   with a display window whose origin is (-2, -1) and a data window starting at a negative x,
#   inside it, so a reader that ignores either origin puts samples in the wrong place.
# flat-alpha-offset-window.exr: the same windows, alpha-only, A = the pattern above plus 1/16
#   over the data window, so no data pixel reads 0 like the padding around it.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1
# Python module.
import os
import sys

import numpy
import OpenImageIO as oiio


def pattern(x, y):
    return ((7 * x + 3 * y) % 16) / 16.0


def write(path, channels, pixels, data_origin, full_window=None):
    height, width = pixels.shape[0], pixels.shape[1]
    spec = oiio.ImageSpec(width, height, len(channels), "half")
    spec.channelnames = channels
    spec.x, spec.y = data_origin
    if full_window is not None:
        spec.full_x, spec.full_y, spec.full_width, spec.full_height = full_window
    else:
        spec.full_x, spec.full_y, spec.full_width, spec.full_height = data_origin[0], data_origin[1], width, height
    spec.attribute("compression", "zip")
    # The EXR writer stamps a DateTime attribute with the current time unless the spec already
    # has one; fix it so regenerating the file is byte-identical.
    spec.attribute("DateTime", "2000:01:01 00:00:00")

    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())
    if not out.write_image(pixels.astype(numpy.float16)):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

    size = 16
    alpha = numpy.zeros((size, size, 1))
    rgba = numpy.zeros((size, size, 4))
    for y in range(size):
        for x in range(size):
            alpha[y, x, 0] = pattern(x, y)
            rgba[y, x] = (0.5, 0.25, 0.75, pattern(x, y))
    write("%s/flat-alpha-pattern.exr" % outdir, ["A"], alpha, (0, 0))
    write("%s/flat-rgba-pattern.exr" % outdir, ["R", "G", "B", "A"], rgba, (0, 0))

    full_window = (-2, -1, 12, 10)
    data_origin = (-1, 2)
    data_width, data_height = 6, 4
    rga = numpy.zeros((data_height, data_width, 3))
    offset_alpha = numpy.zeros((data_height, data_width, 1))
    for row in range(data_height):
        for col in range(data_width):
            x = data_origin[0] + col
            y = data_origin[1] + row
            rga[row, col] = (x, y, 1.0)
            offset_alpha[row, col, 0] = pattern(col, row) + 1.0 / 16
    write("%s/flat-rgb-a-offset-window.exr" % outdir, ["R", "G", "A"], rga, data_origin, full_window)
    write("%s/flat-alpha-offset-window.exr" % outdir, ["A"], offset_alpha, data_origin, full_window)


if __name__ == "__main__":
    main()
