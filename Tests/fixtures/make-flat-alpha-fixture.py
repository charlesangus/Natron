#!/usr/bin/env python3
# Regenerates the alpha-only EXR fixture ColorViewsRender_Test.cpp reads:
#
#   python3 Tests/fixtures/make-flat-alpha-fixture.py Tests/fixtures
#
# Writes flat-alpha-only.exr: an 8x8, single-part, scanline, half-float EXR whose only channel is
# A (opaque, 1.0), so a Read of it presents Color as Alpha with no R/G/B, exercising the alpha
# colour view's zero-filled RGB.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1
# Python module.
import os
import sys

import numpy
import OpenImageIO as oiio

WIDTH = 8
HEIGHT = 8
CHANNELS = ["A"]
VALUES = [1.0]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    path = "%s/flat-alpha-only.exr" % outdir

    spec = oiio.ImageSpec(WIDTH, HEIGHT, len(CHANNELS), "half")
    spec.channelnames = CHANNELS
    spec.attribute("compression", "zip")
    # The EXR writer stamps a DateTime attribute with the current time unless the spec already
    # has one; fix it so regenerating the file is byte-identical.
    spec.attribute("DateTime", "2000:01:01 00:00:00")

    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())

    pixels = numpy.tile(numpy.array(VALUES, dtype=numpy.float16),
                         (HEIGHT, WIDTH, 1))
    if not out.write_image(pixels):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


if __name__ == "__main__":
    main()
