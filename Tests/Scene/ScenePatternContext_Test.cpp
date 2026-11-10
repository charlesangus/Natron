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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

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
// /World/Block_<b>/B_<n>, each building a payload holding /Geo and /car_0.
class CarCity {
public:
    CarCity(int blocks,
            int buildingsPerBlock)
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_pattern_city_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir / "buildings");
        _dir = std::filesystem::canonical(_dir);
        std::ofstream root(rootPath());
        root << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n    metersPerUnit = 1\n)\ndef Xform \"World\"\n{\n";
        for (int b = 0; b < blocks; ++b) {
            root << "    def Xform \"Block_" << b << "\"\n    {\n";
            for (int n = 0; n < buildingsPerBlock; ++n) {
                const std::string name = "b" + std::to_string(b) + "_" + std::to_string(n) + ".usda";
                std::ofstream building((_dir / "buildings" / name).string());
                building << "#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\"\n{\n"
                         << "    def Cube \"Geo\"\n    {\n    }\n    def Xform \"car_0\"\n    {\n    }\n}\n";
                root << "        def Xform \"B_" << n << "\" (\n            prepend payload = @./buildings/" << name << "@\n        )\n        {\n        }\n";
            }
            root << "    }\n";
        }
        root << "}\n";
    }

    ~CarCity()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    std::string rootPath() const
    {
        return (_dir / "root.usda").string();
    }

private:
    std::filesystem::path _dir;
};

std::string
car(int block,
    int index)
{
    return "/World/Block_" + std::to_string(block) + "/B_" + std::to_string(index) + "/car_0";
}

// Loads nothing but the subtrees of paths.
SceneLoadSetPtr
loading(const std::vector<std::string>& paths)
{
    std::vector<SceneLoadSet::Entry> entries;
    SceneLoadSet::Entry none;

    none.path = "/";
    none.rule = SceneLoadSet::eRuleNone;
    entries.push_back(none);
    for (const std::string& path : paths) {
        SceneLoadSet::Entry all;
        all.path = path;
        all.rule = SceneLoadSet::eRuleAll;
        entries.push_back(all);
    }

    return std::make_shared<const SceneLoadSet>(entries);
}

QString
getMessage(const NodePtr& node,
           int* type)
{
    QString message;

    node->getPersistentMessage(&message, type, false);

    return message;
}
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
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loading({ "/World/Block_0" })), &loaded));
    EXPECT_FALSE(activeWhenLoaded(loaded, car(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(loaded, car(0, 1)));

    ScenePayload unloaded;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &unloaded));
    EXPECT_TRUE(activeWhenLoaded(unloaded, car(0, 0)));

    // A render loads everything, and matches everything.
    ScenePayload rendered;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(), &rendered));
    EXPECT_FALSE(activeWhenLoaded(rendered, car(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(rendered, car(0, 1)));
    EXPECT_TRUE(activeWhenLoaded(rendered, car(1, 0)));
}

TEST_F(ScenePatternContextTest, LoadingWhereThePatternCannotReachDoesNotQueryAgain)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);
    const U64 viewer = SceneEvaluationContext::makeId();

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loading({ "/World/Block_0" })), &payload));
    const U64 before = queries(node);
    EXPECT_GE(before, (U64)1);

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loading({ "/World/Block_0", "/World/Block_1" })), &payload));
    EXPECT_EQ(before, queries(node));
    EXPECT_FALSE(activeWhenLoaded(payload, car(0, 0)));

    // Unloading inside the pattern's reach does query again.
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, loading({ "/World/Block_0/B_1" })), &payload));
    EXPECT_EQ(before + 1, queries(node));
    EXPECT_TRUE(activeWhenLoaded(payload, car(0, 0)));
    EXPECT_FALSE(activeWhenLoaded(payload, car(0, 1)));
}

TEST_F(ScenePatternContextTest, ALoadOrUnloadReEvaluatesOnlyWhereAPatternReaches)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    ASSERT_TRUE(bool(source));
    const U64 viewer = SceneEvaluationContext::makeId();
    const SceneLoadSetPtr nothing = loading({});
    const SceneLoadSetPtr building = loading({ "/World/Block_0/B_0" });
    const SceneLoadSetPtr otherBlock = loading({ "/World/Block_1/B_0" });
    const SceneLoadSetPtr both = loading({ "/World/Block_0/B_0", "/World/Block_1/B_0" });

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
    EXPECT_TRUE(activeWhenLoaded(payload, car(0, 0)));
    const U64 before = queries(node);

    // A pattern that cannot reach the payload: still nothing to evaluate.
    EXPECT_FALSE(SceneOps::loadChangeReaches(nothing.get(), otherBlock.get(), reach->getPrefixes()));
    EXPECT_FALSE(SceneOps::loadChangeReaches(both.get(), building.get(), reach->getPrefixes()));

    // One that can: evaluated once more, it matches inside the loaded payload.
    ASSERT_TRUE(SceneOps::loadChangeReaches(nothing.get(), building.get(), reach->getPrefixes()));
    reach = std::make_shared<SceneLoadReach>();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, building, reach), &payload));
    EXPECT_EQ(before + 1, queries(node));
    EXPECT_FALSE(activeWhenLoaded(payload, car(0, 0)));
    EXPECT_TRUE(activeWhenLoaded(payload, car(0, 1)));

    // Its unload reaches it as well, and the match goes with it.
    ASSERT_TRUE(SceneOps::loadChangeReaches(building.get(), nothing.get(), reach->getPrefixes()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(viewer, nothing, reach), &payload));
    EXPECT_TRUE(activeWhenLoaded(payload, car(0, 0)));
}

TEST_F(ScenePatternContextTest, PlainPathsMatchInsideUnloadedPayloads)
{
    CarCity city(1, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, car(0, 1));
    ASSERT_TRUE(source && node);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, SceneEvaluationContext(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none())), &payload));
    EXPECT_FALSE(activeWhenLoaded(payload, car(0, 1)));
    EXPECT_TRUE(activeWhenLoaded(payload, car(0, 0)));
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
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(none, SceneEvaluationContext(viewer, loading({ "/World" })), &payload));
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
    EXPECT_EQ(search->getMatches(), std::vector<std::string>({ car(0, 0), car(0, 1), car(1, 0), car(1, 1) }));

    ASSERT_TRUE(viewerLoads && viewerStage.getLoadSet());
    EXPECT_EQ(*viewerLoads, *viewerStage.getLoadSet());
    EXPECT_FALSE(viewerStage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_0")).IsLoaded());
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
