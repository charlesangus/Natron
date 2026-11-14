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

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <pxr/base/tf/weakBase.h>
#include <pxr/base/tf/weakPtr.h>

#include "BaseTest.h"
#include "SceneTestAuthoring.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
QString
getMessage(const NodePtr& node,
           int* type)
{
    QString message;

    node->getPersistentMessage(&message, type, false);

    return message;
}

// Records whether a stage ever sent a notice while one prim was on it and active.
class ActivePrimWatch
    : public PXR_NS::TfWeakBase {
public:
    ActivePrimWatch(const PXR_NS::UsdStageRefPtr& stage,
                    const std::string& path)
        : _stage(stage)
        , _path(path)
    {
        _key = PXR_NS::TfNotice::Register(PXR_NS::TfCreateWeakPtr(this), &ActivePrimWatch::onObjectsChanged,
                                          PXR_NS::UsdStagePtr(stage));
    }

    ~ActivePrimWatch()
    {
        PXR_NS::TfNotice::Revoke(_key);
    }

    void onObjectsChanged(const PXR_NS::UsdNotice::ObjectsChanged&)
    {
        ++notices;
        const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(_path);
        if (prim && prim.IsActive()) {
            everActive = true;
        }
    }

    int notices = 0;
    bool everActive = false;

private:
    PXR_NS::UsdStageRefPtr _stage;
    PXR_NS::SdfPath _path;
    PXR_NS::TfNotice::Key _key;
};

const SceneStage::LockedSection kUnlocked = [](const std::function<void()>& section) { section(); };
} // anonymous namespace

class ScenePatternContextTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr reader(const CarCity& city)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (node) {
            KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(node->getKnobByName(kUSDReadParamFile));
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(city.rootPath());
            }
        }

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDPruneParamPrimPattern).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(pattern);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     const SceneEvaluationContext& context,
                                                     ScenePayload* payload)
    {
        std::string error;

        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), payload, &error);
    }

    // Whether the prim at path is active once every payload of payload is loaded.
    static bool activeWhenLoaded(const ScenePayload& payload,
                                 const std::string& path)
    {
        const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);

        EXPECT_TRUE(bool(stage));
        const PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath(path)) : PXR_NS::UsdPrim();
        EXPECT_TRUE(bool(prim)) << path;

        return prim && prim.IsActive();
    }

    static U64 queries(const NodePtr& node)
    {
        const USDPrune* effect = dynamic_cast<const USDPrune*>(node->getEffectInstance().get());

        EXPECT_TRUE(effect != NULL);

        return effect ? effect->getPatternQueryCount() : 0;
    }
};

TEST(PrimSelectorReach, PrefixesAndLiteralsComeFromTheExpressionAlone)
{
    PrimSelector::Reach reach;
    std::string error;

    ASSERT_TRUE(PrimSelector::getReach("/World/Block_3//car*", &reach, &error)) << error;
    EXPECT_EQ(reach.prefixes, std::vector<std::string>({ "/World/Block_3" }));
    EXPECT_FALSE(reach.literalOnly);

    ASSERT_TRUE(PrimSelector::getReach("/World/A /World/B", &reach, &error)) << error;
    EXPECT_TRUE(reach.literalOnly);
    EXPECT_EQ(reach.literals, std::vector<std::string>({ "/World/A", "/World/B" }));

    // What a difference takes away adds no reach.
    ASSERT_TRUE(PrimSelector::getReach("/World/Block_3//* - /Other//*", &reach, &error)) << error;
    EXPECT_EQ(reach.prefixes, std::vector<std::string>({ "/World/Block_3" }));
    EXPECT_FALSE(reach.literalOnly);

    ASSERT_TRUE(PrimSelector::getReach("~/World/A", &reach, &error)) << error;
    EXPECT_EQ(reach.prefixes, std::vector<std::string>({ "/" }));

    ASSERT_TRUE(PrimSelector::getReach("//car*", &reach, &error)) << error;
    EXPECT_EQ(reach.prefixes, std::vector<std::string>({ "/" }));

    EXPECT_FALSE(PrimSelector::getReach("/World/[", &reach, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ScenePatternContextTest, AViewerMatchesOnlyInsideWhatItLoads)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);
    const U64 viewer = SceneEvaluationContext::makeId();

    ScenePayload loaded;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loadingOnly({ "/World/Block_0" })), &loaded));
    EXPECT_FALSE(activeWhenLoaded(loaded, carPath(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(loaded, carPath(0, 1)));

    ScenePayload unloaded;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &unloaded));
    EXPECT_TRUE(activeWhenLoaded(unloaded, carPath(0, 0)));

    // A render loads everything, and matches everything.
    ScenePayload rendered;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(), &rendered));
    EXPECT_FALSE(activeWhenLoaded(rendered, carPath(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(rendered, carPath(0, 1)));
    EXPECT_TRUE(activeWhenLoaded(rendered, carPath(1, 0)));
}

TEST_F(ScenePatternContextTest, LoadingWhereThePatternCannotReachDoesNotQueryAgain)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);
    const U64 viewer = SceneEvaluationContext::makeId();

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loadingOnly({ "/World/Block_0" })), &payload));
    const U64 before = queries(node);
    EXPECT_GE(before, (U64)1);

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loadingOnly({ "/World/Block_0", "/World/Block_1" })), &payload));
    EXPECT_EQ(before, queries(node));
    EXPECT_FALSE(activeWhenLoaded(payload, carPath(0, 0)));

    // Unloading inside the pattern's reach does query again.
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loadingOnly({ "/World/Block_0/B_1" })), &payload));
    EXPECT_EQ(before + 1, queries(node));
    EXPECT_TRUE(activeWhenLoaded(payload, carPath(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(payload, carPath(0, 1)));
}

TEST_F(ScenePatternContextTest, ALoadOrUnloadReEvaluatesOnlyWhereAPatternReaches)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    ASSERT_TRUE(bool(source));
    const U64 viewer = SceneEvaluationContext::makeId();
    const SceneLoadSetPtr nothing = loadingOnly({});
    const SceneLoadSetPtr building = loadingOnly({ "/World/Block_0/B_0" });
    const SceneLoadSetPtr otherBlock = loadingOnly({ "/World/Block_1/B_0" });
    const SceneLoadSetPtr both = loadingOnly({ "/World/Block_0/B_0", "/World/Block_1/B_0" });

    // No pattern: neither a load nor an unload asks for the scene again.
    ScenePayload payload;
    SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, SceneEvaluationContext(viewer, nothing, reach), &payload));
    EXPECT_TRUE(reach->getPrefixes().empty());
    EXPECT_FALSE(SceneOps::loadChangeReaches(nothing.get(), building.get(), reach->getPrefixes()));
    EXPECT_FALSE(SceneOps::loadChangeReaches(building.get(), nothing.get(), reach->getPrefixes()));

    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(bool(node));
    reach = std::make_shared<SceneLoadReach>();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, nothing, reach), &payload));
    EXPECT_EQ(reach->getPrefixes(), std::vector<std::string>({ "/World/Block_0" }));
    EXPECT_TRUE(activeWhenLoaded(payload, carPath(0, 0)));
    const U64 before = queries(node);

    // A pattern that cannot reach the payload: still nothing to evaluate.
    EXPECT_FALSE(SceneOps::loadChangeReaches(nothing.get(), otherBlock.get(), reach->getPrefixes()));
    EXPECT_FALSE(SceneOps::loadChangeReaches(both.get(), building.get(), reach->getPrefixes()));

    // One that can: evaluated once more, it matches inside the loaded payload.
    ASSERT_TRUE(SceneOps::loadChangeReaches(nothing.get(), building.get(), reach->getPrefixes()));
    reach = std::make_shared<SceneLoadReach>();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, building, reach), &payload));
    EXPECT_EQ(before + 1, queries(node));
    EXPECT_FALSE(activeWhenLoaded(payload, carPath(0, 0)));
    EXPECT_TRUE(activeWhenLoaded(payload, carPath(0, 1)));

    // Its unload reaches it as well, and the match goes with it.
    ASSERT_TRUE(SceneOps::loadChangeReaches(building.get(), nothing.get(), reach->getPrefixes()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, nothing, reach), &payload));
    EXPECT_TRUE(activeWhenLoaded(payload, carPath(0, 0)));
}

TEST_F(ScenePatternContextTest, PlainPathsMatchInsideUnloadedPayloads)
{
    CarCity city(1, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, carPath(0, 1));
    ASSERT_TRUE(source && node);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &payload));
    EXPECT_FALSE(activeWhenLoaded(payload, carPath(0, 1)));
    EXPECT_TRUE(activeWhenLoaded(payload, carPath(0, 0)));
    int type = 0;
    getMessage(node, &type);
    EXPECT_NE((int)eMessageTypeWarning, type);
}

TEST_F(ScenePatternContextTest, NoMatchSaysHowManyUnloadedBranchesWereNotSearched)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World//car*");
    ASSERT_TRUE(source && node);
    const U64 viewer = SceneEvaluationContext::makeId();
    ScenePayload payload;
    int type = 0;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &payload));
    QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("0 matches, 4 unloaded branches not searched"))) << message.toStdString();

    // Everything loaded and still no match: the plain warning.
    NodePtr none = prune(source, "/World//truck*");
    ASSERT_TRUE(bool(none));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(none, SceneEvaluationContext(viewer, loadingOnly({ "/World" })), &payload));
    message = getMessage(none, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("matches no prim"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("unloaded"))) << message.toStdString();

    // A render loads everything: the plain warning too.
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(none, SceneEvaluationContext(), &payload));
    message = getMessage(none, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_FALSE(message.contains(QString::fromUtf8("unloaded"))) << message.toStdString();

    // A pattern that does not parse stays an error, in any context.
    NodePtr invalid = prune(source, "/World/[");
    ASSERT_TRUE(bool(invalid));
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(invalid, SceneEvaluationContext(viewer, std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &payload));
    getMessage(invalid, &type);
    EXPECT_EQ((int)eMessageTypeError, type);
}

TEST_F(ScenePatternContextTest, APrunedPrimInsideAPayloadNeverShowsDuringItsLoad)
{
    CarCity city(1, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);
    const U64 viewer = SceneEvaluationContext::makeId();
    const PXR_NS::SdfPath building("/World/Block_0/B_0");
    const PXR_NS::SdfPath pruned(carPath(0, 0));
    ScenePayload shown;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loadingOnly({})), &shown));

    // Loaded without the scene the load makes the pattern match, the car shows: the watch can see it.
    {
        SceneStage stage(PXR_NS::UsdStage::LoadNone);
        ASSERT_TRUE(stage.sync(shown));
        ActivePrimWatch watch(stage.getStage(), pruned.GetString());
        stage.requestLoad(PXR_NS::SdfPathVector(1, building), false);
        EXPECT_TRUE(stage.serviceLoads(kUnlocked, kUnlocked));
        EXPECT_TRUE(watch.everActive);
    }

    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(shown));
    ActivePrimWatch watch(stage.getStage(), pruned.GetString());
    int evaluations = 0;
    const SceneStage::LoadPreparer prepare = [&](const SceneLoadSetPtr& loadSet) -> std::function<void()> {
        ScenePayload payload;
        if (evaluate(node, SceneEvaluationContext(viewer, loadSet), &payload) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << "the prune did not evaluate for the load";

            return std::function<void()>();
        }
        ++evaluations;

        return [&stage, payload]() { EXPECT_TRUE(stage.sync(payload)); };
    };
    stage.requestLoad(PXR_NS::SdfPathVector(1, building), false);
    EXPECT_TRUE(stage.serviceLoads(kUnlocked, kUnlocked, std::function<bool()>(), prepare));

    EXPECT_EQ(1, evaluations);
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building).IsLoaded());
    const PXR_NS::UsdPrim prim = stage.getStage()->GetPrimAtPath(pruned);
    ASSERT_TRUE(bool(prim));
    EXPECT_FALSE(prim.IsActive());
    EXPECT_GT(watch.notices, 0);
    EXPECT_FALSE(watch.everActive);
}

TEST_F(ScenePatternContextTest, AnUnloadedSearchFindsMatchesInsidePayloadsWithoutLoadingTheViewers)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    ASSERT_TRUE(bool(source));
    ScenePayload scene;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, SceneEvaluationContext(), &scene));

    SceneStage viewerStage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(viewerStage.sync(scene));
    const SceneLoadSetPtr viewerLoads = viewerStage.getLoadSet();

    const std::shared_ptr<SceneOps::UnloadedSearch> search = SceneOps::UnloadedSearch::start(scene, "/World//car*");
    ASSERT_TRUE(bool(search));
    ASSERT_TRUE(search->wait(60000.));
    EXPECT_EQ(SceneOps::UnloadedSearch::eStatusDone, search->getStatus()) << search->getError();
    EXPECT_EQ(search->getMatches(), std::vector<std::string>({ carPath(0, 0), carPath(0, 1), carPath(1, 0), carPath(1, 1) }));

    ASSERT_TRUE(viewerLoads && viewerStage.getLoadSet());
    EXPECT_EQ(*viewerLoads, *viewerStage.getLoadSet());
    EXPECT_FALSE(viewerStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_0")).IsLoaded());

    // Over a pattern node's input, beside a viewer that loads only another block.
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(bool(node));
    const U64 viewer = SceneEvaluationContext::makeId();
    const SceneLoadSetPtr otherBlock = loadingOnly({ "/World/Block_1" });
    ScenePayload shown;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, otherBlock), &shown));
    EXPECT_TRUE(activeWhenLoaded(shown, carPath(0, 0)));
    SceneStage shownStage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(shownStage.sync(shown));
    shownStage.applyLoadSet(otherBlock);
    const SceneLoadSetPtr shownLoads = shownStage.getLoadSet();
    const U64 shownLoadEdits = shownStage.getLoadEditCount();
    const U64 shownEdits = shownStage.getEditCount();
    const U64 queriesBefore = queries(node);

    const std::shared_ptr<SceneOps::UnloadedSearch> inputSearch = SceneOps::UnloadedSearch::start(node->getEffectInstance(), 0, "/World/Block_0//car*");
    ASSERT_TRUE(bool(inputSearch));
    ASSERT_TRUE(inputSearch->wait(60000.));
    EXPECT_EQ(SceneOps::UnloadedSearch::eStatusDone, inputSearch->getStatus()) << inputSearch->getError();
    EXPECT_EQ(inputSearch->getMatches(), std::vector<std::string>({ carPath(0, 0), carPath(0, 1) }));

    ASSERT_TRUE(shownLoads && shownStage.getLoadSet());
    EXPECT_EQ(*shownLoads, *shownStage.getLoadSet());
    EXPECT_EQ(shownLoadEdits, shownStage.getLoadEditCount());
    EXPECT_EQ(shownEdits, shownStage.getEditCount());
    EXPECT_FALSE(shownStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_0")).IsLoaded());

    // What the search found is not what the viewer's evaluation matches: its layer stays as it was.
    ScenePayload again;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, otherBlock), &again));
    EXPECT_EQ(queriesBefore, queries(node));
    EXPECT_TRUE(activeWhenLoaded(again, carPath(0, 0)));
}

TEST_F(ScenePatternContextTest, AnUnloadedSearchStopsWhenAsked)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    ASSERT_TRUE(bool(source));
    ScenePayload scene;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, SceneEvaluationContext(), &scene));

    // The search polls at least once before loading and once before matching.
    std::shared_ptr<std::atomic<int>> polls = std::make_shared<std::atomic<int>>(0);
    const std::shared_ptr<SceneOps::UnloadedSearch> search = SceneOps::UnloadedSearch::start(scene, "/World//car*", [polls]() { return ++(*polls) >= 2; });
    ASSERT_TRUE(bool(search));
    ASSERT_TRUE(search->wait(60000.));
    EXPECT_EQ(SceneOps::UnloadedSearch::eStatusCancelled, search->getStatus());
    EXPECT_TRUE(search->getMatches().empty());
}
