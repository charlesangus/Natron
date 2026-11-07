#!/usr/bin/env python3
# Regenerates the texture fixtures USDRenderMoonRay_Test.cpp reads:
#
#   python3 Tests/fixtures/make-moonray-texture-fixture.py Tests/fixtures
#
# moonray-texture.jpg: a 256x256 8-bit image, a two-texel checker on the left and a colour ramp on
# the right. The checker averages out only through MIP levels, so a render that reads the top level
# alone differs from a mapped one.
# moonray-texture.png: the JPEG's decoded pixels, so that it and the JPEG are the same image.
# moonray-texture.tx: maketx's tiled, MIP-mapped conversion of the PNG.
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships maketx and the
# OpenImageIO 3.1 Python module.
import os
import subprocess
import sys

import numpy
import OpenImageIO as oiio

SIZE = 256


def write(path, pixels, attributes=()):
    spec = oiio.ImageSpec(SIZE, SIZE, 3, oiio.UINT8)
    for name, value in attributes:
        spec.attribute(name, value)
    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())
    if not out.write_image(pixels):
        raise RuntimeError(out.geterror())
    out.close()


def main(directory):
    y, x = numpy.mgrid[0:SIZE, 0:SIZE]
    checker = (((x // 2) + (y // 2)) % 2).astype(numpy.float32)
    ramp = numpy.stack([x / (SIZE - 1), y / (SIZE - 1), 1 - x / (SIZE - 1)], axis=-1)
    left = numpy.stack([checker * 0.9 + 0.05] * 3, axis=-1)
    image = numpy.where((x < SIZE // 2)[..., None], left, ramp)
    pixels = numpy.round(image * 255).astype(numpy.uint8)

    jpg = os.path.join(directory, "moonray-texture.jpg")
    png = os.path.join(directory, "moonray-texture.png")
    tx = os.path.join(directory, "moonray-texture.tx")
    write(jpg, pixels, [("Compression", "jpeg:95")])
    decoded = oiio.ImageInput.open(jpg)
    if decoded is None:
        raise RuntimeError(oiio.geterror())
    write(png, decoded.read_image(format=oiio.UINT8))
    decoded.close()
    subprocess.run(["maketx", png, "-o", tx], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main(sys.argv[1])
