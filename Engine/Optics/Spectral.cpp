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

/*
 * Ported in part from realflare (https://github.com/beatreichenbach/realflare v1.0.3,
 * GPL-3.0-or-later), with the sources named in Spectral.h; this port is GPL-3.0-or-later.
 */

#include "Engine/Optics/Spectral.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Engine/Optics/SpectralData.h"

namespace Natron {
namespace Optics {

    namespace {

        const SpectralData::Illuminant* findIlluminant(const std::string& name)
        {
            for (const SpectralData::Illuminant& illuminant : SpectralData::kIlluminants) {
                if (name == illuminant.name) {
                    return &illuminant;
                }
            }
            return nullptr;
        }

        // realflare names the XYZ space through this built-in because a config's XYZ colour space may be
        // display-referred, and converting to it from scene_linear would pass through a view transform.
        constexpr const char* kAcesToXyzBuiltin = "UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD";
        constexpr const char* kXyzInterchangeRole = "cie_xyz_d65_interchange";
        constexpr const char* kAcesInterchangeRole = "aces_interchange";

        bool sceneReferredRole(const OCIO_NAMESPACE::ConstConfigRcPtr& config, const char* role)
        {
            if (!config->hasRole(role)) {
                return false;
            }
            OCIO_NAMESPACE::ConstColorSpaceRcPtr space = config->getColorSpace(config->getRoleColorSpace(role));
            return space && space->getReferenceSpaceType() == OCIO_NAMESPACE::REFERENCE_SPACE_SCENE;
        }

        bool matrixOf(const OCIO_NAMESPACE::ConstCPUProcessorRcPtr& cpu, Matrix3* out)
        {
            Matrix3 m {};
            for (int c = 0; c < 3; ++c) {
                float pixel[3] = { 0.f, 0.f, 0.f };
                pixel[c] = 1.f;
                cpu->applyRGB(pixel);
                for (int r = 0; r < 3; ++r) {
                    m[3 * r + c] = pixel[r];
                }
            }
            // A matrix is linear; a conversion with a curve or an offset in it is not, and a basis read of it
            // would be wrong everywhere but at the basis vectors.
            const Xyz probe = { 0.37, 2.5, 0.11 };
            float pixel[3] = { float(probe[0]), float(probe[1]), float(probe[2]) };
            cpu->applyRGB(pixel);
            const Rgb expected = applyMatrix(m, probe);
            for (int r = 0; r < 3; ++r) {
                if (!std::isfinite(pixel[r]) || std::abs(pixel[r] - expected[r]) > 1e-4 * (1. + std::abs(expected[r]))) {
                    return false;
                }
            }
            *out = m;
            return true;
        }

        Matrix3 multiply(const Matrix3& a, const Matrix3& b)
        {
            Matrix3 out {};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    for (int k = 0; k < 3; ++k) {
                        out[3 * r + c] += a[3 * r + k] * b[3 * k + c];
                    }
                }
            }
            return out;
        }

        bool invert(const Matrix3& m, Matrix3* out)
        {
            const Matrix3 cofactor = {
                m[4] * m[8] - m[5] * m[7],
                m[2] * m[7] - m[1] * m[8],
                m[1] * m[5] - m[2] * m[4],
                m[5] * m[6] - m[3] * m[8],
                m[0] * m[8] - m[2] * m[6],
                m[2] * m[3] - m[0] * m[5],
                m[3] * m[7] - m[4] * m[6],
                m[1] * m[6] - m[0] * m[7],
                m[0] * m[4] - m[1] * m[3],
            };
            const double det = m[0] * cofactor[0] + m[1] * cofactor[3] + m[2] * cofactor[6];
            if (!std::isfinite(det) || det == 0.) {
                return false;
            }
            for (int i = 0; i < 9; ++i) {
                (*out)[i] = cofactor[i] / det;
            }
            return true;
        }

        Xyz whiteXyz(const Chromaticity& xy)
        {
            return { xy[0] / xy[1], 1., (1. - xy[0] - xy[1]) / xy[1] };
        }

        // The Bradford cone response OCIO's _BFD built-ins adapt with, so that this undoes theirs exactly.
        const Matrix3 kBradford = {
            0.8951,
            0.2664,
            -0.1614,
            -0.7502,
            1.7135,
            0.0367,
            0.0389,
            -0.0685,
            1.0296,
        };

        Matrix3 bradfordAdaptation(const Chromaticity& from, const Chromaticity& to)
        {
            Matrix3 inverse;
            invert(kBradford, &inverse);
            const Rgb src = applyMatrix(kBradford, whiteXyz(from));
            const Rgb dst = applyMatrix(kBradford, whiteXyz(to));
            const Matrix3 scale = { dst[0] / src[0], 0., 0., 0., dst[1] / src[1], 0., 0., 0., dst[2] / src[2] };
            return multiply(inverse, multiply(scale, kBradford));
        }

        // Interop IDs are [namespace:]<encoding>_<gamut>_<scene|display>.
        bool whiteOfInteropId(const std::string& id, Chromaticity* white)
        {
            const size_t last = id.rfind('_');
            if (last == std::string::npos || last == 0) {
                return false;
            }
            const size_t before = id.rfind('_', last - 1);
            if (before == std::string::npos) {
                return false;
            }
            const std::string gamut = id.substr(before + 1, last - before - 1);
            if (gamut == "ap0" || gamut == "ap1") {
                *white = kWhiteAces;
                return true;
            }
            for (const char* d65 : { "rec709", "rec2020", "p3d65", "adobergb", "ciexyzd65" }) {
                if (gamut == d65) {
                    *white = kWhiteD65;
                    return true;
                }
            }
            return false;
        }

    } // namespace

    const Chromaticity kWhiteD65 = { 0.3127, 0.3290 };
    const Chromaticity kWhiteAces = { 0.32168, 0.33767 };

    const Matrix3 kXyzD65ToRec709 = {
        3.2404542,
        -1.5371385,
        -0.4985314,
        -0.9692660,
        1.8760108,
        0.0415560,
        0.0556434,
        -0.2040259,
        1.0572252,
    };

    std::vector<double> sampleWavelengths(int count)
    {
        std::vector<double> out;
        if (count <= 0) {
            return out;
        }
        out.reserve(count);
        for (int i = 0; i < count; ++i) {
            out.push_back(kSpectralMinNm + (i + 0.5) / count * (kSpectralMaxNm - kSpectralMinNm));
        }
        return out;
    }

    std::vector<std::string> illuminantNames()
    {
        std::vector<std::string> out;
        for (const SpectralData::Illuminant& illuminant : SpectralData::kIlluminants) {
            out.emplace_back(illuminant.name);
        }
        return out;
    }

    bool hasIlluminant(const std::string& name)
    {
        return findIlluminant(name) != nullptr;
    }

    Xyz colourMatching(double nm)
    {
        const double t = std::clamp(nm - SpectralData::kCmfFirstNm, 0., double(SpectralData::kCmfCount - 1));
        const int i = std::min(int(t), SpectralData::kCmfCount - 2);
        const double f = t - i;
        Xyz out;
        for (int k = 0; k < 3; ++k) {
            out[k] = SpectralData::kCmf1931[i][k] + f * (SpectralData::kCmf1931[i + 1][k] - SpectralData::kCmf1931[i][k]);
        }
        return out;
    }

    double illuminantPower(const std::string& name, double nm)
    {
        const SpectralData::Illuminant* illuminant = findIlluminant(name);
        if (!illuminant) {
            return 0.;
        }
        const double t = std::clamp((nm - illuminant->firstNm) / illuminant->stepNm, 0., double(illuminant->count - 1));
        const int i = std::min(int(t), illuminant->count - 2);
        const double f = t - i;
        return illuminant->power[i] + f * (illuminant->power[i + 1] - illuminant->power[i]);
    }

    Xyz spectralXyz(const std::string& illuminant, double nm)
    {
        const Xyz cmf = colourMatching(nm);
        const double power = illuminantPower(illuminant, nm) * 0.01;
        return { cmf[0] * power, cmf[1] * power, cmf[2] * power };
    }

    namespace {

        Xyz summedXyz(const std::string& illuminant, const std::vector<double>& wavelengthsNm)
        {
            Xyz sum = { 0., 0., 0. };
            for (double nm : wavelengthsNm) {
                const Xyz xyz = spectralXyz(illuminant, nm);
                for (int k = 0; k < 3; ++k) {
                    sum[k] += xyz[k];
                }
            }
            return sum;
        }

        std::vector<double> everyWholeNm()
        {
            std::vector<double> nm;
            for (int i = int(kCmfMinNm); i <= int(kCmfMaxNm); ++i) {
                nm.push_back(i);
            }
            return nm;
        }

    } // namespace

    Xyz illuminantXyz(const std::string& name)
    {
        if (!hasIlluminant(name)) {
            return { 0., 0., 0. };
        }
        const Xyz sum = summedXyz(name, everyWholeNm());
        return { sum[0] / sum[1], 1., sum[2] / sum[1] };
    }

    Rgb applyMatrix(const Matrix3& m, const Xyz& xyz)
    {
        Rgb out;
        for (int r = 0; r < 3; ++r) {
            out[r] = m[3 * r] * xyz[0] + m[3 * r + 1] * xyz[1] + m[3 * r + 2] * xyz[2];
        }
        return out;
    }

    bool workingSpaceWhite(const OCIO_NAMESPACE::ConstConfigRcPtr& config, Chromaticity* white)
    {
        *white = kWhiteD65;
        if (!config || !config->hasRole(OCIO_NAMESPACE::ROLE_SCENE_LINEAR)) {
            return false;
        }
        OCIO_NAMESPACE::ConstColorSpaceRcPtr space = config->getColorSpace(config->getRoleColorSpace(OCIO_NAMESPACE::ROLE_SCENE_LINEAR));
        return space && whiteOfInteropId(space->getInteropID(), white);
    }

    Matrix3 xyzToWorkingSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config, XyzRoute* route)
    {
        if (route) {
            *route = XyzRoute::Rec709Fallback;
        }
        if (!config || !config->hasRole(OCIO_NAMESPACE::ROLE_SCENE_LINEAR)) {
            return kXyzD65ToRec709;
        }
        try {
            OCIO_NAMESPACE::GroupTransformRcPtr group = OCIO_NAMESPACE::GroupTransform::Create();
            XyzRoute chosen;
            if (sceneReferredRole(config, kXyzInterchangeRole)) {
                chosen = XyzRoute::XyzInterchangeRole;
                OCIO_NAMESPACE::ColorSpaceTransformRcPtr toLinear = OCIO_NAMESPACE::ColorSpaceTransform::Create();
                toLinear->setSrc(kXyzInterchangeRole);
                toLinear->setDst(OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
                group->appendTransform(toLinear);
            } else if (sceneReferredRole(config, kAcesInterchangeRole)) {
                chosen = XyzRoute::AcesInterchangeRole;
                OCIO_NAMESPACE::BuiltinTransformRcPtr fromXyz = OCIO_NAMESPACE::BuiltinTransform::Create();
                fromXyz->setStyle(kAcesToXyzBuiltin);
                fromXyz->setDirection(OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE);
                group->appendTransform(fromXyz);
                OCIO_NAMESPACE::ColorSpaceTransformRcPtr toLinear = OCIO_NAMESPACE::ColorSpaceTransform::Create();
                toLinear->setSrc(kAcesInterchangeRole);
                toLinear->setDst(OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
                group->appendTransform(toLinear);
            } else {
                return kXyzD65ToRec709;
            }
            Matrix3 m;
            if (!matrixOf(config->getProcessor(group)->getDefaultCPUProcessor(), &m)) {
                return kXyzD65ToRec709;
            }
            if (route) {
                *route = chosen;
            }
            Chromaticity white;
            if (workingSpaceWhite(config, &white) && white != kWhiteD65) {
                m = multiply(m, bradfordAdaptation(white, kWhiteD65));
            }
            return m;
        } catch (const OCIO_NAMESPACE::Exception&) {
            return kXyzD65ToRec709;
        }
    }

    bool localWhite(const Matrix3& xyzToWorking, const std::string& illuminant, Matrix3* out)
    {
        Matrix3 inverse;
        if (!hasIlluminant(illuminant) || !invert(xyzToWorking, &inverse)) {
            return false;
        }
        const Xyz xyz = illuminantXyz(illuminant);
        const Rgb rgb = applyMatrix(xyzToWorking, xyz);
        const double greyLuminance = applyMatrix(inverse, { 1., 1., 1. })[1];
        if (!(greyLuminance > 0.)) {
            return false;
        }
        const double grey = xyz[1] / greyLuminance;
        Matrix3 balanced = xyzToWorking;
        for (int r = 0; r < 3; ++r) {
            if (!(rgb[r] > 0.)) {
                return false;
            }
            for (int c = 0; c < 3; ++c) {
                balanced[3 * r + c] *= grey / rgb[r];
            }
        }
        *out = balanced;
        return true;
    }

    bool spectralColours(const std::string& illuminant, const std::vector<double>& wavelengthsNm,
                         const Matrix3& xyzToWorking, SpectralWhite white, std::vector<Rgb>* out,
                         std::string* error)
    {
        if (!hasIlluminant(illuminant)) {
            if (error) {
                *error = "unknown illuminant \"" + illuminant + "\"";
            }
            return false;
        }
        Matrix3 m = xyzToWorking;
        if (white == SpectralWhite::WorkingSpace && !localWhite(xyzToWorking, illuminant, &m)) {
            if (error) {
                *error = "illuminant \"" + illuminant + "\" is outside the working space";
            }
            return false;
        }
        // A few samples do not integrate to the illuminant's colour or brightness: 5 samples of D65
        // sum to a tint, and the sum grows with the count. Scaling X, Y and Z by the dense sum over the
        // sampled one makes every sample set sum to what the 1 nm integral gives.
        const Xyz dense = summedXyz(illuminant, everyWholeNm());
        const Xyz sampled = summedXyz(illuminant, wavelengthsNm);
        Xyz balance = { 1., 1., 1. };
        if (!wavelengthsNm.empty()) {
            for (int k = 0; k < 3; ++k) {
                if (!(sampled[k] > 0.)) {
                    if (error) {
                        *error = "the wavelengths miss one of X, Y and Z under illuminant \"" + illuminant + "\"";
                    }
                    return false;
                }
                balance[k] = dense[k] / sampled[k];
            }
        }
        std::vector<Rgb> colours;
        colours.reserve(wavelengthsNm.size());
        for (double nm : wavelengthsNm) {
            const Xyz xyz = spectralXyz(illuminant, nm);
            colours.push_back(applyMatrix(m, { xyz[0] * balance[0], xyz[1] * balance[1], xyz[2] * balance[2] }));
        }
        *out = std::move(colours);
        return true;
    }

} // namespace Optics
} // namespace Natron
