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

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The headless end-to-end path a viewer takes: USDRead, the scene action through a Dot, the
// composed stage from the StageCache, and what the stage holds. If USD's libraries or plug-ins
// fail to load at run time, it fails here rather than in the GUI.
class SceneIntegrationTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        StageCache::instance().clear();

        BaseTest::TearDown();
    }
};

TEST_F(SceneIntegrationTest, ReferenceSceneThroughTheActionComposesWithItsBounds)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(reader && dot);
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    ASSERT_TRUE(dot->connectInput(reader, 0));
    ASSERT_TRUE(dot->getEffectInstance()->producesSceneData());

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              dot->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
    ASSERT_TRUE(bool(stage));
    // The cache hands every consumer of the stack the same stage.
    EXPECT_EQ(stage, StageCache::instance().getStage(payload));

    const char* const prims[] = { "/World", "/World/Box", "/World/Ball", "/World/Cam" };
    for (const char* path : prims) {
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(path)).IsValid()) << path;
    }
    EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Camera"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")).GetTypeName());
    EXPECT_EQ(PXR_NS::UsdGeomTokens->y, PXR_NS::UsdGeomGetStageUpAxis(stage));

    // The box spans [-1, 1] on every axis; the ball (radius 0.5) sits at x = 3 and rises from
    // y = 0 at timecode 1 to y = 2 at timecode 24.
    const PXR_NS::TfTokenVector purposes = { PXR_NS::UsdGeomTokens->default_ };
    {
        PXR_NS::UsdGeomBBoxCache bboxes(PXR_NS::UsdTimeCode(1.), purposes);
        const PXR_NS::GfRange3d range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        EXPECT_NEAR(-1., range.GetMin()[0], 1e-6);
        EXPECT_NEAR(-1., range.GetMin()[1], 1e-6);
        EXPECT_NEAR(3.5, range.GetMax()[0], 1e-6);
        EXPECT_NEAR(1., range.GetMax()[1], 1e-6);
    }
    {
        PXR_NS::UsdGeomBBoxCache bboxes(PXR_NS::UsdTimeCode(24.), purposes);
        const PXR_NS::GfRange3d range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        EXPECT_NEAR(2.5, range.GetMax()[1], 1e-6);
    }
}
