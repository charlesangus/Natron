#!/usr/bin/env python3
# Regenerates the flat multi-layer EXR fixtures the layer/channel tests read:
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
# Also writes flat-no-color-layers.exr: same size, same diffuse/specular values, but no R/G/B/A
# channels at all, used to exercise ReadOIIO's no-Color-plane fallback (the first layer is
# duplicated into Color while remaining present as its own plane).
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

THREE_LAYERS_CHANNELS = ["R", "G", "B", "A",
                          "diffuse.R", "diffuse.G", "diffuse.B",
                          "specular.R", "specular.G", "specular.B"]
THREE_LAYERS_VALUES = [1.0, 0.0, 0.0, 1.0,
                        0.0, 1.0, 0.0,
                        0.0, 0.0, 1.0]

NO_COLOR_LAYERS_CHANNELS = ["diffuse.R", "diffuse.G", "diffuse.B",
                             "specular.R", "specular.G", "specular.B"]
NO_COLOR_LAYERS_VALUES = [0.0, 1.0, 0.0,
                           0.0, 0.0, 1.0]


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
    write_fixture("%s/flat-three-layers.exr" % outdir, THREE_LAYERS_CHANNELS, THREE_LAYERS_VALUES)
    write_fixture("%s/flat-no-color-layers.exr" % outdir, NO_COLOR_LAYERS_CHANNELS, NO_COLOR_LAYERS_VALUES)


if __name__ == "__main__":
    main()
