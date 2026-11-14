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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "UtilityLight.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdLux/blackbody.h>

#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// One-degree steps keep the resampling error below what the files' own spacing allows.
const int kIesThetaSteps = 181;
const int kIesPhiSteps = 360;
// Photometric type C, the only one in use for luminaires: vertical angles from the nadir.
const int kIesTypeC = 1;

// The whitespace- or comma-separated numbers of an LM-63 body, in order.
class IesNumbers {
public:
    explicit IesNumbers(const std::string& text)
    {
        std::string cleaned = text;

        std::replace(cleaned.begin(), cleaned.end(), ',', ' ');
        _stream.str(cleaned);
    }

    bool next(double* value)
    {
        return static_cast<bool>(_stream >> *value);
    }

    bool next(int* value)
    {
        double d = 0.;

        if (!next(&d) || !std::isfinite(d)) {
            return false;
        }
        *value = static_cast<int>(std::lround(d));

        return true;
    }

private:
    std::istringstream _stream;
};

// The candela of the file's own table at a vertical and horizontal angle in degrees, folding the
// horizontal angle by the symmetry the last horizontal angle declares.
double
fileCandela(const std::vector<double>& vertical,
            const std::vector<double>& horizontal,
            const std::vector<double>& values,
            double theta,
            double phi)
{
    const double last = horizontal.back();

    phi = std::fmod(phi, 360.);
    if (phi < 0.) {
        phi += 360.;
    }
    if (horizontal.size() == 1) {
        phi = horizontal[0];
    } else if (last == 90.) {
        if (phi > 180.) {
            phi = 360. - phi;
        }
        if (phi > 90.) {
            phi = 180. - phi;
        }
    } else if (last == 180.) {
        if (phi > 180.) {
            phi = 360. - phi;
        }
    }

    const auto column = [&](std::size_t h) {
        const double* row = values.data() + h * vertical.size();

        if (theta < vertical.front() || theta > vertical.back()) {
            return 0.;
        }

        const std::size_t upper = std::min<std::size_t>(vertical.size() - 1, std::upper_bound(vertical.begin(), vertical.end(), theta) - vertical.begin());

        if (upper == 0) {
            return row[0];
        }

        const std::size_t lower = upper - 1;
        const double span = vertical[upper] - vertical[lower];
        const double f = span > 0. ? (theta - vertical[lower]) / span : 0.;

        return row[lower] + std::max(0., std::min(1., f)) * (row[upper] - row[lower]);
    };

    if (horizontal.size() == 1 || phi <= horizontal.front()) {
        return column(0);
    }
    if (phi >= last) {
        return column(horizontal.size() - 1);
    }

    const std::size_t upper = std::upper_bound(horizontal.begin(), horizontal.end(), phi) - horizontal.begin();
    const std::size_t lower = upper - 1;
    const double span = horizontal[upper] - horizontal[lower];
    const double f = span > 0. ? (phi - horizontal[lower]) / span : 0.;

    return column(lower) + f * (column(upper) - column(lower));
}

float
floatParam(HdSceneDelegate* delegate,
           const SdfPath& id,
           const TfToken& name,
           float fallback)
{
    const VtValue value = delegate->GetLightParamValue(id, name);

    if (value.IsHolding<float>()) {
        return value.UncheckedGet<float>();
    }
    if (value.IsHolding<double>()) {
        return static_cast<float>(value.UncheckedGet<double>());
    }

    return fallback;
}

bool
boolParam(HdSceneDelegate* delegate,
          const SdfPath& id,
          const TfToken& name,
          bool fallback)
{
    return delegate->GetLightParamValue(id, name).GetWithDefault<bool>(fallback);
}

GfVec3f
colorParam(HdSceneDelegate* delegate,
           const SdfPath& id,
           const TfToken& name,
           const GfVec3f& fallback)
{
    const VtValue value = delegate->GetLightParamValue(id, name);

    if (value.IsHolding<GfVec3f>()) {
        return value.UncheckedGet<GfVec3f>();
    }
    if (value.IsHolding<GfVec3d>()) {
        return GfVec3f(value.UncheckedGet<GfVec3d>());
    }

    return fallback;
}

std::string
assetParam(HdSceneDelegate* delegate,
           const SdfPath& id,
           const TfToken& name)
{
    const VtValue value = delegate->GetLightParamValue(id, name);

    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& asset = value.UncheckedGet<SdfAssetPath>();

        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }

    return std::string();
}

TfToken
tokenParam(HdSceneDelegate* delegate,
           const SdfPath& id,
           const TfToken& name)
{
    return delegate->GetLightParamValue(id, name).GetWithDefault<TfToken>(TfToken());
}

UtilityLightDesc::Type
typeOf(const TfToken& lightType)
{
    if (lightType == HdPrimTypeTokens->sphereLight) {
        return UtilityLightDesc::eTypeSphere;
    }
    if (lightType == HdPrimTypeTokens->rectLight) {
        return UtilityLightDesc::eTypeRect;
    }
    if (lightType == HdPrimTypeTokens->diskLight) {
        return UtilityLightDesc::eTypeDisk;
    }
    if (lightType == HdPrimTypeTokens->cylinderLight) {
        return UtilityLightDesc::eTypeCylinder;
    }
    if (lightType == HdPrimTypeTokens->domeLight) {
        return UtilityLightDesc::eTypeDome;
    }

    return UtilityLightDesc::eTypeDistant;
}
} // anon namespace

float
UtilityIesProfile::evaluate(float theta,
                            float phi) const
{
    if (thetaCount < 2 || phiCount < 1 || !(theta >= 0.f) || theta > static_cast<float>(M_PI)) {
        return 0.f;
    }

    const float x = theta / static_cast<float>(M_PI) * (thetaCount - 1);
    const int t0 = std::min(thetaCount - 2, static_cast<int>(x));
    const float ft = x - t0;
    float y = phi / static_cast<float>(2. * M_PI) * phiCount;

    y -= std::floor(y / phiCount) * phiCount;

    const int p0 = std::min(phiCount - 1, static_cast<int>(y));
    const int p1 = (p0 + 1) % phiCount;
    const float fp = phiCount > 1 ? y - p0 : 0.f;
    const auto at = [&](int t, int p) {
        return candela[static_cast<std::size_t>(p) * thetaCount + t];
    };
    const float a = at(t0, p0) + ft * (at(t0 + 1, p0) - at(t0, p0));
    const float b = at(t0, p1) + ft * (at(t0 + 1, p1) - at(t0, p1));

    return a + fp * (b - a);
}

UtilityIesProfileConstPtr
utilityParseIes(const std::string& text,
                std::string* error)
{
    const auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }

        return UtilityIesProfileConstPtr();
    };

    std::istringstream lines(text);
    std::string line;
    bool tiltFound = false;
    std::string tilt;

    while (std::getline(lines, line)) {
        const std::size_t start = line.find_first_not_of(" \t");

        if (start != std::string::npos && line.compare(start, 5, "TILT=") == 0) {
            tilt = line.substr(start + 5);
            tilt.erase(tilt.find_last_not_of(" \t\r") + 1);
            tiltFound = true;
            break;
        }
    }
    if (!tiltFound) {
        return fail("it has no TILT= line");
    }

    std::stringstream rest;

    rest << lines.rdbuf();

    IesNumbers numbers(rest.str());

    if (tilt == "INCLUDE") {
        int geometry = 0;
        int tiltCount = 0;
        double ignored = 0.;

        if (!numbers.next(&geometry) || !numbers.next(&tiltCount) || tiltCount < 0) {
            return fail("its tilt table is truncated");
        }
        for (int i = 0; i < 2 * tiltCount; ++i) {
            if (!numbers.next(&ignored)) {
                return fail("its tilt table is truncated");
            }
        }
    }

    int lamps = 0;
    double lumens = 0.;
    double multiplier = 1.;
    int verticalCount = 0;
    int horizontalCount = 0;
    int photometricType = 0;
    int units = 0;
    double size[3] = { 0., 0., 0. };
    double ballast[3] = { 0., 0., 0. };

    if (!numbers.next(&lamps) || !numbers.next(&lumens) || !numbers.next(&multiplier) || !numbers.next(&verticalCount)
        || !numbers.next(&horizontalCount) || !numbers.next(&photometricType) || !numbers.next(&units)
        || !numbers.next(&size[0]) || !numbers.next(&size[1]) || !numbers.next(&size[2])
        || !numbers.next(&ballast[0]) || !numbers.next(&ballast[1]) || !numbers.next(&ballast[2])) {
        return fail("its header numbers are truncated");
    }
    if (verticalCount < 1 || horizontalCount < 1 || verticalCount > 100000 || horizontalCount > 100000) {
        return fail("it declares " + std::to_string(verticalCount) + " vertical and " + std::to_string(horizontalCount) + " horizontal angles");
    }
    if (photometricType != kIesTypeC) {
        return fail("it uses photometric type " + std::to_string(photometricType) + ", and only type C (1) is read");
    }

    std::vector<double> vertical(verticalCount);
    std::vector<double> horizontal(horizontalCount);
    std::vector<double> values(static_cast<std::size_t>(verticalCount) * horizontalCount);

    for (double& v : vertical) {
        if (!numbers.next(&v)) {
            return fail("its vertical angles are truncated");
        }
    }
    for (double& h : horizontal) {
        if (!numbers.next(&h)) {
            return fail("its horizontal angles are truncated");
        }
    }
    for (double& c : values) {
        if (!numbers.next(&c)) {
            return fail("its candela table is truncated");
        }
        c *= multiplier;
    }
    if (!std::is_sorted(vertical.begin(), vertical.end()) || !std::is_sorted(horizontal.begin(), horizontal.end())) {
        return fail("its angles are not in increasing order");
    }

    std::shared_ptr<UtilityIesProfile> profile = std::make_shared<UtilityIesProfile>();
    const bool symmetric = horizontalCount == 1;

    profile->thetaCount = kIesThetaSteps;
    profile->phiCount = symmetric ? 1 : kIesPhiSteps;
    profile->candela.resize(static_cast<std::size_t>(profile->thetaCount) * profile->phiCount);

    double integral = 0.;
    const double dTheta = M_PI / (profile->thetaCount - 1);
    const double dPhi = 2. * M_PI / profile->phiCount;

    for (int p = 0; p < profile->phiCount; ++p) {
        const double phi = 360. * p / profile->phiCount;

        for (int t = 0; t < profile->thetaCount; ++t) {
            const double theta = 180. * t / (profile->thetaCount - 1);
            const double c = std::max(0., fileCandela(vertical, horizontal, values, theta, phi));
            // The trapezoid's end weights, at the poles, are halved.
            const double weight = (t == 0 || t == profile->thetaCount - 1) ? 0.5 : 1.;

            profile->candela[static_cast<std::size_t>(p) * profile->thetaCount + t] = static_cast<float>(c);
            integral += weight * c * std::sin(theta * M_PI / 180.) * dTheta * dPhi;
        }
    }
    profile->mean = static_cast<float>(integral / (4. * M_PI));

    return profile;
}

UtilityLight::UtilityLight(const SdfPath& id,
                           const TfToken& lightType)
    : HdLight(id)
    , _type(typeOf(lightType))
{
}

UtilityLight::~UtilityLight()
{
}

const TfTokenVector&
UtilityLight::supportedTypes()
{
    static const TfTokenVector types = {
        HdPrimTypeTokens->distantLight,
        HdPrimTypeTokens->sphereLight,
        HdPrimTypeTokens->rectLight,
        HdPrimTypeTokens->diskLight,
        HdPrimTypeTokens->cylinderLight,
        HdPrimTypeTokens->domeLight,
    };

    return types;
}

HdDirtyBits
UtilityLight::GetInitialDirtyBitsMask() const
{
    return HdLight::AllDirty;
}

void
UtilityLight::Sync(HdSceneDelegate* sceneDelegate,
                   HdRenderParam* renderParam,
                   HdDirtyBits* dirtyBits)
{
    const SdfPath& id = GetId();
    UtilityScene* scene = static_cast<UtilityRenderParam*>(renderParam)->scene();

    if (*dirtyBits == HdLight::Clean) {
        return;
    }
    *dirtyBits = HdLight::Clean;
    if (!sceneDelegate->GetVisible(id)) {
        scene->removeLight(id);

        return;
    }

    UtilityLightDesc& d = _desc;

    d.type = _type;
    d.transform = sceneDelegate->GetTransform(id);

    const float intensity = floatParam(sceneDelegate, id, HdLightTokens->intensity, 1.f);
    const float exposure = floatParam(sceneDelegate, id, HdLightTokens->exposure, 0.f);
    GfVec3f color = colorParam(sceneDelegate, id, HdLightTokens->color, GfVec3f(1.f));

    if (boolParam(sceneDelegate, id, HdLightTokens->enableColorTemperature, false)) {
        const GfVec3f blackbody = UsdLuxBlackbodyTemperatureAsRgb(floatParam(sceneDelegate, id, HdLightTokens->colorTemperature, 6500.f));

        color = GfCompMult(color, blackbody);
    }
    d.emission = color * (intensity * std::exp2(exposure));
    d.normalize = boolParam(sceneDelegate, id, HdLightTokens->normalize, false);
    d.angleDegrees = floatParam(sceneDelegate, id, HdLightTokens->angle, 0.53f);
    d.radius = floatParam(sceneDelegate, id, HdLightTokens->radius, 0.5f);
    d.width = floatParam(sceneDelegate, id, HdLightTokens->width, 1.f);
    d.height = floatParam(sceneDelegate, id, HdLightTokens->height, 1.f);
    d.length = floatParam(sceneDelegate, id, HdLightTokens->length, 1.f);
    d.treatAsPoint = boolParam(sceneDelegate, id, TfToken("treatAsPoint"), false);
    d.coneAngleDegrees = floatParam(sceneDelegate, id, HdLightTokens->shapingConeAngle, 180.f);
    d.coneSoftness = floatParam(sceneDelegate, id, HdLightTokens->shapingConeSoftness, 0.f);
    d.focus = floatParam(sceneDelegate, id, HdLightTokens->shapingFocus, 0.f);
    d.focusTint = colorParam(sceneDelegate, id, HdLightTokens->shapingFocusTint, GfVec3f(0.f));
    d.iesAngleScale = floatParam(sceneDelegate, id, HdLightTokens->shapingIesAngleScale, 0.f);
    d.iesNormalize = boolParam(sceneDelegate, id, HdLightTokens->shapingIesNormalize, false);
    d.shadowEnable = boolParam(sceneDelegate, id, HdLightTokens->shadowEnable, true);
    d.lightLink = tokenParam(sceneDelegate, id, HdTokens->lightLink);
    d.shadowLink = tokenParam(sceneDelegate, id, HdTokens->shadowLink);

    const std::string iesPath = assetParam(sceneDelegate, id, HdLightTokens->shapingIesFile);

    if (iesPath != _iesPath) {
        _iesPath = iesPath;
        d.ies.reset();
        d.iesProblem.clear();
        if (!iesPath.empty()) {
            std::ifstream file(iesPath, std::ios::binary);
            std::stringstream text;
            std::string why;

            if (file) {
                text << file.rdbuf();
                d.ies = utilityParseIes(text.str(), &why);
            } else {
                why = "it could not be opened";
            }
            if (!d.ies) {
                d.iesProblem = "The IES profile " + iesPath + " of " + id.GetString() + " is ignored: " + why + ".";
            }
        }
    }

    d.domeTexture = UtilityTextureRef();
    if (_type == UtilityLightDesc::eTypeDome) {
        d.domeTexture.asset = assetParam(sceneDelegate, id, HdLightTokens->textureFile);
        d.domeTexture.wrapS = UtilityTextureRef::eWrapRepeat;
        d.domeTexture.wrapT = UtilityTextureRef::eWrapClamp;
        d.domeTexture.fallback = GfVec4f(1.f);
    }

    scene->setLight(id, std::make_shared<const UtilityLightDesc>(d));
}

void
UtilityLight::Finalize(HdRenderParam* renderParam)
{
    static_cast<UtilityRenderParam*>(renderParam)->scene()->removeLight(GetId());
}

NATRON_NAMESPACE_EXIT
