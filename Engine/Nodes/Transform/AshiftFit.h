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

#ifndef Engine_Nodes_Transform_AshiftFit_h
#define Engine_Nodes_Transform_AshiftFit_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

#include "Engine/Nodes/Image/TransformMath.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The perspective-correction model of darktable's rotate and perspective module (ashift):
 * its homography, its automatic crop and its Nelder-Mead fit of rotation, lens shift and shear to
 * a set of lines. Coordinates are darktable's: square pixels, pixel (i, j) at (i, j), y down, over
 * an image of width x height pixels. Everything is computed in double where darktable uses float.
 **/
namespace Ashift {

constexpr double kDefaultFocalLength = 28.;
constexpr double kRotationRange = 180.;
constexpr double kLensShiftRange = 2.;
constexpr double kShearRange = 0.5;
constexpr int kMinimumFitLines = 2;

enum LineTypeFlags {
    eLineIrrelevant = 0,
    eLineRelevant = 1 << 0,
    eLineDirVert = 1 << 1,
    eLineSelected = 1 << 2,
    eLineVerticalNotSelected = eLineRelevant | eLineDirVert,
    eLineHorizontalNotSelected = eLineRelevant,
    eLineVerticalSelected = eLineRelevant | eLineDirVert | eLineSelected,
    eLineHorizontalSelected = eLineRelevant | eLineSelected,
    eLineMask = eLineRelevant | eLineDirVert | eLineSelected,
};

enum FitAxisFlags {
    eFitNone = 0,
    eFitRotation = 1 << 0,
    eFitLensVert = 1 << 1,
    eFitLensHor = 1 << 2,
    eFitShear = 1 << 3,
    eFitLinesVert = 1 << 4,
    eFitLinesHor = 1 << 5,
    eFitLensBoth = eFitLensVert | eFitLensHor,
    eFitLinesBoth = eFitLinesVert | eFitLinesHor,
    eFitVertically = eFitRotation | eFitLensVert | eFitLinesVert,
    eFitHorizontally = eFitRotation | eFitLensHor | eFitLinesHor,
    eFitBoth = eFitRotation | eFitLensVert | eFitLensHor | eFitLinesVert | eFitLinesHor,
    eFitVerticallyNoRotation = eFitLensVert | eFitLinesVert,
    eFitHorizontallyNoRotation = eFitLensHor | eFitLinesHor,
    eFitBothNoRotation = eFitLensVert | eFitLensHor | eFitLinesVert | eFitLinesHor,
    eFitBothShear = eFitBoth | eFitShear,
    eFitRotationVerticalLines = eFitRotation | eFitLinesVert,
    eFitRotationHorizontalLines = eFitRotation | eFitLinesHor,
    eFitRotationBothLines = eFitRotation | eFitLinesVert | eFitLinesHor,
};

enum CropModeEnum {
    eCropOff = 0,
    eCropLargest,
    eCropAspect,
};

enum NmsResultEnum {
    eNmsSuccess = 0,
    eNmsNotEnoughLines,
    eNmsDidNotConverge,
    eNmsInsane,
};

void vec3prodn(double* dst, const double* v1, const double* v2);
void vec3norm(double* dst, const double* v);
void vec3lnorm(double* dst, const double* v);
double vec3scalar(const double* v1, const double* v2);
bool vec3isnull(const double* v);

/**
 * @brief The model parameters as the pipeline uses them: rotation in degrees as stored (the
 * negated slider value), the focal length already multiplied by the crop factor, and orthocorr and
 * aspect as the lens model makes them (0 and 1 for the generic model).
 **/
struct Model {
    double rotation = 0.;
    double lensShiftV = 0.;
    double lensShiftH = 0.;
    double shear = 0.;
    double focalLength = kDefaultFocalLength;
    double orthocorr = 0.;
    double aspect = 1.;
};

/**
 * @brief The forward (input to output) or inverse homography for an image of width x height
 * pixels. The inverse is the identity when the forward matrix is singular.
 **/
TransformMath::Mat3 homography(const Model& model, int width, int height, bool inverse);

/**
 * @brief darktable's _isneutral(): the model and crop box leave the image unchanged.
 **/
bool isNeutral(const Model& model, double cl, double cr, double ct, double cb);

/**
 * @brief The output size modify_roi_out() computes for an input of width x height and a crop box,
 * floored, without the bypass for sizes under 4.
 **/
void outputSize(const Model& model, int width, int height, double cl, double cr, double ct, double cb, int* outWidth, int* outHeight);

struct CropBox {
    double cl = 0.;
    double cr = 1.;
    double ct = 0.;
    double cb = 1.;
};

/**
 * @brief do_crop(): the largest axis-aligned rectangle inside the corrected image, of free aspect
 * (eCropLargest) or of the input's aspect (eCropAspect), as margins relative to the corrected
 * image's bounding box. Returns false, with the full box, when the fit fails; eCropOff returns the
 * full box and true.
 **/
bool computeCrop(const Model& model, int width, int height, CropModeEnum mode, CropBox* box);

/**
 * @brief crop_adjust(): moves the crop box so that its centre is at (x, y), relative to the
 * corrected image's bounding box, keeping the rectangle's angle and refitting its size. Returns
 * false and leaves box unchanged when the new box would be under 1% of the image.
 **/
bool adjustCrop(const Model& model, int width, int height, double x, double y, CropBox* box);

struct Line {
    double p1[3] = { 0., 0., 1. };
    double p2[3] = { 0., 0., 1. };
    double L[3] = { 0., 0., 0. };
    double length = 0.;
    double width = 0.;
    double weight = 0.;
    int type = eLineIrrelevant;
};

/**
 * @brief Lines in input image coordinates, with their per-direction counts and weights.
 **/
struct LineSet {
    std::vector<Line> lines;
    int width = 0;
    int height = 0;
    int verticalCount = 0;
    int horizontalCount = 0;
    double verticalWeight = 0.;
    double horizontalWeight = 0.;
};

/**
 * @brief nmsfit(): fits the parameters dir selects to lines, starting from model, and on success
 * writes them into model.
 **/
NmsResultEnum fit(const LineSet& lines, int dir, Model* model);

/**
 * @brief The Nelder-Mead simplex of ashift_nmsimplex.c. Minimises objfunc from start, which it
 * overwrites with the best vertex, and returns the iteration count, maxiter + 1 when it did not
 * converge.
 **/
int simplex(const std::function<double(const double*)>& objfunc,
            double* start,
            int n,
            double epsilon,
            double scale,
            int maxiter,
            const std::function<void(double*, int)>& constrain);

} // namespace Ashift

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AshiftFit_h
