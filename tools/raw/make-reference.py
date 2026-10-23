#!/usr/bin/env python3
"""Renders the darktable references of Tests/fixtures/raw/reference with darktable-cli.

Runs inside the image tools/raw/make-reference.sh builds. Every render goes through only the
modules the raw nodes port: rawprepare, temperature (as shot), highlights, demosaic and colorin,
then colorout to linear Rec. 709 and a 32-bit float TIFF. Usage:

    make-reference.py <fixtures dir> <output dir> <scratch dir>
"""

import os
import struct
import subprocess
import sys

import numpy as np
import tifffile

# Sizes and offsets the tests repeat: Tests/Native/RawReference_Test.cpp and
# Tests/Native/NativeRawHighlights_Test.cpp.
CROP_SIZE = 192
DEVELOP_JOBS = [
    # (fixture, crop left, crop top from the top-left of the developed image, method, output)
    ("canon-eos-5d-mark-iii.cr2", 3072, 1728, "ppg", "canon-eos-5d-mark-iii-ppg.tif"),
    ("canon-eos-5d-mark-iii.cr2", 3072, 1728, "rcd", "canon-eos-5d-mark-iii-rcd.tif"),
    ("canon-eos-5d-mark-iii.cr2", 3072, 1728, "amaze", "canon-eos-5d-mark-iii-amaze.tif"),
    ("fujifilm-x-pro2-14bit-uncompressed.raf", 3264, 3072, "markesteijn", "fujifilm-x-pro2-markesteijn.tif"),
    ("fujifilm-x-pro2-14bit-uncompressed.raf", 3264, 3072, "markesteijn3", "fujifilm-x-pro2-markesteijn3.tif"),
]

SYNTH_WIDTH = 48
SYNTH_HEIGHT = 42
SYNTH_BLACK = 256
SYNTH_WHITE = 4095
# White balance multipliers the synthetic mosaics are shot with: AsShotNeutral is their inverse.
SYNTH_COEFFS = (2.0, 1.0, 1.5)
BAYER_PATTERN = "RGGB"
XTRANS_PATTERN = ("GGRGGB"
                  "GGBGGR"
                  "BRGRBG"
                  "GGBGGR"
                  "GGRGGB"
                  "RBGBRG")
HIGHLIGHT_MODES = ["clip", "unclipped", "opposed", "lch"]

DT_DEMOSAIC_XTRANS = 1024
DEMOSAIC_METHODS = {
    "ppg": 0,
    "amaze": 1,
    "rcd": 5,
    "passthrough-colour": 4,
    "markesteijn": DT_DEMOSAIC_XTRANS | 1,
    "markesteijn3": DT_DEMOSAIC_XTRANS | 2,
    "passthrough-colour-xtrans": DT_DEMOSAIC_XTRANS | 5,
}
HIGHLIGHTS_MODES = {"clip": 0, "lch": 1, "opposed": 5}

DT_COLORSPACE_LIN_REC709 = 3
DT_COLORSPACE_STANDARD_MATRIX = 11
ICC_LEN = 512

BLEND_PARAMS = "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ="


def demosaic_params(method):
    # dt_iop_demosaic_params_t, version 6: no green equilibration, no colour smoothing, no
    # dual demosaic and no capture sharpening.
    return struct.pack("<ifiiiffffifi", 0, 0.0, 0, DEMOSAIC_METHODS[method], 1, 0.2, 0.0, 0.4, 0.0, 8, 0.0, 0)


def highlights_params(mode):
    # dt_iop_highlights_params_t, version 4, at its defaults but for the method.
    return struct.pack("<ifffffiiffif", HIGHLIGHTS_MODES.get(mode, 0), 1.0, 0.0, 0.0, 1.0, 0.0, 30, 6, 0.4, 2.0, 0, 0.0)


def colorin_params(input_profile):
    # dt_iop_colorin_params_t, version 7: perceptual intent (a matrix profile ignores it), no
    # gamut clipping, no blue mapping, linear Rec. 709 working space.
    return struct.pack("<i%dsiiii%ds" % (ICC_LEN, ICC_LEN), input_profile, b"", 0, 0, 0, DT_COLORSPACE_LIN_REC709, b"")


def colorout_params():
    # dt_iop_colorout_params_t, version 5: linear Rec. 709.
    return struct.pack("<i%dsi" % ICC_LEN, DT_COLORSPACE_LIN_REC709, b"", 0)


def flip_params():
    # dt_iop_flip_params_t, version 2: ORIENTATION_NONE.
    return struct.pack("<i", 0)


def history_entry(num, operation, enabled, version, params):
    return ("     <rdf:li\n"
            "      darktable:num=\"%d\"\n"
            "      darktable:operation=\"%s\"\n"
            "      darktable:enabled=\"%d\"\n"
            "      darktable:modversion=\"%d\"\n"
            "      darktable:params=\"%s\"\n"
            "      darktable:multi_name=\"\"\n"
            "      darktable:multi_priority=\"0\"\n"
            "      darktable:blendop_version=\"10\"\n"
            "      darktable:blendop_params=\"%s\"/>\n") % (num, operation, enabled, version, params.hex(), BLEND_PARAMS)


def write_xmp(path, demosaic, highlights, input_profile):
    """A sidecar whose history sets only what differs from darktable's per-image defaults.

    rawprepare and temperature stay out of it, so darktable fills them from the file as it does
    on import: black and white levels, the sensor crop and the as-shot white balance.
    auto_presets_applied keeps the workflow's automatic presets (exposure, filmic or sigmoid,
    colour calibration, sharpen) away.
    """
    entries = [
        ("flip", 0, 2, flip_params()),
        ("highlights", 0 if highlights == "unclipped" else 1, 4, highlights_params(highlights)),
        ("demosaic", 1, 6, demosaic_params(demosaic)),
        ("colorin", 1, 7, colorin_params(input_profile)),
        ("colorout", 1, 5, colorout_params()),
    ]
    history = "".join(history_entry(i, *e) for i, e in enumerate(entries))
    xmp = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 4.4.0-Exiv2\">\n"
           " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
           "  <rdf:Description rdf:about=\"\"\n"
           "    xmlns:darktable=\"http://darktable.sf.net/\"\n"
           "   darktable:xmp_version=\"4\"\n"
           "   darktable:raw_params=\"0\"\n"
           "   darktable:auto_presets_applied=\"1\"\n"
           "   darktable:history_end=\"%d\"\n"
           "   darktable:iop_order_version=\"2\">\n"
           "   <darktable:masks_history>\n"
           "    <rdf:Seq/>\n"
           "   </darktable:masks_history>\n"
           "   <darktable:history>\n"
           "    <rdf:Seq>\n"
           "%s"
           "    </rdf:Seq>\n"
           "   </darktable:history>\n"
           "  </rdf:Description>\n"
           " </rdf:RDF>\n"
           "</x:xmpmeta>\n") % (len(entries), history)
    with open(path, "w") as f:
        f.write(xmp)


def darktable_cli(source, xmp, output, scratch):
    if os.path.exists(output):
        os.remove(output)
    config = os.path.join(scratch, "config")
    os.makedirs(config, exist_ok=True)
    command = [
        "darktable-cli", source, xmp, output,
        "--hq", "true", "--apply-custom-presets", "false", "--icc-type", "LIN_REC709",
        "--core", "--configdir", config, "--disable-opencl",
        "--conf", "plugins/darkroom/workflow=none",
        "--conf", "plugins/imageio/format/tiff/bpp=32",
        "--conf", "plugins/imageio/format/tiff/pixelformat=FALSE",
        "--conf", "plugins/imageio/format/tiff/compress=0",
        "--conf", "plugins/lighttable/export/force_lcms2=FALSE",
        "--conf", "plugins/lighttable/export/iccintent=0",
        "--conf", "write_sidecar_files=never",
        "--conf", "host_memory_limit=4096",
        "--conf", "worker_threads=4", "-t", "4",
    ]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    image = tifffile.imread(output)
    if image.dtype != np.float32 or image.ndim != 3 or image.shape[2] < 3:
        raise RuntimeError("%s: expected float RGB, got %s %s" % (output, image.dtype, image.shape))
    return image[:, :, :3]


def write_reference(path, image, description):
    tifffile.imwrite(path, np.ascontiguousarray(image, dtype=np.float32), photometric="rgb" if image.ndim == 3 else "minisblack",
                     compression="zlib", description=description, metadata=None)


def develop(out, scratch):
    for fixture, left, top, method, name in DEVELOP_JOBS:
        xmp = os.path.join(scratch, "develop-%s.xmp" % method)
        write_xmp(xmp, method, "opposed", DT_COLORSPACE_STANDARD_MATRIX)
        image = darktable_cli(os.path.join(scratch, fixture), xmp, os.path.join(scratch, "full.tif"), scratch)
        height, width = image.shape[:2]
        crop = image[top:top + CROP_SIZE, left:left + CROP_SIZE, :]
        description = "darktable %s, %s, demosaic %s, developed %dx%d, crop left %d top %d size %d, linear Rec. 709" % (
            os.environ.get("DARKTABLE_REF", "?"), fixture, method, width, height, left, top, CROP_SIZE)
        write_reference(os.path.join(out, name), crop, description)
        print("%s: developed %dx%d, crop mean %s max %s" % (name, width, height, crop.mean(axis=(0, 1)), crop.max()))


def colour_index(pattern, period, x, y):
    return "RGB".index(pattern[(y % period) * period + (x % period)])


def synthetic_scene(pattern, period):
    """Pre-white-balance photosite values in [0, 1] of the white level, top row first.

    A ramp that turns neutral grey once white balanced, a disc where every photosite clipped and
    a rim around it where only green did.
    """
    values = np.zeros((SYNTH_HEIGHT, SYNTH_WIDTH), dtype=np.float64)
    for y in range(SYNTH_HEIGHT):
        for x in range(SYNTH_WIDTH):
            c = colour_index(pattern, period, x, y)
            d = np.hypot(x - 24.0, y - 20.0)
            v = (0.3 + 0.2 * x / SYNTH_WIDTH) / SYNTH_COEFFS[c] * 1.4
            if d < 8.0:
                v = 1.0
            elif d < 11.0 and c == 1:
                v = 1.0
            values[y, x] = min(v, 1.0)
    return values


def write_cfa_dng(path, pattern, period):
    scene = synthetic_scene(pattern, period)
    data = np.round(SYNTH_BLACK + scene * (SYNTH_WHITE - SYNTH_BLACK)).astype(np.uint16)
    neutral = [(1, 1)] * 3
    for c in range(3):
        neutral[c] = (int(round(1000000 / SYNTH_COEFFS[c])), 1000000)
    # XYZ to linear Rec. 709, the matrix a camera whose RGB is linear Rec. 709 would carry.
    xyz_to_cam = [3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252]
    srational = []
    for v in xyz_to_cam:
        srational += [int(round(v * 10000)), 10000]
    rational = []
    for n, d in neutral:
        rational += [n, d]
    cfa = bytes("RGB".index(ch) for ch in pattern)
    extratags = [
        (271, "s", 0, "Natron", True),
        (272, "s", 0, "Synthetic CFA", True),
        (33421, "H", 2, (period, period), True),
        (33422, "B", len(cfa), cfa, True),
        (50706, "B", 4, (1, 4, 0, 0), True),
        (50707, "B", 4, (1, 1, 0, 0), True),
        (50708, "s", 0, "Natron Synthetic CFA", True),
        (50710, "B", 3, (0, 1, 2), True),
        (50711, "H", 1, 1, True),
        (50713, "H", 2, (1, 1), True),
        (50714, "H", 1, SYNTH_BLACK, True),
        (50717, "H", 1, SYNTH_WHITE, True),
        (50721, 10, 9, srational, True),
        (50728, 5, 3, rational, True),
        (50778, "H", 1, 21, True),
    ]
    tifffile.imwrite(path, data, photometric=32803, subfiletype=0, extratags=extratags, metadata=None)


def highlights(out, scratch):
    for name, pattern, period, passthrough in (("bayer", BAYER_PATTERN, 2, "passthrough-colour"),
                                               ("xtrans", XTRANS_PATTERN, 6, "passthrough-colour-xtrans")):
        dng = os.path.join(out, "highlights-%s.dng" % name)
        write_cfa_dng(dng, pattern, period)
        source = os.path.join(scratch, os.path.basename(dng))
        with open(dng, "rb") as src, open(source, "wb") as dst:
            dst.write(src.read())
        for mode in HIGHLIGHT_MODES:
            xmp = os.path.join(scratch, "highlights-%s.xmp" % mode)
            write_xmp(xmp, passthrough, mode, DT_COLORSPACE_LIN_REC709)
            image = darktable_cli(source, xmp, os.path.join(scratch, "highlights.tif"), scratch)
            if image.shape[:2] != (SYNTH_HEIGHT, SYNTH_WIDTH):
                raise RuntimeError("highlights %s %s: developed %s" % (name, mode, image.shape))
            mosaic = np.zeros((SYNTH_HEIGHT, SYNTH_WIDTH), dtype=np.float32)
            leak = 0.0
            for y in range(SYNTH_HEIGHT):
                for x in range(SYNTH_WIDTH):
                    c = colour_index(pattern, period, x, y)
                    mosaic[y, x] = image[y, x, c]
                    leak = max(leak, float(np.abs(np.delete(image[y, x], c)).max()))
            description = "darktable %s, highlights %s on highlights-%s.dng, photosite colour, white-balanced mosaic" % (
                os.environ.get("DARKTABLE_REF", "?"), mode, name)
            write_reference(os.path.join(out, "highlights-%s-%s.tif" % (name, mode)), mosaic, description)
            print("highlights-%s-%s: max %g, largest other-channel value %g" % (name, mode, mosaic.max(), leak))


def main():
    fixtures, out, scratch = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    for fixture in sorted({job[0] for job in DEVELOP_JOBS}):
        with open(os.path.join(fixtures, fixture), "rb") as src, open(os.path.join(scratch, fixture), "wb") as dst:
            dst.write(src.read())
    highlights(out, scratch)
    develop(out, scratch)


if __name__ == "__main__":
    main()
