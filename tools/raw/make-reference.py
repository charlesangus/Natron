#!/usr/bin/env python3
"""Renders the darktable references of Tests/fixtures/raw/reference with darktable-cli.

Runs inside the image tools/raw/make-reference.sh builds. Each job writes a sidecar XMP whose
history packs the parameters of the darktable modules the raw nodes port, exports a 32-bit float
linear Rec. 709 TIFF and keeps either a crop of it (camera files) or its white-balanced mosaic
(synthetic DNGs), or, for the ashift jobs, the whole of a synthetic image's perspective correction.
Usage:

    make-reference.py [--dry-run] [--only TEXT] <fixtures dir> <output dir> <scratch dir>

--dry-run lists every job and writes every XMP to the scratch directory without running
darktable-cli or writing a fixture, with placeholder white balance coefficients in place of the
probed ones. --only keeps the jobs whose output name contains TEXT.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

try:
    import numpy as np
    import tifffile
except ImportError:
    # The dry run needs neither, so it runs outside the image.
    np = None
    tifffile = None

CR2 = "canon-eos-5d-mark-iii.cr2"
RAF = "fujifilm-x-pro2-14bit-uncompressed.raf"
DNG = "canon-eos-5d-mark-iii-linear-rgb.dng"
CAMERA_FIXTURES = (CR2, RAF, DNG)
PREFIX = {CR2: "canon-eos-5d-mark-iii", RAF: "fujifilm-x-pro2", DNG: "canon-eos-5d-mark-iii-linear-rgb"}
# Sizes and offsets the tests repeat: Tests/Native/RawReference_Test.cpp. The linear DNG is the
# CR2 scaled to 3960x2640, so its crop sits where the CR2's does.
CROP_SIZE = 192
CROP_ORIGIN = {CR2: (3072, 1728), RAF: (3264, 3072), DNG: (2100, 1180)}
# darktable's default demosaic for each file, which the jobs of the other modules use.
BASE_DEMOSAIC = {CR2: "rcd", RAF: "markesteijn", DNG: "rcd"}

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
# (x, y) photosites of the hot pixel mosaic set to the white level: isolated ones, one inside the
# 2-photosite border the module skips, and two same-colour neighbours only the permissive
# detection (3 neighbours) catches.
HOT_PIXELS = ((10, 10), (31, 7), (21, 30), (1, 1), (36, 20), (38, 20))

DT_DEMOSAIC_XTRANS = 1024
DT_DEMOSAIC_DUAL = 2048
DEMOSAIC_METHODS = {
    "ppg": 0,
    "amaze": 1,
    "vng4": 2,
    "passthrough-mono": 3,
    "passthrough-colour": 4,
    "rcd": 5,
    "lmmse": 6,
    "rcd-dual": DT_DEMOSAIC_DUAL | 5,
    "amaze-dual": DT_DEMOSAIC_DUAL | 1,
    "vng": DT_DEMOSAIC_XTRANS | 0,
    "markesteijn": DT_DEMOSAIC_XTRANS | 1,
    "markesteijn3": DT_DEMOSAIC_XTRANS | 2,
    "passthrough-mono-xtrans": DT_DEMOSAIC_XTRANS | 3,
    "fdc": DT_DEMOSAIC_XTRANS | 4,
    "passthrough-colour-xtrans": DT_DEMOSAIC_XTRANS | 5,
    "markesteijn3-dual": DT_DEMOSAIC_DUAL | DT_DEMOSAIC_XTRANS | 2,
}
HIGHLIGHTS_MODES = {"clip": 0, "lch": 1, "inpaint": 2, "laplacian": 3, "segments": 4, "opposed": 5}
RECOVERY_MODES = {"off": 0, "small": 1, "generic": 5}

DT_IOP_TEMP_AS_SHOT = 0
DT_IOP_TEMP_USER = 2
DT_IOP_TEMP_D65 = 3
DT_IOP_TEMP_D65_LATE = 4
USER_COEFFS = (2.2, 1.0, 1.6)

DT_COLORSPACE_LIN_REC709 = 3
DT_COLORSPACE_EMBEDDED_MATRIX = 10
DT_COLORSPACE_STANDARD_MATRIX = 11
DT_COLORSPACE_ENHANCED_MATRIX = 12
# colorin's generic input profiles (src/common/colorspaces.c, every profile given an in_pos).
GENERIC_INPUT_PROFILES = [
    ("srgb", 1), ("adobergb", 2), ("lin-rec709", 3), ("rec709", 20), ("lin-rec2020", 4),
    ("pq-rec2020", 22), ("hlg-rec2020", 23), ("pq-p3", 24), ("hlg-p3", 25), ("display-p3", 26),
    ("lin-prophoto", 21), ("xyz", 5), ("lab", 6), ("infrared", 7), ("brg", 15),
]
GAMUT_CLIPPING = [("srgb", 1), ("adobergb", 2), ("lin-rec709", 3), ("lin-rec2020", 4)]
ICC_LEN = 512

ILLUMINANTS = {"pipe": 0, "a": 1, "d": 2, "e": 3, "f": 4, "led": 5, "bb": 6, "custom": 7, "camera": 10}
FLUO = {"f2": 1, "f3": 2, "f11": 10}
LED = {"b1": 0, "b5": 4, "rgb1": 6}
ADAPTATIONS = {"bradford": 0, "cat16": 1, "full-bradford": 2, "xyz": 3, "none": 4}
IDENTITY_MIX = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
MIX = ((0.9, 0.2, -0.05), (0.1, 0.85, 0.1), (-0.05, 0.15, 1.1))

SCENE_REFERRED = "scene-referred (sigmoid)"
BLEND_PARAMS = "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ="

# Placeholder white balance for the dry run, which cannot probe the files.
PLACEHOLDER_COEFFS = {"as_shot": (2.0, 1.0, 1.5), "d65": (2.1, 1.0, 1.4)}
PROBE_LINE = re.compile(r"\[dt_iop_reload_defaults\].*D65 ([0-9.]+) ([0-9.]+) ([0-9.]+), "
                        r"AS-SHOT ([0-9.]+) ([0-9.]+) ([0-9.]+)")
AUTORADIUS_LINE = re.compile(r"(bayer|xtrans) autoradius")
COMMIT_CC_LINE = re.compile(r"\[commit color calibration\]")


def demosaic_params(method, green_eq=0, median_thrs=0.0, color_smoothing=0, lmmse_refine=1, dual_thrs=0.2,
                    cs_enabled=0, cs_radius=0.0, cs_thrs=0.4):
    # dt_iop_demosaic_params_t, version 6. Capture sharpening keeps its corner boost, iterations
    # and centre at their defaults.
    return struct.pack("<ifiiiffffifi", green_eq, median_thrs, color_smoothing, DEMOSAIC_METHODS[method],
                       lmmse_refine, dual_thrs, cs_radius, cs_thrs, 0.0, 8, 0.0, cs_enabled)


def highlights_params(mode, strength=0.0, clip=1.0, noise_level=0.0, iterations=30, recovery="off", solid_color=0.0):
    # dt_iop_highlights_params_t, version 4; blendL and blendC are unused, scales stays at 128 px,
    # candidating at 0.4 and combine at 2.
    return struct.pack("<ifffffiiffif", HIGHLIGHTS_MODES.get(mode, 0), 1.0, 0.0, strength, clip, noise_level,
                       iterations, 6, 0.4, 2.0, RECOVERY_MODES[recovery], solid_color)


def colorin_params(input_profile, normalize=0):
    # dt_iop_colorin_params_t, version 7: perceptual intent (a matrix profile ignores it), no blue
    # mapping, linear Rec. 709 working space.
    return struct.pack("<i%dsiiii%ds" % (ICC_LEN, ICC_LEN), input_profile, b"", 0, normalize, 0,
                       DT_COLORSPACE_LIN_REC709, b"")


def colorout_params():
    # dt_iop_colorout_params_t, version 5: linear Rec. 709.
    return struct.pack("<i%dsi" % ICC_LEN, DT_COLORSPACE_LIN_REC709, b"", 0)


def flip_params():
    # dt_iop_flip_params_t, version 2: ORIENTATION_NONE.
    return struct.pack("<i", 0)


def temperature_params(coeffs, preset):
    # dt_iop_temperature_params_t, version 4: red, green, blue, various (the fourth CFA colour,
    # unused on these files) and the preset.
    return struct.pack("<ffffi", coeffs[0], coeffs[1], coeffs[2], coeffs[1], preset)


def hotpixels_params(strength=0.25, threshold=0.05, permissive=0):
    # dt_iop_hotpixels_params_t, version 1, without marking the fixed photosites.
    return struct.pack("<ffii", strength, threshold, 0, permissive)


def cacorrect_params(iterations=2, avoidshift=0):
    # dt_iop_cacorrect_params_t, version 2.
    return struct.pack("<ii", avoidshift, iterations)


def rawdenoise_params(threshold, all_curve=(0.5,) * 5):
    # dt_iop_rawdenoise_params_t, version 2: x[4][5] then y[4][5] for all, R, G, B, the x
    # positions evenly spaced as darktable's defaults are.
    xs = [k / 4.0 for k in range(5)] * 4
    ys = list(all_curve) + [0.5] * 15
    return struct.pack("<f20f20f", threshold, *(xs + ys))


def rawprepare_params(black, white, flat_field):
    # dt_iop_rawprepare_params_t, version 2: no crop, the four black levels, the white point and
    # the flat field enum, which sits after two bytes of padding.
    return struct.pack("<4i4HH2xi", 0, 0, 0, 0, black, black, black, black, white, flat_field)


def channelmixerrgb_params(illuminant="d", adaptation="cat16", temperature=4500.0, x=0.333, y=0.333, fluo="f3",
                           led="b5", gamut=1.0, clip=1, version=3, mix=IDENTITY_MIX, normalize_mix=0,
                           saturation=(0.0, 0.0, 0.0), lightness=(0.0, 0.0, 0.0), grey=(0.0, 0.0, 0.0),
                           normalize_grey=0):
    # dt_iop_channelmixer_rgb_params_t, version 3: six float[4] rows (red, green, blue,
    # saturation, lightness, grey), six normalize gbooleans (R, G, B, saturation, lightness,
    # grey), illuminant, illum_fluo, illum_led, adaptation, x, y, temperature, gamut, clip and the
    # saturation algorithm, whose enum counts from 0 for version 1.
    rows = []
    for row in tuple(mix) + (saturation, lightness, grey):
        rows += list(row) + [0.0]
    normalize = [normalize_mix] * 3 + [0, 0, normalize_grey]
    return struct.pack("<24f6i4i4f2i", *rows, *normalize, ILLUMINANTS[illuminant], FLUO[fluo], LED[led],
                       ADAPTATIONS[adaptation], x, y, temperature, gamut, clip, version - 1)


def sigmoid_params():
    # dt_iop_sigmoid_params_t, version 3, at its defaults.
    return struct.pack("<4fi8fi", 1.5, 0.0, 100.0, 0.0152, 0, 100.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0)


def exposure_params():
    # dt_iop_exposure_params_t, version 7, at its defaults.
    return struct.pack("<i4fii", 0, 0.0, 0.0, 50.0, -4.0, 0, 1)


def sharpen_params():
    # dt_iop_sharpen_params_t, version 1, at its defaults.
    return struct.pack("<3f", 2.0, 0.5, 0.5)


# AutoStraighten's source: a synthetic linear Rec. 709 image the ashift jobs correct, written as
# a half-float TIFF that darktable and the test both read.
ASHIFT_SOURCE = "ashift-source.tif"
ASHIFT_WIDTH = 160
ASHIFT_HEIGHT = 120
ASHIFT_MAX_SAVED_LINES = 50
ASHIFT_MODES = {"generic": 0, "specific": 1}
# darktable's warp interpolator for the jobs; AutoStraighten's filter knob is set to match.
ASHIFT_INTERPOLATOR = "bilinear"
# (suffix, rotation, lensShiftV, lensShiftH, shear, lens model, focal length, crop factor, lens
# dependence, aspect adjust, crop box cl cr ct cb). Rotation is AutoStraighten's knob, the slider
# value; darktable stores its negation. Every crop box is explicit, as darktable-cli never runs the
# automatic crop the GUI computes; Tests/Native/NativeAutoStraighten_Test.cpp mirrors this table.
ASHIFT_CASES = [
    ("rotation", 3.0, 0.0, 0.0, 0.0, "generic", 28.0, 1.0, 100.0, 1.0, (0.0, 1.0, 0.0, 1.0)),
    ("keystone", 0.0, 0.3, 0.0, 0.0, "generic", 28.0, 1.0, 100.0, 1.0, (0.05, 0.95, 0.04, 0.96)),
    ("all", -2.0, 0.2, -0.15, 0.05, "generic", 28.0, 1.0, 100.0, 1.0, (0.08, 0.9, 0.06, 0.93)),
    ("specific", 1.5, -0.25, 0.1, 0.0, "specific", 50.0, 1.5, 60.0, 1.2, (0.0, 1.0, 0.0, 1.0)),
]


def ashift_params(rotation, lens_shift_v, lens_shift_h, shear, mode, focal_length, crop_factor, orthocorr, aspect,
                  crop):
    # dt_iop_ashift_params_t, version 5: rotation (stored negated), lens shifts, shear, focal
    # length, crop factor, orthocorr, aspect, mode, crop mode (off: the box is taken as given),
    # the crop box, then the saved lines and quad, empty.
    cl, cr, ct, cb = crop
    return struct.pack("<8f2i4f%dfi8f" % (ASHIFT_MAX_SAVED_LINES * 4),
                       -rotation, lens_shift_v, lens_shift_h, shear, focal_length, crop_factor, orthocorr, aspect,
                       ASHIFT_MODES[mode], 0, cl, cr, ct, cb, *([0.0] * (ASHIFT_MAX_SAVED_LINES * 4)), 0,
                       *([0.0] * 8))


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


def develop_entries(demosaic, highlights="opposed", input_profile=DT_COLORSPACE_STANDARD_MATRIX, normalize=0,
                    demosaic_options=None, highlights_options=None, extra=()):
    entries = [
        ("flip", 0, 2, flip_params()),
        ("highlights", 0 if highlights == "unclipped" else 1, 4, highlights_params(highlights, **(highlights_options or {}))),
        ("demosaic", 1, 6, demosaic_params(demosaic, **(demosaic_options or {}))),
        ("colorin", 1, 7, colorin_params(input_profile, normalize)),
        ("colorout", 1, 5, colorout_params()),
    ]
    return entries + list(extra)


def write_xmp(path, entries, auto_presets_applied=True):
    """A sidecar whose history sets only what differs from darktable's per-image defaults.

    Modules left out of it, rawprepare and temperature unless a job sets them, take darktable's
    defaults for the file as on import: black and white levels, the sensor crop and the as-shot
    white balance. auto_presets_applied keeps the workflow's automatic presets (exposure, filmic
    or sigmoid, colour calibration, sharpen) away; without it darktable puts them first in the
    history and this history's entries follow.
    """
    history = "".join(history_entry(i, *e) for i, e in enumerate(entries))
    xmp = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 4.4.0-Exiv2\">\n"
           " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
           "  <rdf:Description rdf:about=\"\"\n"
           "    xmlns:darktable=\"http://darktable.sf.net/\"\n"
           "   darktable:xmp_version=\"4\"\n"
           "   darktable:raw_params=\"0\"\n"
           "   darktable:auto_presets_applied=\"%d\"\n"
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
           "</x:xmpmeta>\n") % (1 if auto_presets_applied else 0, len(entries), history)
    with open(path, "w") as f:
        f.write(xmp)


class Job:
    """One reference: a crop of a camera file's develop, or a synthetic DNG's mosaic."""

    def __init__(self, name, source, entries, what, workflow="none", auto_presets_applied=True, debug=None,
                 log_line=None, mosaic=None, whole=False, conf=()):
        self.name = name
        self.source = source
        self.entries = entries
        self.what = what
        self.workflow = workflow
        self.auto_presets_applied = auto_presets_applied
        self.debug = debug
        self.log_line = log_line
        # (pattern, period) of a synthetic mosaic; None for a camera crop.
        self.mosaic = mosaic
        # Keep the whole render, as half float, instead of a crop.
        self.whole = whole
        # Extra darktable-cli --conf settings.
        self.conf = tuple(conf)


def camera_job(fixture, suffix, what, entries, **kwargs):
    return Job("%s-%s.tif" % (PREFIX[fixture], suffix), fixture, entries, what, **kwargs)


def temperature_entry(coeffs, preset):
    return ("temperature", 1, 4, temperature_params(coeffs, preset))


def develop_jobs():
    jobs = [camera_job(CR2, m, "demosaic %s" % m, develop_entries(m)) for m in ("ppg", "rcd", "amaze")]
    jobs += [camera_job(RAF, m, "demosaic %s" % m, develop_entries(m)) for m in ("markesteijn", "markesteijn3")]
    return jobs


def demosaic_jobs():
    bayer = [("vng4", "vng4", {}), ("ppg-median-0.3", "ppg", {"median_thrs": 0.3})]
    bayer += [("lmmse-refine-%d" % r, "lmmse", {"lmmse_refine": r}) for r in range(5)]
    bayer += [("%s-%s" % (m, t), m, {"dual_thrs": float(t)}) for m in ("rcd-dual", "amaze-dual") for t in ("0.2", "0.6")]
    bayer += [("rcd-greeneq-%s" % n, "rcd", {"green_eq": g}) for n, g in (("local", 1), ("full", 2), ("both", 3))]
    bayer += [("rcd-smoothing-%d" % s, "rcd", {"color_smoothing": s}) for s in (1, 5)]
    bayer += [("passthrough-mono", "passthrough-mono", {}), ("passthrough-colour", "passthrough-colour", {})]
    xtrans = [("vng", "vng", {}), ("fdc", "fdc", {}), ("markesteijn3-dual", "markesteijn3-dual", {}),
              ("passthrough-mono", "passthrough-mono-xtrans", {}), ("passthrough-colour", "passthrough-colour-xtrans", {})]
    jobs = []
    for fixture, variants in ((CR2, bayer), (RAF, xtrans)):
        for suffix, method, options in variants:
            what = "demosaic %s %s" % (method, " ".join("%s=%s" % kv for kv in sorted(options.items())))
            jobs.append(camera_job(fixture, suffix, what.strip(), develop_entries(method, demosaic_options=options)))
    return jobs


def capture_sharpen_jobs():
    jobs = []
    for fixture in (CR2, RAF):
        base = BASE_DEMOSAIC[fixture]
        fixed = {"cs_enabled": 1, "cs_radius": 0.7, "cs_thrs": 0.3}
        jobs.append(camera_job(fixture, "capture-sharpen-r0.7-t0.3", "demosaic %s, capture sharpening radius 0.7 "
                               "threshold 0.3" % base, develop_entries(base, demosaic_options=fixed)))
        auto = {"cs_enabled": 1, "cs_radius": 0.0, "cs_thrs": 0.3}
        jobs.append(camera_job(fixture, "capture-sharpen-auto-t0.3", "demosaic %s, capture sharpening automatic "
                               "radius, threshold 0.3" % base, develop_entries(base, demosaic_options=auto),
                               debug="pipe", log_line=AUTORADIUS_LINE))
    return jobs


def temperature_jobs(coeffs):
    jobs = []
    for fixture in (CR2, RAF):
        base = BASE_DEMOSAIC[fixture]
        c = coeffs[fixture]
        jobs.append(camera_job(fixture, "temperature-as-shot", "demosaic %s, temperature as shot (darktable's "
                               "default)" % base, develop_entries(base)))
        jobs.append(camera_job(fixture, "temperature-camera-reference", "demosaic %s, temperature camera reference "
                               "(preset 3) %.3f %.3f %.3f" % ((base,) + c["d65"]),
                               develop_entries(base, extra=[temperature_entry(c["d65"], DT_IOP_TEMP_D65)])))
        jobs.append(camera_job(fixture, "temperature-as-shot-to-reference", "demosaic %s, temperature as shot to "
                               "reference (preset 4) %.3f %.3f %.3f" % ((base,) + c["as_shot"]),
                               develop_entries(base, extra=[temperature_entry(c["as_shot"], DT_IOP_TEMP_D65_LATE)])))
        jobs.append(camera_job(fixture, "temperature-user", "demosaic %s, temperature user coefficients "
                               "%.3f %.3f %.3f" % ((base,) + USER_COEFFS),
                               develop_entries(base, extra=[temperature_entry(USER_COEFFS, DT_IOP_TEMP_USER)])))
    return jobs


def hotpixels_variants():
    return [("default", {}), ("permissive", {"permissive": 1}), ("strength-1", {"strength": 1.0})]


def hotpixels_jobs():
    jobs = []
    for fixture in (CR2, RAF):
        base = BASE_DEMOSAIC[fixture]
        for suffix, options in hotpixels_variants():
            jobs.append(camera_job(fixture, "hotpixels-" + suffix, "demosaic %s, hot pixels %s" % (base, suffix),
                                   develop_entries(base, extra=[("hotpixels", 1, 1, hotpixels_params(**options))])))
    return jobs


def cacorrect_jobs():
    variants = [("iterations-%d" % i, {"iterations": i}) for i in (1, 2, 5)]
    variants.append(("avoidshift", {"iterations": 2, "avoidshift": 1}))
    return [camera_job(CR2, "cacorrect-" + suffix, "demosaic rcd, raw chromatic aberrations %s" % suffix,
                       develop_entries("rcd", extra=[("cacorrect", 1, 2, cacorrect_params(**options))]))
            for suffix, options in variants]


def rawdenoise_jobs():
    shaped = (0.9, 0.7, 0.5, 0.3, 0.1)
    jobs = []
    for fixture in (CR2, RAF):
        base = BASE_DEMOSAIC[fixture]
        for suffix, curve in (("flat", (0.5,) * 5), ("shaped", shaped)):
            jobs.append(camera_job(fixture, "rawdenoise-0.05-" + suffix, "demosaic %s, raw denoise threshold 0.05, "
                                   "all-channels curve %s" % (base, curve),
                                   develop_entries(base, extra=[("rawdenoise", 1, 2, rawdenoise_params(0.05, curve))])))
    return jobs


def colorin_jobs():
    jobs = [camera_job(f, "colorin-standard", "demosaic %s, input profile standard matrix" % BASE_DEMOSAIC[f],
                       develop_entries(BASE_DEMOSAIC[f])) for f in (CR2, RAF)]
    # src/common/colormatrices.c lists the 5D Mark III among the enhanced matrices only, and the
    # X-Pro2 in none of them.
    jobs.append(camera_job(CR2, "colorin-enhanced", "demosaic rcd, input profile enhanced matrix",
                           develop_entries("rcd", input_profile=DT_COLORSPACE_ENHANCED_MATRIX)))
    for name, profile in GENERIC_INPUT_PROFILES:
        jobs.append(camera_job(CR2, "colorin-" + name, "demosaic rcd, input profile %s (%d)" % (name, profile),
                               develop_entries("rcd", input_profile=profile)))
    for name, normalize in GAMUT_CLIPPING:
        jobs.append(camera_job(CR2, "colorin-gamut-" + name, "demosaic rcd, standard matrix, gamut clipping %s" % name,
                               develop_entries("rcd", normalize=normalize)))
    jobs.append(camera_job(DNG, "colorin-embedded", "input profile embedded matrix",
                           develop_entries("rcd", input_profile=DT_COLORSPACE_EMBEDDED_MATRIX)))
    return jobs


SYNTHETIC_DNGS = {
    # name: (pattern, period, scene, gain maps, LinearResponseLimit)
    "highlights-bayer.dng": (BAYER_PATTERN, 2, "disc", False, None),
    "highlights-xtrans.dng": (XTRANS_PATTERN, 6, "disc", False, None),
    "linear-response-limit-bayer.dng": (BAYER_PATTERN, 2, "disc", False, 0.8),
    "gainmap-bayer.dng": (BAYER_PATTERN, 2, "ramp", True, None),
    "hotpixels-bayer.dng": (BAYER_PATTERN, 2, "hotpixels", False, None),
}


def mosaic_job(dng, suffix, what, highlights, extra=(), highlights_options=None):
    pattern, period = SYNTHETIC_DNGS[dng][:2]
    passthrough = "passthrough-colour" if period == 2 else "passthrough-colour-xtrans"
    entries = develop_entries(passthrough, highlights, DT_COLORSPACE_LIN_REC709,
                              highlights_options=highlights_options, extra=extra)
    return Job("%s-%s.tif" % (dng[:-4], suffix), dng, entries, what, mosaic=(pattern, period))


def highlights_jobs():
    variants = [(mode, mode, {}) for mode in HIGHLIGHT_MODES]
    variants += [
        ("segments-rebuild-off", "segments", {"recovery": "off"}),
        ("segments-rebuild-generic", "segments", {"recovery": "generic", "strength": 0.5}),
        ("segments-rebuild-small", "segments", {"recovery": "small", "strength": 0.5}),
        ("laplacian", "laplacian", {}),
        ("laplacian-i64-n0.1-s0.5", "laplacian", {"iterations": 64, "noise_level": 0.1, "solid_color": 0.5}),
        ("inpaint", "inpaint", {}),
    ]
    jobs = []
    for dng in ("highlights-bayer.dng", "highlights-xtrans.dng"):
        for suffix, mode, options in variants:
            what = "highlights %s %s" % (mode, " ".join("%s=%s" % kv for kv in sorted(options.items())))
            jobs.append(mosaic_job(dng, suffix, what.strip(), mode, highlights_options=options))
    return jobs


def synthetic_jobs():
    jobs = [mosaic_job("linear-response-limit-bayer.dng", "clip", "highlights clip, LinearResponseLimit 0.8", "clip")]
    for name, flat_field in (("on", 1), ("off", 0)):
        prepare = ("rawprepare", 1, 2, rawprepare_params(SYNTH_BLACK, SYNTH_WHITE, flat_field))
        jobs.append(mosaic_job("gainmap-bayer.dng", "flat-field-" + name, "flat field correction %s" % name,
                               "unclipped", extra=[prepare]))
    for suffix, options in hotpixels_variants():
        jobs.append(mosaic_job("hotpixels-bayer.dng", suffix, "hot pixels %s" % suffix, "unclipped",
                               extra=[("hotpixels", 1, 1, hotpixels_params(**options))]))
    return jobs


def ashift_jobs():
    jobs = []
    for case in ASHIFT_CASES:
        suffix, rotation, v, h, shear, mode, focal, crop_factor, orthocorr, aspect, crop = case
        entries = [
            ("flip", 0, 2, flip_params()),
            ("colorin", 1, 7, colorin_params(DT_COLORSPACE_LIN_REC709)),
            ("colorout", 1, 5, colorout_params()),
            ("ashift", 1, 5, ashift_params(rotation, v, h, shear, mode, focal, crop_factor, orthocorr, aspect, crop)),
        ]
        what = ("ashift rotation %g lensShiftV %g lensShiftH %g shear %g lensModel %s focalLength %g cropFactor %g "
                "lensDependence %g aspectAdjust %g crop %g %g %g %g, %s interpolation"
                % (rotation, v, h, shear, mode, focal, crop_factor, orthocorr, aspect, crop[0], crop[1], crop[2],
                   crop[3], ASHIFT_INTERPOLATOR))
        jobs.append(Job("ashift-%s.tif" % suffix, ASHIFT_SOURCE, entries, what, whole=True,
                        conf=["plugins/lighttable/export/pixel_interpolator_warp=" + ASHIFT_INTERPOLATOR]))
    return jobs


def ashift_scene():
    """Soft-edged grid lines and colour gradients, band-limited so that the interpolators differ
    little between the grid's pixels."""
    y, x = np.mgrid[0:ASHIFT_HEIGHT, 0:ASHIFT_WIDTH].astype(np.float64)
    image = np.empty((ASHIFT_HEIGHT, ASHIFT_WIDTH, 3))
    image[:, :, 0] = 0.15 + 0.5 * x / ASHIFT_WIDTH
    image[:, :, 1] = 0.2 + 0.4 * y / ASHIFT_HEIGHT
    image[:, :, 2] = 0.25 + 0.2 * np.sin(x / 13.0) * np.cos(y / 11.0)
    lines = np.zeros((ASHIFT_HEIGHT, ASHIFT_WIDTH))
    for cx in range(12, ASHIFT_WIDTH, 24):
        lines = np.maximum(lines, np.exp(-0.5 * ((x - cx) / 1.2) ** 2))
    for cy in range(10, ASHIFT_HEIGHT, 20):
        lines = np.maximum(lines, np.exp(-0.5 * ((y - cy) / 1.2) ** 2))
    image += 0.35 * lines[:, :, None]
    return np.clip(image, 0.0, 1.0).astype(np.float16)


def colour_calibration_jobs(coeffs):
    def cc(fixture, suffix, what, preset=DT_IOP_TEMP_AS_SHOT, **options):
        c = coeffs[fixture]["as_shot"]
        entries = develop_entries(BASE_DEMOSAIC[fixture], extra=[
            temperature_entry(c, preset), ("channelmixerrgb", 1, 3, channelmixerrgb_params(**options))])
        what = "demosaic %s, temperature %s %.3f %.3f %.3f, color calibration %s" % (
            BASE_DEMOSAIC[fixture], "as shot to reference (preset 4)" if preset == DT_IOP_TEMP_D65_LATE else "as shot",
            c[0], c[1], c[2], what)
        return camera_job(fixture, "colorcal-" + suffix, what, entries)

    jobs = []
    for fixture in (CR2, RAF):
        for adaptation in ("bradford", "cat16", "full-bradford", "xyz"):
            jobs.append(cc(fixture, "adaptation-" + adaptation, "%s, D 4500 K" % adaptation, adaptation=adaptation))
        jobs.append(cc(fixture, "adaptation-none", "none, D 4500 K, mix %s" % (MIX,), adaptation="none", mix=MIX))
        illuminants = [
            ("pipe", {"illuminant": "pipe"}), ("a", {"illuminant": "a"}), ("e", {"illuminant": "e"}),
            ("f2", {"illuminant": "f", "fluo": "f2"}), ("f11", {"illuminant": "f", "fluo": "f11"}),
            ("led-b1", {"illuminant": "led", "led": "b1"}), ("led-rgb1", {"illuminant": "led", "led": "rgb1"}),
            ("planckian-3200", {"illuminant": "bb", "temperature": 3200.0}),
            ("custom-xy", {"illuminant": "custom", "x": 0.36, "y": 0.37}),
        ]
        for suffix, options in illuminants:
            jobs.append(cc(fixture, "illuminant-" + suffix, "CAT16, %s" % suffix, **options))
        jobs.append(cc(fixture, "camera", "CAT16, as shot in camera", illuminant="camera"))
        jobs.append(cc(fixture, "camera-preset-4", "CAT16, as shot in camera", preset=DT_IOP_TEMP_D65_LATE,
                       illuminant="camera"))
        for gamut in (0.0, 1.0, 4.0):
            for clip in (1, 0):
                jobs.append(cc(fixture, "gamut-%g-clip-%s" % (gamut, "on" if clip else "off"),
                               "CAT16, D 4500 K, gamut compression %g, clip %d" % (gamut, clip), gamut=gamut, clip=clip))
        for normalize in (1, 0):
            jobs.append(cc(fixture, "mix-normalize-%s" % ("on" if normalize else "off"),
                           "CAT16, D 4500 K, mix %s, normalize %d" % (MIX, normalize), mix=MIX, normalize_mix=normalize))
        for version in (1, 2, 3):
            for sign, s in (("plus", 0.5), ("minus", -0.5)):
                jobs.append(cc(fixture, "colorfulness-%s-v%d" % (sign, version),
                               "CAT16, D 4500 K, colorfulness red %+g, version %d" % (s, version),
                               saturation=(s, 0.0, 0.0), version=version))
        lightness = (0.4, 0.2, -0.3)
        jobs.append(cc(fixture, "brightness", "CAT16, D 4500 K, brightness %s" % (lightness,), lightness=lightness))
        grey = (0.4, 0.6, 0.3)
        for normalize in (1, 0):
            jobs.append(cc(fixture, "gray-normalize-%s" % ("on" if normalize else "off"),
                           "CAT16, D 4500 K, gray %s, normalize %d" % (grey, normalize), grey=grey,
                           normalize_grey=normalize))
    c = coeffs[DNG]["as_shot"]
    jobs.append(camera_job(DNG, "colorcal-camera", "input profile embedded matrix, temperature as shot %.3f %.3f %.3f, "
                           "color calibration CAT16, as shot in camera" % c,
                           develop_entries("rcd", input_profile=DT_COLORSPACE_EMBEDDED_MATRIX, extra=[
                               temperature_entry(c, DT_IOP_TEMP_AS_SHOT),
                               ("channelmixerrgb", 1, 3, channelmixerrgb_params(illuminant="camera"))])))
    for fixture in CAMERA_FIXTURES:
        profile = DT_COLORSPACE_EMBEDDED_MATRIX if fixture == DNG else DT_COLORSPACE_STANDARD_MATRIX
        off = [("sigmoid", 0, 3, sigmoid_params()), ("exposure", 0, 7, exposure_params()),
               ("sharpen", 0, 1, sharpen_params())]
        jobs.append(camera_job(fixture, "scene-referred-default", "workflow %s with its automatic presets, sigmoid, "
                               "exposure and sharpen off" % SCENE_REFERRED,
                               develop_entries(BASE_DEMOSAIC[fixture], input_profile=profile, extra=off),
                               workflow=SCENE_REFERRED, auto_presets_applied=False, debug="params",
                               log_line=COMMIT_CC_LINE))
    return jobs


def darktable_cli(source, xmp, output, scratch, workflow="none", debug=None, size=0, conf=()):
    if os.path.exists(output):
        os.remove(output)
    config = os.path.join(scratch, "config-" + re.sub(r"[^a-z]+", "-", workflow))
    os.makedirs(config, exist_ok=True)
    command = [
        "darktable-cli", source, xmp, output,
        "--hq", "true", "--apply-custom-presets", "false", "--icc-type", "LIN_REC709",
        "--width", str(size), "--height", str(size),
        "--core", "--configdir", config, "--disable-opencl",
        "--conf", "plugins/darkroom/workflow=%s" % workflow,
        "--conf", "plugins/imageio/format/tiff/bpp=32",
        "--conf", "plugins/imageio/format/tiff/pixelformat=FALSE",
        "--conf", "plugins/imageio/format/tiff/compress=0",
        "--conf", "plugins/lighttable/export/force_lcms2=FALSE",
        "--conf", "plugins/lighttable/export/iccintent=0",
        "--conf", "write_sidecar_files=never",
        "--conf", "host_memory_limit=4096",
        "--conf", "worker_threads=4", "-t", "4",
    ]
    for setting in conf:
        command += ["--conf", setting]
    if debug:
        command += ["-d", debug]
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace")
    image = tifffile.imread(output)
    if image.dtype != np.float32 or image.ndim != 3 or image.shape[2] < 3:
        raise RuntimeError("%s: expected float RGB, got %s %s" % (output, image.dtype, image.shape))
    return image[:, :, :3], result.stdout.splitlines()


def write_reference(path, image, description, dtype=None):
    tifffile.imwrite(path, np.ascontiguousarray(image, dtype=dtype or np.float32), photometric="rgb" if image.ndim == 3 else "minisblack",
                     compression="zlib", description=description, metadata=None)


def probe_coefficients(fixture, scratch):
    """darktable's as-shot and D65 coefficients for a file, from temperature's reload_defaults line.

    darktable logs them to three decimals only, so the jobs that pack them carry that rounding.
    """
    xmp = os.path.join(scratch, "probe.xmp")
    write_xmp(xmp, develop_entries(BASE_DEMOSAIC[fixture]))
    _, log = darktable_cli(os.path.join(scratch, fixture), xmp, os.path.join(scratch, "probe.tif"), scratch,
                           debug="params", size=256)
    for line in log:
        match = PROBE_LINE.search(line)
        if match:
            v = [float(g) for g in match.groups()]
            print("%s: %s" % (fixture, line.strip()))
            return {"d65": tuple(v[0:3]), "as_shot": tuple(v[3:6])}
    raise RuntimeError("%s: no [dt_iop_reload_defaults] line in darktable's -d params output" % fixture)


def colour_index(pattern, period, x, y):
    return "RGB".index(pattern[(y % period) * period + (x % period)])


def synthetic_scene(pattern, period, scene):
    """Pre-white-balance photosite values in [0, 1] of the white level, top row first.

    disc: a ramp that turns neutral grey once white balanced, a disc where every photosite clipped
    and a rim around it where only green did. ramp: a gentler diagonal ramp, under white even
    after the gain maps. hotpixels: the ramp with the HOT_PIXELS photosites at white.
    """
    values = np.zeros((SYNTH_HEIGHT, SYNTH_WIDTH), dtype=np.float64)
    for y in range(SYNTH_HEIGHT):
        for x in range(SYNTH_WIDTH):
            c = colour_index(pattern, period, x, y)
            if scene == "disc":
                d = np.hypot(x - 24.0, y - 20.0)
                v = (0.3 + 0.2 * x / SYNTH_WIDTH) / SYNTH_COEFFS[c] * 1.4
                if d < 8.0:
                    v = 1.0
                elif d < 11.0 and c == 1:
                    v = 1.0
            else:
                v = (0.15 + 0.25 * x / SYNTH_WIDTH + 0.1 * y / SYNTH_HEIGHT) / SYNTH_COEFFS[c] * 1.4
            values[y, x] = min(v, 1.0)
    if scene == "hotpixels":
        for x, y in HOT_PIXELS:
            values[y, x] = 1.0
    return values


def gain_map_opcodes(pattern):
    """OpcodeList2 with one 3x3 GainMap per Bayer cell, big-endian as the DNG specification has it.

    rawprepare applies them only as four maps, one per cell, each with row and column pitch 2,
    covering the whole image and sharing one shape.
    """
    opcodes = b""
    for cell in range(4):
        top, left = cell >> 1, cell & 1
        gains = [1.0 + 0.05 * cell + 0.1 * i + 0.05 * j for i in range(3) for j in range(3)]
        params = struct.pack(">10I4dI", top, left, SYNTH_HEIGHT, SYNTH_WIDTH, 0, 1, 2, 2, 3, 3, 0.5, 0.5, 0.0, 0.0, 1)
        params += struct.pack(">%df" % len(gains), *gains)
        opcodes += struct.pack(">IBBBBII", 9, 1, 3, 0, 0, 1, len(params)) + params
    return struct.pack(">I", 4) + opcodes


def write_cfa_dng(path, pattern, period, scene="disc", gain_maps=False, linear_response_limit=None):
    data = np.round(SYNTH_BLACK + synthetic_scene(pattern, period, scene) * (SYNTH_WHITE - SYNTH_BLACK)).astype(np.uint16)
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
    if linear_response_limit is not None:
        extratags.append((50734, 5, 1, (int(round(linear_response_limit * 10000)), 10000), True))
    if gain_maps:
        opcodes = gain_map_opcodes(pattern)
        extratags.append((51009, 7, len(opcodes), opcodes, True))
    tifffile.imwrite(path, data, photometric=32803, subfiletype=0, extratags=extratags, metadata=None)


def run_job(job, out, scratch):
    xmp = os.path.join(scratch, job.name[:-4] + ".xmp")
    write_xmp(xmp, job.entries, job.auto_presets_applied)
    image, log = darktable_cli(os.path.join(scratch, job.source), xmp, os.path.join(scratch, "render.tif"), scratch,
                               job.workflow, job.debug, conf=job.conf)
    height, width = image.shape[:2]
    logged = [line.strip() for line in log if job.log_line and job.log_line.search(line)]
    for line in logged:
        print("%s: log: %s" % (job.name, line))
    dtype = None
    if job.whole:
        result = image
        dtype = np.float16
        where = "whole %dx%d output, linear Rec. 709" % (width, height)
        summary = "%dx%d, mean %s max %s" % (width, height, result.mean(axis=(0, 1)), result.max())
    elif job.mosaic is None:
        left, top = CROP_ORIGIN[job.source]
        result = image[top:top + CROP_SIZE, left:left + CROP_SIZE, :]
        if result.shape[:2] != (CROP_SIZE, CROP_SIZE):
            raise RuntimeError("%s: developed %dx%d, too small for the crop" % (job.name, width, height))
        where = "developed %dx%d, crop left %d top %d size %d, linear Rec. 709" % (width, height, left, top, CROP_SIZE)
        summary = "mean %s max %s" % (result.mean(axis=(0, 1)), result.max())
    else:
        if (height, width) != (SYNTH_HEIGHT, SYNTH_WIDTH):
            raise RuntimeError("%s: developed %s" % (job.name, image.shape))
        pattern, period = job.mosaic
        result = np.zeros((SYNTH_HEIGHT, SYNTH_WIDTH), dtype=np.float32)
        leak = 0.0
        for y in range(SYNTH_HEIGHT):
            for x in range(SYNTH_WIDTH):
                c = colour_index(pattern, period, x, y)
                result[y, x] = image[y, x, c]
                leak = max(leak, float(np.abs(np.delete(image[y, x], c)).max()))
        where = "photosite colour, white-balanced mosaic"
        summary = "max %g, largest other-channel value %g" % (result.max(), leak)
    description = "darktable %s, %s, workflow %s, %s, %s" % (
        os.environ.get("DARKTABLE_REF", "?"), job.source, job.workflow, job.what, where)
    if logged:
        description += "; darktable log: " + " | ".join(logged)
    write_reference(os.path.join(out, job.name), result, description, dtype)
    print("%s: %s" % (job.name, summary))


def all_jobs(coeffs):
    return (develop_jobs() + demosaic_jobs() + capture_sharpen_jobs() + temperature_jobs(coeffs) + hotpixels_jobs()
            + cacorrect_jobs() + rawdenoise_jobs() + colorin_jobs() + highlights_jobs() + synthetic_jobs()
            + colour_calibration_jobs(coeffs) + ashift_jobs())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only")
    parser.add_argument("fixtures")
    parser.add_argument("out")
    parser.add_argument("scratch")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.scratch, exist_ok=True)

    jobs = all_jobs({f: PLACEHOLDER_COEFFS for f in CAMERA_FIXTURES})
    if args.only:
        jobs = [job for job in jobs if args.only in job.name]
    names = [job.name for job in jobs]
    if len(set(names)) != len(names):
        raise RuntimeError("duplicate output names")

    if args.dry_run:
        for job in jobs:
            write_xmp(os.path.join(args.scratch, job.name[:-4] + ".xmp"), job.entries, job.auto_presets_applied)
            print("%s <- %s, workflow %s%s: %s" % (job.name, job.source, job.workflow,
                                                     ", -d %s" % job.debug if job.debug else "", job.what))
        print("%d jobs, %d XMPs in %s" % (len(jobs), len(jobs), args.scratch))
        return

    if any(job.source == ASHIFT_SOURCE for job in jobs):
        path = os.path.join(args.out, ASHIFT_SOURCE)
        write_reference(path, ashift_scene(), "synthetic AutoStraighten source, linear Rec. 709", np.float16)
        with open(path, "rb") as src, open(os.path.join(args.scratch, ASHIFT_SOURCE), "wb") as dst:
            dst.write(src.read())
    camera = {job.source for job in jobs if job.mosaic is None and job.source in CAMERA_FIXTURES}
    for fixture in sorted(camera):
        with open(os.path.join(args.fixtures, fixture), "rb") as src, open(os.path.join(args.scratch, fixture), "wb") as dst:
            dst.write(src.read())
    coeffs = {f: probe_coefficients(f, args.scratch) for f in sorted(camera)}
    for f in CAMERA_FIXTURES:
        coeffs.setdefault(f, PLACEHOLDER_COEFFS)
    jobs = [job for job in all_jobs(coeffs) if job.name in names]

    for dng in sorted({job.source for job in jobs if job.mosaic is not None}):
        pattern, period, scene, gain_maps, limit = SYNTHETIC_DNGS[dng]
        path = os.path.join(args.out, dng)
        write_cfa_dng(path, pattern, period, scene, gain_maps, limit)
        with open(path, "rb") as src, open(os.path.join(args.scratch, dng), "wb") as dst:
            dst.write(src.read())
    for job in jobs:
        run_job(job, args.out, args.scratch)


if __name__ == "__main__":
    sys.exit(main())
