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

    } // namespace

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
            out[k] = SpectralData::kCmf2015[i][k] + f * (SpectralData::kCmf2015[i + 1][k] - SpectralData::kCmf2015[i][k]);
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

    Rgb applyMatrix(const Matrix3& m, const Xyz& xyz)
    {
        Rgb out;
        for (int r = 0; r < 3; ++r) {
            out[r] = m[3 * r] * xyz[0] + m[3 * r + 1] * xyz[1] + m[3 * r + 2] * xyz[2];
        }
        return out;
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
            return m;
        } catch (const OCIO_NAMESPACE::Exception&) {
            return kXyzD65ToRec709;
        }
    }

    bool spectralColours(const std::string& illuminant, const std::vector<double>& wavelengthsNm,
                         const Matrix3& xyzToWorking, std::vector<Rgb>* out, std::string* error)
    {
        if (!hasIlluminant(illuminant)) {
            if (error) {
                *error = "unknown illuminant \"" + illuminant + "\"";
            }
            return false;
        }
        std::vector<Rgb> colours;
        colours.reserve(wavelengthsNm.size());
        for (double nm : wavelengthsNm) {
            colours.push_back(applyMatrix(xyzToWorking, spectralXyz(illuminant, nm)));
        }
        *out = std::move(colours);
        return true;
    }

} // namespace Optics
} // namespace Natron
