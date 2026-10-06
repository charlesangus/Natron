#!/usr/bin/env python3
# Regenerates the PNG fixtures ProjectOCIODefaults_Test.cpp reads:
#
#   python3 Tests/fixtures/make-png-depth-fixtures.py Tests/fixtures
#
# png-8bit.png and png-16bit.png: 8x8 RGB files holding the same constant mid-gray, so a Read
# of either differs from the other only in the file's bit depth, which is what picks the
# project's per-file-type default colorspace.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1
# Python module.
import os
import sys

import numpy
import OpenImageIO as oiio

SIZE = 8


def write(path, typedesc, dtype, value):
    spec = oiio.ImageSpec(SIZE, SIZE, 3, typedesc)
    spec.channelnames = ["R", "G", "B"]

    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())
    pixels = numpy.full((SIZE, SIZE, 3), value, dtype=dtype)
    if not out.write_image(pixels):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

    write("%s/png-8bit.png" % outdir, "uint8", numpy.uint8, 128)
    write("%s/png-16bit.png" % outdir, "uint16", numpy.uint16, 32896)


if __name__ == "__main__":
    main()
