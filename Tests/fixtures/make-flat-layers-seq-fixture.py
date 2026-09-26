#!/usr/bin/env python3
# Regenerates the flat-seq-layers EXR sequence the layers-vary-with-time tests read:
#
#   python3 Tests/fixtures/make-flat-layers-seq-fixture.py Tests/fixtures
#
# Writes a 2-frame, 8x8, single-part, scanline, half-float EXR sequence whose channel set
# changes between frames:
#
#   flat-seq-layers.0001.exr  R, G, B, A and the diffuse/specular layers, same values as
#                              flat-three-layers.exr (see make-flat-layers-fixture.py), so
#                              existing pixel assertions carry over
#   flat-seq-layers.0002.exr  R, G, B, A only; diffuse and specular are dropped
#
#   R, G, B, A                          = 1, 0, 0, 1  (the default/RGBA layer: opaque red)
#   diffuse.R, diffuse.G, diffuse.B     = 0, 1, 0      (opaque green, frame 1 only)
#   specular.R, specular.G, specular.B  = 0, 0, 1      (opaque blue, frame 1 only)
#
# Half-float with zip compression: these fixtures are only ever read by ReadOIIO, so they are not
# constrained by Tests/FlatExrReader.h, which parses only the uncompressed 32-bit-float layout
# WriteOIIO itself produces.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1
# Python module.
import os
import sys

import numpy
import OpenImageIO as oiio

WIDTH = 8
HEIGHT = 8

FRAME_1_CHANNELS = ["R", "G", "B", "A",
                     "diffuse.R", "diffuse.G", "diffuse.B",
                     "specular.R", "specular.G", "specular.B"]
FRAME_1_VALUES = [1.0, 0.0, 0.0, 1.0,
                   0.0, 1.0, 0.0,
                   0.0, 0.0, 1.0]

FRAME_2_CHANNELS = ["R", "G", "B", "A"]
FRAME_2_VALUES = [1.0, 0.0, 0.0, 1.0]


def write_fixture(path, channels, values):
    spec = oiio.ImageSpec(WIDTH, HEIGHT, len(channels), "half")
    spec.channelnames = channels
    spec.attribute("compression", "zip")
    # The EXR writer stamps a DateTime attribute with the current time unless the spec already
    # has one; fix it so regenerating the file is byte-identical.
    spec.attribute("DateTime", "2000:01:01 00:00:00")

    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())

    pixels = numpy.tile(numpy.array(values, dtype=numpy.float16),
                         (HEIGHT, WIDTH, 1))
    if not out.write_image(pixels):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    write_fixture("%s/flat-seq-layers.0001.exr" % outdir, FRAME_1_CHANNELS, FRAME_1_VALUES)
    write_fixture("%s/flat-seq-layers.0002.exr" % outdir, FRAME_2_CHANNELS, FRAME_2_VALUES)


if __name__ == "__main__":
    main()
