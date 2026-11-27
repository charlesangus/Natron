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
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <pxr/base/plug/registry.h>

#include "BaseTest.h"
#include "CyclesTestUtils.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/CyclesSettings.h"
#include "Engine/Nodes/Scene/DelegateSupport.h"
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
#include "Engine/ViewerInstance.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "ofxNatron.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

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
        *lease = SceneQueryPool::instance().borrow(payload, SceneLoadSet::everything(), &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }
};

// A project saved where a renderer's delegate loads is opened where it does not: that renderer's
// settings have no knobs, and saving again must not lose them.
TEST_F(USDRenderSettingsTest, SavingKeepsTheSettingsOfARendererThatDidNotLoad)
{
    const char* const missing = "someRendererSamples";

    Serialization::NodeSerializationPtr saved = std::make_shared<Serialization::NodeSerialization>();
    saved->pluginID = PLUGINID_NATRON_USDRENDERSETTINGS;
    saved->knobs.emplace_back();
    saved->knobs.back().name = missing;
    saved->knobs.back().typeName = "Int";
    saved->knobs.back().dimension = 1;

    for (int save = 0; save < 2; ++save) {
        CreateNodeArgs args(PLUGINID_NATRON_USDRENDERSETTINGS, getApp()->getProject());
        args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, saved);
        NodePtr node = getApp()->createNode(args);
        ASSERT_TRUE(bool(node));
        EXPECT_FALSE(bool(node->getKnobByName(missing)));

        Serialization::NodeSerializationPtr out = std::make_shared<Serialization::NodeSerialization>();
        node->toSerialization(out.get());
        const std::ptrdiff_t kept = std::count_if(out->knobs.begin(), out->knobs.end(), [missing](const Serialization::KnobSerialization& knob) {
            return knob.name == missing;
        });
        EXPECT_EQ(1, kept) << "save " << save;
        saved = out;
    }
}

// The prim is the stage's active settings prim, its standard attributes as the knobs set them,
// each authored once, as the schema's uniform attributes have no time samples.
TEST_F(USDRenderSettingsTest, AuthorsTheStandardAttributes)
{
    NodePtr cam = camera("/World/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    {
        const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
        job->wait();
        EXPECT_TRUE(effect->applyCameraMenu(job));
    }
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

// The delegate declares Device as a token, which the page shows as a menu of its three values.
TEST_F(USDRenderSettingsTest, NatronUtilityDeviceIsAMenuOfAutoGpuAndCpu)
{
    NodePtr node = settings(NodePtr());
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    ASSERT_TRUE(bool(knobs->page(kRenderDelegateUtilityId)));
    std::string scriptName;
    for (const std::pair<std::string, std::string>& r : knobs->audit(kRenderDelegateUtilityId)) {
        if (r.first == "device") {
            scriptName = r.second;
        }
    }
    ASSERT_FALSE(scriptName.empty());
    KnobChoice* device = dynamic_cast<KnobChoice*>(node->getKnobByName(scriptName).get());
    ASSERT_TRUE(device != nullptr) << scriptName;
    const std::vector<ChoiceOption> entries = device->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ(std::string("auto"), entries[0].id);
    EXPECT_EQ(std::string("Auto"), entries[0].label);
    EXPECT_EQ(std::string("gpu"), entries[1].id);
    EXPECT_EQ(std::string("GPU"), entries[1].label);
    EXPECT_EQ(std::string("cpu"), entries[2].id);
    EXPECT_EQ(std::string("CPU"), entries[2].label);
    EXPECT_EQ(std::string("auto"), device->getActiveEntry().id);
}

// Storm declares few settings, each placed by its name: volumes, lights and performance, all open.
TEST_F(USDRenderSettingsTest, StormSettingsAreGroupedByTheirNames)
{
    NodePtr node = settings(NodePtr());
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    const std::string id = kRenderDelegateStormId;
    const std::map<std::string, std::string> expected = {
        { "volumeRaymarchingStepSize", "Volumes" },
        { "volumeRaymarchingStepSizeLighting", "Volumes" },
        { "volumeMaxTextureMemoryPerField", "Volumes" },
        { "maxLights", "Lights" },
        { "enableTinyPrimCulling", "Performance" },
    };
    const std::string prefix = knobs->page(id) ? knobs->page(id)->getName() : std::string();
    ASSERT_FALSE(prefix.empty());
    std::set<std::string> groupsSeen;
    for (const std::pair<const std::string, std::string>& e : expected) {
        KnobIPtr k = knobs->knob(id, e.first);
        if (!k) {
            std::printf("[  SKIPPED ] Storm declares no %s\n", e.first.c_str());
            continue;
        }
        KnobIPtr group = k->getParentKnob();
        ASSERT_TRUE(bool(group)) << e.first;
        EXPECT_EQ(prefix + e.second, group->getName()) << e.first;
        EXPECT_EQ(knobs->page(id), group->getParentKnob()) << e.first;
        KnobGroup* g = dynamic_cast<KnobGroup*>(group.get());
        ASSERT_TRUE(g != nullptr);
        EXPECT_TRUE(g->getValue()) << e.second << " starts open";
        groupsSeen.insert(e.second);
    }
    for (const std::string& name : { "Sampling", "Denoise", "Bounces", "PathGuiding", "OtherSettings" }) {
        EXPECT_FALSE(bool(node->getKnobByName(prefix + name))) << name;
    }
}

namespace {
struct CuratedTable {
    std::string prefix;
    std::string docsHeading;
    const RenderSettingsKnobs::Curated* curated;
};

std::vector<CuratedTable>
curatedTables()
{
    return {
        { "cycles", "### Cycles", &CyclesSettings::curated() },
        { "moonray", "### MoonRay", &MoonRaySettings::curated() },
    };
}

// The delegate may not be installed, so this can be empty.
std::string
installedRenderer(const RenderSettingsKnobs::Curated* curated)
{
    for (const std::string& id : RenderSettingsKnobs::rendererIds()) {
        if (RenderSettingsKnobs::curatedFor(id) == curated) {
            return id;
        }
    }

    return std::string();
}

void
expectExplains(const std::string& what,
               const std::string& tooltip,
               const std::string& name)
{
    EXPECT_FALSE(tooltip.empty()) << what;
    EXPECT_NE(name, tooltip) << what;
    EXPECT_EQ(std::string::npos, tooltip.find("Nuke")) << what;
    EXPECT_EQ(std::string::npos, tooltip.find("\xE2\x80\x94")) << what;
    if (!tooltip.empty()) {
        EXPECT_EQ('.', tooltip.back()) << what;
    }
}

std::string
groupLabel(const std::string& prefix,
           std::string name)
{
    name[0] = (char)std::toupper((unsigned char)name[0]);

    return RenderSettingsKnobs::labelFromScriptName(prefix + name, prefix);
}
} // anon namespace

TEST_F(USDRenderSettingsTest, CuratedTooltipsExplainEverySetting)
{
    for (const CuratedTable& table : curatedTables()) {
        std::map<std::string, std::string> declaredNames;
        const std::string id = installedRenderer(table.curated);
        if (id.empty()) {
            std::printf("[  SKIPPED ] %s's delegate is not installed: declared names unchecked\n", table.prefix.c_str());
        } else {
            for (const RenderSettingsKnobs::Descriptor& d : RenderSettingsKnobs::descriptors(id)) {
                declaredNames[d.key] = d.name;
            }
        }
        for (const RenderSettingsKnobs::CuratedSetting& s : table.curated->settings) {
            expectExplains(s.key, s.tooltip, s.name);
            std::map<std::string, std::string>::const_iterator declared = declaredNames.find(s.key);
            if (declared != declaredNames.end()) {
                EXPECT_NE(declared->second, s.tooltip) << s.key;
            }
        }
        for (const RenderSettingsKnobs::GroupSpec& g : table.curated->groups) {
            expectExplains(g.name, g.tooltip, g.name);
            EXPECT_NE(groupLabel(table.prefix, g.name), g.tooltip) << g.name;
        }
    }
}

TEST_F(USDRenderSettingsTest, TooltipsSayWhenAControlHasNoEffect)
{
    for (const char* key : { "cycles:integrator:use_sample_subset", "cycles:integrator:sample_subset_offset",
                             "cycles:integrator:sample_subset_length", "cycles:integrator:aa_samples" }) {
        bool found = false;
        for (const RenderSettingsKnobs::CuratedSetting& s : CyclesSettings::curated().settings) {
            if (s.key == key) {
                found = true;
                EXPECT_NE(std::string::npos, s.tooltip.find("no effect")) << key;
            }
        }
        EXPECT_TRUE(found) << key;
    }
}

TEST_F(USDRenderSettingsTest, BoolPairsShareARow)
{
    NodePtr node = settings(NodePtr());
    ASSERT_TRUE(bool(node));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const RenderSettingsKnobs* knobs = effect->renderSettings();
    ASSERT_TRUE(knobs != nullptr);
    const std::map<std::string, std::string> pairs = {
        { "cycles:integrator:caustics_reflective", "cycles:integrator:caustics_refractive" },
        { "denoiseAlbedoGuiding", "denoiseNormalGuiding" },
    };
    std::set<std::string> seen;
    for (const CuratedTable& table : curatedTables()) {
        const std::vector<RenderSettingsKnobs::CuratedSetting>& list = table.curated->settings;
        const std::string id = installedRenderer(table.curated);
        for (size_t i = 0; i < list.size(); ++i) {
            if (!list[i].sameRowAsNext) {
                continue;
            }
            std::map<std::string, std::string>::const_iterator pair = pairs.find(list[i].key);
            ASSERT_TRUE(pair != pairs.end()) << list[i].key;
            ASSERT_LT(i + 1, list.size()) << list[i].key;
            EXPECT_EQ(pair->second, list[i + 1].key);
            EXPECT_EQ(list[i].group, list[i + 1].group) << list[i].key;
            EXPECT_TRUE(list[i].defaultValue.IsHolding<bool>()) << list[i].key;
            EXPECT_TRUE(list[i + 1].defaultValue.IsHolding<bool>()) << list[i + 1].key;
            seen.insert(list[i].key);
            if (id.empty()) {
                continue;
            }
            KnobIPtr first = knobs->knob(id, list[i].key);
            KnobIPtr second = knobs->knob(id, list[i + 1].key);
            if (first && second) {
                EXPECT_FALSE(first->isNewLineActivated()) << list[i].key;
                EXPECT_TRUE(second->isNewLineActivated()) << list[i + 1].key;
                EXPECT_NE(std::string::npos, first->getHintToolTip().find("Authored as ")) << list[i].key;
            }
        }
    }
    EXPECT_EQ(pairs.size(), seen.size());
}

TEST_F(USDRenderSettingsTest, DocsListTheGroupsOfTheTables)
{
    const std::string path = std::string(NATRON_TESTS_ENGINE_DIR) + "/Nodes/Scene/docs/USDRenderSettings.md";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << path;
    std::stringstream text;
    text << file.rdbuf();
    const std::string docs = text.str();
    for (const CuratedTable& table : curatedTables()) {
        const std::string heading = table.docsHeading + "\n";
        const size_t start = docs.find(heading);
        ASSERT_NE(std::string::npos, start) << heading;
        std::istringstream section(docs.substr(start + heading.size()));
        std::vector<std::string> listed;
        std::string line;
        while (std::getline(section, line) && (line.compare(0, 1, "#") != 0)) {
            if ((line.compare(0, 2, "- ") == 0) || (line.compare(0, 4, "  - ") == 0)) {
                listed.push_back(line.substr(0, line.find(':')));
            }
        }
        std::vector<std::string> expected;
        for (const RenderSettingsKnobs::GroupSpec& g : table.curated->groups) {
            expected.push_back((g.parent.empty() ? "- " : "  - ") + groupLabel(table.prefix, g.name));
        }
        expected.push_back("- Other Settings");
        EXPECT_EQ(expected, listed) << table.docsHeading;
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
    // The read's hash is a placeholder until a first evaluation walks its file.
    USDRender* overRenderEffect = dynamic_cast<USDRender*>(overRender->getEffectInstance().get());
    ASSERT_TRUE(overRenderEffect != nullptr);
    std::string settleError;
    std::async(std::launch::async, [overRenderEffect, &settleError]() {
        std::string id;

        return overRenderEffect->activeRenderer(1., ViewIdx(0), &id, &settleError); }).get();
    hash = overRender->getHashValue();
    ASSERT_TRUE(nudge(overSamples));
    EXPECT_EQ(hash, overRender->getHashValue()) << "a MoonRay knob changed the hash of a Cycles render over a file";
}

namespace {
// Fixed checklists for Cycles and MoonRay, each a stand-in renderer when its delegate is not installed.
class ChecklistRenderers {
public:
    explicit ChecklistRenderers(const std::vector<std::string>& cyclesAovs = { "color", "depth", "normal" })
    {
        const std::vector<std::string> ids = RenderSettingsKnobs::rendererIds();
        RenderSettingsKnobs::Descriptor flag;
        flag.key = "flag";
        flag.name = "Flag";
        flag.defaultValue = PXR_NS::VtValue(false);
        for (const char* id : { kRenderDelegateCyclesId, kMoonRayRendererId }) {
            if (std::find(ids.begin(), ids.end(), std::string(id)) == ids.end()) {
                RenderSettingsKnobs::setDescriptorsForTests(id, RenderSettingsKnobs::DescriptorList(1, flag));
                _standIns.push_back(id);
            }
        }
        DelegateSupport::setAvailableAovsForTests(kRenderDelegateCyclesId, cyclesAovs);
        DelegateSupport::setAvailableAovsForTests(kMoonRayRendererId, { "beauty", "albedo" });
    }

    ~ChecklistRenderers()
    {
        DelegateSupport::setAvailableAovsForTests(kRenderDelegateCyclesId, std::vector<std::string>());
        DelegateSupport::setAvailableAovsForTests(kMoonRayRendererId, std::vector<std::string>());
        for (const std::string& id : _standIns) {
            RenderSettingsKnobs::setDescriptorsForTests(id, RenderSettingsKnobs::DescriptorList());
        }
    }

    bool isStandIn(const char* id) const
    {
        return std::find(_standIns.begin(), _standIns.end(), std::string(id)) != _standIns.end();
    }

private:
    std::vector<std::string> _standIns;
};

struct AuthoredVar {
    std::string sourceName;
    std::string sourceType;
    std::string dataType;
    std::string layer;
};

// The vars the product of the settings prim at primPath orders, in order.
std::vector<AuthoredVar>
productVars(const UsdStageRefPtr& stage,
            const char* primPath = kUSDRenderSettingsDefaultPrimPath)
{
    std::vector<AuthoredVar> vars;
    const PXR_NS::UsdRenderSettings prim(stage ? stage->GetPrimAtPath(PXR_NS::SdfPath(primPath)) : PXR_NS::UsdPrim());
    if (!prim) {
        ADD_FAILURE() << "no RenderSettings prim at " << primPath;

        return vars;
    }
    PXR_NS::SdfPathVector products;
    prim.GetProductsRel().GetTargets(&products);
    for (const PXR_NS::SdfPath& productPath : products) {
        const PXR_NS::UsdRenderProduct product(stage->GetPrimAtPath(productPath));
        PXR_NS::SdfPathVector ordered;
        if (!product || !product.GetOrderedVarsRel().GetTargets(&ordered)) {
            continue;
        }
        for (const PXR_NS::SdfPath& varPath : ordered) {
            const PXR_NS::UsdRenderVar var(stage->GetPrimAtPath(varPath));
            if (!var) {
                ADD_FAILURE() << "the product orders " << varPath.GetString() << ", which is no RenderVar";
                continue;
            }
            AuthoredVar v;
            var.GetSourceNameAttr().Get(&v.sourceName);
            PXR_NS::TfToken token;
            if (var.GetSourceTypeAttr().HasAuthoredValue() && var.GetSourceTypeAttr().Get(&token)) {
                v.sourceType = token.GetString();
            }
            if (var.GetDataTypeAttr().HasAuthoredValue() && var.GetDataTypeAttr().Get(&token)) {
                v.dataType = token.GetString();
            }
            const PXR_NS::UsdAttribute layer = var.GetPrim().GetAttribute(PXR_NS::TfToken("natron:layer"));
            if (layer) {
                layer.Get(&v.layer);
            }
            vars.push_back(v);
        }
    }

    return vars;
}

const AuthoredVar*
findVar(const std::vector<AuthoredVar>& vars,
        const std::string& sourceName)
{
    for (const AuthoredVar& v : vars) {
        if (v.sourceName == sourceName) {
            return &v;
        }
    }

    return nullptr;
}
} // anon namespace

// Cycles normal plus a primvars:st row are two RenderVars; a MoonRay tick adds a moonray:* var a
// Cycles render leaves out; USDWrite keeps them; unticking removes the var.
TEST_F(USDRenderSettingsTest, TheAovChecklistAuthorsRenderVars)
{
    const ChecklistRenderers renderers;
    NodePtr cam = camera("/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(RenderSettingsTestUtils::configure(node, "/Cam", 64, 32));
    const std::string cyclesPrefix = RenderSettingsKnobs::scriptPrefix(kRenderDelegateCyclesId);
    const std::string moonRayPrefix = RenderSettingsKnobs::scriptPrefix(kMoonRayRendererId);
    KnobGroup* cyclesGroup = knob<KnobGroup>(node, (cyclesPrefix + "Aovs").c_str());
    KnobBool* normal = knob<KnobBool>(node, (cyclesPrefix + "AovNormal").c_str());
    KnobBool* beauty = knob<KnobBool>(node, (moonRayPrefix + "AovBeauty").c_str());
    KnobRenderVars* custom = knob<KnobRenderVars>(node, kUSDRenderSettingsParamCustomAovs);
    ASSERT_TRUE(cyclesGroup && normal && beauty && custom);
    EXPECT_EQ(std::string("Normal"), normal->getLabel());
    EXPECT_EQ(std::string("Custom AOVs"), custom->getLabel());
    EXPECT_FALSE(normal->getValue());

    // Nothing ticked: no product.
    SceneQueryPool::Lease lease;
    EXPECT_TRUE(productVars(stageOf(node, &lease)).empty());
    lease = SceneQueryPool::Lease();

    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(render->connectInput(node, 0));
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderSettingsParamDefaultRenderer);
    ASSERT_TRUE(renderer != nullptr);
    (void)renderer->setValueFromID(kRenderDelegateCyclesId, 0);

    normal->setValue(true);
    custom->setTable({ { "st", "primvar", "texCoord2f", "" } });
    {
        const UsdStageRefPtr authored = stageOf(node, &lease);
        const PXR_NS::UsdRenderSettings prim(authored->GetPrimAtPath(PXR_NS::SdfPath(kUSDRenderSettingsDefaultPrimPath)));
        ASSERT_TRUE(bool(prim));
        PXR_NS::SdfPathVector products;
        prim.GetProductsRel().GetTargets(&products);
        EXPECT_EQ(PXR_NS::SdfPathVector({ PXR_NS::SdfPath("/Render/Products/rendersettings") }), products);
        const PXR_NS::UsdRenderProduct product(authored->GetPrimAtPath(PXR_NS::SdfPath("/Render/Products/rendersettings")));
        ASSERT_TRUE(bool(product));
        PXR_NS::SdfPathVector ordered;
        product.GetOrderedVarsRel().GetTargets(&ordered);
        EXPECT_EQ(PXR_NS::SdfPathVector({ PXR_NS::SdfPath("/Render/Products/Vars/cycles_normal"), PXR_NS::SdfPath("/Render/Products/Vars/st") }), ordered);
        EXPECT_TRUE(authored->GetPrimAtPath(PXR_NS::SdfPath("/Render/Products")).IsA<PXR_NS::UsdGeomScope>());
        EXPECT_TRUE(authored->GetPrimAtPath(PXR_NS::SdfPath("/Render/Products/Vars")).IsA<PXR_NS::UsdGeomScope>());
    }
    lease = SceneQueryPool::Lease();
    std::vector<AuthoredVar> vars = productVars(stageOf(node, &lease));
    lease = SceneQueryPool::Lease();
    ASSERT_EQ(2u, vars.size());
    EXPECT_EQ(std::string("cycles:normal"), vars[0].sourceName);
    EXPECT_EQ(std::string("raw"), vars[0].sourceType);
    EXPECT_EQ(std::string(), vars[0].dataType);
    EXPECT_EQ(std::string("st"), vars[1].sourceName);
    EXPECT_EQ(std::string("primvar"), vars[1].sourceType);
    EXPECT_EQ(std::string("texCoord2f"), vars[1].dataType);

    const U64 renderHash = render->getHashValue();
    beauty->setValue(true);
    EXPECT_EQ(renderHash, render->getHashValue()) << "a MoonRay tick changed the hash of a Cycles render";
    UsdStageRefPtr stage = stageOf(node, &lease);
    vars = productVars(stage);
    ASSERT_EQ(3u, vars.size());
    EXPECT_TRUE(findVar(vars, "moonray:beauty") != nullptr);
    const PXR_NS::UsdRenderSettings prim(stage->GetPrimAtPath(PXR_NS::SdfPath(kUSDRenderSettingsDefaultPrimPath)));
    RenderSettingsPrim::AovRequests requests;
    std::string error;
    ASSERT_TRUE(RenderSettingsPrim::aovRequests(prim, kRenderDelegateCyclesId, PXR_NS::UsdTimeCode(1.), &requests, &error)) << error;
    std::vector<std::string> names;
    for (const RenderSettingsPrim::AovRequest& request : requests.requests) {
        names.push_back(request.name.GetString());
    }
    EXPECT_EQ(std::vector<std::string>({ "cycles:normal", "primvars:st" }), names);
    EXPECT_TRUE(requests.rejected.empty());
    ASSERT_TRUE(RenderSettingsPrim::aovRequests(prim, kMoonRayRendererId, PXR_NS::UsdTimeCode(1.), &requests, &error)) << error;
    names.clear();
    for (const RenderSettingsPrim::AovRequest& request : requests.requests) {
        names.push_back(request.name.GetString());
    }
    EXPECT_EQ(std::vector<std::string>({ "moonray:beauty", "primvars:st" }), names);
    stage = UsdStageRefPtr();
    lease = SceneQueryPool::Lease();

    // USDWrite keeps the product and its vars.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QString::fromUtf8("/rendervars.usda")).toStdString();
    NodePtr writer = createNode(QString::fromUtf8(PLUGINID_NATRON_USDWRITE));
    ASSERT_TRUE(bool(writer));
    ASSERT_TRUE(writer->connectInput(node, 0));
    KnobOutputFile* file = knob<KnobOutputFile>(writer, kUSDWriteParamFile);
    KnobChoice* exportMode = knob<KnobChoice>(writer, kUSDWriteParamExportMode);
    KnobChoice* frameRange = knob<KnobChoice>(writer, kUSDWriteParamFrameRange);
    KnobInt* first = knob<KnobInt>(writer, kUSDWriteParamFirstFrame);
    KnobInt* last = knob<KnobInt>(writer, kUSDWriteParamLastFrame);
    ASSERT_TRUE(file && exportMode && frameRange && first && last);
    file->setValue(path);
    exportMode->setValue((int)USDWrite::eExportModeFlattened);
    frameRange->setValue((int)USDWrite::eFrameRangeManual);
    first->setValue(1);
    last->setValue(1);
    USDWrite* write = dynamic_cast<USDWrite*>(writer->getEffectInstance().get());
    ASSERT_TRUE(write != nullptr);
    ASSERT_EQ(eStatusOK, write->writeFrameRange(1, 1, ViewIdx(0)));
    ASSERT_TRUE(QFile::exists(QString::fromStdString(path)));
    {
        const UsdStageRefPtr written = PXR_NS::UsdStage::Open(path);
        ASSERT_TRUE(written != nullptr);
        const std::vector<AuthoredVar> back = productVars(written);
        ASSERT_EQ(3u, back.size());
        EXPECT_TRUE(findVar(back, "cycles:normal") != nullptr);
        EXPECT_TRUE(findVar(back, "moonray:beauty") != nullptr);
        const AuthoredVar* st = findVar(back, "st");
        ASSERT_TRUE(st != nullptr);
        EXPECT_EQ(std::string("primvar"), st->sourceType);
        EXPECT_EQ(std::string("texCoord2f"), st->dataType);
    }

    // A named layer, then unticking.
    custom->setTable({ { "st", "primvar", "texCoord2f", "uv" } });
    beauty->setValue(false);
    vars = productVars(stageOf(node, &lease));
    lease = SceneQueryPool::Lease();
    ASSERT_EQ(2u, vars.size());
    EXPECT_TRUE(findVar(vars, "moonray:beauty") == nullptr);
    const AuthoredVar* st = findVar(vars, "st");
    ASSERT_TRUE(st != nullptr);
    EXPECT_EQ(std::string("uv"), st->layer);

    // A source type the schema has no name for is an error.
    custom->setTable({ { "st", "texture", "", "" } });
    ScenePayload payload;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error));
    EXPECT_NE(std::string::npos, error.find("texture")) << error;
    custom->setTable({ { "st", "primvar", "texCoord2f", "" } });

    // A Cycles render: normal is the layer Cycles always gives, st the declared primvar.
    if (renderers.isStandIn(kRenderDelegateCyclesId) || !CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    QCoreApplication::processEvents();
    std::list<ImageLayerDesc> present;
    render->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &present);
    bool haveNormal = false;
    bool haveSt = false;
    for (const ImageLayerDesc& layer : present) {
        haveNormal = haveNormal || (layer.getLayerID() == "normal");
        haveSt = haveSt || (layer.getLayerID() == "st");
        EXPECT_NE(std::string("beauty"), layer.getLayerID());
    }
    EXPECT_TRUE(haveNormal);
    EXPECT_TRUE(haveSt);
}

// A list in the renderer's own namespace, as hdCycles publishes it, names each tick after the AOV
// without that namespace; a name in another namespace keeps it.
TEST_F(USDRenderSettingsTest, ChecklistTicksAreNamedWithoutTheRenderersNamespace)
{
    const ChecklistRenderers renderers({ "cycles:ao", "cycles:diffuse_direct", "cycles:normal", "cycles:lightgroup:key", "primvars:st" });
    NodePtr cam = camera("/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    const std::string prefix = RenderSettingsKnobs::scriptPrefix(kRenderDelegateCyclesId);
    const std::vector<std::pair<std::string, std::string>> expected = {
        { "AovAo", "AO" },
        { "AovDiffuseDirect", "Diffuse Direct" },
        { "AovNormal", "Normal" },
        { "AovLightgroupKey", "Lightgroup Key" },
        { "AovPrimvarsSt", "Primvars St" },
    };
    for (const std::pair<std::string, std::string>& tick : expected) {
        KnobBool* k = knob<KnobBool>(node, (prefix + tick.first).c_str());
        EXPECT_TRUE(k != nullptr) << prefix + tick.first;
        if (k) {
            EXPECT_EQ(tick.second, k->getLabel()) << prefix + tick.first;
        }
    }
    EXPECT_TRUE(node->getKnobByName(prefix + "AovCyclesAo") == nullptr);

    KnobBool* ao = knob<KnobBool>(node, (prefix + "AovAo").c_str());
    ASSERT_TRUE(ao != nullptr);
    ao->setValue(true);
    SceneQueryPool::Lease lease;
    const std::vector<AuthoredVar> vars = productVars(stageOf(node, &lease));
    lease = SceneQueryPool::Lease();
    ASSERT_EQ(1u, vars.size());
    EXPECT_EQ(std::string("cycles:ao"), vars[0].sourceName);
}

// Scripts author custom vars through the Custom AOVs table; a malformed row raises instead of
// corrupting the table.
TEST_F(USDRenderSettingsTest, PythonAuthorsCustomAovs)
{
    NodePtr cam = camera("/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(RenderSettingsTestUtils::configure(node, "/Cam", 64, 32));
    KnobRenderVars* custom = knob<KnobRenderVars>(node, kUSDRenderSettingsParamCustomAovs);
    ASSERT_TRUE(custom != nullptr);

    const std::string param = "param = " + getApp()->getAppIDString() + ".getNode(\"" + node->getScriptName() + "\").getParam(\"" + kUSDRenderSettingsParamCustomAovs + "\")\n";
    const std::string script = "import NatronEngine\n" + param + "assert isinstance(param, NatronEngine.RenderVarsParam), type(param)\n"
                                                                 "param.setTable([['st', 'primvar', 'texCoord2f', '']])\n"
                                                                 "param.appendRow(['C<RD>L', 'lpe', '', 'diffuseLpe'])\n"
                                                                 "assert param.getTable() == [['st', 'primvar', 'texCoord2f', ''], ['C<RD>L', 'lpe', '', 'diffuseLpe']], param.getTable()\n";
    std::string error, output;
    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;

    std::list<std::vector<std::string>> rows;
    custom->getTable(&rows);
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ(std::string("SourceType"), custom->getColumnTag(KnobRenderVars::eColumnSourceType));
    EXPECT_EQ(std::vector<std::string>({ "st", "primvar", "texCoord2f", "" }), rows.front());

    SceneQueryPool::Lease lease;
    const std::vector<AuthoredVar> vars = productVars(stageOf(node, &lease));
    lease = SceneQueryPool::Lease();
    const AuthoredVar* st = findVar(vars, "st");
    ASSERT_TRUE(st != nullptr);
    EXPECT_EQ(std::string("primvar"), st->sourceType);
    const AuthoredVar* lpe = findVar(vars, "C<RD>L");
    ASSERT_TRUE(lpe != nullptr);
    EXPECT_EQ(std::string("lpe"), lpe->sourceType);

    const std::string shortRow = "import NatronEngine\n" + param + "param.appendRow(['st', 'primvar'])\n";
    EXPECT_FALSE(interpretPythonScript(shortRow, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
    rows.clear();
    custom->getTable(&rows);
    EXPECT_EQ(2u, rows.size());
}

// The layers a USDRender produces follow the vars upstream, so a viewer below it re-lists its
// layer menu on each AOV edit, with no reconnection.
TEST_F(USDRenderSettingsTest, AovEditsRefreshTheLayerMenuOfAViewerDownstream)
{
    const ChecklistRenderers renderers;
    NodePtr cam = camera("/Cam");
    NodePtr node = settings(cam);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(RenderSettingsTestUtils::configure(node, "/Cam", 64, 32));
    KnobBool* normal = knob<KnobBool>(node, (RenderSettingsKnobs::scriptPrefix(kRenderDelegateCyclesId) + "AovNormal").c_str());
    KnobRenderVars* custom = knob<KnobRenderVars>(node, kUSDRenderSettingsParamCustomAovs);
    ASSERT_TRUE(normal && custom);

    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(render->connectInput(node, 0));
    CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
    viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr viewerNode = getApp()->createNode(viewerArgs);
    ASSERT_TRUE(bool(viewerNode));
    ViewerInstance* viewer = viewerNode->isEffectViewer();
    ASSERT_TRUE(viewer != nullptr);
    ASSERT_TRUE(viewerNode->connectInput(render, 0));
    QCoreApplication::processEvents();

    int refreshes = 0;
    QMetaObject::Connection counter = QObject::connect(viewer, &ViewerInstance::availableComponentsChanged, [&refreshes]() {
        ++refreshes;
    });
    normal->setValue(true);
    EXPECT_GT(refreshes, 0) << "ticking an AOV left the viewer's layer menu as it was";

    refreshes = 0;
    custom->setTable({ { "st", "primvar", "texCoord2f", "" } });
    EXPECT_GT(refreshes, 0) << "a custom AOV left the viewer's layer menu as it was";
    QObject::disconnect(counter);
}

// The input scene's product stays listed beside the node's, and a var of the input scene's that
// shares a name with one of the node's takes none of its data type or layer.
TEST_F(USDRenderSettingsTest, TheAovChecklistKeepsTheInputScenesProducts)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(file != nullptr);
    file->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/scene-rendervars.usda"));
    NodePtr node = settings(reader);
    ASSERT_TRUE(bool(node));
    KnobRenderVars* custom = knob<KnobRenderVars>(node, kUSDRenderSettingsParamCustomAovs);
    ASSERT_TRUE(custom != nullptr);
    custom->setTable({ { "normal", "raw", "", "" }, { "st", "primvar", "texCoord2f", "" } });

    SceneQueryPool::Lease lease;
    const UsdStageRefPtr stage = stageOf(node, &lease);
    ASSERT_TRUE(stage != nullptr);
    const PXR_NS::UsdRenderSettings prim(stage->GetPrimAtPath(PXR_NS::SdfPath(kUSDRenderSettingsDefaultPrimPath)));
    ASSERT_TRUE(bool(prim));
    PXR_NS::SdfPathVector products;
    prim.GetProductsRel().GetTargets(&products);
    EXPECT_EQ(PXR_NS::SdfPathVector({ PXR_NS::SdfPath("/Render/Products/rendersettings"), PXR_NS::SdfPath("/Render/Products/beauty") }), products);
    const PXR_NS::UsdRenderVar normal(stage->GetPrimAtPath(PXR_NS::SdfPath("/Render/Products/Vars/normal")));
    ASSERT_TRUE(bool(normal));
    EXPECT_FALSE(normal.GetDataTypeAttr().HasAuthoredValue());
    const std::vector<AuthoredVar> vars = productVars(stage);
    EXPECT_TRUE(findVar(vars, "depth") != nullptr);
    EXPECT_TRUE(findVar(vars, "instanceId") != nullptr);
    EXPECT_TRUE(findVar(vars, "st") != nullptr);
}
