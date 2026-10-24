/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * The colour checker calibration of darktable's color calibration module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7): _extract_patches(), _compute_patches_delta_E(),
 * _extract_color_checker(), _validate_color_checker() and the checker section of gui_init() in
 * src/iop/channelmixerrgb.c, get_homography() and apply_homography() in src/chart/common.c, and
 * gauss_solve() and pseudo_solve_gaussian() in src/iop/gaussian_elimination.h, Copyright (C)
 * 2016-2026 darktable developers, distributed under the GNU GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawColorCalibrationChecker_h
#define Engine_Nodes_Raw_RawColorCalibrationChecker_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawColorCheckerData.h"

NATRON_NAMESPACE_ENTER

namespace RawColorCalibrationChecker {
/**
 * @brief dt_solving_strategy_t, in the order of the Optimize For choice.
 **/
enum class Optimization : int {
    None = 0,
    NeutralColors,
    SaturatedColors,
    Skin,
    Foliage,
    Sky,
    AverageDeltaE,
    MaximumDeltaE,
    Last
};

/**
 * @brief darktable's labels for the Chart and Optimize For choices, nullptr for Last.
 **/
const char* chartLabel(RawColorCheckerData::ColorCheckerTarget target);
const char* optimizationLabel(Optimization optimization);

struct Point {
    float x;
    float y;
};

/**
 * @brief The chart's corners, in the order of the ideal box: top left, top right, bottom right
 * and bottom left.
 **/
using Quad = std::array<Point, 4>;

/**
 * @brief A 3x3 projective transform, row-major.
 **/
using Homography = std::array<float, 9>;

/**
 * @brief The unit square the patch centres are given in, y pointing down.
 **/
Quad idealBox();

/**
 * @brief gauss_solve(): solves A x = b in place for the n x n matrix A, row-major; b receives x.
 * False when A is singular, b then being left partly solved.
 **/
bool gaussSolve(double* A,
                double* b,
                int n);

/**
 * @brief pseudo_solve_gaussian(): the least squares solution of the m x n system A x = y
 * (m >= n), through the normal equations; the first n entries of y receive x. False when m < n
 * or A'A is singular.
 **/
bool pseudoSolveGaussian(const double* A,
                         double* y,
                         std::size_t m,
                         std::size_t n);

/**
 * @brief get_homography(): the projective transform taking each source corner to the target
 * corner of the same index, nullopt when the corners are degenerate.
 **/
std::optional<Homography> getHomography(const Quad& source,
                                        const Quad& target);

Point applyHomography(const Point& p,
                      const Homography& h);

/**
 * @brief Working RGB over a rectangle of pixels: x1, y1 is the bottom left pixel, rgb holds
 * width * height pixels of three floats, the row at y1 first.
 **/
struct RgbImage {
    int x1 = 0;
    int y1 = 0;
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
};

/**
 * @brief Where the chart lies in an image: box is its corners in pixel coordinates, in the
 * order of idealBox(); patchScale (darktable's safety margin) scales the sampled part of each
 * patch.
 **/
struct ChartPlacement {
    const RawColorCheckerData::ColorChecker* checker = nullptr;
    Quad box {};
    float patchScale = 0.5f;
};

struct PatchExtraction {
    // The mean of each patch in XYZ (D50), exposure normalised when asked.
    std::vector<RawColorCalibrationMath::Vec3> xyz;
    // The exposure compensation and the black offset that fit the patches to the references in
    // camera RGB, as the exposure module would apply them.
    float black = 0.f;
    float exposure = 1.f;
};

/**
 * @brief _extract_patches(): the mean colour of the sampled part of every patch, through
 * rgbToXyz. With normalizeExposure, each patch is scaled so that its luminance relative to the
 * white patch is the reference's. xyzToCam (XYZ D50 to camera RGB) gives the black offset; the
 * user exposure and black are darktable's defaults, 1 and 0. Nullopt when the box is degenerate
 * or a patch covers no pixel centre of image.
 **/
std::optional<PatchExtraction> extractPatches(const RgbImage& image,
                                              const ChartPlacement& placement,
                                              const RawColorCalibrationMath::Mat3& rgbToXyz,
                                              const RawColorCalibrationMath::Mat3* xyzToCam,
                                              bool normalizeExposure);

/**
 * @brief The CIE 2000 colour difference of _compute_patches_delta_E(), which keeps darktable's
 * mapping of a negative hue angle h to 2 pi - h.
 **/
float deltaE2000(const RawColorCalibrationMath::Vec3& labReference,
                 const RawColorCalibrationMath::Vec3& labTest);

struct DeltaE {
    std::vector<float> perPatch;
    float average = 0.f;
    float maximum = 0.f;
};

/**
 * @brief _compute_patches_delta_E(): the difference of each patch (XYZ D50) from its reference.
 **/
DeltaE computePatchesDeltaE(const std::vector<RawColorCalibrationMath::Vec3>& xyz,
                            const RawColorCheckerData::ColorChecker& checker);

/**
 * @brief What _extract_color_checker() finds and _commit_profile_callback() writes.
 **/
struct Profile {
    // The scene illuminant, from the grey patch.
    RawColorCalibrationMath::Xy illuminant { 0.f, 0.f };
    // The mix in the adaptation's space, row-major: the R, G and B rows.
    RawColorCalibrationMath::Mat3 mix {};
    float temperature = 0.f;
    // D, BlackBody, or Custom when neither models the illuminant.
    RawColorCalibrationMath::Illuminant model = RawColorCalibrationMath::Illuminant::Custom;
    float inputAverage = 0.f;
    float inputMaximum = 0.f;
    float whiteBalancedAverage = 0.f;
    float whiteBalancedMaximum = 0.f;
    float outputAverage = 0.f;
    float outputMaximum = 0.f;
    float exposure = 1.f;
    float black = 0.f;
};

/**
 * @brief _extract_color_checker(): the illuminant and the mix that map the chart in image (the
 * node's input) onto its references with the adaptation kind, weighting the patches by
 * optimization. Nullopt when the patches cannot be extracted or the fit has no solution.
 **/
std::optional<Profile> extractColorChecker(const RgbImage& image,
                                           const ChartPlacement& placement,
                                           Optimization optimization,
                                           RawColorCalibrationMath::Adaptation kind,
                                           const RawColorCalibrationMath::Mat3& rgbToXyz,
                                           const RawColorCalibrationMath::Mat3& xyzToCam);

struct Validation {
    float average = 0.f;
    float maximum = 0.f;
    float exposure = 1.f;
    float black = 0.f;
};

/**
 * @brief _validate_color_checker(): the difference of the chart in image (the node's output)
 * from its references, without exposure normalisation.
 **/
std::optional<Validation> validateColorChecker(const RgbImage& image,
                                               const ChartPlacement& placement,
                                               const RawColorCalibrationMath::Mat3& rgbToXyz,
                                               const RawColorCalibrationMath::Mat3& xyzToCam);

/**
 * @brief The verdict on an average delta E: very good, good, passable or bad.
 **/
const char* qualityDiagnostic(float averageDeltaE);

/**
 * @brief darktable's quality reports, as plain text.
 **/
std::string profileReport(const Profile& profile);
std::string validationReport(const Validation& validation);
} // namespace RawColorCalibrationChecker

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorCalibrationChecker_h
