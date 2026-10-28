#!/usr/bin/env python3
"""Renders the darktable references of Tests/fixtures/raw/reference with darktable-cli.

Runs inside the image tools/raw/make-reference.sh builds. Each job writes a sidecar XMP whose
history packs the parameters of the darktable modules the raw nodes port, exports a 32-bit float
linear Rec. 709 TIFF and keeps either a crop of it (camera files) or its white-balanced mosaic
(synthetic DNGs). Usage:

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


DENOISEPROFILE_MODES = {"nlm": 0, "wavelets": 1, "variance": 2, "nlm-auto": 3, "wavelets-auto": 4}
DENOISEPROFILE_COLOR_MODES = {"rgb": 0, "y0u0v0": 1}


def denoiseprofile_params(mode="wavelets", color_mode="y0u0v0", radius=1.0, nbhood=7.0, strength=1.0, shadows=1.0,
                          bias=0.0, scattering=0.0, central_pixel_weight=0.1, overshooting=1.0, compensate_hilite_pres=1):
    # dt_iop_denoiseprofile_params_t, version 12: the eight sliders, a[3] and b[3] with a[0] = -1
    # for the profile darktable finds for the file, the mode, x[6][7] and y[6][7] (all, R, G, B,
    # Y0, U0V0) flat at 0.5 with the x positions evenly spaced as darktable's defaults are, then
    # wb_adaptive_anscombe, fix_anscombe_and_nlmeans_norm, use_new_vst, wavelet_color_mode and
    # compensate_hilite_pres.
    xs = [k / 6.0 for k in range(7)] * 6
    ys = [0.5] * 42
    return struct.pack("<8f3f3fi42f42f5i", radius, nbhood, strength, shadows, bias, scattering, central_pixel_weight,
                       overshooting, -1.0, 0.0, 0.0, 0.0, 0.0, 0.0, DENOISEPROFILE_MODES[mode], *(xs + ys), 1, 1, 1,
                       DENOISEPROFILE_COLOR_MODES[color_mode], compensate_hilite_pres)


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
                 log_line=None, mosaic=None):
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


# (suffix, params): each mode, both wavelet colour modes, and shadows, bias and strength off their
# defaults, which the auto modes would replace.
DENOISEPROFILE_VARIANTS = (
    ("wavelets-y0u0v0", {}),
    ("wavelets-rgb", {"color_mode": "rgb"}),
    ("wavelets-auto", {"mode": "wavelets-auto"}),
    ("nlm", {"mode": "nlm"}),
    ("nlm-auto", {"mode": "nlm-auto"}),
    ("nlm-p2-k4-s0.5", {"mode": "nlm", "radius": 2.0, "nbhood": 4.0, "scattering": 0.5, "central_pixel_weight": 0.3}),
    ("wavelets-shadows-0.5", {"shadows": 0.5}),
    ("wavelets-bias-minus-2", {"bias": -2.0}),
    ("wavelets-strength-2", {"strength": 2.0}),
)


def denoiseprofile_jobs():
    jobs = []
    for fixture in (CR2, RAF):
        base = BASE_DEMOSAIC[fixture]
        for suffix, options in DENOISEPROFILE_VARIANTS:
            jobs.append(camera_job(fixture, "denoiseprofile-" + suffix, "demosaic %s, denoise (profiled) %s, auto profile"
                                   % (base, suffix), develop_entries(base, extra=[("denoiseprofile", 1, 12,
                                                                                   denoiseprofile_params(**options))])))
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


def darktable_cli(source, xmp, output, scratch, workflow="none", debug=None, size=0):
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
    if debug:
        command += ["-d", debug]
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace")
    image = tifffile.imread(output)
    if image.dtype != np.float32 or image.ndim != 3 or image.shape[2] < 3:
        raise RuntimeError("%s: expected float RGB, got %s %s" % (output, image.dtype, image.shape))
    return image[:, :, :3], result.stdout.splitlines()


def write_reference(path, image, description):
    tifffile.imwrite(path, np.ascontiguousarray(image, dtype=np.float32), photometric="rgb" if image.ndim == 3 else "minisblack",
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
    if getattr(job, "plate", None):
        return run_retouch_job(job, out, scratch)
    xmp = os.path.join(scratch, job.name[:-4] + ".xmp")
    write_xmp(xmp, job.entries, job.auto_presets_applied)
    image, log = darktable_cli(os.path.join(scratch, job.source), xmp, os.path.join(scratch, "render.tif"), scratch,
                               job.workflow, job.debug)
    height, width = image.shape[:2]
    logged = [line.strip() for line in log if job.log_line and job.log_line.search(line)]
    for line in logged:
        print("%s: log: %s" % (job.name, line))
    if job.mosaic is None:
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
    write_reference(os.path.join(out, job.name), result, description)
    print("%s: %s" % (job.name, summary))


# Retouch (darktable src/iop/retouch.c): clone, heal, blur and fill with circle, ellipse and path
# forms that the XMP's masks history carries in a group, on the image, detail scales and the
# residual. The plate is the tests' ParitySource (Tests/NativeParity.cpp) with its noise at time 1,
# so NativeRetouch_Test renders the same pixels; each reference keeps the whole 64x48 result as a
# half-float TIFF of about 15 KB.
RETOUCH_WIDTH = 64
RETOUCH_HEIGHT = 48
RETOUCH_PLATE = "retouch-plate.tif"
RETOUCH_TIME = 1.0
DT_MASKS_CIRCLE = 1
DT_MASKS_PATH = 2
DT_MASKS_GROUP = 4
DT_MASKS_CLONE = 8
DT_MASKS_ELLIPSE = 32
DT_MASKS_NON_CLONE = 128
# use, show, union
DT_MASKS_GROUP_STATE = 1 | 2 | 8
# The control points as given, not recomputed.
DT_MASKS_POINT_STATE_USER = 2
DEVELOP_MASKS_VERSION = 6
RETOUCH_ALGORITHMS = {"clone": 1, "heal": 2, "blur": 3, "fill": 4}
RETOUCH_BLUR_TYPES = {"gaussian": 0, "bilateral": 1}
RETOUCH_FILL_MODES = {"erase": 0, "color": 1}
RETOUCH_NO_FORMS = 300
RETOUCH_GROUP_ID = 1000
# New forms' distortion mode (DT_IOP_RETOUCH_DISTORT_MODE).
RETOUCH_DISTORT_MODE = 2

# In Natron's canonical pixels, y up, as NativeRetouch_Test builds them: a circle and a rotated
# ellipse (darktable's rotation turns clockwise on screen) given by centre and radii, a path by
# (corner, left handle, right handle) per point. offset goes from the shape to its source; the
# source of a path is its first corner's.
RETOUCH_SHAPES = {
    "circle": {"centre": (20.0, 30.0), "radii": (6.0, 6.0), "feather": 3.0, "opacity": 1.0,
               "offset": (22.0, -8.0)},
    "ellipse": {"centre": (42.0, 24.0), "radii": (9.0, 5.0), "rotation": 30.0, "feather": 2.0, "opacity": 0.6,
                "offset": (-22.0, 8.0)},
    "path": {"points": (((26.0, 8.0), (24.0, 9.0), (28.0, 7.0)),
                        ((38.0, 10.0), (37.0, 8.0), (39.0, 12.0)),
                        ((36.0, 20.0), (38.0, 19.0), (34.0, 21.0)),
                        ((24.0, 18.0), (25.0, 20.0), (23.0, 16.0))),
             "feather": 2.0, "opacity": 1.0, "offset": (16.0, 20.0)},
}


def retouch_point(x, y):
    """Natron canonical (pixel centres at +0.5, y up) to darktable's normalised image position."""
    return ((x - 0.5) / RETOUCH_WIDTH, (RETOUCH_HEIGHT - y - 0.5) / RETOUCH_HEIGHT)


def retouch_length(d):
    return d / min(RETOUCH_WIDTH, RETOUCH_HEIGHT)


def retouch_form(kind, formid, clone):
    """(mask type, points blob, point count, source blob) of one masks_history entry."""
    shape = RETOUCH_SHAPES[kind]
    ox, oy = shape["offset"]
    if kind == "path":
        points = b"".join(struct.pack("<8fi", *retouch_point(*corner), *retouch_point(*left),
                                      *retouch_point(*right), retouch_length(shape["feather"]),
                                      retouch_length(shape["feather"]), DT_MASKS_POINT_STATE_USER)
                          for corner, left, right in shape["points"])
        count = len(shape["points"])
        origin = shape["points"][0][0]
        mask_type = DT_MASKS_PATH
    else:
        centre = retouch_point(*shape["centre"])
        if kind == "circle":
            points = struct.pack("<4f", *centre, retouch_length(shape["radii"][0]), retouch_length(shape["feather"]))
            mask_type = DT_MASKS_CIRCLE
        else:
            # Equidistant feather (flags 0).
            points = struct.pack("<6fi", *centre, *(retouch_length(r) for r in shape["radii"]), shape["rotation"],
                                 retouch_length(shape["feather"]), 0)
            mask_type = DT_MASKS_ELLIPSE
        count = 1
        origin = shape["centre"]
    source = struct.pack("<2f", *retouch_point(origin[0] + ox, origin[1] + oy)) if clone else struct.pack("<2f", 0.0, 0.0)
    return mask_type | (DT_MASKS_CLONE if clone else DT_MASKS_NON_CLONE), points, count, source


def retouch_mask_entry(num, formid, mask_type, name, points, count, source):
    return ("     <rdf:li\n"
            "      darktable:mask_num=\"%d\"\n"
            "      darktable:mask_id=\"%d\"\n"
            "      darktable:mask_type=\"%d\"\n"
            "      darktable:mask_name=\"%s\"\n"
            "      darktable:mask_version=\"%d\"\n"
            "      darktable:mask_points=\"%s\"\n"
            "      darktable:mask_nb=\"%d\"\n"
            "      darktable:mask_src=\"%s\"/>\n") % (num, formid, mask_type, name, DEVELOP_MASKS_VERSION, points.hex(),
                                                   count, source.hex())


def retouch_params(forms, num_scales, merge_from_scale=0):
    """dt_iop_retouch_params_t, version 3. forms: (formid, settings) with settings a dict of
    scale, algorithm and the blur and fill settings."""
    packed = b""
    for formid, s in forms:
        packed += struct.pack("<Iiiifi3ffi", formid, s["scale"], RETOUCH_ALGORITHMS[s["algorithm"]],
                              RETOUCH_BLUR_TYPES[s.get("blur_type", "gaussian")], s.get("blur_radius", 10.0),
                              RETOUCH_FILL_MODES[s.get("fill_mode", "erase")], *s.get("fill_color", (0.0, 0.0, 0.0)),
                              s.get("fill_brightness", 0.0), RETOUCH_DISTORT_MODE)
    packed += b"\0" * (44 * (RETOUCH_NO_FORMS - len(forms)))
    # Globals: the default algorithm, scales, current scale, merge from, preview levels, the
    # default blur and fill settings and max_heal_iter (darktable's defaults).
    packed += struct.pack("<iiii3fifi3ffi", RETOUCH_ALGORITHMS["heal"], num_scales, 0, merge_from_scale,
                          -3.0, 0.0, 3.0, 0, 10.0, 0, 0.0, 0.0, 0.0, 0.0, 2000)
    if len(packed) != 13260:
        raise RuntimeError("retouch params are %d bytes" % len(packed))
    return packed


def retouch_blend_params():
    """BLEND_PARAMS with mask_id, at offset 24 of dt_develop_blend_params_t, set to the group."""
    import base64
    import zlib
    blob = bytearray(zlib.decompress(base64.b64decode(BLEND_PARAMS[4:])))
    struct.pack_into("<i", blob, 24, RETOUCH_GROUP_ID)
    return bytes(blob).hex()


class RetouchJob(Job):
    """A retouch of the synthetic plate; shapes is a list of (kind, settings)."""

    def __init__(self, name, shapes, num_scales, merge_from_scale=0):
        self.shapes = shapes
        forms = [(RETOUCH_GROUP_ID + 1 + i, s) for i, (kind, s) in enumerate(shapes)]
        entries = [
            ("colorin", 1, 7, colorin_params(DT_COLORSPACE_LIN_REC709)),
            ("colorout", 1, 5, colorout_params()),
            ("retouch", 1, 3, retouch_params(forms, num_scales, merge_from_scale)),
        ]
        what = "retouch %d scales, merge from %d: %s" % (num_scales, merge_from_scale, "; ".join(
            "%s %s on scale %d" % (s["algorithm"], kind, s["scale"]) for kind, s in shapes))
        super().__init__(name, RETOUCH_PLATE, entries, what)
        self.plate = RETOUCH_PLATE
        num = len(entries) - 1
        group = b""
        masks = ""
        for (formid, s), (kind, _) in zip(forms, shapes):
            mask_type, points, count, source = retouch_form(kind, formid, s["algorithm"] in ("clone", "heal"))
            masks += retouch_mask_entry(num, formid, mask_type, "%s %d" % (kind, formid), points, count, source)
            group += struct.pack("<iiif", formid, RETOUCH_GROUP_ID, DT_MASKS_GROUP_STATE, RETOUCH_SHAPES[kind]["opacity"])
        masks += retouch_mask_entry(num, RETOUCH_GROUP_ID, DT_MASKS_GROUP, "grp retouch", group, len(forms), b"\0" * 8)
        self.masks = masks
        self.blend = retouch_blend_params()


def write_retouch_xmp(path, job):
    write_xmp(path, job.entries, job.auto_presets_applied)
    with open(path) as f:
        xmp = f.read()
    xmp = xmp.replace("    <rdf:Seq/>\n", "    <rdf:Seq>\n%s    </rdf:Seq>\n" % job.masks, 1)
    xmp = xmp.replace(BLEND_PARAMS, job.blend)
    with open(path, "w") as f:
        f.write(xmp)


def parity_source_noise(channel, x, y):
    mask = 0xffffffff
    h = ((x * 0x8da6b343) ^ (y * 0xd8163841) ^ (channel * 0xcb1ab31f) ^ 0x9e3779b9) & mask
    h ^= h >> 16
    h = (h * 0x7feb352d) & mask
    h ^= h >> 15
    h = (h * 0x846ca68b) & mask
    h ^= h >> 16
    return np.float32(h >> 8) * np.float32(1.0 / 16777216.0) - np.float32(0.5)


def write_retouch_plate(path):
    """Tests/NativeParity.cpp's ParitySource colour at RETOUCH_TIME with noise, rows top down."""
    import math
    image = np.zeros((RETOUCH_HEIGHT, RETOUCH_WIDTH, 3), dtype=np.float32)
    offset = 0.01 * RETOUCH_TIME
    for cy in range(RETOUCH_HEIGHT):
        v = (cy + 0.5) / RETOUCH_HEIGHT
        for cx in range(RETOUCH_WIDTH):
            u = (cx + 0.5) / RETOUCH_WIDTH
            values = (-0.25 + 2.0 * u + 0.15 * math.sin(6.1 * v + 0.3) + offset,
                      -0.25 + 2.0 * v + 0.15 * math.sin(4.3 * u + 1.1) + offset,
                      -0.25 + (u + v) + 0.15 * math.sin(9.7 * u * v + 2.0) + offset)
            for c in range(3):
                image[RETOUCH_HEIGHT - 1 - cy, cx, c] = (np.float32(values[c])
                                                         + np.float32(0.1) * parity_source_noise(c, cx, cy))
    tifffile.imwrite(path, image, photometric="rgb", metadata=None)


def run_retouch_job(job, out, scratch):
    xmp = os.path.join(scratch, job.name[:-4] + ".xmp")
    write_retouch_xmp(xmp, job)
    image, _ = darktable_cli(os.path.join(scratch, job.source), xmp, os.path.join(scratch, "render.tif"), scratch,
                             job.workflow)
    if image.shape[:2] != (RETOUCH_HEIGHT, RETOUCH_WIDTH):
        raise RuntimeError("%s: developed %s" % (job.name, image.shape))
    description = "darktable %s, ParitySource plate with noise at time %g, %s, whole image, linear Rec. 709" % (
        os.environ.get("DARKTABLE_REF", "?"), RETOUCH_TIME, job.what)
    tifffile.imwrite(os.path.join(out, job.name), np.ascontiguousarray(image, dtype=np.float16), photometric="rgb",
                     compression="zlib", description=description, metadata=None)
    print("%s: mean %s max %s" % (job.name, image.mean(axis=(0, 1)), image.max()))


def retouch_jobs():
    """Each algorithm on each kind of shape: the circle (feathered) on scale 1, the ellipse (opacity
    0.6) on scale 2 and the path on the residual of two scales; then the image scale, the bilateral
    blur, the colour fill and a mix of shapes with merged scales."""
    settings = {
        "clone": {},
        "heal": {},
        "blur": {"blur_radius": 4.0},
        "fill": {"fill_brightness": 0.05},
    }
    scales = {"circle": 1, "ellipse": 2, "path": 3}
    jobs = []
    for algorithm in ("clone", "heal", "blur", "fill"):
        for kind in ("circle", "ellipse", "path"):
            s = dict(settings[algorithm], algorithm=algorithm, scale=scales[kind])
            jobs.append(RetouchJob("retouch-%s-%s.tif" % (algorithm, kind), [(kind, s)], 2))
    jobs.append(RetouchJob("retouch-heal-circle-image.tif", [("circle", {"algorithm": "heal", "scale": 0})], 2))
    jobs.append(RetouchJob("retouch-blur-bilateral-circle.tif",
                           [("circle", {"algorithm": "blur", "scale": 1, "blur_type": "bilateral", "blur_radius": 6.0})], 2))
    jobs.append(RetouchJob("retouch-fill-color-ellipse.tif",
                           [("ellipse", {"algorithm": "fill", "scale": 0, "fill_mode": "color",
                                         "fill_color": (0.8, 0.2, 0.1), "fill_brightness": 0.1})], 2))
    jobs.append(RetouchJob("retouch-mixed-merge.tif",
                           [("circle", {"algorithm": "heal", "scale": 1}),
                            ("ellipse", {"algorithm": "clone", "scale": 2}),
                            ("path", {"algorithm": "fill", "scale": 4, "fill_brightness": 0.05})], 3, 2))
    return jobs


def all_jobs(coeffs):
    return (develop_jobs() + demosaic_jobs() + capture_sharpen_jobs() + temperature_jobs(coeffs) + hotpixels_jobs()
            + cacorrect_jobs() + rawdenoise_jobs() + denoiseprofile_jobs() + colorin_jobs() + highlights_jobs() + synthetic_jobs()
            + colour_calibration_jobs(coeffs) + retouch_jobs())


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
            if getattr(job, "plate", None):
                write_retouch_xmp(os.path.join(args.scratch, job.name[:-4] + ".xmp"), job)
            else:
                write_xmp(os.path.join(args.scratch, job.name[:-4] + ".xmp"), job.entries, job.auto_presets_applied)
            print("%s <- %s, workflow %s%s: %s" % (job.name, job.source, job.workflow,
                                                     ", -d %s" % job.debug if job.debug else "", job.what))
        print("%d jobs, %d XMPs in %s" % (len(jobs), len(jobs), args.scratch))
        return

    for fixture in sorted({job.source for job in jobs if job.source in CAMERA_FIXTURES}):
        with open(os.path.join(args.fixtures, fixture), "rb") as src, open(os.path.join(args.scratch, fixture), "wb") as dst:
            dst.write(src.read())
    coeffs = {f: probe_coefficients(f, args.scratch) for f in sorted({job.source for job in jobs if job.source in CAMERA_FIXTURES})}
    for f in CAMERA_FIXTURES:
        coeffs.setdefault(f, PLACEHOLDER_COEFFS)
    jobs = [job for job in all_jobs(coeffs) if job.name in names]

    for dng in sorted({job.source for job in jobs if job.mosaic is not None}):
        pattern, period, scene, gain_maps, limit = SYNTHETIC_DNGS[dng]
        path = os.path.join(args.out, dng)
        write_cfa_dng(path, pattern, period, scene, gain_maps, limit)
        with open(path, "rb") as src, open(os.path.join(args.scratch, dng), "wb") as dst:
            dst.write(src.read())
    if any(getattr(job, "plate", None) for job in jobs):
        write_retouch_plate(os.path.join(args.scratch, RETOUCH_PLATE))
    for job in jobs:
        run_job(job, args.out, args.scratch)


if __name__ == "__main__":
    sys.exit(main())
