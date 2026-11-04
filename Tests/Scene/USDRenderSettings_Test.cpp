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

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"
#include "ofxNatron.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
template <typename KNOB>
KNOB*
knob(const NodePtr& node,
     const char* name)
{
    KNOB* k = node ? dynamic_cast<KNOB*>(node->getKnobByName(name).get()) : nullptr;

    EXPECT_TRUE(k != nullptr) << name;

    return k;
}
} // anon namespace

class USDRenderSettingsTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    NodePtr camera(const char* path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        EXPECT_TRUE(bool(node));
        if (KnobString* primPath = knob<KnobString>(node, kSceneParamPrimPath)) {
            primPath->setValue(std::string(path));
        }

        return node;
    }

    NodePtr settings(const NodePtr& input,
                     const char* primPath = kUSDRenderSettingsDefaultPrimPath)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        EXPECT_TRUE(bool(node)) << "USDRenderSettings is not registered";
        if (!node) {
            return node;
        }
        if (KnobString* path = knob<KnobString>(node, kSceneParamPrimPath)) {
            path->setValue(std::string(primPath));
        }
        if (input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    // The stage node outputs; *lease keeps it.
    static UsdStageRefPtr stageOf(const NodePtr& node,
                                  SceneQueryPool::Lease* lease)
    {
        ScenePayload payload;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        *lease = SceneQueryPool::instance().borrow(payload, &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }
};

// The prim is the stage's active settings prim, its standard attributes as the knobs set them,
// each authored once, as the schema's uniform attributes have no time samples.
TEST_F(USDRenderSettingsTest, AuthorsTheStandardAttributes)
{
    NodePtr cam = camera("/World/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    effect->refreshCameraMenu();
    KnobChoice* cameraKnob = knob<KnobChoice>(node, kUSDRenderSettingsParamCamera);
    ASSERT_TRUE(cameraKnob != nullptr);
    (void)cameraKnob->setValueFromID("/World/Cam", 0);
    ASSERT_EQ(std::string("/World/Cam"), cameraKnob->getActiveEntry().id);
    KnobInt* size = knob<KnobInt>(node, kNatronParamFormatSize);
    KnobDouble* par = knob<KnobDouble>(node, kNatronParamFormatPar);
    KnobChoice* policy = knob<KnobChoice>(node, kUSDRenderSettingsParamConformPolicy);
    KnobDouble* dataWindow = knob<KnobDouble>(node, kUSDRenderSettingsParamDataWindow);
    KnobBool* motionBlur = knob<KnobBool>(node, kUSDRenderSettingsParamDisableMotionBlur);
    KnobBool* depthOfField = knob<KnobBool>(node, kUSDRenderSettingsParamDisableDepthOfField);
    ASSERT_TRUE(size && par && policy && dataWindow && motionBlur && depthOfField);
    EXPECT_EQ(5, policy->getNumEntries());
    EXPECT_EQ(UsdRenderTokens->expandAperture.GetString(), policy->getActiveEntry().id);
    size->setValue(640, ViewSpec::all(), 0);
    size->setValue(360, ViewSpec::all(), 1);
    par->setValue(2.);
    (void)policy->setValueFromID(UsdRenderTokens->cropAperture.GetString(), 0);
    ASSERT_EQ(UsdRenderTokens->cropAperture.GetString(), policy->getActiveEntry().id);
    dataWindow->setValue(0.1, ViewSpec::all(), 1);
    dataWindow->setValue(0.9, ViewSpec::all(), 3);
    motionBlur->setValue(true);
    depthOfField->setValue(true);

    SceneQueryPool::Lease lease;
    UsdStageRefPtr stage = stageOf(node, &lease);
    ASSERT_TRUE(bool(stage));
    std::string error;
    const UsdRenderSettings prim = RenderSettingsPrim::activePrim(stage, std::string(), &error);
    ASSERT_TRUE(bool(prim)) << error;
    EXPECT_EQ(SdfPath(kUSDRenderSettingsDefaultPrimPath), prim.GetPath());
    EXPECT_EQ(prim.GetPath(), UsdRenderSettings::GetStageRenderSettings(stage).GetPath());
    const RenderSettingsPrim::StandardAttributes a = RenderSettingsPrim::standardAttributes(prim);
    EXPECT_EQ(SdfPath("/World/Cam"), a.camera);
    EXPECT_EQ(GfVec2i(640, 360), a.resolution);
    EXPECT_EQ(2.f, a.pixelAspectRatio);
    EXPECT_EQ(UsdRenderTokens->cropAperture, a.aspectRatioConformPolicy);
    EXPECT_FLOAT_EQ(0.f, a.dataWindowNDC[0]);
    EXPECT_FLOAT_EQ(0.1f, a.dataWindowNDC[1]);
    EXPECT_FLOAT_EQ(1.f, a.dataWindowNDC[2]);
    EXPECT_FLOAT_EQ(0.9f, a.dataWindowNDC[3]);
    EXPECT_TRUE(a.disableMotionBlur);
    EXPECT_TRUE(a.disableDepthOfField);
    EXPECT_EQ(0u, prim.GetResolutionAttr().GetNumTimeSamples());
    EXPECT_EQ(SdfVariabilityUniform, prim.GetResolutionAttr().GetVariability());

    std::string renderer;
    ASSERT_TRUE(RenderSettingsPrim::rendererFor(prim.GetPrim(), &renderer, &error)) << error;
    EXPECT_EQ(std::string(kRenderDelegateStormId), renderer);
    const UsdAttribute rendererAttribute = prim.GetPrim().GetAttribute(TfToken(kRenderSettingsPrimRendererAttribute));
    ASSERT_TRUE(bool(rendererAttribute));
    EXPECT_EQ(SdfVariabilityUniform, rendererAttribute.GetVariability());
    EXPECT_EQ(SdfValueTypeNames->Token, rendererAttribute.GetTypeName());
}

// Make Active off leaves the input's active prim active; on, the node's own prim takes over.
TEST_F(USDRenderSettingsTest, MakeActiveChoosesTheStagesSettingsPrim)
{
    NodePtr first = settings(NodePtr(), "/Render/first");
    NodePtr second = settings(first, "/Render/second");
    ASSERT_TRUE(first && second);
    KnobBool* makeActive = knob<KnobBool>(second, kUSDRenderSettingsParamMakeActive);
    ASSERT_TRUE(makeActive != nullptr);
    EXPECT_TRUE(makeActive->getValue());
    {
        SceneQueryPool::Lease lease;
        UsdStageRefPtr stage = stageOf(second, &lease);
        ASSERT_TRUE(bool(stage));
        EXPECT_EQ(SdfPath("/Render/second"), UsdRenderSettings::GetStageRenderSettings(stage).GetPath());
        EXPECT_TRUE(bool(UsdRenderSettings(stage->GetPrimAtPath(SdfPath("/Render/first")))));
    }
    makeActive->setValue(false);
    SceneQueryPool::Lease lease;
    UsdStageRefPtr stage = stageOf(second, &lease);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(SdfPath("/Render/first"), UsdRenderSettings::GetStageRenderSettings(stage).GetPath());
}

// The default renderer chosen is the one natron:renderer names.
TEST_F(USDRenderSettingsTest, DefaultRendererIsAuthored)
{
    NodePtr node = settings(NodePtr());
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(renderer != nullptr);
    EXPECT_EQ(std::string(kRenderDelegateStormId), renderer->getActiveEntry().id);
    std::string other;
    for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
        if (!RenderDelegates::isStorm(delegate.id)) {
            other = delegate.id;
            break;
        }
    }
    if (other.empty()) {
        std::printf("[  SKIPPED ] only Storm is installed\n");

        return;
    }
    (void)renderer->setValueFromID(other, 0);
    ASSERT_EQ(other, renderer->getActiveEntry().id);
    SceneQueryPool::Lease lease;
    UsdStageRefPtr stage = stageOf(node, &lease);
    ASSERT_TRUE(bool(stage));
    std::string id;
    std::string error;
    ASSERT_TRUE(RenderSettingsPrim::rendererFor(stage->GetPrimAtPath(SdfPath(kUSDRenderSettingsDefaultPrimPath)), &id, &error)) << error;
    EXPECT_EQ(other, id);
}

// Every installed renderer gets a page; every setting its delegate declares, of a type a knob can
// hold, is reached by a knob of the curated group or of Other Settings, and none by two.
TEST_F(USDRenderSettingsTest, EveryDeclaredSettingHasAKnob)
{
    NodePtr node = settings(NodePtr());
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    for (const std::string& id : RenderSettingsKnobs::rendererIds()) {
        KnobPagePtr page = knobs->page(id);
        ASSERT_TRUE(bool(page)) << id;
        EXPECT_FALSE(page->getIsSecret()) << id;
        std::set<std::string> names;
        for (const RenderSettingsKnobs::Descriptor& d : RenderSettingsKnobs::descriptors(id)) {
            std::string reach;
            for (const std::pair<std::string, std::string>& r : knobs->audit(id)) {
                if (r.first == d.key) {
                    reach = r.second;
                }
            }
            std::printf("[  AUDIT   ] %s %s -> %s\n", id.c_str(), d.key.c_str(), reach.empty() ? "UNREACHABLE" : reach.c_str());
            if (RenderSettingsKnobs::isSupported(d.defaultValue)) {
                EXPECT_FALSE(reach.empty()) << id << " " << d.key;
            }
            if (!reach.empty() && (reach != "(set by Natron)")) {
                EXPECT_TRUE(names.insert(reach).second) << reach << " reaches two settings";
            }
        }
    }
}
