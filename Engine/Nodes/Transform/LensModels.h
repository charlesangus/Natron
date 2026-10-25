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
 * The lens models are ports of openfx-misc's DistortionModel.cpp (Distortion/), (C) 2013-2018
 * INRIA and 2018-2021 The Natron Developers, GPL v2 or later. The 3DE4 models follow the 3DE4
 * Lens Distortion Plugin Kit (ldpk) definitions they were derived from.
 */

#ifndef Engine_Nodes_Transform_LensModels_h
#define Engine_Nodes_Transform_LensModels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Engine-free lens distortion models with Newton inversion and analytic Jacobians.
 *
 * Points are full-resolution pixel coordinates of the reference format: (0,0) is the bottom left
 * corner of the bottom left pixel, x is not corrected for the pixel aspect ratio and render scale
 * is 1. A caller rendering at a lower scale divides by the render scale before mapping; the
 * Jacobian needs no rescaling since both sides scale alike.
 *
 * Every model is three stages: an affine map `toCore` from pixels into the model's normalized
 * space, a closed-form core evaluated there in one direction (Model::core), and an affine map
 * `fromCore` back to pixels. The other direction is a damped Newton solve of the core in the
 * normalized space, so its tolerance does not depend on the format size.
 *
 * The evaluation code is a C-like subset (POD data, flat coefficient arrays, a switch instead of
 * virtual calls) because the GPU kernels mirror it line for line.
 **/
namespace LensModels {
enum Direction {
    eDirectionDistort = 0,
    eDirectionUndistort
};

/// The order of the `model` choice.
enum ModelType {
    eModelNuke = 0,
    eModelPFBarrel,
    eModel3DEClassic,
    eModel3DEAnamorphic6,
    eModel3DEFisheye8,
    eModel3DEStandard,
    eModel3DEAnamorphic4,
    eModelPanoTools
};

/// The reference format in full-resolution pixels, and its pixel aspect ratio.
struct Frame {
    double x1, y1, x2, y2;
    double par;
};

/// p' = M p + t, M = [[m00, m01], [m10, m11]].
struct Affine {
    double m00, m01, m10, m11;
    double tx, ty;
};

const int kMaxCoeffs = 24;

/// Newton is stopped, and reports failure, after this many iterations.
const int kNewtonMaxIterations = 20;
/// A Newton step that does not reduce the residual is halved at most this many times.
const int kNewtonMaxHalvings = 10;
/// Residual norm, in the core's normalized units, below which the Newton solve has converged.
const double kNewtonTolerance = 1e-12;
/// Jacobian determinants at or below this magnitude are singular.
const double kMinDeterminant = 1e-14;

/// A 2x2 Jacobian, in the layout resampleRowFromCoords() reads: Jxx = dx'/dx, Jxy = dx'/dy, Jyx = dy'/dx, Jyy = dy'/dy.
struct Sample {
    double x, y;
    double J[4];
};

struct Model {
    /// False when the parameters give a singular or non-finite mapping; map() then always fails.
    bool valid;
    ModelType type;
    Direction core;
    Affine toCore;
    Affine fromCore;
    Affine toCoreInverse;
    Affine fromCoreInverse;
    double c[kMaxCoeffs];

    /// False, with *out untouched, outside the model's domain or when Newton does not converge.
    bool map(Direction direction, double x, double y, Sample* out) const;
    bool distort(double x, double y, double* xd, double* yd) const;
    bool undistort(double x, double y, double* xu, double* yu) const;
    bool jacobian(double x, double y, Direction direction, double J[4]) const;
};

struct NukeParams {
    double k1, k2;
    /// Offset of the distortion centre from the format centre, in normalized units.
    double centerX, centerY;
    double anamorphicSqueeze;
    double asymmetricX, asymmetricY;
};

struct PanoToolsParams {
    double a, b, c;
    /// Shift in full-resolution pixels; e is measured downwards.
    double d, e;
    /// Shear in full-resolution pixels.
    double g, t;
};

struct PFBarrelParams {
    double c3, c5;
    /// Distortion centre as a fraction of the format width and height.
    double centerX, centerY;
    double squeeze;
};

Model makeNuke(const Frame& frame, const NukeParams& params);
Model makePanoTools(const Frame& frame, const PanoToolsParams& params);
Model makePFBarrel(const Frame& frame, const PFBarrelParams& params);

/// Fills the inverse affines and Model::valid once a builder has set everything else.
void finalizeModel(Model* model);

/// One closed-form core evaluation in normalized space. False outside the domain or when not finite.
bool evaluateCore(const Model& model, double x, double y, double* gx, double* gy, double J[4]);

/// Central differences of Model::map() with step h, in pixels.
bool jacobianFiniteDifference(const Model& model, Direction direction, double x, double y, double h, double J[4]);

Affine affineCompose(const Affine& outer, const Affine& inner);
/// False, with *out untouched, when the linear part is singular.
bool affineInvert(const Affine& a, Affine* out);

struct PFBarrelKey {
    int frame;
    double c3, c5;
    double centerX, centerY;
};

/// A PFBarrel .pfb file (The Pixel Farm, "#PFBarrel 2011 v1" and "v2").
struct PFBarrelFile {
    int version;
    int originalWidth, originalHeight;
    double originalPixelAspect;
    int undistortedWidth, undistortedHeight;
    bool highOrder;
    double squeeze;
    std::vector<PFBarrelKey> keys;
};

bool parsePFBarrelFile(const std::string& text, PFBarrelFile* file, std::string* error);
} // namespace LensModels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensModels_h
