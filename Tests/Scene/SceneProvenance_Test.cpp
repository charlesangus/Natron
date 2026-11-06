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

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

std::string
getString(const NodePtr& node,
          const char* name)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    EXPECT_TRUE(knob != NULL) << name;

    return knob ? knob->getValue() : std::string();
}

PXR_NS::UsdAttribute
attributeAt(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& primPath,
            const std::string& name)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(primPath));

    EXPECT_TRUE(bool(prim)) << primPath;

    return prim ? prim.GetAttribute(PXR_NS::TfToken(name)) : PXR_NS::UsdAttribute();
}
} // anonymous namespace

class SceneProvenanceTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return SceneOps::compose(payload);
    }
};

TEST_F(SceneProvenanceTest, EachOpinionResolvesToTheNodeThatAuthoredIt)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    ASSERT_TRUE(camera && transform);
    const std::string cameraPath = getString(camera, kSceneParamPrimPath);
    ASSERT_FALSE(cameraPath.empty());
    setString(transform, kUSDEditTransformParamPrimPattern, cameraPath);
    KnobDouble* translate = dynamic_cast<KnobDouble*>(transform->getKnobByName(kSceneParamTranslate).get());
    ASSERT_TRUE(translate != NULL);
    translate->setValue(5., ViewSpec::all(), 1);
    ASSERT_TRUE(transform->connectInput(camera, 0));

    PXR_NS::UsdStageRefPtr stage = compose(transform);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdTimeCode time(1.);

    std::string identifier;
    const PXR_NS::UsdAttribute focalLength = attributeAt(stage, cameraPath, "focalLength");
    ASSERT_TRUE(bool(focalLength));
    EXPECT_EQ(camera, SceneOps::authoringNodeOf(focalLength, time, &identifier));
    EXPECT_FALSE(identifier.empty());

    const std::string opName = "xformOp:transform:" + transform->getScriptName_mt_safe();
    const PXR_NS::UsdAttribute op = attributeAt(stage, cameraPath, opName);
    ASSERT_TRUE(bool(op)) << opName;
    EXPECT_EQ(transform, SceneOps::authoringNodeOf(op, time, &identifier));

    // The edit's xformOpOrder is stronger than the camera's.
    const PXR_NS::UsdAttribute order = attributeAt(stage, cameraPath, "xformOpOrder");
    ASSERT_TRUE(bool(order));
    EXPECT_EQ(transform, SceneOps::authoringNodeOf(order, time, &identifier));

    // An attribute nobody authored falls back to its schema: no node, no layer.
    const PXR_NS::UsdAttribute exposure = attributeAt(stage, cameraPath, "exposure");
    if (exposure && !exposure.HasAuthoredValue()) {
        EXPECT_FALSE(bool(SceneOps::authoringNodeOf(exposure, time, &identifier)));
        EXPECT_TRUE(identifier.empty());
    }
}

TEST_F(SceneProvenanceTest, AReauthoredLayerResolvesToTheSameNode)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    const std::string cameraPath = getString(camera, kSceneParamPrimPath);
    KnobDouble* focal = dynamic_cast<KnobDouble*>(camera->getKnobByName(kUSDCameraParamFocalLength).get());
    ASSERT_TRUE(focal != NULL);

    ASSERT_TRUE(bool(compose(camera)));
    focal->setValue(focal->getValue() + 10.);
    PXR_NS::UsdStageRefPtr stage = compose(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(camera, SceneOps::authoringNodeOf(attributeAt(stage, cameraPath, "focalLength"), PXR_NS::UsdTimeCode(1.), NULL));
}

TEST_F(SceneProvenanceTest, GraftedOpinionsKeepTheirAuthors)
{
    NodePtr world = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    ASSERT_TRUE(world && a);
    setString(world, kSceneParamPrimPath, "/World");
    setString(a, kSceneParamPrimPath, "/A");
    dynamic_cast<KnobDouble*>(world->getKnobByName(kSceneParamTranslate).get())->setValue(100., ViewSpec::all(), 0);
    dynamic_cast<KnobDouble*>(a->getKnobByName(kSceneParamTranslate).get())->setValue(3., ViewSpec::all(), 1);
    ASSERT_TRUE(world->connectInput(a, 0));

    PXR_NS::UsdStageRefPtr stage = compose(world);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdTimeCode time(1.);

    // A's opinion reaches /World/A through the graft's reference, from A's own layer.
    const PXR_NS::UsdAttribute aOp = attributeAt(stage, "/World/A", "xformOp:transform");
    ASSERT_TRUE(bool(aOp));
    EXPECT_EQ(a, SceneOps::authoringNodeOf(aOp, time, NULL));

    // /World's own transform is in the re-parenting node's layers.
    const PXR_NS::UsdAttribute worldOp = attributeAt(stage, "/World", "xformOp:transform");
    ASSERT_TRUE(bool(worldOp));
    EXPECT_EQ(world, SceneOps::authoringNodeOf(worldOp, time, NULL));
}

TEST_F(SceneProvenanceTest, AFileReadInReportsItsPath)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    ASSERT_TRUE(bool(reader));
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");

    PXR_NS::UsdStageRefPtr stage = compose(reader);
    ASSERT_TRUE(bool(stage));
    std::string identifier;
    const PXR_NS::UsdAttribute size = attributeAt(stage, "/World/Box", "size");
    ASSERT_TRUE(bool(size));
    EXPECT_FALSE(bool(SceneOps::authoringNodeOf(size, PXR_NS::UsdTimeCode(1.), &identifier)));
    const std::string suffix = "scene-reference.usda";
    ASSERT_GE(identifier.size(), suffix.size()) << identifier;
    EXPECT_EQ(suffix, identifier.substr(identifier.size() - suffix.size()));
}
