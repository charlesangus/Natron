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

#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerCustomData.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDConfigureLayer.h"
#include "Engine/Nodes/Scene/USDLayerBegin.h"
#include "Engine/Nodes/Scene/USDLayerEnd.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

ScenePayload
push(const ScenePayload& base,
     U64 hash)
{
    return SceneOps::appendLayer(base, SceneOps::createNodeLayer("SceneLayerRegionTest"), hash);
}

// A layer sublayering a file, as a USDRead's does.
ScenePayload
pushRead(const ScenePayload& base,
         U64 hash)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneLayerRegionTestRead");

    layer->InsertSubLayerPath(kReferenceScene);

    return SceneOps::appendLayer(base, layer, hash);
}

SceneLayerRegionSettings
savingTo(const std::string& path)
{
    SceneLayerRegionSettings settings;

    settings.savePath = path;
    settings.node = "Configure";

    return settings;
}

std::vector<PXR_NS::SdfLayer*>
layersOf(const ScenePayload& payload)
{
    std::vector<PXR_NS::SdfLayer*> layers;

    for (const SceneLayerEntry& entry : SceneOps::getLayers(payload)) {
        layers.push_back(entry.layer.operator->());
    }

    return layers;
}
} // anonymous namespace

TEST(SceneLayerRegion, EntriesAppendedWhileARegionIsOpenShareIt)
{
    const ScenePayload opened = SceneOps::openRegion(ScenePayload(), "Begin", SceneLayerRegionSettings());
    EXPECT_TRUE(opened.isEmpty());
    EXPECT_EQ((std::size_t)1, opened.getNumOpenRegions());
    EXPECT_NE((U64)0, opened.getStackHash());

    const ScenePayload closed = SceneOps::closeRegion(push(push(opened, 1), 2));
    EXPECT_EQ((std::size_t)0, closed.getNumOpenRegions());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(push(closed, 3));
    ASSERT_EQ((std::size_t)3, regions.size());
    ASSERT_TRUE(regions[0] != nullptr);
    EXPECT_EQ(regions[0], regions[1]);
    EXPECT_EQ(std::string("Begin"), regions[0]->node);
    EXPECT_TRUE(regions[0]->parent == nullptr);
    EXPECT_TRUE(regions[2] == nullptr);

    // Closing with nothing open changes nothing.
    EXPECT_TRUE(SceneOps::closeRegion(closed).isSameStack(closed));
    EXPECT_EQ(closed.getStackHash(), SceneOps::closeRegion(closed).getStackHash());
}

TEST(SceneLayerRegion, ARegionOpenedInsideAnotherNestsInIt)
{
    ScenePayload payload = push(SceneOps::openRegion(ScenePayload(), "Outer", SceneLayerRegionSettings()), 1);
    payload = push(SceneOps::openRegion(payload, "Inner", SceneLayerRegionSettings()), 2);
    EXPECT_EQ((std::size_t)2, payload.getNumOpenRegions());
    payload = push(SceneOps::closeRegion(payload), 3);
    payload = push(SceneOps::closeRegion(payload), 4);

    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(payload);
    ASSERT_EQ((std::size_t)4, regions.size());
    ASSERT_TRUE(regions[0] != nullptr);
    ASSERT_TRUE(regions[1] != nullptr);
    EXPECT_EQ(regions[0], regions[2]);
    EXPECT_EQ(regions[0], regions[1]->parent);
    EXPECT_TRUE(SceneOps::isRegionWithin(regions[1].get(), regions[0].get()));
    EXPECT_FALSE(SceneOps::isRegionWithin(regions[0].get(), regions[1].get()));
    EXPECT_TRUE(regions[3] == nullptr);
    EXPECT_EQ((std::size_t)3, SceneOps::countRegionEntries(payload, regions[0].get()));
}

TEST(SceneLayerRegion, PrecedingGathersBackToTheReadFile)
{
    const ScenePayload input = push(push(pushRead(ScenePayload(), 1), 2), 3);
    EXPECT_TRUE(SceneOps::isFileEntry(SceneOps::getLayers(input)[0]));
    EXPECT_FALSE(SceneOps::isFileEntry(SceneOps::getLayers(input)[1]));
    EXPECT_EQ(std::vector<std::size_t>({ 1, 2 }), SceneOps::getPrecedingRun(input));

    const ScenePayload claimed = SceneOps::claimPreceding(input, "Configure", savingTo("lighting.usda"));
    EXPECT_EQ(layersOf(input), layersOf(claimed));
    EXPECT_EQ((std::size_t)0, claimed.getNumOpenRegions());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(claimed);
    ASSERT_EQ((std::size_t)3, regions.size());
    EXPECT_TRUE(regions[0] == nullptr);
    ASSERT_TRUE(regions[1] != nullptr);
    EXPECT_EQ(regions[1], regions[2]);
    EXPECT_EQ(std::string("lighting.usda"), regions[1]->settings.savePath);

    // The claimed run is a boundary for the next one.
    EXPECT_EQ(std::vector<std::size_t>({ 3 }), SceneOps::getPrecedingRun(push(claimed, 4)));
    // So is a break.
    EXPECT_EQ(std::vector<std::size_t>({ 1 }), SceneOps::getPrecedingRun(push(SceneOps::markContext(push(ScenePayload(), 5)), 6)));
}

TEST(SceneLayerRegion, PrecedingAfterAMergeReachesIntoEveryInput)
{
    const ScenePayload read = pushRead(ScenePayload(), 1);
    const ScenePayload left = push(push(read, 2), 3);
    const ScenePayload right = push(read, 4);
    const ScenePayload merged = USDMerge::mergeStacks({ left, right });
    ASSERT_EQ((std::size_t)4, SceneOps::getLayers(merged).size());
    EXPECT_TRUE(SceneOps::getLayers(merged)[3].mergeTop);
    EXPECT_EQ(std::vector<std::size_t>({ 1, 2, 3 }), SceneOps::getPrecedingRun(merged));

    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(SceneOps::claimPreceding(merged, "Configure", savingTo("both.usda")));
    ASSERT_EQ((std::size_t)4, regions.size());
    EXPECT_TRUE(regions[0] == nullptr);
    ASSERT_TRUE(regions[1] != nullptr);
    EXPECT_EQ(regions[1], regions[2]);
    EXPECT_EQ(regions[1], regions[3]);

    // Once claimed, the merge no longer opens a way into its inputs.
    const ScenePayload claimed = SceneOps::claimPreceding(merged, "Configure", savingTo("both.usda"));
    EXPECT_EQ(std::vector<std::size_t>({ 4 }), SceneOps::getPrecedingRun(push(claimed, 5)));
}

TEST(SceneLayerRegion, ARegionOpenAtAMergeClosesThere)
{
    const ScenePayload left = push(SceneOps::openRegion(push(ScenePayload(), 1), "Begin", SceneLayerRegionSettings()), 2);
    const ScenePayload right = push(ScenePayload(), 3);
    ASSERT_EQ((std::size_t)1, left.getNumOpenRegions());

    const ScenePayload merged = USDMerge::mergeStacks({ left, right });
    EXPECT_EQ((std::size_t)0, merged.getNumOpenRegions());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(push(merged, 4));
    ASSERT_EQ((std::size_t)4, regions.size());
    EXPECT_TRUE(regions[1] != nullptr);
    EXPECT_TRUE(regions[3] == nullptr);

    EXPECT_EQ((std::size_t)0, USDMerge::mergeStacks({ left }).getNumOpenRegions());
}

TEST(SceneLayerRegion, EveryRegionChangeAltersTheStackHash)
{
    const ScenePayload plain = push(ScenePayload(), 1);
    const ScenePayload inRegion = push(SceneOps::openRegion(ScenePayload(), "Begin", SceneLayerRegionSettings()), 1);
    EXPECT_NE(plain.getStackHash(), inRegion.getStackHash());
    EXPECT_NE(inRegion.getStackHash(), SceneOps::closeRegion(inRegion).getStackHash());

    const ScenePayload claimedA = SceneOps::claimPreceding(plain, "Configure", savingTo("a.usda"));
    const ScenePayload claimedB = SceneOps::claimPreceding(plain, "Configure", savingTo("b.usda"));
    EXPECT_NE(plain.getStackHash(), claimedA.getStackHash());
    EXPECT_NE(claimedA.getStackHash(), claimedB.getStackHash());
    EXPECT_EQ(claimedA.getStackHash(), SceneOps::claimPreceding(plain, "Configure", savingTo("a.usda")).getStackHash());

    const ScenePayload configured = SceneOps::setOpenRegionSettings(inRegion, savingTo("c.usda"));
    EXPECT_NE(inRegion.getStackHash(), configured.getStackHash());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(configured);
    ASSERT_EQ((std::size_t)1, regions.size());
    ASSERT_TRUE(regions[0] != nullptr);
    EXPECT_EQ(std::string("c.usda"), regions[0]->settings.savePath);
    EXPECT_EQ(regions[0], SceneOps::getInnermostOpenRegion(configured));
}

TEST(SceneLayerRegion, CustomDataRowsParseByType)
{
    SceneLayerCustomDatum datum;
    std::string error;

    ASSERT_TRUE(SceneOps::parseCustomDatum("note", "String", "two words", &datum, &error)) << error;
    EXPECT_TRUE(datum.value.IsHolding<std::string>());
    EXPECT_EQ(std::string("two words"), datum.value.UncheckedGet<std::string>());

    ASSERT_TRUE(SceneOps::parseCustomDatum("names", "String Array", "a \"b c\" d", &datum, &error)) << error;
    ASSERT_TRUE(datum.value.IsHolding<PXR_NS::VtStringArray>());
    ASSERT_EQ((std::size_t)3, datum.value.UncheckedGet<PXR_NS::VtStringArray>().size());
    EXPECT_EQ(std::string("b c"), datum.value.UncheckedGet<PXR_NS::VtStringArray>()[1]);

    ASSERT_TRUE(SceneOps::parseCustomDatum("kind", "Token", "component", &datum, &error)) << error;
    EXPECT_TRUE(datum.value.IsHolding<PXR_NS::TfToken>());

    ASSERT_TRUE(SceneOps::parseCustomDatum("tags", "Token Array", "x \"y z\"", &datum, &error)) << error;
    ASSERT_TRUE(datum.value.IsHolding<PXR_NS::VtTokenArray>());
    EXPECT_EQ((std::size_t)3, datum.value.UncheckedGet<PXR_NS::VtTokenArray>().size());

    ASSERT_TRUE(SceneOps::parseCustomDatum("scale", "Float", "0.5", &datum, &error)) << error;
    ASSERT_TRUE(datum.value.IsHolding<double>());
    EXPECT_EQ(0.5, datum.value.UncheckedGet<double>());

    ASSERT_TRUE(SceneOps::parseCustomDatum("version", "Integer", " 12 ", &datum, &error)) << error;
    ASSERT_TRUE(datum.value.IsHolding<int>());
    EXPECT_EQ(12, datum.value.UncheckedGet<int>());

    ASSERT_TRUE(SceneOps::parseCustomDatum("approved", "Boolean", "true", &datum, &error)) << error;
    ASSERT_TRUE(datum.value.IsHolding<bool>());
    EXPECT_TRUE(datum.value.UncheckedGet<bool>());

    EXPECT_FALSE(SceneOps::parseCustomDatum("version", "Integer", "twelve", &datum, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(SceneOps::parseCustomDatum("scale", "Float", "1.5x", &datum, &error));
    EXPECT_FALSE(SceneOps::parseCustomDatum("", "String", "x", &datum, &error));
    EXPECT_FALSE(SceneOps::parseCustomDatum("x", "Matrix", "1", &datum, &error));
}

class SceneLayerRegionNodeTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload)
    {
        std::string error;

        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, &error);
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));

        return payload;
    }

    NodePtr read()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kUSDReadParamFile).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(std::string(kReferenceScene));
        }

        return node;
    }

    NodePtr after(const char* pluginID,
                  const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (node && input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const char* pattern)
    {
        NodePtr node = after(PLUGINID_NATRON_USDPRUNE, input);
        KnobString* knob = node ? dynamic_cast<KnobString*>(node->getKnobByName(kUSDPruneParamPrimPattern).get()) : NULL;

        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(std::string(pattern));
        }

        return node;
    }

    NodePtr configure(const NodePtr& input,
                      const char* appliesTo,
                      const char* savePath)
    {
        NodePtr node = after(PLUGINID_NATRON_USDCONFIGURELAYER, input);
        if (!node) {
            return node;
        }
        KnobChoice* mode = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDConfigureLayerParamAppliesTo).get());
        KnobString* path = dynamic_cast<KnobString*>(node->getKnobByName(kUSDConfigureLayerParamSavePath).get());
        EXPECT_TRUE(mode != NULL);
        EXPECT_TRUE(path != NULL);
        if (mode) {
            mode->setValueFromID(appliesTo, 0);
        }
        if (path) {
            path->setValue(std::string(savePath));
        }

        return node;
    }
};

TEST_F(SceneLayerRegionNodeTest, BeginAndEndBracketTheEditsBetweenThem)
{
    NodePtr reader = read();
    ASSERT_TRUE(bool(reader));
    NodePtr begin = after(PLUGINID_NATRON_USDLAYERBEGIN, reader);
    NodePtr inside = prune(begin, "/World/Ball");
    NodePtr end = after(PLUGINID_NATRON_USDLAYEREND, inside);
    NodePtr outside = prune(end, "/World/Cam");
    ASSERT_TRUE(begin && inside && end && outside);

    const std::size_t n = SceneOps::getLayers(evaluate(reader)).size();
    EXPECT_EQ((std::size_t)1, evaluate(begin).getNumOpenRegions());
    const ScenePayload output = evaluate(outside);
    EXPECT_EQ((std::size_t)0, output.getNumOpenRegions());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(output);
    ASSERT_EQ(n + 2, regions.size());
    EXPECT_TRUE(regions[n - 1] == nullptr);
    ASSERT_TRUE(regions[n] != nullptr);
    EXPECT_EQ(begin->getFullyQualifiedName(), regions[n]->node);
    EXPECT_TRUE(regions[n + 1] == nullptr);
    EXPECT_FALSE(end->hasPersistentMessage());
}

TEST_F(SceneLayerRegionNodeTest, EndWithNoOpenRegionWarnsAndPassesThrough)
{
    NodePtr reader = read();
    ASSERT_TRUE(bool(reader));
    NodePtr end = after(PLUGINID_NATRON_USDLAYEREND, reader);
    ASSERT_TRUE(bool(end));

    const ScenePayload input = evaluate(reader);
    const ScenePayload output = evaluate(end);
    EXPECT_TRUE(output.isSameStack(input));
    EXPECT_EQ(input.getStackHash(), output.getStackHash());
    EXPECT_TRUE(end->hasPersistentMessage());
}

TEST_F(SceneLayerRegionNodeTest, EachModeTagsItsLayers)
{
    NodePtr reader = read();
    ASSERT_TRUE(bool(reader));
    NodePtr first = prune(reader, "/World/Ball");
    NodePtr second = prune(first, "/World/Cam");
    NodePtr preceding = configure(second, "preceding", "edits.usda");
    NodePtr following = configure(preceding, "following", "more.usda");
    NodePtr third = prune(following, "/World/Box");
    ASSERT_TRUE(first && second && preceding && following && third);

    const std::size_t n = SceneOps::getLayers(evaluate(reader)).size();
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(evaluate(third));
    ASSERT_EQ(n + 3, regions.size());
    EXPECT_TRUE(regions[n - 1] == nullptr);
    ASSERT_TRUE(regions[n] != nullptr);
    EXPECT_EQ(regions[n], regions[n + 1]);
    EXPECT_EQ(std::string("edits.usda"), regions[n]->settings.savePath);
    EXPECT_EQ(preceding->getFullyQualifiedName(), regions[n]->settings.node);
    ASSERT_TRUE(regions[n + 2] != nullptr);
    EXPECT_EQ(std::string("more.usda"), regions[n + 2]->settings.savePath);
    EXPECT_EQ((std::size_t)1, evaluate(third).getNumOpenRegions());
}

TEST_F(SceneLayerRegionNodeTest, OnlyToggledMetadataReachesTheRegion)
{
    NodePtr reader = read();
    NodePtr edit = prune(reader, "/World/Ball");
    NodePtr node = configure(edit, "preceding", "");
    ASSERT_TRUE(reader && edit && node);

    KnobString* defaultPrim = dynamic_cast<KnobString*>(node->getKnobByName(kUSDConfigureLayerParamDefaultPrim).get());
    KnobDouble* startTime = dynamic_cast<KnobDouble*>(node->getKnobByName(kUSDConfigureLayerParamStartTime).get());
    KnobBool* startTimeAuthored = dynamic_cast<KnobBool*>(node->getKnobByName(std::string(kUSDConfigureLayerParamStartTime) + "Authored").get());
    KnobBool* defaultPrimAuthored = dynamic_cast<KnobBool*>(node->getKnobByName(std::string(kUSDConfigureLayerParamDefaultPrim) + "Authored").get());
    ASSERT_TRUE(defaultPrim != NULL);
    ASSERT_TRUE(startTime != NULL);
    ASSERT_TRUE(startTimeAuthored != NULL);
    ASSERT_TRUE(defaultPrimAuthored != NULL);
    defaultPrim->setValue(std::string("World"));
    defaultPrimAuthored->setValue(true);
    startTime->setValue(12.);
    startTimeAuthored->setValue(false);

    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(evaluate(node));
    ASSERT_FALSE(regions.empty());
    ASSERT_TRUE(regions.back() != nullptr);
    const SceneLayerRegionSettings& settings = regions.back()->settings;
    ASSERT_TRUE(settings.defaultPrim.has_value());
    EXPECT_EQ(std::string("World"), *settings.defaultPrim);
    EXPECT_FALSE(settings.startTime.has_value());
    EXPECT_FALSE(settings.endTime.has_value());
    EXPECT_FALSE(settings.upAxis.has_value());
}

TEST_F(SceneLayerRegionNodeTest, ConfiguringRightAfterABeginSetsItsRegion)
{
    NodePtr reader = read();
    NodePtr begin = after(PLUGINID_NATRON_USDLAYERBEGIN, reader);
    NodePtr node = configure(begin, "following", "shot.usda");
    NodePtr edit = prune(node, "/World/Ball");
    NodePtr end = after(PLUGINID_NATRON_USDLAYEREND, edit);
    ASSERT_TRUE(reader && begin && node && edit && end);

    const ScenePayload output = evaluate(end);
    EXPECT_EQ((std::size_t)0, output.getNumOpenRegions());
    const std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(output);
    ASSERT_FALSE(regions.empty());
    ASSERT_TRUE(regions.back() != nullptr);
    EXPECT_EQ(begin->getFullyQualifiedName(), regions.back()->node);
    EXPECT_EQ(std::string("shot.usda"), regions.back()->settings.savePath);
    EXPECT_TRUE(regions.back()->parent == nullptr);
}

TEST_F(SceneLayerRegionNodeTest, ACustomDataRowThatDoesNotParseFailsTheNode)
{
    NodePtr reader = read();
    NodePtr edit = prune(reader, "/World/Ball");
    NodePtr node = configure(edit, "preceding", "edits.usda");
    ASSERT_TRUE(reader && edit && node);
    KnobLayerCustomData* table = dynamic_cast<KnobLayerCustomData*>(node->getKnobByName(kUSDConfigureLayerParamCustomData).get());
    ASSERT_TRUE(table != NULL);

    std::list<std::vector<std::string>> rows;
    rows.push_back({ "department", "String", "lighting" });
    table->setTable(rows);
    std::vector<SceneLayerRegionPtr> regions = SceneOps::regionsOf(evaluate(node));
    ASSERT_FALSE(regions.empty());
    ASSERT_TRUE(regions.back() != nullptr);
    ASSERT_EQ((std::size_t)1, regions.back()->settings.customData.size());
    EXPECT_EQ(std::string("department"), regions.back()->settings.customData[0].name);

    rows.push_back({ "version", "Integer", "twelve" });
    table->setTable(rows);
    ScenePayload payload;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
}
