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
#include <cstdio>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <pxr/base/plug/registry.h>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/CyclesSettings.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/MoonRaySettings.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
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

namespace {
// Moves an int, float or bool knob to a value it did not have.
bool
nudge(const KnobIPtr& k)
{
    if (KnobBool* b = dynamic_cast<KnobBool*>(k.get())) {
        b->setValue(!b->getValue());
    } else if (KnobInt* i = dynamic_cast<KnobInt*>(k.get())) {
        i->setValue(i->getValue() + 1);
    } else if (KnobDouble* d = dynamic_cast<KnobDouble*>(k.get())) {
        d->setValue(d->getValue() + 0.5);
    } else {
        return false;
    }

    return true;
}
} // anon namespace

// A USDRender's hash folds the knobs of the active prim's renderer only: a MoonRay knob edited
// while the prim names Cycles leaves it as it was, a Cycles knob does not; once the prim names
// MoonRay it is the other way round.
TEST_F(USDRenderSettingsTest, TheRenderHashFoldsTheActiveRenderersKnobsOnly)
{
    const std::vector<std::string> ids = RenderSettingsKnobs::rendererIds();
    const bool haveCycles = std::find(ids.begin(), ids.end(), std::string(kRenderDelegateCyclesId)) != ids.end();
    const bool haveMoonRay = std::find(ids.begin(), ids.end(), std::string(kMoonRayRendererId)) != ids.end();
    if (!haveCycles || !haveMoonRay) {
        std::printf("[  SKIPPED ] needs both Cycles and MoonRay installed\n");

        return;
    }
    NodePtr node = settings(camera("/Cam"));
    ASSERT_TRUE(bool(node));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(render->connectInput(node, 0));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    KnobIPtr cyclesSamples = knobs->knob(kRenderDelegateCyclesId, CyclesSettings::samplesKey());
    KnobIPtr moonRaySamples = knobs->knob(kMoonRayRendererId, MoonRaySettings::samplesKey());
    ASSERT_TRUE(bool(cyclesSamples));
    ASSERT_TRUE(bool(moonRaySamples));
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(renderer != nullptr);
    (void)renderer->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_EQ(std::string(kRenderDelegateCyclesId), renderer->getActiveEntry().id);

    // Cycles is active.
    U64 hash = render->getHashValue();
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_EQ(hash, render->getHashValue()) << "a MoonRay knob changed the hash of a Cycles render";
    ASSERT_TRUE(nudge(cyclesSamples));
    EXPECT_NE(hash, render->getHashValue()) << "a Cycles knob left the hash of a Cycles render";

    // MoonRay is active.
    hash = render->getHashValue();
    (void)renderer->setValueFromID(kMoonRayRendererId, 0);
    ASSERT_EQ(std::string(kMoonRayRendererId), renderer->getActiveEntry().id);
    EXPECT_NE(hash, render->getHashValue());
    hash = render->getHashValue();
    ASSERT_TRUE(nudge(cyclesSamples));
    EXPECT_EQ(hash, render->getHashValue()) << "a Cycles knob changed the hash of a MoonRay render";
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_NE(hash, render->getHashValue()) << "a MoonRay knob left the hash of a MoonRay render";

    // The render after the edit is made with the edited value, not a cached one.
    KnobInt* moonRaySamplesInt = dynamic_cast<KnobInt*>(moonRaySamples.get());
    ASSERT_TRUE(moonRaySamplesInt != nullptr);
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray")) {
        ASSERT_TRUE(RenderSettingsTestUtils::configure(node, "/Cam", 16, 16));
        HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
        std::list<ImageLayerDesc> rgba;
        rgba.push_back(ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;
        // Bypassing the cache would empty the caches after the render, releasing the engine asked next.
        appPTR->clearAllCaches();
        ASSERT_TRUE(renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, 16, 16), rgba, &planes, &error, false)) << error;
        PXR_NS::VtValue reported;
        ASSERT_TRUE(HydraRender::engineSetting(dynamic_cast<USDRender*>(render->getEffectInstance().get()), MoonRaySettings::samplesKey(), &reported));
        EXPECT_EQ(PXR_NS::VtValue(moonRaySamplesInt->getValue()), reported);
        HydraRender::releaseAllCpuEngines();
    } else {
        std::printf("[  SKIPPED ] rendering: OpenUSD has no hd_moonray plug-in registered\n");
    }

    // USDRender's Renderer knob picks the namespaces as the prim's renderer does.
    KnobChoice* rendererKnob = knob<KnobChoice>(render, kUSDRenderParamRenderer);
    ASSERT_TRUE(rendererKnob != nullptr);
    (void)rendererKnob->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_EQ(std::string(kRenderDelegateCyclesId), rendererKnob->getActiveEntry().id);
    hash = render->getHashValue();
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_EQ(hash, render->getHashValue()) << "a MoonRay knob changed the hash of a render the node makes with Cycles";
    ASSERT_TRUE(nudge(cyclesSamples));
    EXPECT_NE(hash, render->getHashValue());

    // A disabled settings node is no prim: every knob of it is out of the hash.
    node->setNodeDisabled(true);
    hash = render->getHashValue();
    ASSERT_TRUE(nudge(cyclesSamples));
    EXPECT_EQ(hash, render->getHashValue());
}

// Every node but USDRender folds every renderer's knobs: a MoonRay knob edited while the prim
// names Cycles is in the stage the settings node, a node forwarding it and a USDWrite output, so
// it moves their hashes, while a Cycles render below the same node keeps its own.
TEST_F(USDRenderSettingsTest, OtherNodesFoldEveryRenderersKnobs)
{
    const std::vector<std::string> ids = RenderSettingsKnobs::rendererIds();
    const bool haveCycles = std::find(ids.begin(), ids.end(), std::string(kRenderDelegateCyclesId)) != ids.end();
    const bool haveMoonRay = std::find(ids.begin(), ids.end(), std::string(kMoonRayRendererId)) != ids.end();
    if (!haveCycles || !haveMoonRay) {
        std::printf("[  SKIPPED ] needs both Cycles and MoonRay installed\n");

        return;
    }
    NodePtr node = settings(camera("/Cam"));
    ASSERT_TRUE(bool(node));
    NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
    ASSERT_TRUE(bool(prune));
    ASSERT_TRUE(prune->connectInput(node, 0));
    NodePtr writer = createNode(QString::fromUtf8(PLUGINID_NATRON_USDWRITE));
    ASSERT_TRUE(bool(writer));
    ASSERT_TRUE(writer->connectInput(prune, 0));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(render->connectInput(prune, 0));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    KnobIPtr cyclesSamples = knobs->knob(kRenderDelegateCyclesId, CyclesSettings::samplesKey());
    KnobIPtr moonRaySamples = knobs->knob(kMoonRayRendererId, MoonRaySettings::samplesKey());
    ASSERT_TRUE(bool(cyclesSamples));
    ASSERT_TRUE(bool(moonRaySamples));
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(renderer != nullptr);
    (void)renderer->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_EQ(std::string(kRenderDelegateCyclesId), renderer->getActiveEntry().id);

    U64 settingsHash = node->getHashValue();
    U64 pruneHash = prune->getHashValue();
    U64 writerHash = writer->getHashValue();
    U64 renderHash = render->getHashValue();
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_NE(settingsHash, node->getHashValue()) << "a MoonRay knob left the hash of the node authoring it";
    EXPECT_NE(pruneHash, prune->getHashValue()) << "a MoonRay knob left the hash of a node forwarding the stage";
    EXPECT_NE(writerHash, writer->getHashValue()) << "a MoonRay knob left the hash of a USDWrite writing it";
    EXPECT_EQ(renderHash, render->getHashValue()) << "a MoonRay knob changed the hash of a Cycles render";

    // A Cycles knob moves all four.
    settingsHash = node->getHashValue();
    pruneHash = prune->getHashValue();
    writerHash = writer->getHashValue();
    ASSERT_TRUE(nudge(cyclesSamples));
    EXPECT_NE(settingsHash, node->getHashValue());
    EXPECT_NE(pruneHash, prune->getHashValue());
    EXPECT_NE(writerHash, writer->getHashValue());
    EXPECT_NE(renderHash, render->getHashValue());
}

// Two settings nodes authoring the same prim through a USDMerge: the later input is stronger, so
// its renderer is the one a Scene Default USDRender renders with, and an edit to that renderer's
// knobs moves the render's hash.
TEST_F(USDRenderSettingsTest, TheStrongerInputsRendererKeysTheRender)
{
    const std::vector<std::string> ids = RenderSettingsKnobs::rendererIds();
    const bool haveCycles = std::find(ids.begin(), ids.end(), std::string(kRenderDelegateCyclesId)) != ids.end();
    const bool haveMoonRay = std::find(ids.begin(), ids.end(), std::string(kMoonRayRendererId)) != ids.end();
    if (!haveCycles || !haveMoonRay) {
        std::printf("[  SKIPPED ] needs both Cycles and MoonRay installed\n");

        return;
    }
    NodePtr weaker = settings(camera("/Cam"));
    NodePtr stronger = settings(camera("/Cam2"));
    ASSERT_TRUE(weaker && stronger);
    KnobChoice* weakerRenderer = knob<KnobChoice>(weaker, kUSDRenderSettingsParamDefaultRenderer);
    KnobChoice* strongerRenderer = knob<KnobChoice>(stronger, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(weakerRenderer && strongerRenderer);
    (void)weakerRenderer->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_EQ(std::string(kRenderDelegateCyclesId), weakerRenderer->getActiveEntry().id);
    (void)strongerRenderer->setValueFromID(kMoonRayRendererId, 0);
    ASSERT_EQ(std::string(kMoonRayRendererId), strongerRenderer->getActiveEntry().id);

    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(bool(merge));
    ASSERT_TRUE(merge->connectInput(weaker, 0));
    ASSERT_TRUE(merge->connectInput(stronger, 1));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(render->connectInput(merge, 0));
    KnobChoice* renderRenderer = knob<KnobChoice>(render, kUSDRenderParamRenderer);
    ASSERT_TRUE(renderRenderer != nullptr);
    ASSERT_EQ(std::string(kRenderDelegateSceneDefaultId), renderRenderer->getActiveEntry().id);

    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(stronger->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    KnobIPtr moonRaySamples = knobs->knob(kMoonRayRendererId, MoonRaySettings::samplesKey());
    ASSERT_TRUE(bool(moonRaySamples));

    const U64 hash = render->getHashValue();
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_NE(hash, render->getHashValue()) << "a MoonRay knob of the stronger input left the hash of the render it keys";
}

// A file may name another renderer or active prim than the nodes do. Merged with one, the render
// folds every renderer's knobs; a settings node over the file settles both, so it folds its own
// renderer's only.
TEST_F(USDRenderSettingsTest, AFileMakesTheRenderFoldEveryRendererUntilANodeSettlesIt)
{
    const std::vector<std::string> ids = RenderSettingsKnobs::rendererIds();
    const bool haveCycles = std::find(ids.begin(), ids.end(), std::string(kRenderDelegateCyclesId)) != ids.end();
    const bool haveMoonRay = std::find(ids.begin(), ids.end(), std::string(kMoonRayRendererId)) != ids.end();
    if (!haveCycles || !haveMoonRay) {
        std::printf("[  SKIPPED ] needs both Cycles and MoonRay installed\n");

        return;
    }
    NodePtr node = settings(camera("/Cam"));
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(node && read && merge && render);
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(renderer != nullptr);
    (void)renderer->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_EQ(std::string(kRenderDelegateCyclesId), renderer->getActiveEntry().id);
    ASSERT_TRUE(merge->connectInput(node, 0));
    ASSERT_TRUE(merge->connectInput(read, 1));
    ASSERT_TRUE(render->connectInput(merge, 0));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    KnobIPtr moonRaySamples = effect->renderSettings()->knob(kMoonRayRendererId, MoonRaySettings::samplesKey());
    ASSERT_TRUE(bool(moonRaySamples));

    U64 hash = render->getHashValue();
    ASSERT_TRUE(nudge(moonRaySamples));
    EXPECT_NE(hash, render->getHashValue()) << "a MoonRay knob left the hash of a render a file may send to MoonRay";

    // A settings node over the file: the prim's renderer and the active prim are its own.
    NodePtr over = settings(read);
    NodePtr overRender = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(over && overRender);
    KnobChoice* overRenderer = knob<KnobChoice>(over, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(overRenderer != nullptr);
    (void)overRenderer->setValueFromID(kRenderDelegateCyclesId, 0);
    ASSERT_TRUE(overRender->connectInput(over, 0));
    USDRenderSettings* overEffect = dynamic_cast<USDRenderSettings*>(over->getEffectInstance().get());
    ASSERT_TRUE(overEffect != nullptr);
    KnobIPtr overSamples = overEffect->renderSettings()->knob(kMoonRayRendererId, MoonRaySettings::samplesKey());
    ASSERT_TRUE(bool(overSamples));
    hash = overRender->getHashValue();
    ASSERT_TRUE(nudge(overSamples));
    EXPECT_EQ(hash, overRender->getHashValue()) << "a MoonRay knob changed the hash of a Cycles render over a file";
}
