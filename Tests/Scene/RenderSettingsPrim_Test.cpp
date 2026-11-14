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

#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
// A renderer no plug-in provides, whose settings the tests declare; its namespace is its script prefix.
const char* const kRenderer = "NatronTestPrimRenderer";
const char* const kNamespace = "natronTestPrimRenderer";

RenderSettingsKnobs::Descriptor
descriptor(const char* key,
           const VtValue& defaultValue)
{
    RenderSettingsKnobs::Descriptor d;

    d.key = key;
    d.name = key;
    d.defaultValue = defaultValue;

    return d;
}

UsdStageRefPtr
stageFrom(const std::string& text)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");

    if (!layer->ImportFromString(text)) {
        return UsdStageRefPtr();
    }

    return UsdStage::Open(layer);
}

const char* const kTwoPrims = "#usda 1.0\n"
                              "(\n"
                              "    renderSettingsPrimPath = \"/Render/A\"\n"
                              ")\n"
                              "def Scope \"Render\"\n"
                              "{\n"
                              "    def RenderSettings \"A\"\n"
                              "    {\n"
                              "        rel camera = </World/Cam>\n"
                              "        uniform int2 resolution = (320, 240)\n"
                              "        uniform float pixelAspectRatio = 2\n"
                              "        uniform token aspectRatioConformPolicy = \"cropAperture\"\n"
                              "        uniform float4 dataWindowNDC = (0, 0.25, 1, 0.75)\n"
                              "        uniform bool disableMotionBlur = 1\n"
                              "        uniform bool disableDepthOfField = 1\n"
                              "        bool natronTestPrimRenderer:flag = 1\n"
                              "        int natronTestPrimRenderer:prefixed:count = 7\n"
                              "        float natronTestPrimRenderer:exposure = 1.5\n"
                              "        double natronTestPrimRenderer:undeclared = 4\n"
                              "        double natronTestPrimRenderer:gain.timeSamples = {\n"
                              "            1: 0.25,\n"
                              "            5: 0.75,\n"
                              "        }\n"
                              "        int cycles:samples = 16\n"
                              "        uniform token natron:renderer = \"HdStormRendererPlugin\"\n"
                              "    }\n"
                              "    def RenderSettings \"B\"\n"
                              "    {\n"
                              "    }\n"
                              "}\n"
                              "def Xform \"World\"\n"
                              "{\n"
                              "    def Camera \"Cam\"\n"
                              "    {\n"
                              "    }\n"
                              "}\n";
} // anon namespace

class RenderSettingsPrimTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        RenderSettingsKnobs::DescriptorList list;
        list.push_back(descriptor("flag", VtValue(false)));
        list.push_back(descriptor("natronTestPrimRenderer:prefixed:count", VtValue(1)));
        list.push_back(descriptor("exposure", VtValue(0.)));
        list.push_back(descriptor("gain", VtValue(1.)));
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, list);
    }

    virtual void TearDown() OVERRIDE
    {
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList());
        BaseTest::TearDown();
    }
};

// Cycles' and MoonRay's delegates name no namespaces, so theirs come from the table; a key in the
// namespace is its own attribute name, any other key gains the namespace.
TEST_F(RenderSettingsPrimTest, KeysMapToAttributesBothWays)
{
    EXPECT_EQ(std::vector<std::string> { "cycles" }, RenderSettingsPrim::namespaces("HdCyclesPlugin"));
    EXPECT_EQ(std::vector<std::string> { "moonray" }, RenderSettingsPrim::namespaces("HdMoonrayRendererPlugin"));
    EXPECT_EQ(std::vector<std::string> { "storm" }, RenderSettingsPrim::namespaces(kRenderDelegateStormId));
    EXPECT_EQ(std::vector<std::string> { kNamespace }, RenderSettingsPrim::namespaces(kRenderer));

    EXPECT_EQ(std::string("cycles:integrator:max_diffuse_bounce"), RenderSettingsPrim::attributeName("HdCyclesPlugin", "cycles:integrator:max_diffuse_bounce"));
    EXPECT_EQ(std::string("moonray:sceneVariable:pixel_samples"), RenderSettingsPrim::attributeName("HdMoonrayRendererPlugin", "moonray:sceneVariable:pixel_samples"));
    EXPECT_EQ(std::string("moonray:debug"), RenderSettingsPrim::attributeName("HdMoonrayRendererPlugin", "debug"));
    EXPECT_EQ(std::string("storm:enableTinyPrimCulling"), RenderSettingsPrim::attributeName(kRenderDelegateStormId, "enableTinyPrimCulling"));
    // A namespace is only a prefix up to its colon.
    EXPECT_EQ(std::string("cycles:cyclesx"), RenderSettingsPrim::attributeName("HdCyclesPlugin", "cyclesx"));

    EXPECT_EQ(std::string("flag"), RenderSettingsPrim::keyFromAttribute(kRenderer, "natronTestPrimRenderer:flag"));
    EXPECT_EQ(std::string("natronTestPrimRenderer:prefixed:count"), RenderSettingsPrim::keyFromAttribute(kRenderer, "natronTestPrimRenderer:prefixed:count"));
    EXPECT_EQ(std::string("natronTestPrimRenderer:undeclared"), RenderSettingsPrim::keyFromAttribute(kRenderer, "natronTestPrimRenderer:undeclared"));
    EXPECT_EQ(std::string(), RenderSettingsPrim::keyFromAttribute(kRenderer, "cycles:samples"));
    EXPECT_EQ(std::string(), RenderSettingsPrim::keyFromAttribute(kRenderer, kRenderSettingsPrimRendererAttribute));
    for (const char* key : { "flag", "natronTestPrimRenderer:prefixed:count", "exposure", "gain" }) {
        EXPECT_EQ(std::string(key), RenderSettingsPrim::keyFromAttribute(kRenderer, RenderSettingsPrim::attributeName(kRenderer, key))) << key;
    }
}

// The prim's attributes in the renderer's namespaces come back by key, at the asked time and as
// the declared type; other namespaces, natron:renderer and the schema attributes are left out.
TEST_F(RenderSettingsPrimTest, SettingsAreTheRenderersNamespaceOnly)
{
    UsdStageRefPtr stage = stageFrom(kTwoPrims);
    ASSERT_TRUE(bool(stage));
    const UsdPrim prim = stage->GetPrimAtPath(SdfPath("/Render/A"));
    ASSERT_TRUE(bool(prim));

    RenderSettingsPrim::Values values = RenderSettingsPrim::settings(prim, kRenderer, UsdTimeCode(1.));
    EXPECT_EQ(5u, values.size());
    EXPECT_EQ(VtValue(true), values["flag"]);
    EXPECT_EQ(VtValue(7), values["natronTestPrimRenderer:prefixed:count"]);
    EXPECT_EQ(VtValue(1.5), values["exposure"]) << "the authored float is sent as the declared double";
    EXPECT_EQ(VtValue(4.), values["natronTestPrimRenderer:undeclared"]);
    EXPECT_EQ(VtValue(0.25), values["gain"]);
    EXPECT_EQ(0u, values.count("cycles:samples"));
    EXPECT_EQ(VtValue(0.75), RenderSettingsPrim::settings(prim, kRenderer, UsdTimeCode(5.))["gain"]);

    values = RenderSettingsPrim::settings(prim, "HdCyclesPlugin", UsdTimeCode::Default());
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ(VtValue(16), values["cycles:samples"]);
    EXPECT_TRUE(RenderSettingsPrim::settings(stage->GetPrimAtPath(SdfPath("/Render/B")), kRenderer, UsdTimeCode::Default()).empty());
}

// The keys Natron sets itself or keeps off stay out of what a prim sends, whichever way they are
// spelled, and the rest of its settings are sent.
TEST_F(RenderSettingsPrimTest, SettingsLeaveOutReservedAndDisabledKeys)
{
    UsdStageRefPtr stage = stageFrom("#usda 1.0\n"
                                     "def RenderSettings \"Prim\"\n"
                                     "{\n"
                                     "    int moonray:localReservedCores = 9999\n"
                                     "    int moonray:maxConnectRetries = 7\n"
                                     "    bool moonray:useRemoteHosts = true\n"
                                     "    string moonray:remoteHosts = \"farm\"\n"
                                     "    int moonray:someSetting = 3\n"
                                     "    bool cycles:integrator:denoise_use_gpu = true\n"
                                     "    int cycles:samples = 16\n"
                                     "}\n");
    ASSERT_TRUE(bool(stage));
    const UsdPrim prim = stage->GetPrimAtPath(SdfPath("/Prim"));
    ASSERT_TRUE(bool(prim));

    const RenderSettingsPrim::Values moonRay = RenderSettingsPrim::settings(prim, kMoonRayRendererId, UsdTimeCode::Default());
    for (const char* key : { "localReservedCores", "maxConnectRetries", "useRemoteHosts", "remoteHosts" }) {
        EXPECT_EQ(0u, moonRay.count(key)) << key;
        EXPECT_EQ(0u, moonRay.count(std::string("moonray:") + key)) << key;
    }
    EXPECT_EQ(1u, moonRay.size());

    const RenderSettingsPrim::Values cycles = RenderSettingsPrim::settings(prim, "HdCyclesPlugin", UsdTimeCode::Default());
    EXPECT_EQ(0u, cycles.count("cycles:integrator:denoise_use_gpu"));
    EXPECT_EQ(1u, cycles.size());
}

// Every cycles: attribute is sent under its own name, whatever node of Cycles' scene it names and
// whether or not the delegate declares it. Storm's skydome reads domeLightCameraVisibility though
// Storm declares no such setting, so the attribute is sent under the bare Hydra key.
TEST_F(RenderSettingsPrimTest, CyclesAttributesAndDomeVisibilityAreSent)
{
    UsdStageRefPtr stage = stageFrom("#usda 1.0\n"
                                     "def RenderSettings \"Prim\"\n"
                                     "{\n"
                                     "    bool cycles:background:transparent = 1\n"
                                     "    float cycles:film:filter_width = 2\n"
                                     "    int cycles:foo:bar = 3\n"
                                     "    bool storm:domeLightCameraVisibility = 0\n"
                                     "}\n");
    ASSERT_TRUE(bool(stage));
    const UsdPrim prim = stage->GetPrimAtPath(SdfPath("/Prim"));
    ASSERT_TRUE(bool(prim));

    RenderSettingsPrim::Values cycles = RenderSettingsPrim::settings(prim, "HdCyclesPlugin", UsdTimeCode::Default());
    EXPECT_EQ(3u, cycles.size());
    EXPECT_EQ(VtValue(true), cycles["cycles:background:transparent"]);
    EXPECT_EQ(VtValue(2.f), cycles["cycles:film:filter_width"]);
    EXPECT_EQ(VtValue(3), cycles["cycles:foo:bar"]);

    RenderSettingsPrim::Values storm = RenderSettingsPrim::settings(prim, kRenderDelegateStormId, UsdTimeCode::Default());
    EXPECT_EQ(1u, storm.size());
    EXPECT_EQ(VtValue(false), storm["domeLightCameraVisibility"]);
    EXPECT_EQ(std::string("storm:domeLightCameraVisibility"), RenderSettingsPrim::attributeName(kRenderDelegateStormId, "domeLightCameraVisibility"));
    // Only Storm reads the bare key: another delegate's attribute of that name keeps its namespace.
    EXPECT_EQ(std::string("cycles:domeLightCameraVisibility"), RenderSettingsPrim::keyFromAttribute("HdCyclesPlugin", "cycles:domeLightCameraVisibility"));
}

// An override path beats the stage's renderSettingsPrimPath; with neither there is no prim, and a
// path naming no settings prim is an error.
TEST_F(RenderSettingsPrimTest, ActivePrimPrecedence)
{
    UsdStageRefPtr stage = stageFrom(kTwoPrims);
    ASSERT_TRUE(bool(stage));
    std::string error;

    EXPECT_EQ(SdfPath("/Render/A"), RenderSettingsPrim::activePrim(stage, std::string(), &error).GetPath());
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(SdfPath("/Render/B"), RenderSettingsPrim::activePrim(stage, "/Render/B", &error).GetPath());
    EXPECT_TRUE(error.empty()) << error;

    EXPECT_FALSE(bool(RenderSettingsPrim::activePrim(stage, "/World/Cam", &error)));
    EXPECT_NE(std::string::npos, error.find("/World/Cam")) << error;

    error.clear();
    stage->GetRootLayer()->EraseField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath);
    EXPECT_FALSE(bool(RenderSettingsPrim::activePrim(stage, std::string(), &error)));
    EXPECT_TRUE(error.empty()) << error;

    stage->SetMetadata(UsdRenderTokens->renderSettingsPrimPath, std::string("/Render/Missing"));
    EXPECT_FALSE(bool(RenderSettingsPrim::activePrim(stage, std::string(), &error)));
    EXPECT_NE(std::string::npos, error.find("/Render/Missing")) << error;
}

// The standard attributes come from the prim, the schema's fallbacks where it authors none.
TEST_F(RenderSettingsPrimTest, StandardAttributes)
{
    UsdStageRefPtr stage = stageFrom(kTwoPrims);
    ASSERT_TRUE(bool(stage));

    const RenderSettingsPrim::StandardAttributes a = RenderSettingsPrim::standardAttributes(UsdRenderSettings(stage->GetPrimAtPath(SdfPath("/Render/A"))));
    EXPECT_EQ(SdfPath("/World/Cam"), a.camera);
    EXPECT_EQ(GfVec2i(320, 240), a.resolution);
    EXPECT_EQ(2.f, a.pixelAspectRatio);
    EXPECT_EQ(UsdRenderTokens->cropAperture, a.aspectRatioConformPolicy);
    EXPECT_EQ(GfVec4f(0.f, 0.25f, 1.f, 0.75f), a.dataWindowNDC);
    EXPECT_TRUE(a.disableMotionBlur);
    EXPECT_TRUE(a.disableDepthOfField);

    const RenderSettingsPrim::StandardAttributes b = RenderSettingsPrim::standardAttributes(UsdRenderSettings(stage->GetPrimAtPath(SdfPath("/Render/B"))));
    EXPECT_TRUE(b.camera.IsEmpty());
    EXPECT_EQ(GfVec2i(2048, 1080), b.resolution);
    EXPECT_EQ(1.f, b.pixelAspectRatio);
    EXPECT_EQ(UsdRenderTokens->expandAperture, b.aspectRatioConformPolicy);
    EXPECT_EQ(GfVec4f(0.f, 0.f, 1.f, 1.f), b.dataWindowNDC);
    EXPECT_FALSE(b.disableMotionBlur);
    EXPECT_FALSE(b.disableDepthOfField);
}

// natron:renderer names the prim's renderer; none authored is no renderer, and an id no plug-in
// has is an error naming it rather than a fall back to Storm.
TEST_F(RenderSettingsPrimTest, RendererForThePrim)
{
    UsdStageRefPtr stage = stageFrom(kTwoPrims);
    ASSERT_TRUE(bool(stage));
    const UsdPrim a = stage->GetPrimAtPath(SdfPath("/Render/A"));
    const UsdPrim b = stage->GetPrimAtPath(SdfPath("/Render/B"));
    std::string id;
    std::string error;

    ASSERT_TRUE(RenderSettingsPrim::rendererFor(a, &id, &error)) << error;
    EXPECT_EQ(std::string(kRenderDelegateStormId), id);

    id = "stale";
    ASSERT_TRUE(RenderSettingsPrim::rendererFor(b, &id, &error)) << error;
    EXPECT_TRUE(id.empty());

    UsdAttribute renderer = b.CreateAttribute(TfToken(kRenderSettingsPrimRendererAttribute), SdfValueTypeNames->Token, false, SdfVariabilityUniform);
    ASSERT_TRUE(renderer.Set(TfToken("HdNoSuchRendererPlugin")));
    EXPECT_FALSE(RenderSettingsPrim::rendererFor(b, &id, &error));
    EXPECT_TRUE(id.empty());
    EXPECT_NE(std::string::npos, error.find("HdNoSuchRendererPlugin")) << error;
}
