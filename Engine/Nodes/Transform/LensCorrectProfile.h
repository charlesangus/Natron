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

#ifndef Engine_Nodes_Transform_LensCorrectProfile_h
#define Engine_Nodes_Transform_LensCorrectProfile_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

struct lfLens;

NATRON_NAMESPACE_ENTER

/**
 * @brief lensfun's correction for one lens and image, flattened into a fixed list of steps that the
 * CPU evaluates here and lenscorrect.slang evaluates on the GPU.
 *
 * lfModifier keeps its callbacks private, so build() repeats its Enable* calls (coefficient
 * rescaling, callback choice and priority order) and map()/vignettingGain() repeat the callbacks'
 * arithmetic, in the same float/double precision. lfModifier itself is the reference the tests
 * compare against.
 **/
namespace LensCorrectProfile {
enum class CoordOp : std::uint32_t {
    Scale = 0,
    DistPoly3,
    UnDistPoly3,
    DistPoly5,
    UnDistPoly5,
    DistPTLens,
    UnDistPTLens,
    DistACM,
    GeomFishEyeRect,
    GeomRectFishEye,
    GeomPanoramicRect,
    GeomRectPanoramic,
    GeomFishEyePanoramic,
    GeomPanoramicFishEye,
    GeomERectRect,
    GeomRectERect,
    GeomERectFishEye,
    GeomFishEyeERect,
    GeomERectPanoramic,
    GeomPanoramicERect,
    GeomOrthographicERect,
    GeomERectOrthographic,
    GeomStereographicERect,
    GeomERectStereographic,
    GeomEquisolidERect,
    GeomERectEquisolid,
    GeomThobyERect,
    GeomERectThoby,
};

enum class TcaOp : std::uint32_t {
    None = 0,
    Linear,
    Poly3,
    UnPoly3,
    ACM,
};

enum class VignettingOp : std::uint32_t {
    None = 0,
    // Divide by the falloff (correct mode).
    Devignette,
    // Multiply by the falloff (distort mode).
    Vignette,
};

// Distortion, up to two projection steps and the scale.
inline constexpr int kMaxCoordSteps = 4;

struct CoordStep {
    CoordOp op = CoordOp::Scale;
    float terms[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
};

struct Profile {
    double normScale = 1.;
    double normUnScale = 1.;
    double centerX = 0.;
    double centerY = 0.;
    int nCoordSteps = 0;
    CoordStep coord[kMaxCoordSteps];
    TcaOp tca = TcaOp::None;
    float tcaTerms[12] = {};
    VignettingOp vignetting = VignettingOp::None;
    float vignettingTerms[3] = { 0.f, 0.f, 0.f };

    bool movesPixels() const { return nCoordSteps > 0 || tca != TcaOp::None; }

    bool hasVignetting() const { return vignetting != VignettingOp::None; }

    /**
     * @brief lfModifier::ApplySubpixelGeometryDistortion for the single pixel (xu, yu): out holds
     * the red, green and blue source positions as x, y pairs, in lensfun's pixel coordinates (pixel
     * centres on integers, y down). A position the model has no answer for comes back NaN or huge.
     **/
    void map(float xu, float yu, float out[6]) const;

    /**
     * @brief The factor lfModifier::ApplyColorModification multiplies a pixel at (x, y) by.
     **/
    float vignettingGain(float x, float y) const;
};

struct Request {
    // The model's image, in pixels: the full-resolution format of the corrected image.
    int width = 0;
    int height = 0;
    float focal = 0.f;
    float crop = 0.f;
    float aperture = 0.f;
    float distance = 1000.f;
    // Distort (apply the lens) instead of correct.
    bool reverse = false;
    bool distortion = true;
    bool tca = true;
    bool vignetting = true;
    // lfLensType to reproject to; LF_UNKNOWN keeps the lens's own projection.
    int targetGeometry = 0;
    // lfModifier::EnableScaling's argument: 0 picks the scale that leaves no empty border.
    float scale = 1.f;
    bool tcaOverride = false;
    float tcaRed = 1.f;
    float tcaBlue = 1.f;
};

/**
 * @brief The steps lfModifier(lens, request.focal, request.crop, width, height, LF_PF_F32,
 * reverse) would run once darktable's enables (distortion, projection, scaling, TCA, vignetting)
 * have been applied to it. The lens is read only; database queries are the caller's to lock.
 **/
Profile build(const lfLens& lens, const Request& request);

/**
 * @brief The scale build() applies when request.scale is 0.
 **/
float autoScale(const lfLens& lens, const Request& request);
} // namespace LensCorrectProfile

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensCorrectProfile_h
