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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdLux/portalLight.h>

#include "Engine/Nodes/Scene/MoonRayAovs.h"

NATRON_NAMESPACE_USING

// What MoonRayAovs decides without MoonRay: these run in every build.

TEST(MoonRayAovs, CameraDepthMarksMissesInfinite)
{
    EXPECT_EQ(5.25f, MoonRayAovs::cameraDepth(5.25f));
    EXPECT_TRUE(std::isinf(MoonRayAovs::cameraDepth(std::numeric_limits<float>::infinity())));
    EXPECT_TRUE(std::isinf(MoonRayAovs::cameraDepth(std::numeric_limits<float>::quiet_NaN())));
    EXPECT_TRUE(std::isinf(MoonRayAovs::cameraDepth(0.f)));
    EXPECT_TRUE(std::isinf(MoonRayAovs::cameraDepth(-1.f)));
}

TEST(MoonRayAovs, AsksForBeautyDepthIdsAndNormal)
{
    const PXR_NS::TfTokenVector& aovs = MoonRayAovs::aovs();
    for (const PXR_NS::TfToken& name : { PXR_NS::HdAovTokens->color, PXR_NS::TfToken("cameraDepth"), PXR_NS::HdAovTokens->primId,
                                         PXR_NS::HdAovTokens->instanceId, PXR_NS::HdAovTokens->normal }) {
        EXPECT_TRUE(std::find(aovs.begin(), aovs.end(), name) != aovs.end()) << name.GetString();
    }
    // hdMoonray's depth is remapped to a window depth; the distance is cameraDepth.
    EXPECT_TRUE(std::find(aovs.begin(), aovs.end(), PXR_NS::HdAovTokens->depth) == aovs.end());
}

TEST(MoonRayAovs, ReservedCoresLeaveTheBudget)
{
    EXPECT_EQ(14, MoonRayAovs::reservedCores(2, 16));
    EXPECT_EQ(1, MoonRayAovs::reservedCores(15, 16));
    // Arras renders on hardware_concurrency() minus the reserve: at least one is always reserved,
    // as hdMoonray does by default.
    EXPECT_EQ(1, MoonRayAovs::reservedCores(16, 16));
    EXPECT_EQ(1, MoonRayAovs::reservedCores(64, 16));
    EXPECT_EQ(1, MoonRayAovs::reservedCores(0, 16));
    EXPECT_EQ(1, MoonRayAovs::reservedCores(4, 1));
}

TEST(MoonRayAovs, SettingsUseHdMoonrayNames)
{
    const std::vector<std::pair<PXR_NS::TfToken, PXR_NS::VtValue>> settings = MoonRayAovs::renderSettings(2);
    bool reserved = false;
    bool retries = false;
    for (const std::pair<PXR_NS::TfToken, PXR_NS::VtValue>& setting : settings) {
        // hdMoonray reads both as int.
        EXPECT_TRUE(setting.second.IsHolding<int>()) << setting.first.GetString();
        reserved = reserved || (setting.first == PXR_NS::TfToken("localReservedCores"));
        retries = retries || (setting.first == PXR_NS::TfToken("maxConnectRetries"));
    }
    EXPECT_TRUE(reserved);
    EXPECT_TRUE(retries);
}

TEST(MoonRayAovs, SessionFailureFromStatsOrLog)
{
    PXR_NS::VtDictionary stats;
    std::vector<MoonRayAovs::Notice> notices;
    EXPECT_TRUE(MoonRayAovs::sessionFailure(stats, notices).empty());

    stats["renderProgressAnnotation"] = PXR_NS::VtValue(std::string("Render Prep 40%"));
    EXPECT_TRUE(MoonRayAovs::sessionFailure(stats, notices).empty());
    stats["renderProgressAnnotation"] = PXR_NS::VtValue(std::string("Disconnected"));
    EXPECT_FALSE(MoonRayAovs::sessionFailure(stats, notices).empty());

    stats.clear();
    MoonRayAovs::Notice notice;
    notice.level = MoonRayAovs::Notice::eLevelError;
    notice.text = "Failed to create an Arras session: no execComp";
    notices.push_back(notice);
    EXPECT_EQ(notice.text, MoonRayAovs::sessionFailure(stats, notices));

    notices.back().text = "Arras session stopped, computation exited";
    EXPECT_EQ(notices.back().text, MoonRayAovs::sessionFailure(stats, notices));

    // A warning that merely mentions a session is not a failure.
    notices.back().level = MoonRayAovs::Notice::eLevelWarning;
    EXPECT_TRUE(MoonRayAovs::sessionFailure(stats, notices).empty());
}

namespace {
PXR_NS::UsdStageRefPtr
lightStage()
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    (void)PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World"));
    (void)PXR_NS::UsdLuxDistantLight::Define(stage, PXR_NS::SdfPath("/World/Key"));
    (void)PXR_NS::UsdLuxPortalLight::Define(stage, PXR_NS::SdfPath("/World/Portal"));
    (void)PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath("/World/Ground"));
    PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, PXR_NS::SdfPath("/World/Looks/Red"));
    (void)PXR_NS::UsdShadeShader::Define(stage, material.GetPath().AppendChild(PXR_NS::TfToken("Surface")));

    return stage;
}

MoonRayAovs::Notice
notice(MoonRayAovs::Notice::LevelEnum level,
       const std::string& text)
{
    MoonRayAovs::Notice result;
    result.level = level;
    result.text = text;

    return result;
}
} // anon namespace

TEST(MoonRayAovs, NamesLightsTheDelegateCannotRender)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    const PXR_NS::TfTokenVector supported = { PXR_NS::HdPrimTypeTokens->distantLight, PXR_NS::HdPrimTypeTokens->mesh };

    const std::vector<MoonRayAovs::Light> lights = MoonRayAovs::unsupportedLights(stage, supported);
    ASSERT_EQ(1u, lights.size());
    EXPECT_EQ(PXR_NS::SdfPath("/World/Portal"), lights[0].path);
    EXPECT_EQ(std::string("PortalLight"), lights[0].type);

    const std::string warning = MoonRayAovs::warningText(stage, lights, std::vector<MoonRayAovs::Notice>());
    EXPECT_NE(std::string::npos, warning.find("lights"));
    EXPECT_NE(std::string::npos, warning.find("/World/Portal (PortalLight)"));
    EXPECT_EQ(std::string::npos, warning.find("/World/Key"));
}

TEST(MoonRayAovs, WarnsOnePerKindAndKeepsOtherErrorsForTheNode)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    std::vector<MoonRayAovs::Notice> notices;
    // As hdMoonray words them.
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "/World/Key: Unsupported light type foo replaced by DiskLight"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "/World/Ground.material: /World/Looks/Red has no moonray or fallback shaders"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "/World/Looks/Red/Surface: failed to load the shader DSO"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground/proto_0: unknown Rprim type foo"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "RenderPassState without camera is unsupported"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelInfo, "/World/Ground: info only"));

    const std::string warning = MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), notices);
    std::vector<std::string> lines;
    std::string::size_type start = 0;
    while (start <= warning.size()) {
        const std::string::size_type end = warning.find('\n', start);
        lines.push_back(warning.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    ASSERT_EQ(3u, lines.size()) << warning;
    EXPECT_NE(std::string::npos, lines[0].find("lights"));
    EXPECT_NE(std::string::npos, lines[0].find("/World/Key"));
    EXPECT_NE(std::string::npos, lines[1].find("materials"));
    EXPECT_NE(std::string::npos, lines[1].find("/World/Ground"));
    EXPECT_NE(std::string::npos, lines[1].find("/World/Looks/Red/Surface"));
    EXPECT_NE(std::string::npos, lines[2].find("prims"));
    // An instance proxy path is named by the nearest prim on the stage.
    EXPECT_NE(std::string::npos, lines[2].find("/World/Ground"));

    EXPECT_EQ(std::string("RenderPassState without camera is unsupported"), MoonRayAovs::errorText(stage, notices));
}

TEST(MoonRayAovs, NothingToSayForACleanRender)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();

    EXPECT_TRUE(MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), std::vector<MoonRayAovs::Notice>()).empty());
    EXPECT_TRUE(MoonRayAovs::errorText(stage, std::vector<MoonRayAovs::Notice>()).empty());
}
