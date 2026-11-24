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

#include <pxr/usd/usd/stageLoadRules.h>

#include "Global/Macros.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING

namespace {
const SceneStage::LockedSection kNoLock = [](const std::function<void()>& section) { section(); };

// What leaves the graph: the node's composed output, the bytes a flattened USDWrite writes, and
// the stage a USDRender of the node renders, with the prims it loaded.
struct Output {
    std::string scene;
    QByteArray written;
    std::string renderStage;
    std::vector<std::string> renderLoaded;
};

void
expectSameOutput(const Output& expected,
                 const Output& actual,
                 const char* what)
{
    EXPECT_EQ(expected.scene, actual.scene) << what << ": the output scene";
    EXPECT_TRUE(expected.written == actual.written) << what << ": USDWrite's bytes";
    EXPECT_EQ(expected.renderStage, actual.renderStage) << what << ": USDRender's stage";
    EXPECT_EQ(expected.renderLoaded, actual.renderLoaded) << what << ": the payloads USDRender's stage loads";
}

// Every open layer, anonymous ones included, and whether it has unsaved edits.
std::map<std::string, bool>
layerDirtiness()
{
    std::map<std::string, bool> dirty;

    for (const PXR_NS::SdfLayerHandle& layer : PXR_NS::SdfLayer::GetLoadedLayers()) {
        if (layer) {
            dirty[layer->GetIdentifier()] = layer->IsDirty();
        }
    }

    return dirty;
}

// The layers open at before that were clean then and are dirty now, and the files on disk with
// unsaved edits.
std::vector<std::string>
layersDirtiedSince(const std::map<std::string, bool>& before)
{
    std::vector<std::string> dirtied;

    for (const PXR_NS::SdfLayerHandle& layer : PXR_NS::SdfLayer::GetLoadedLayers()) {
        if (!layer || !layer->IsDirty()) {
            continue;
        }
        const std::map<std::string, bool>::const_iterator found = before.find(layer->GetIdentifier());
        if (found != before.end() ? !found->second : !layer->IsAnonymous()) {
            dirtied.push_back(layer->GetIdentifier());
        }
    }

    return dirtied;
}

SceneLoadRules
loadRulesOf(const SceneStage& stage)
{
    SceneLoadRules rules;

    for (const std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>& rule : stage.getStage()->GetLoadRules().GetRules()) {
        if (rule.second == PXR_NS::UsdStageLoadRules::AllRule) {
            rules[rule.first.GetString()] = eSceneLoadRuleAll;
        } else if (rule.second == PXR_NS::UsdStageLoadRules::OnlyRule) {
            rules[rule.first.GetString()] = eSceneLoadRuleOnly;
        } else if (!rule.first.IsAbsoluteRootPath()) {
            rules[rule.first.GetString()] = eSceneLoadRuleNone;
        }
    }

    return rules;
}
} // anonymous namespace

// A USDRead of a city of payloads, viewed by a viewer whose draw states and load set live on a
// stage of its own, as the 3D view keeps them.
class SceneDrawStatesOutputTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ASSERT_TRUE(_dir.isValid());
        writeCity();
    }

    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    std::string path(const char* name) const
    {
        return (_dir.path() + QString::fromUtf8("/") + QString::fromUtf8(name)).toStdString();
    }

    void writeCity()
    {
        std::string root = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n)\ndef Xform \"World\"\n{\n"
                           "    def Camera \"Cam\"\n    {\n    }\n";
        for (int b = 0; b < 3; ++b) {
            root += "    def Xform \"Block_" + std::to_string(b) + "\"\n    {\n";
            for (int n = 0; n < 3; ++n) {
                const std::string name = "b" + std::to_string(b) + "_" + std::to_string(n) + ".usda";
                QFile building(QString::fromStdString(path(name.c_str())));
                ASSERT_TRUE(building.open(QIODevice::WriteOnly | QIODevice::Truncate));
                building.write(("#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\"\n{\n"
                                "    def Cube \"Geo\"\n    {\n        double size = "
                                + std::to_string(n + 1) + "\n    }\n}\n")
                                   .c_str());
                root += "        def Xform \"B_" + std::to_string(n) + "\" (\n            prepend payload = @./" + name + "@\n        )\n        {\n        }\n";
            }
            root += "    }\n";
        }
        root += "}\n";
        QFile file(QString::fromStdString(path("city.usda")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(root.c_str());
    }

    void createGraph()
    {
        _reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        ASSERT_TRUE(bool(_reader));
        KnobFile* file = dynamic_cast<KnobFile*>(_reader->getKnobByName(kUSDReadParamFile).get());
        ASSERT_TRUE(file != nullptr);
        file->setValue(path("city.usda"));

        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_USDWRITE));
        ASSERT_TRUE(bool(_writer));
        ASSERT_TRUE(_writer->connectInput(_reader, 0));
        KnobOutputFile* out = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kUSDWriteParamFile).get());
        KnobChoice* exportMode = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kUSDWriteParamExportMode).get());
        KnobChoice* frameRange = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kUSDWriteParamFrameRange).get());
        KnobInt* first = dynamic_cast<KnobInt*>(_writer->getKnobByName(kUSDWriteParamFirstFrame).get());
        KnobInt* last = dynamic_cast<KnobInt*>(_writer->getKnobByName(kUSDWriteParamLastFrame).get());
        ASSERT_TRUE(out && exportMode && frameRange && first && last);
        out->setValue(path("written.usda"));
        exportMode->setValue((int)USDWrite::eExportModeFlattened);
        frameRange->setValue((int)USDWrite::eFrameRangeManual);
        first->setValue(1);
        last->setValue(1);

        _render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        ASSERT_TRUE(bool(_render));
        ASSERT_TRUE(_render->connectInput(_reader, 0));

        CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
        viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        _viewerNode = getApp()->createNode(viewerArgs);
        ASSERT_TRUE(bool(_viewerNode));
        _viewer = _viewerNode->isEffectViewer();
        ASSERT_TRUE(_viewer != nullptr);
        ASSERT_TRUE(_viewerNode->connectInput(_reader, 0));
    }

    ScenePayload evaluate(const NodePtr& node) const
    {
        ScenePayload payload;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    Output output() const
    {
        Output result;
        const PXR_NS::UsdStageRefPtr composed = SceneOps::compose(evaluate(_reader));
        EXPECT_TRUE(composed && composed->ExportToString(&result.scene, false));

        USDWrite* writer = dynamic_cast<USDWrite*>(_writer->getEffectInstance().get());
        EXPECT_TRUE(writer != nullptr);
        if (writer) {
            EXPECT_EQ(eStatusOK, writer->writeFrameRange(1, 1, ViewIdx(0)));
            QFile written(QString::fromStdString(path("written.usda")));
            EXPECT_TRUE(written.open(QIODevice::ReadOnly));
            result.written = written.readAll();
        }

        // What USDRender renders: the stage the query pool composes of its input's scene.
        EffectInstancePtr renderInput = _render->getEffectInstance()->getInput(0);
        EXPECT_TRUE(bool(renderInput));
        if (renderInput) {
            std::string error;
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(evaluate(renderInput->getNode()), SceneLoadSet::everything(), &error);
            const PXR_NS::UsdStageRefPtr stage = lease.getStage();
            EXPECT_TRUE(bool(stage)) << error;
            if (stage) {
                EXPECT_TRUE(stage->ExportToString(&result.renderStage, false));
                for (const PXR_NS::SdfPath& loaded : stage->GetLoadSet()) {
                    result.renderLoaded.push_back(loaded.GetString());
                }
            }
        }

        return result;
    }

    // The 3D view's stage of the viewed scene, which loads nothing until asked.
    void viewScene(SceneStage* stage) const
    {
        ASSERT_TRUE(stage->sync(evaluate(_reader)));
    }

    // A load above a prim loads on its own first and asks for its descendants again, so a stage
    // takes more than one pass to load a set.
    static void serviceAll(SceneStage* stage)
    {
        for (int pass = 0; pass < 16 && stage->hasPendingLoads(); ++pass) {
            EXPECT_TRUE(stage->serviceLoads(kNoLock, kNoLock));
        }
        EXPECT_FALSE(stage->hasPendingLoads());
    }

    static void applyLoads(SceneStage* stage,
                           const SceneDrawDelta& delta)
    {
        for (const SceneDrawLoadRequest& load : delta.loads) {
            PXR_NS::SdfPathVector excluded;
            for (const std::string& path : load.excluded) {
                excluded.push_back(PXR_NS::SdfPath(path));
            }
            stage->requestLoad(PXR_NS::SdfPathVector(1, PXR_NS::SdfPath(load.path)), load.withDescendants, excluded);
        }
    }

    QTemporaryDir _dir;
    NodePtr _reader;
    NodePtr _writer;
    NodePtr _render;
    NodePtr _viewerNode;
    ViewerInstance* _viewer = nullptr;
};

TEST_F(SceneDrawStatesOutputTest, ViewerStatesAndLoadsLeaveEveryOutputAsItWas)
{
    createGraph();
    ASSERT_FALSE(HasFatalFailure());
    const Output before = output();
    ASSERT_FALSE(before.scene.empty());
    ASSERT_FALSE(before.written.isEmpty());
    ASSERT_FALSE(before.renderStage.empty());
    const std::map<std::string, bool> layersBefore = layerDirtiness();

    SceneStage viewStage(PXR_NS::UsdStage::LoadNone);
    viewScene(&viewStage);
    SceneDrawStates states;
    states.setState("/World/Block_0", eSceneDrawStateLoadAll);
    states.setState("/World/Block_0/B_1", eSceneDrawStateHidden);
    states.setState("/World/Block_1", eSceneDrawStateBBox);
    states.setState("/World/Block_1/B_2", eSceneDrawStateFull);
    states.setState("/World/Gone", eSceneDrawStateHidden);
    expectSameOutput(before, output(), "with draw states set");

    SceneDrawStates fresh;
    applyLoads(&viewStage, fresh.replaceSetStates(states.getSetStates()));
    viewStage.requestLoad(PXR_NS::SdfPathVector(1, PXR_NS::SdfPath("/World/Block_2/B_0")), false);
    serviceAll(&viewStage);
    ASSERT_TRUE(viewStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_0")).IsLoaded());
    ASSERT_TRUE(viewStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_2/B_0")).IsLoaded());
    ASSERT_FALSE(viewStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_2/B_1")).IsLoaded());
    expectSameOutput(before, output(), "with the viewer's load set applied");

    const SceneLoadRules rules = loadRulesOf(viewStage);
    ASSERT_FALSE(rules.empty());
    _viewer->setSceneViewSaver([&states, &rules](Serialization::SceneViewSerialization* serialization) {
        sceneViewToSerialization(states.getSetStates(), rules, serialization);
    });
    Serialization::NodeSerialization viewerSaved;
    _viewerNode->toSerialization(&viewerSaved);
    EXPECT_FALSE(viewerSaved.sceneView.isEmpty());
    // A viewer tab opened now restores what the saver saves, not nothing.
    std::vector<std::pair<std::string, SceneDrawStates::Stored>> reopened;
    SceneLoadRules reopenedRules;
    sceneViewFromSerialization(_viewer->getLoadedSceneView(), &reopened, &reopenedRules);
    EXPECT_EQ(reopened.size(), states.getSetStates().size());
    EXPECT_EQ(reopenedRules, rules);
    for (const NodePtr& node : { _reader, _writer, _render }) {
        Serialization::NodeSerialization saved;
        node->toSerialization(&saved);
        EXPECT_TRUE(saved.sceneView.isEmpty()) << node->getScriptName() << " saves no viewer state";
    }
    expectSameOutput(before, output(), "with the viewer's view saved");

    viewStage.requestUnload(PXR_NS::SdfPathVector(1, PXR_NS::SdfPath("/World/Block_0")));
    serviceAll(&viewStage);
    states.clearAllStates();
    _viewer->setSceneViewSaver(ViewerInstance::SceneViewSaver());
    expectSameOutput(before, output(), "with the states cleared and the loads undone");

    const std::vector<std::string> dirtied = layersDirtiedSince(layersBefore);
    EXPECT_TRUE(dirtied.empty()) << dirtied.front();
}

TEST_F(SceneDrawStatesOutputTest, ARestoredViewLeavesEveryOutputAsItWas)
{
    createGraph();
    ASSERT_FALSE(HasFatalFailure());
    const Output before = output();
    const std::map<std::string, bool> layersBefore = layerDirtiness();

    Serialization::SceneViewSerialization saved;
    saved.drawStates["/World/Block_0"] = "loadAll";
    saved.drawStates["/World/Block_2"] = "children";
    saved.drawStates["/World/Block_2/B_0"] = "hidden";
    saved.loadRules["/World/Block_1/B_1"] = "only";
    saved.loadRules["/World/Block_1/Missing"] = "only";
    std::vector<std::pair<std::string, SceneDrawStates::Stored>> restoredStates;
    SceneLoadRules restoredRules;
    sceneViewFromSerialization(saved, &restoredStates, &restoredRules);

    SceneStage viewStage(PXR_NS::UsdStage::LoadNone);
    viewScene(&viewStage);
    SceneDrawStates states;
    applyLoads(&viewStage, states.replaceSetStates(restoredStates));
    for (const SceneDrawLoadRequest& load : sceneLoadRequestsFor(restoredRules)) {
        PXR_NS::SdfPathVector excluded;
        for (const std::string& path : load.excluded) {
            excluded.push_back(PXR_NS::SdfPath(path));
        }
        viewStage.requestLoad(PXR_NS::SdfPathVector(1, PXR_NS::SdfPath(load.path)), load.withDescendants, excluded);
    }
    serviceAll(&viewStage);
    EXPECT_TRUE(viewStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_2")).IsLoaded());
    EXPECT_TRUE(viewStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_1/B_1")).IsLoaded());
    expectSameOutput(before, output(), "with a saved view restored");
    const std::vector<std::string> dirtied = layersDirtiedSince(layersBefore);
    EXPECT_TRUE(dirtied.empty()) << dirtied.front();
}
