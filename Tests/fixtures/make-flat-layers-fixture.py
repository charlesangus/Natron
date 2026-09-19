#!/usr/bin/env python3
# Regenerates the flat multi-layer EXR fixture WriteAllLayers_Test.cpp reads:
#
#   python3 Tests/fixtures/make-flat-layers-fixture.py Tests/fixtures
#
# Writes flat-three-layers.exr: an 8x8, single-part, scanline, half-float EXR carrying three
# layers of solid color, used as the ReadOIIO source for a Write node's "All Layers" test. Every
# pixel holds the same values:
#
#   R, G, B, A                          = 1, 0, 0, 1  (the default/RGBA layer: opaque red)
#   diffuse.R, diffuse.G, diffuse.B     = 0, 1, 0      (opaque green)
#   specular.R, specular.G, specular.B  = 0, 0, 1      (opaque blue)
#
# Half-float with zip compression: this fixture is only ever read by ReadOIIO, so it is not
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
CHANNELS = ["R", "G", "B", "A",
            "diffuse.R", "diffuse.G", "diffuse.B",
            "specular.R", "specular.G", "specular.B"]
VALUES = [1.0, 0.0, 0.0, 1.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    path = "%s/flat-three-layers.exr" % outdir

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
