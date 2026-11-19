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
 * flare/engine/tasks/raytrace.py and flare/ocio.py, GPL-3.0-or-later). A wavelength's colour is the
 * CIE 2015 2 degree colour matching functions weighted by the illuminant's power / 100, as CIE XYZ
 * with a D65 white, then turned into the working space by one 3x3 matrix.
 */

#include <array>
#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

namespace Natron {
namespace Optics {

    constexpr double kSpectralMinNm = 390.;
    constexpr double kSpectralMaxNm = 730.;

    using Rgb = std::array<double, 3>;
    using Xyz = std::array<double, 3>;
    // Row-major: out[r] = sum over c of m[3 * r + c] * in[c].
    using Matrix3 = std::array<double, 9>;

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

    Rgb applyMatrix(const Matrix3& m, const Xyz& xyz);

    enum class XyzRoute {
        // The config's cie_xyz_d65_interchange role is scene-referred: role to scene_linear.
        XyzInterchangeRole,
        // The inverse ACES AP0 to CIE XYZ D65 utility built-in, then aces_interchange to scene_linear.
        AcesInterchangeRole,
        // kXyzD65ToRec709: no config, no usable role, or a conversion that is not a matrix.
        Rec709Fallback,
    };

    // XYZ (D65) to the config's scene_linear role. A null config gives the fallback.
    Matrix3 xyzToWorkingSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config, XyzRoute* route = nullptr);

    // One colour per wavelength: applyMatrix(xyzToWorking, spectralXyz(illuminant, nm)). False, with out
    // untouched, for an unknown illuminant.
    bool spectralColours(const std::string& illuminant, const std::vector<double>& wavelengthsNm,
                         const Matrix3& xyzToWorking, std::vector<Rgb>* out, std::string* error = nullptr);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_SPECTRAL_H
