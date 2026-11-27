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

#include "Engine/Nodes/Scene/DelegateSupport.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"

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

// hdMoonray publishes no aovNames stat, so its table is the list; the table leaves out the AOVs
// that stop a render or that no Hydra render takes. Storm, never made to be asked, lists its own.
TEST(MoonRayAovs, AvailableAovs)
{
    const std::vector<std::string> aovs = DelegateSupport::availableAovs(kMoonRayRendererId);
    if (MoonRayAovs::publishedAovs().empty()) {
        EXPECT_EQ(MoonRayAovs::catalogue(), aovs);
    } else {
        EXPECT_EQ(MoonRayAovs::publishedAovs(), aovs);
    }
    const std::vector<std::string>& table = MoonRayAovs::catalogue();
    for (const char* name : { "color", "cameraDepth", "primId", "instanceId", "normal", "P", "N", "St", "Z", "motionvec" }) {
        EXPECT_TRUE(std::find(table.begin(), table.end(), name) != table.end()) << name;
    }
    for (const char* name : { "wireframe", "elementId", "depth", "primvars:st" }) {
        EXPECT_TRUE(std::find(table.begin(), table.end(), name) == table.end()) << name;
    }
    EXPECT_EQ(std::string("P"), RenderSettingsPrim::delegateAovName("moonray:P", kMoonRayRendererId));
    EXPECT_EQ(std::string("moonray:albedo"), RenderSettingsPrim::delegateAovName("moonray:albedo", kMoonRayRendererId));

    const std::vector<std::string> storm = DelegateSupport::availableAovs(kRenderDelegateStormId);
    for (const char* name : { "color", "depth", "primId", "instanceId", "elementId", "edgeId", "pointId", "Neye" }) {
        EXPECT_TRUE(std::find(storm.begin(), storm.end(), name) != storm.end()) << name;
    }
    for (const char* name : { "normal", "Peye", "depthStencil" }) {
        EXPECT_TRUE(std::find(storm.begin(), storm.end(), name) == storm.end()) << name;
    }
}

// A delegate's aovNames stat, as tokens or strings, is its list over Natron's.
TEST(MoonRayAovs, PublishedAovsWin)
{
    PXR_NS::VtDictionary stats;
    EXPECT_TRUE(DelegateSupport::publishedAovs(stats).empty());
    stats["aovNames"] = PXR_NS::VtValue(PXR_NS::VtArray<PXR_NS::TfToken>({ PXR_NS::TfToken("cycles:ao"), PXR_NS::TfToken("color") }));
    EXPECT_EQ(std::vector<std::string>({ "cycles:ao", "color" }), DelegateSupport::publishedAovs(stats));
    stats["aovNames"] = PXR_NS::VtValue(std::vector<std::string>({ "beauty" }));
    EXPECT_EQ(std::vector<std::string>({ "beauty" }), DelegateSupport::publishedAovs(stats));

    DelegateSupport::setAvailableAovsForTests(kMoonRayRendererId, { "beauty", "albedo" });
    EXPECT_EQ(std::vector<std::string>({ "beauty", "albedo" }), DelegateSupport::availableAovs(kMoonRayRendererId));
    EXPECT_EQ(std::string("albedo"), RenderSettingsPrim::delegateAovName("moonray:albedo", kMoonRayRendererId));
    DelegateSupport::setAvailableAovsForTests(kMoonRayRendererId, {});
    EXPECT_EQ(MoonRayAovs::publishedAovs().empty() ? MoonRayAovs::catalogue() : MoonRayAovs::publishedAovs(), DelegateSupport::availableAovs(kMoonRayRendererId));
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

TEST(MoonRayAovs, KeepsMoonRaysTextForALightAlsoNamedAsUnsupported)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    const PXR_NS::TfTokenVector supported = { PXR_NS::HdPrimTypeTokens->distantLight, PXR_NS::HdPrimTypeTokens->mesh };
    const std::vector<MoonRayAovs::Light> lights = MoonRayAovs::unsupportedLights(stage, supported);
    ASSERT_EQ(1u, lights.size());

    std::vector<MoonRayAovs::Notice> notices;
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Portal: no portal support"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Portal: no portal support"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Portal: bad rotation"));

    const std::string warning = MoonRayAovs::warningText(stage, lights, notices);
    EXPECT_NE(std::string::npos, warning.find("/World/Portal (PortalLight: no portal support; bad rotation)")) << warning;
    EXPECT_EQ(std::string::npos, warning.find("/World/Portal (PortalLight)")) << warning;
}

TEST(MoonRayAovs, KeepsMoonRaysTextForEachPrimOnce)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    std::vector<MoonRayAovs::Notice> notices;
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground/proto_0: unknown Rprim type foo"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground/proto_0: unknown Rprim type foo"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground/proto_0: another problem"));

    const std::string warning = MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), notices);
    EXPECT_EQ(std::string("MoonRay could not render these prims as authored: /World/Ground (unknown Rprim type foo), /World/Ground (another problem)"), warning);
}

TEST(MoonRayAovs, DropsOnlyTheFrameHoldPrimvarNoticesThatAreNotTranslated)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    std::vector<MoonRayAovs::Notice> benign;
    benign.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold1: GfMatrix4d not translated"));
    benign.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold12Column0: VtArray<GfVec4f> not translated"));
    benign.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold1Column2: VtArray<GfVec4f> not translated"));
    const char* const frames[] = { "m20", "m20Column1", "1_5", "m0_25Column2", "1ep20", "m1_5em05", "2_5ep100Column0", "m1ep20" };
    for (const char* frame : frames) {
        benign.push_back(notice(MoonRayAovs::Notice::eLevelWarning, std::string("/World/Ground.primvars:natronProjectionFrameHold") + frame + ": GfMatrix4d not translated"));
    }
    EXPECT_TRUE(MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), benign).empty());

    std::vector<MoonRayAovs::Notice> kept;
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHoldmm1: GfMatrix4d not translated"));
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold1_: GfMatrix4d not translated"));
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold1e: GfMatrix4d not translated"));
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHoldX: GfMatrix4d not translated"));
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:natronProjectionFrameHold1Column3: VtArray<GfVec4f> not translated"));
    kept.push_back(notice(MoonRayAovs::Notice::eLevelWarning, "/World/Ground.primvars:foo: GfMatrix4d not translated"));
    const std::string warning = MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), kept);
    EXPECT_NE(std::string::npos, warning.find("natronProjectionFrameHoldX")) << warning;
    EXPECT_NE(std::string::npos, warning.find("natronProjectionFrameHold1Column3")) << warning;
    EXPECT_NE(std::string::npos, warning.find("(foo: ")) << warning;
}

TEST(MoonRayAovs, NamesTheTextureFilesNotThePrimsTheyShade)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();
    std::vector<MoonRayAovs::Notice> notices;
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "UsdUVTexture(\"/World/Looks/Red/Tex.rgb\"): FATAL: failed to open texture file \"/tex/a.jpg\" (Invalid image file \"/tex/a.jpg\": image was untiled)"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "/World/Ground: FATAL: failed to open texture file \"/tex/b.png\" ()"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "UsdUVTexture(\"/World/Looks/Red/Tex.rgb\"): FATAL: failed to open texture file \"/tex/a.jpg\" ()"));
    notices.push_back(notice(MoonRayAovs::Notice::eLevelError, "Unable to read texture file /tex/c.png"));

    const std::string warning = MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), notices);
    EXPECT_EQ(std::string("MoonRay could not read these texture files: /tex/a.jpg, /tex/b.png, /tex/c.png"), warning);
    EXPECT_TRUE(MoonRayAovs::errorText(stage, notices).empty());
}

TEST(MoonRayAovs, NothingToSayForACleanRender)
{
    const PXR_NS::UsdStageRefPtr stage = lightStage();

    EXPECT_TRUE(MoonRayAovs::warningText(stage, std::vector<MoonRayAovs::Light>(), std::vector<MoonRayAovs::Notice>()).empty());
    EXPECT_TRUE(MoonRayAovs::errorText(stage, std::vector<MoonRayAovs::Notice>()).empty());
}
