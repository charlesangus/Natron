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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDLayerBreak.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

std::vector<bool>
contextOf(const ScenePayload& payload)
{
    std::vector<bool> flags;

    for (const SceneLayerEntry& entry : SceneOps::getLayers(payload)) {
        flags.push_back(entry.context);
    }

    return flags;
}

// context flagged entries, weakest first, then plain unflagged ones.
std::vector<bool>
flags(std::size_t context,
      std::size_t plain)
{
    std::vector<bool> result(context, true);
    result.insert(result.end(), plain, false);

    return result;
}

std::string
exportStage(const PXR_NS::UsdStageRefPtr& stage)
{
    std::string text;

    EXPECT_TRUE(stage && stage->ExportToString(&text, false));

    return text;
}
} // anonymous namespace

class USDLayerBreakTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    NodePtr read(const char* path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kUSDReadParamFile).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(std::string(path));
        }

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const char* pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDPruneParamPrimPattern).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(std::string(pattern));
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr layerBreak(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLAYERBREAK));

        EXPECT_TRUE(bool(node));
        if (node) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }
};

// The layers above the break are context, the one after it is not, and the stage composes as it
// does without the break.
TEST_F(USDLayerBreakTest, LayersAboveTheBreakAreContextAndComposeTheSame)
{
    NodePtr reader = read(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    NodePtr before = prune(reader, "/World/Ball");
    ASSERT_TRUE(bool(before));
    NodePtr brk = layerBreak(before);
    ASSERT_TRUE(bool(brk));
    NodePtr after = prune(brk, "/World/Cam");
    NodePtr unbroken = prune(before, "/World/Cam");
    ASSERT_TRUE(after && unbroken);

    const ScenePayload input = evaluate(before);
    const std::size_t n = contextOf(input).size();
    ASSERT_LE((std::size_t)2, n);
    EXPECT_EQ(flags(0, n), contextOf(input));
    const ScenePayload marked = evaluate(brk);
    EXPECT_EQ(flags(n, 0), contextOf(marked));
    EXPECT_TRUE(marked.isSameStack(input));
    EXPECT_NE(input.getStackHash(), marked.getStackHash());

    const ScenePayload output = evaluate(after);
    EXPECT_EQ(flags(n, 1), contextOf(output));

    const std::string withBreak = exportStage(SceneOps::compose(output));
    ASSERT_FALSE(withBreak.empty());
    EXPECT_EQ(std::string::npos, withBreak.find("\"Cam\"")) << withBreak;
    EXPECT_EQ(std::string::npos, withBreak.find("\"Ball\"")) << withBreak;
    EXPECT_NE(std::string::npos, withBreak.find("\"Box\"")) << withBreak;
    EXPECT_EQ(exportStage(SceneOps::compose(evaluate(unbroken))), withBreak);
}

// A node below the break that has already authored its layer over the unbroken stack picks up
// the flags when a break is inserted above it, though its layer does not change.
TEST_F(USDLayerBreakTest, InsertingABreakReflagsAMemoizedDownstreamStack)
{
    NodePtr reader = read(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    NodePtr after = prune(reader, "/World/Cam");
    ASSERT_TRUE(bool(after));
    const std::size_t n = contextOf(evaluate(reader)).size();
    ASSERT_LE((std::size_t)1, n);
    const ScenePayload unbroken = evaluate(after);
    EXPECT_EQ(flags(0, n + 1), contextOf(unbroken));

    NodePtr brk = layerBreak(reader);
    ASSERT_TRUE(bool(brk));
    ASSERT_TRUE(after->replaceInput(brk, 0));
    const ScenePayload broken = evaluate(after);
    EXPECT_EQ(flags(n, 1), contextOf(broken));
    EXPECT_TRUE(broken.isSameStack(unbroken));
    EXPECT_NE(unbroken.getStackHash(), broken.getStackHash());
}

TEST_F(USDLayerBreakTest, ChainedBreaksAccumulateAndADisabledBreakMarksNothing)
{
    NodePtr reader = read(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    NodePtr first = layerBreak(reader);
    ASSERT_TRUE(bool(first));
    NodePtr between = prune(first, "/World/Ball");
    ASSERT_TRUE(bool(between));
    NodePtr second = layerBreak(between);
    ASSERT_TRUE(bool(second));
    NodePtr after = prune(second, "/World/Cam");
    ASSERT_TRUE(bool(after));

    const std::size_t n = contextOf(evaluate(reader)).size();
    ASSERT_LE((std::size_t)1, n);
    EXPECT_EQ(flags(n + 1, 1), contextOf(evaluate(after)));

    second->setNodeDisabled(true);
    EXPECT_EQ(flags(n, 2), contextOf(evaluate(after)));
    first->setNodeDisabled(true);
    EXPECT_EQ(flags(0, n + 2), contextOf(evaluate(after)));
}
