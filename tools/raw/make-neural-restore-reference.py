#!/usr/bin/env python3
"""Crops darktable's neural restore of the Canon fixture into a RawNeuralDenoise reference.

darktable-cli cannot run neural restore: it is a lighttable utility (src/libs/neural_restore.c)
that writes a re-mosaiced CFA DNG next to the image. So the DNG is made by hand in darktable
release-5.6.2 with RawNIND installed (Preferences > AI), and this script takes the reference from
it. Usage:

    make-neural-restore-reference.py <darktable DNG> <output TIFF>

The DNG is canon-eos-5d-mark-iii_raw-denoise.dng, from Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2
processed by the neural restore module, raw denoise, strength 100 %. The script keeps the
512 x 512 photosites at (3072, 1728) from the top-left of the DNG's ActiveArea, normalised as
(value - black) / (white - black) with the 2x2 BlackLevel indexed from the ActiveArea, and writes
them as a 32-bit float TIFF whose three channels all hold the mosaic.
Tests/Native/NativeRawNeuralDenoise_Test.cpp repeats the size and the offset.
"""

import argparse
import sys

import numpy as np
import tifffile

CROP_SIZE = 512
CROP_LEFT = 3072
CROP_TOP = 1728

PHOTOMETRIC_CFA = 32803
TAG_ACTIVE_AREA = 50829
TAG_BLACK_LEVEL = 50714
TAG_BLACK_LEVEL_REPEAT_DIM = 50713
TAG_WHITE_LEVEL = 50717


def cfa_page(tif):
    pages = []
    for page in tif.pages:
        pages.append(page)
        pages.extend(page.pages if page.pages else [])
    for page in pages:
        if int(page.photometric) == PHOTOMETRIC_CFA:
            return page
    sys.exit("no CFA image in the DNG")


def tag(page, code, default=None):
    t = page.tags.get(code)
    return default if t is None else t.value


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dng")
    parser.add_argument("output")
    args = parser.parse_args()

    with tifffile.TiffFile(args.dng) as tif:
        page = cfa_page(tif)
        mosaic = page.asarray().astype(np.float64)
        top, left, bottom, right = tag(page, TAG_ACTIVE_AREA, (0, 0, mosaic.shape[0], mosaic.shape[1]))
        black = np.asarray(tag(page, TAG_BLACK_LEVEL, (0.,)), dtype=np.float64)
        repeat = tuple(tag(page, TAG_BLACK_LEVEL_REPEAT_DIM, (1, 1)))
        white = float(np.asarray(tag(page, TAG_WHITE_LEVEL, (65535,))).ravel()[0])

    visible = mosaic[top:bottom, left:right]
    print("visible %d x %d, black %s, white %g" % (visible.shape[1], visible.shape[0], black.tolist(), white))
    if (CROP_TOP + CROP_SIZE > visible.shape[0]) or (CROP_LEFT + CROP_SIZE > visible.shape[1]):
        sys.exit("the crop does not fit the visible image")
    black = black.reshape(repeat) if black.size == repeat[0] * repeat[1] else np.full((1, 1), black.ravel()[0])
    rows = np.arange(visible.shape[0]) % black.shape[0]
    cols = np.arange(visible.shape[1]) % black.shape[1]
    black_map = black[rows[:, None], cols[None, :]]
    normalised = (visible - black_map) / (white - black_map)
    crop = normalised[CROP_TOP:CROP_TOP + CROP_SIZE, CROP_LEFT:CROP_LEFT + CROP_SIZE].astype(np.float32)
    tifffile.imwrite(args.output, np.repeat(crop[:, :, None], 3, axis=2), photometric="rgb")
    return 0


if __name__ == "__main__":
    sys.exit(main())
