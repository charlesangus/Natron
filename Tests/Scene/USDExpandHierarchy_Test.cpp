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

#include <map>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/USDExpandHierarchy.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

const PXR_NS::SdfPath kWorld("/World");
const PXR_NS::SdfPath kBox("/World/Box");
const PXR_NS::SdfPath kBall("/World/Ball");
const PXR_NS::SdfPath kCam("/World/Cam");
} // anonymous namespace

class USDExpandHierarchyTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        // The reference scene is in centimetres: a project in the same units reads it without a
        // units warning.
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    // A USDRead of the reference scene; openLevels < 0 opens it fully.
    NodePtr reader(int openLevels)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kUSDReadParamFile).get());
        KnobChoice* openMode = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDReadParamOpenMode).get());
        KnobInt* levels = dynamic_cast<KnobInt*>(node->getKnobByName(kUSDReadParamOpenLevels).get());
        EXPECT_TRUE(file && openMode && levels);
        if (!file || !openMode || !levels) {
            return NodePtr();
        }
        file->setValue(kReferenceScene);
        if (openLevels >= 0) {
            openMode->setValue((int)USDRead::eOpenModeFirstLevels);
            levels->setValue(openLevels);
        }

        return node;
    }

    NodePtr expand(const NodePtr& input,
                   const std::string& pattern,
                   USDExpandHierarchy::ModeEnum modeIndex,
                   int levelCount = 1)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEXPANDHIERARCHY));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* primPattern = dynamic_cast<KnobString*>(node->getKnobByName(kUSDExpandHierarchyParamPrimPattern).get());
        KnobChoice* mode = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDExpandHierarchyParamMode).get());
        KnobInt* levels = dynamic_cast<KnobInt*>(node->getKnobByName(kUSDExpandHierarchyParamLevels).get());
        EXPECT_TRUE(primPattern && mode && levels);
        if (!primPattern || !mode || !levels) {
            return NodePtr();
        }
        primPattern->setValue(pattern);
        mode->setValue((int)modeIndex);
        levels->setValue(levelCount);
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    // The prims a viewer showing node folds, with no Scene Graph twirls.
    static std::set<PXR_NS::SdfPath> viewerFolds(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        SceneStage stage(true);
        stage.sync(payload);
        stage.applyViewerFolds(std::map<PXR_NS::SdfPath, bool>(), std::set<PXR_NS::SdfPath>());

        return stage.getFoldedPrims();
    }
};

TEST_F(USDExpandHierarchyTest, FoldDrawsTheMatchesAsBounds)
{
    NodePtr read = reader(-1);
    ASSERT_TRUE(bool(read));
    EXPECT_TRUE(viewerFolds(read).empty());

    NodePtr fold = expand(read, "/World/Box /World/Cam", USDExpandHierarchy::eModeFold);
    ASSERT_TRUE(bool(fold));
    EXPECT_EQ((std::set<PXR_NS::SdfPath> { kBox, kCam }), viewerFolds(fold));
    EXPECT_FALSE(fold->hasPersistentMessage());

    // A render of the same scene composes every prim, unfolded.
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, fold->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error)) << error;
    SceneStage render;
    render.sync(payload);
    EXPECT_TRUE(render.getFoldedPrims().empty());
    EXPECT_TRUE(render.getStage()->GetPrimAtPath(kBox).IsLoaded());
}

TEST_F(USDExpandHierarchyTest, ExpandOpensTheMatchesBelowAReadersOpenLevels)
{
    NodePtr read = reader(1);
    ASSERT_TRUE(bool(read));
    EXPECT_EQ((std::set<PXR_NS::SdfPath> { kBox, kBall, kCam }), viewerFolds(read));

    NodePtr open = expand(read, "/World/Ball", USDExpandHierarchy::eModeExpand);
    ASSERT_TRUE(bool(open));
    EXPECT_EQ((std::set<PXR_NS::SdfPath> { kBox, kCam }), viewerFolds(open));

    // The ancestors of a match open too, keeping their other children as they were.
    NodePtr closedRead = reader(0);
    ASSERT_TRUE(bool(closedRead));
    EXPECT_EQ((std::set<PXR_NS::SdfPath> { kWorld }), viewerFolds(closedRead));
    NodePtr reopened = expand(closedRead, "/World/Ball", USDExpandHierarchy::eModeExpand);
    ASSERT_TRUE(bool(reopened));
    EXPECT_TRUE(viewerFolds(reopened).count(kWorld) == 0);
}

TEST_F(USDExpandHierarchyTest, ExpandLevelsFoldsBelowTheLevels)
{
    NodePtr read = reader(-1);
    ASSERT_TRUE(bool(read));
    NodePtr levels = expand(read, "/World", USDExpandHierarchy::eModeExpandLevels, 0);
    ASSERT_TRUE(bool(levels));
    EXPECT_EQ((std::set<PXR_NS::SdfPath> { kBox, kBall, kCam }), viewerFolds(levels));

    // A later node overrides an earlier one on the same prim.
    NodePtr after = expand(levels, "/World", USDExpandHierarchy::eModeExpand);
    ASSERT_TRUE(bool(after));
    EXPECT_TRUE(viewerFolds(after).empty());
}

TEST_F(USDExpandHierarchyTest, AnUnmatchedPatternPassesTheSceneThroughWithAWarning)
{
    NodePtr read = reader(-1);
    ASSERT_TRUE(bool(read));
    NodePtr node = expand(read, "/Nowhere", USDExpandHierarchy::eModeFold);
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(viewerFolds(node).empty());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
}
