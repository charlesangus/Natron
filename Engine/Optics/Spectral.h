/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_ENGINE_OPTICS_SPECTRAL_H
#define NATRON_ENGINE_OPTICS_SPECTRAL_H

/*
 * Wavelength to working-space colour, ported from realflare's get_spectral and get_wavelengths
 * (https://github.com/beatreichenbach/realflare v1.0.3, flare/engine/tasks/common/color.py,
 * flare/engine/tasks/raytrace.py and flare/ocio.py, GPL-3.0-or-later); this port is
 * GPL-3.0-or-later. A wavelength's colour is the
 * CIE 1931 2 degree colour matching functions weighted by the illuminant's power / 100, as CIE XYZ,
 * then turned into the working space by one 3x3 matrix. The 1931 observer is the one RGB working
 * spaces are defined with, so a space's own white illuminant renders neutral and others keep their
 * tint.
 */

#include <array>
#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

namespace Natron {
namespace Optics {

    constexpr double kSpectralMinNm = 390.;
    constexpr double kSpectralMaxNm = 730.;
    // The span of the colour matching function table, which illuminantXyz integrates over.
    constexpr double kCmfMinNm = 360.;
    constexpr double kCmfMaxNm = 830.;

    using Rgb = std::array<double, 3>;
    using Xyz = std::array<double, 3>;
    // Row-major: out[r] = sum over c of m[3 * r + c] * in[c].
    using Matrix3 = std::array<double, 9>;

    // CIE xy.
    using Chromaticity = std::array<double, 2>;
    extern const Chromaticity kWhiteD65;
    extern const Chromaticity kWhiteAces;

    // CIE XYZ (D65) to linear Rec.709.
    extern const Matrix3 kXyzD65ToRec709;

    // count wavelengths at the centres of count equal bins over [kSpectralMinNm, kSpectralMaxNm].
    std::vector<double> sampleWavelengths(int count);

    // A, B, C, D50, D55, D60, D65, D75, E, FL1 ... FL11.
    std::vector<std::string> illuminantNames();
    bool hasIlluminant(const std::string& name);

    // Both interpolate linearly and hold the end values outside their tables. illuminantPower is 0 for
    // an unknown name.
    Xyz colourMatching(double nm);
    double illuminantPower(const std::string& name, double nm);

    // colourMatching(nm) * illuminantPower(illuminant, nm) * 0.01.
    Xyz spectralXyz(const std::string& illuminant, double nm);

    // spectralXyz summed at every whole nm over [kCmfMinNm, kCmfMaxNm], scaled to Y = 1. Zero for an
    // unknown name.
    Xyz illuminantXyz(const std::string& name);

    Rgb applyMatrix(const Matrix3& m, const Xyz& xyz);

    enum class SpectralWhite {
        // An illuminant renders with the colour it has in the working space: D65 is neutral in a D65
        // space, E is warm there.
        Physical,
        // "Local White": each colour is scaled per channel so the illuminant renders neutral at its own
        // luminance in the working space.
        WorkingSpace,
    };

    enum class XyzRoute {
        // The config's cie_xyz_d65_interchange role is scene-referred: role to scene_linear.
        XyzInterchangeRole,
        // The inverse ACES AP0 to CIE XYZ D65 utility built-in, then aces_interchange to scene_linear.
        AcesInterchangeRole,
        // kXyzD65ToRec709: no config, no usable role, or a conversion that is not a matrix.
        Rec709Fallback,
    };

    // The white of the config's scene_linear role, read from its Color Interop Forum interop ID. False,
    // with white set to kWhiteD65, for a config, role or interop ID that does not name a known white.
    bool workingSpaceWhite(const OCIO_NAMESPACE::ConstConfigRcPtr& config, Chromaticity* white);

    // CIE XYZ to the config's scene_linear role, without chromatic adaptation: the white of
    // workingSpaceWhite goes to R = G = B. OCIO's XYZ routes adapt to D65, so a Bradford adaptation from
    // the working white to D65 is put in front to undo that. A null config gives the fallback.
    Matrix3 xyzToWorkingSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config, XyzRoute* route = nullptr);

    // xyzToWorking with its rows scaled so illuminantXyz(illuminant) goes to grey of the same luminance.
    // False for an unknown illuminant or one with a channel at or below zero in the working space.
    bool localWhite(const Matrix3& xyzToWorking, const std::string& illuminant, Matrix3* out);

    // One colour per wavelength: applyMatrix(xyzToWorking, b * spectralXyz(illuminant, nm)), through
    // localWhite for SpectralWhite::WorkingSpace, where b scales X, Y and Z so that the colours sum to
    // the spectralXyz sum at every whole nm over [kCmfMinNm, kCmfMaxNm]: any sample set has the dense
    // integral's tint (neutral under Local White) and brightness, whatever its count. False, with out
    // untouched, when the illuminant is unknown, localWhite fails, or the wavelengths sum to 0 in one
    // of X, Y and Z.
    bool spectralColours(const std::string& illuminant, const std::vector<double>& wavelengthsNm,
                         const Matrix3& xyzToWorking, SpectralWhite white, std::vector<Rgb>* out,
                         std::string* error = nullptr);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_SPECTRAL_H
