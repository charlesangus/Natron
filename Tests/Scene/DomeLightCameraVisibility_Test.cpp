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

#include <pxr/imaging/hd/dataSourceTypeDefs.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include "Engine/Nodes/Scene/RenderSettingsPrim.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
UsdPrim
defineDome(const UsdStageRefPtr& stage,
           const char* path)
{
    return UsdLuxDomeLight::Define(stage, SdfPath(path)).GetPrim();
}

void
setVisibleInPrimaryRay(const UsdPrim& prim,
                       bool visible)
{
    ASSERT_TRUE(prim.CreateAttribute(TfToken(kLightVisibleInPrimaryRayAttribute), SdfValueTypeNames->Bool).Set(visible));
}

bool
cameraVisibility(const UsdStageRefPtr& stage)
{
    return RenderSettingsPrim::domeLightCameraVisibility(stage, RenderSettingsPrim::domeLights(stage), UsdTimeCode::Default());
}

bool
settingFor(const UsdStageRefPtr& stage)
{
    RenderSettingsPrim::Values settings;
    RenderSettingsPrim::addDomeLightCameraVisibility(stage, RenderSettingsPrim::domeLights(stage), UsdTimeCode::Default(), &settings);
    const RenderSettingsPrim::Values::const_iterator it = settings.find(HdRenderSettingsTokens->domeLightCameraVisibility.GetString());
    EXPECT_TRUE(it != settings.end());
    EXPECT_TRUE(it != settings.end() && it->second.IsHolding<bool>());

    return (it != settings.end()) && it->second.IsHolding<bool>() && it->second.UncheckedGet<bool>();
}
} // namespace

TEST(DomeLightCameraVisibilityTest, ADomeWithTheFlagOffHidesDomesFromTheCamera)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    setVisibleInPrimaryRay(defineDome(stage, "/World/Sky"), false);

    EXPECT_EQ(SdfPathVector { SdfPath("/World/Sky") }, RenderSettingsPrim::domeLights(stage));
    EXPECT_FALSE(cameraVisibility(stage));
    EXPECT_FALSE(settingFor(stage));
}

TEST(DomeLightCameraVisibilityTest, ADomeWithTheFlagOnOrUnauthoredKeepsDomesVisible)
{
    const UsdStageRefPtr on = UsdStage::CreateInMemory();
    setVisibleInPrimaryRay(defineDome(on, "/Sky"), true);
    EXPECT_TRUE(cameraVisibility(on));
    EXPECT_TRUE(settingFor(on));

    const UsdStageRefPtr absent = UsdStage::CreateInMemory();
    defineDome(absent, "/Sky");
    EXPECT_TRUE(cameraVisibility(absent));
    EXPECT_TRUE(settingFor(absent));
}

TEST(DomeLightCameraVisibilityTest, AStageWithoutDomesKeepsTheSettingOn)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    setVisibleInPrimaryRay(UsdLuxSphereLight::Define(stage, SdfPath("/Key")).GetPrim(), false);

    EXPECT_TRUE(RenderSettingsPrim::domeLights(stage).empty());
    EXPECT_TRUE(settingFor(stage));
    EXPECT_TRUE(settingFor(UsdStageRefPtr()));
}

TEST(DomeLightCameraVisibilityTest, AnInactiveDomeIsIgnored)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim dome = defineDome(stage, "/Off/Sky");
    setVisibleInPrimaryRay(dome, false);
    ASSERT_TRUE(stage->GetPrimAtPath(SdfPath("/Off")).SetActive(false));

    EXPECT_TRUE(RenderSettingsPrim::domeLights(stage).empty());
    EXPECT_TRUE(settingFor(stage));
}

TEST(DomeLightCameraVisibilityTest, AnyHiddenDomeAmongSeveralHidesThem)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    setVisibleInPrimaryRay(defineDome(stage, "/A"), true);
    const UsdPrim dome1 = stage->DefinePrim(SdfPath("/B"), TfToken("DomeLight_1"));
    setVisibleInPrimaryRay(dome1, false);

    EXPECT_EQ(2u, RenderSettingsPrim::domeLights(stage).size());
    EXPECT_FALSE(settingFor(stage));
}

TEST(DomeLightCameraVisibilityTest, AnAnimatedFlagIsReadAtTheTimeAsked)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute attribute = defineDome(stage, "/Sky").CreateAttribute(TfToken(kLightVisibleInPrimaryRayAttribute), SdfValueTypeNames->Bool);
    ASSERT_TRUE(attribute.Set(true, UsdTimeCode(1.)));
    ASSERT_TRUE(attribute.Set(false, UsdTimeCode(2.)));
    const SdfPathVector domes = RenderSettingsPrim::domeLights(stage);

    EXPECT_TRUE(RenderSettingsPrim::domeLightCameraVisibility(stage, domes, UsdTimeCode(1.)));
    EXPECT_FALSE(RenderSettingsPrim::domeLightCameraVisibility(stage, domes, UsdTimeCode(2.)));
}

// A delegate reads the flag from a light's light data source by its bare name, as Cycles does: the
// un-namespaced attribute must reach it there and the "inputs:" one must not.
TEST(DomeLightCameraVisibilityTest, UsdImagingForwardsTheUnNamespacedAttributeToTheLightDataSource)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim forwarded = UsdLuxRectLight::Define(stage, SdfPath("/Forwarded")).GetPrim();
    setVisibleInPrimaryRay(forwarded, false);
    const UsdPrim namespaced = UsdLuxRectLight::Define(stage, SdfPath("/Namespaced")).GetPrim();
    ASSERT_TRUE(namespaced.CreateAttribute(TfToken("inputs:visibleInPrimaryRay"), SdfValueTypeNames->Bool).Set(false));

    const UsdImagingStageSceneIndexRefPtr sceneIndex = UsdImagingStageSceneIndex::New();
    sceneIndex->SetStage(stage);
    const TfToken name(kLightVisibleInPrimaryRayAttribute);

    const HdContainerDataSourceHandle forwardedLight = HdLightSchema::GetFromParent(sceneIndex->GetPrim(SdfPath("/Forwarded")).dataSource).GetContainer();
    ASSERT_TRUE(bool(forwardedLight));
    const HdBoolDataSourceHandle value = HdBoolDataSource::Cast(forwardedLight->Get(name));
    ASSERT_TRUE(bool(value));
    EXPECT_FALSE(value->GetTypedValue(0.f));

    const HdContainerDataSourceHandle namespacedLight = HdLightSchema::GetFromParent(sceneIndex->GetPrim(SdfPath("/Namespaced")).dataSource).GetContainer();
    ASSERT_TRUE(bool(namespacedLight));
    EXPECT_FALSE(bool(HdBoolDataSource::Cast(namespacedLight->Get(name))));
}
