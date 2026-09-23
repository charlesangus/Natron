#!/usr/bin/env python3
# Regenerates the moving-patch EXR sequences TrackerLayer_Test.cpp tracks:
#
#   python3 Tests/fixtures/make-tracker-fixture.py Tests/fixtures
#
# Writes two 3-frame, 128x128, single-part, scanline, half-float EXR sequences. Each frame is a
# flat mid-grey background with one 15x15 patch of fixed pseudo-random grey texture, which moves
# by (+6, +4) pixels (image x right, y up, Natron's orientation) from one frame to the next:
#
#   frame 1: centre (40, 48)    frame 2: centre (46, 52)    frame 3: centre (52, 56)
#
#   tracker-patch.####.exr          R,G,B (A = 1) and diffuse.R/G/B all carry the patch
#   tracker-patch-diffuse.####.exr  R,G,B (A = 1) are flat grey; only diffuse.R/G/B carry it
#
# The texture is grey (R = G = B), so a luminance-weighted and a plain channel mean give the
# same libmv image and the two sequences must track to the same centres.
#
# Half-float with zip compression: these fixtures are only ever read by ReadOIIO. Run it inside
# the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1 Python module.
import os
import sys

import numpy
import OpenImageIO as oiio

WIDTH = 128
HEIGHT = 128
PATCH = 15
BACKGROUND = 0.3
FRAMES = 3
CENTRE_AT_FRAME_1 = (40, 48)
STEP = (6, 4)

CHANNELS = ["R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B"]


def patch_texture():
    # The legacy RandomState stream is frozen by numpy's compatibility policy, so the fixture is
    # byte-identical across regenerations.
    rng = numpy.random.RandomState(20260921)
    return 0.1 + 0.85 * rng.rand(PATCH, PATCH)


def plane_for_frame(frame, texture, with_patch):
    plane = numpy.full((HEIGHT, WIDTH), BACKGROUND, dtype=numpy.float32)
    if not with_patch:
        return plane
    cx = CENTRE_AT_FRAME_1[0] + STEP[0] * (frame - 1)
    cy = CENTRE_AT_FRAME_1[1] + STEP[1] * (frame - 1)
    half = PATCH // 2
    # Row 0 of the file is the top scanline; a Natron y of cy sits on file row HEIGHT - 1 - cy.
    x0 = cx - half
    y0 = HEIGHT - 1 - (cy + half)
    plane[y0:y0 + PATCH, x0:x0 + PATCH] = texture
    return plane


def write_frame(path, frame, texture, color_has_patch):
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

    color = plane_for_frame(frame, texture, color_has_patch)
    diffuse = plane_for_frame(frame, texture, True)
    alpha = numpy.ones((HEIGHT, WIDTH), dtype=numpy.float32)
    pixels = numpy.stack([color, color, color, alpha, diffuse, diffuse, diffuse], axis=-1)
    if not out.write_image(pixels.astype(numpy.float16)):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    texture = patch_texture()
    for frame in range(1, FRAMES + 1):
        write_frame("%s/tracker-patch.%04d.exr" % (outdir, frame), frame, texture, True)
        write_frame("%s/tracker-patch-diffuse.%04d.exr" % (outdir, frame), frame, texture, False)


if __name__ == "__main__":
    main()
