#!/usr/bin/env python3
# Regenerates zdefocus-depth.exr, the CG frame with a depth pass the ZDefocus GUI check reads:
#
#   python3 Tests/fixtures/make-zdefocus-depth-fixture.py Tests/fixtures
#
# A 256x144 ray-traced frame from a pinhole camera at the origin looking down +z: a checkered
# ground plane receding from distance 1 to the horizon, three spheres at distances 3, 6 and 12,
# and a sky. R, G, B, A are half float; depth.Z is float and holds the inverse distance, as most
# renderers write it, with 0 in the sky (infinitely far).
# Run it inside the dev container (tools/ci/local/devshell.sh), which ships the OpenImageIO 3.1
# Python module.
import os
import sys

import numpy
import OpenImageIO as oiio

WIDTH = 256
HEIGHT = 144
# Half the horizontal field of view's tangent.
TAN_HALF_FOV = 0.6
GROUND_Y = -1.0
SPHERES = [
    # Centre, radius, colour.
    ((-1.2, -0.4, 3.0), 0.6, (0.9, 0.2, 0.15)),
    ((0.6, -0.3, 6.0), 0.7, (0.2, 0.7, 0.25)),
    ((3.0, 0.0, 12.0), 1.0, (0.2, 0.35, 0.9)),
]
LIGHT = numpy.array([-0.5, 0.8, -0.4])
LIGHT = LIGHT / numpy.linalg.norm(LIGHT)
CHANNELS = ["R", "G", "B", "A", "depth.Z"]


def render():
    ys, xs = numpy.mgrid[0:HEIGHT, 0:WIDTH].astype(numpy.float64)
    aspect = WIDTH / HEIGHT
    u = ((xs + 0.5) / WIDTH * 2.0 - 1.0) * TAN_HALF_FOV
    v = (1.0 - (ys + 0.5) / HEIGHT * 2.0) * TAN_HALF_FOV / aspect
    dirs = numpy.stack([u, v, numpy.ones_like(u)], axis=-1)
    dirs /= numpy.linalg.norm(dirs, axis=-1, keepdims=True)

    hit_t = numpy.full((HEIGHT, WIDTH), numpy.inf)
    colour = numpy.zeros((HEIGHT, WIDTH, 3))
    # Sky: brighter towards the horizon.
    sky = numpy.clip(1.0 - dirs[..., 1] * 2.0, 0.0, 1.0)[..., None]
    colour[:] = numpy.array([0.25, 0.4, 0.75]) * (0.6 + 0.4 * sky)

    # Ground: a checkerboard of unit squares.
    with numpy.errstate(divide="ignore", invalid="ignore"):
        t = GROUND_Y / dirs[..., 1]
    ground = (dirs[..., 1] < 0.0) & (t > 0.0)
    px = dirs[..., 0] * t
    pz = dirs[..., 2] * t
    check = (numpy.floor(px) + numpy.floor(pz)) % 2.0
    ground_colour = numpy.where(check[..., None] > 0.5, [0.8, 0.8, 0.75], [0.15, 0.15, 0.15]) * (0.4 + 0.6 * LIGHT[1])
    colour = numpy.where(ground[..., None], ground_colour, colour)
    hit_t = numpy.where(ground, t, hit_t)

    for centre, radius, albedo in SPHERES:
        c = numpy.array(centre)
        b = dirs @ c
        disc = b * b - (c @ c - radius * radius)
        with numpy.errstate(invalid="ignore"):
            t = b - numpy.sqrt(disc)
        hit = (disc > 0.0) & (t > 0.0) & (t < hit_t)
        normal = dirs * t[..., None] - c
        normal /= radius
        shade = 0.15 + 0.85 * numpy.clip(normal @ LIGHT, 0.0, 1.0)
        # A specular highlight, bright enough to show the bokeh.
        half = LIGHT - dirs
        half /= numpy.linalg.norm(half, axis=-1, keepdims=True)
        spec = 6.0 * numpy.clip(numpy.sum(normal * half, axis=-1), 0.0, 1.0) ** 60
        sphere_colour = numpy.array(albedo) * shade[..., None] + spec[..., None]
        colour = numpy.where(hit[..., None], sphere_colour, colour)
        hit_t = numpy.where(hit, t, hit_t)

    depth = numpy.where(numpy.isfinite(hit_t), 1.0 / hit_t, 0.0)
    return colour, depth


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(outdir, "zdefocus-depth.exr")
    colour, depth = render()

    spec = oiio.ImageSpec(WIDTH, HEIGHT, len(CHANNELS), "float")
    spec.channelnames = CHANNELS
    spec.channelformats = (oiio.HALF, oiio.HALF, oiio.HALF, oiio.HALF, oiio.FLOAT)
    spec.attribute("compression", "zip")
    # The EXR writer stamps a DateTime attribute with the current time unless the spec already
    # has one; fix it so regenerating the file is byte-identical.
    spec.attribute("DateTime", "2000:01:01 00:00:00")

    pixels = numpy.zeros((HEIGHT, WIDTH, len(CHANNELS)), dtype=numpy.float32)
    pixels[..., 0:3] = colour
    pixels[..., 3] = 1.0
    pixels[..., 4] = depth

    out = oiio.ImageOutput.create(path)
    if out is None:
        raise RuntimeError(oiio.geterror())
    if not out.open(path, spec):
        raise RuntimeError(out.geterror())
    if not out.write_image(pixels):
        raise RuntimeError(out.geterror())
    out.close()
    print("wrote %s" % path)


if __name__ == "__main__":
    main()
