#!/usr/bin/env python3
# Regenerates the deep EXR fixtures DeepReadWrite_Test.cpp reads:
#
#   python3 Tests/fixtures/make-deep-fixtures.py Tests/fixtures
#
# Writes three files. deep-scanline.exr and deep-tiled.exr hold identical
# content and differ only in whether the EXR part is scanline or tiled.
# deep-noncanonical.exr holds the same sample structure over a channel set with
# no RGB at all, which OpenImageIO hands to a reader as A, Z, ZBack, AOV -- an
# order in which neither the alpha nor the depth channel sits where an
# R,G,B,A,Z,ZBack file would put it. deep-nozback.exr carries no ZBack channel
# at all, which the deep specification reads as every sample being a point
# sample whose back is its front.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the
# OpenImageIO 3.1 Python module.
#
# Rows are in file order here (row 0 is the top scanline), which is the
# opposite of Natron's bottom-up pixel coordinates; the test's expectations are
# in Natron coordinates, so row 0 below is the test's y == 2.
import sys

import OpenImageIO as oiio

WIDTH = 4
HEIGHT = 3
CHANNELS = ["R", "G", "B", "A", "Z", "ZBack", "AOV"]
# Indices into a sample below, in the order deep-noncanonical.exr stores them.
NONCANONICAL = [("Z", 4), ("ZBack", 5), ("A", 3), ("AOV", 6)]
# Same, for deep-nozback.exr.
NOZBACK = [("R", 0), ("G", 1), ("B", 2), ("A", 3), ("Z", 4)]

# (x, y) -> list of samples, each [R, G, B, A, Z, ZBack, AOV].
PIXELS = {
    (0, 0): [],
    (1, 0): [[0.25, 0.5, 0.75, 1.0, 3.0, 3.5, 11.0]],
    # Deliberately stored back-to-front, so a reader that sorts (or a writer
    # that reorders) cannot pass unnoticed.
    (2, 0): [[0.5, 0.25, 0.125, 0.5, 5.0, 5.25, 12.0],
             [0.25, 0.75, 0.5, 0.25, 2.0, 2.5, 13.0]],
    (3, 0): [[1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 14.0],
             [0.0, 1.0, 0.0, 0.5, 4.0, 6.0, 15.0],
             [0.0, 0.0, 1.0, 0.75, 5.5, 5.5, 16.0]],
    (0, 1): [[0.125, 0.25, 0.375, 0.5, 7.0, 7.25, 20.0],
             [0.625, 0.75, 0.875, 1.0, 8.0, 8.25, 21.0]],
    (1, 1): [],
    (2, 1): [[0.75, 0.75, 0.75, 0.25, 9.0, 9.0, 22.0]],
    (3, 1): [[0.5, 0.5, 0.5, 0.125, 10.0, 12.0, 23.0]],
    (0, 2): [[0.25, 0.125, 0.0625, 0.5, 13.0, 13.0, 30.0]],
    (1, 2): [[0.0, 0.25, 0.5, 0.75, 14.0, 14.5, 31.0],
             [0.25, 0.5, 0.75, 1.0, 15.0, 15.5, 32.0]],
    (2, 2): [],
    (3, 2): [[1.0, 1.0, 1.0, 1.0, 16.0, 16.25, 33.0],
             [0.5, 0.25, 0.125, 0.5, 17.0, 17.5, 34.0]],
}


def make_spec(names, tiled):
    spec = oiio.ImageSpec(WIDTH, HEIGHT, len(names), "float")
    spec.channelnames = names
    spec.deep = True
    if tiled:
        spec.tile_width = 2
        spec.tile_height = 2
    return spec


def make_deep_data(names, columns):
    deep = oiio.DeepData()
    deep.init(WIDTH * HEIGHT, len(names),
              [oiio.TypeDesc("float")] * len(names), names)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            pixel = y * WIDTH + x
            samples = PIXELS[(x, y)]
            deep.set_samples(pixel, len(samples))
            for s, values in enumerate(samples):
                for c, column in enumerate(columns):
                    deep.set_deep_value(pixel, c, s, values[column])
    return deep


def write(path, names, columns, tiled):
    spec = make_spec(names, tiled)
    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.supports("deepdata"):
        raise RuntimeError("%s does not support deep data" % path)
    if tiled and not out.supports("tiles"):
        raise RuntimeError("%s does not support tiles" % path)
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())
    if not out.write_deep_image(make_deep_data(names, columns)):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    allcolumns = list(range(len(CHANNELS)))
    write("%s/deep-scanline.exr" % outdir, CHANNELS, allcolumns, False)
    write("%s/deep-tiled.exr" % outdir, CHANNELS, allcolumns, True)
    write("%s/deep-noncanonical.exr" % outdir,
          [name for name, _ in NONCANONICAL],
          [column for _, column in NONCANONICAL], False)
    write("%s/deep-nozback.exr" % outdir,
          [name for name, _ in NOZBACK],
          [column for _, column in NOZBACK], False)


if __name__ == "__main__":
    main()
