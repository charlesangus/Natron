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

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDProjectUV.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
template <typename K>
K*
getKnob(const NodePtr& node,
        const char* name)
{
    return dynamic_cast<K*>(node->getKnobByName(name).get());
}

PXR_NS::SdfPath
getNodePath(const NodePtr& node)
{
    return PXR_NS::SdfPath("/" + node->getScriptName_mt_safe());
}

// The st of the mesh at path, one value per face vertex, or empty when it has none.
PXR_NS::VtVec2fArray
getFlattenedSt(const PXR_NS::UsdStageRefPtr& stage,
               const PXR_NS::SdfPath& path,
               PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default())
{
    PXR_NS::VtVec2fArray values;
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(path)).GetPrimvar(PXR_NS::TfToken("st"));

    EXPECT_TRUE(bool(primvar)) << path.GetString();
    if (primvar) {
        EXPECT_EQ(PXR_NS::UsdGeomTokens->faceVarying, primvar.GetInterpolation());
        EXPECT_TRUE(primvar.ComputeFlattened(&values, time));
    }

    return values;
}

void
expectSameUVs(const PXR_NS::VtVec2fArray& expected,
              const PXR_NS::VtVec2fArray& actual,
              float tolerance)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(expected[i][0], actual[i][0], tolerance) << "face vertex " << i;
        EXPECT_NEAR(expected[i][1], actual[i][1], tolerance) << "face vertex " << i;
    }
}
} // anonymous namespace

class USDProjectUVTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    // A 2:1 card at the origin facing +Z, the camera on +Z framing it exactly, and the
    // projection between them with the camera on its Camera input.
    void createRig()
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));

        card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        ASSERT_TRUE(bool(card));
        ASSERT_DOUBLE_EQ(2., getKnob<KnobDouble>(card, kSceneParamScale)->getValue(0));

        camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        ASSERT_TRUE(bool(camera));
        getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
        getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
        getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
        // Half the aperture over the focal length is the half-width per unit of distance: 18/50
        // spans the card's half-width of 1 at 50/18.
        getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(kDistance, ViewSpec::all(), 2);

        projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECTUV));
        ASSERT_TRUE(bool(projector));
        ASSERT_TRUE(projector->connectInput(card, 0));
        ASSERT_TRUE(projector->connectInput(camera, 1));
        getKnob<KnobString>(projector, kUSDProjectUVParamPrimPattern)->setValue(getNodePath(card).GetString());
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    static PXR_NS::UsdStageRefPtr composeNode(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload, &error)) << error;

        return SceneOps::compose(payload);
    }

    static QString getMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message;
    }

    static constexpr double kDistance = 50. / 18.;

    NodePtr card;
    NodePtr camera;
    NodePtr projector;
};

TEST_F(USDProjectUVTest, ACameraFacingCardFillingTheFrameGetsUVsFromCornerToCorner)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    const PXR_NS::VtVec2fArray cardSt = getFlattenedSt(composeNode(card), getNodePath(card));
    ASSERT_FALSE(cardSt.empty());

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::VtVec2fArray projected = getFlattenedSt(stage, getNodePath(card));
    // The card's own st runs (0, 0) at its bottom left to (1, 1) at its top right, as the frame does.
    expectSameUVs(cardSt, projected, 1e-4f);
    float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
    for (const PXR_NS::GfVec2f& uv : projected) {
        minU = std::min(minU, uv[0]);
        minV = std::min(minV, uv[1]);
        maxU = std::max(maxU, uv[0]);
        maxV = std::max(maxV, uv[1]);
    }
    EXPECT_NEAR(0.f, minU, 1e-4f);
    EXPECT_NEAR(0.f, minV, 1e-4f);
    EXPECT_NEAR(1.f, maxU, 1e-4f);
    EXPECT_NEAR(1.f, maxV, 1e-4f);
    // Nothing moves, so the UVs are a default value.
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(getNodePath(card))).GetPrimvar(PXR_NS::TfToken("st"));
    EXPECT_EQ((std::size_t)0, primvar.GetAttr().GetNumTimeSamples());
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();
}

TEST_F(USDProjectUVTest, MovingTheCameraMovesTheUVs)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    const PXR_NS::VtVec2fArray cardSt = getFlattenedSt(composeNode(card), getNodePath(card));
    // Half a unit right is a quarter of the frame's width of 2.
    getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(0.5, ViewSpec::all(), 0);

    const PXR_NS::VtVec2fArray projected = getFlattenedSt(composeNode(projector), getNodePath(card));
    ASSERT_EQ(cardSt.size(), projected.size());
    for (std::size_t i = 0; i < cardSt.size(); ++i) {
        EXPECT_NEAR(cardSt[i][0] - 0.25f, projected[i][0], 1e-4f) << "face vertex " << i;
        EXPECT_NEAR(cardSt[i][1], projected[i][1], 1e-4f) << "face vertex " << i;
    }
}

TEST_F(USDProjectUVTest, AnAnimatedCameraAuthorsASampleAtEveryProjectFrame)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    KnobDouble* translate = getKnob<KnobDouble>(camera, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 0.5, ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(getNodePath(card))).GetPrimvar(PXR_NS::TfToken("st"));
    ASSERT_TRUE(bool(primvar));
    EXPECT_EQ((std::size_t)(std::floor(last) - std::ceil(first) + 1.), primvar.GetAttr().GetNumTimeSamples());
    const PXR_NS::VtVec2fArray atFirst = getFlattenedSt(stage, getNodePath(card), PXR_NS::UsdTimeCode(1.));
    const PXR_NS::VtVec2fArray atLast = getFlattenedSt(stage, getNodePath(card), PXR_NS::UsdTimeCode(11.));
    ASSERT_EQ(atFirst.size(), atLast.size());
    ASSERT_FALSE(atFirst.empty());
    EXPECT_NEAR(atFirst[0][0] - 0.25f, atLast[0][0], 1e-4f);

    // Held, the camera stays where it is at the hold frame on every frame.
    getKnob<KnobBool>(projector, kUSDProjectUVParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProjectUVParamFrameHold)->setValue(11.);
    stage = composeNode(projector);
    ASSERT_TRUE(bool(stage));
    expectSameUVs(atLast, getFlattenedSt(stage, getNodePath(card), PXR_NS::UsdTimeCode(1.)), 1e-4f);
}

TEST_F(USDProjectUVTest, BackOnlyKeepsTheStOfFacesFacingTheCamera)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    const PXR_NS::VtVec2fArray cardSt = getFlattenedSt(composeNode(card), getNodePath(card));
    getKnob<KnobBool>(projector, kUSDProjectUVParamInvertU)->setValue(true);

    PXR_NS::VtVec2fArray projected = getFlattenedSt(composeNode(projector), getNodePath(card));
    ASSERT_EQ(cardSt.size(), projected.size());
    ASSERT_FALSE(cardSt.empty());
    EXPECT_NEAR(1.f - cardSt[0][0], projected[0][0], 1e-4f);

    getKnob<KnobChoice>(projector, kUSDProjectUVParamProjectOn)->setValue((int)USDProjectUV::eProjectOnBack);
    projected = getFlattenedSt(composeNode(projector), getNodePath(card));
    expectSameUVs(cardSt, projected, 0.f);
}

TEST_F(USDProjectUVTest, AChosenCameraTheSceneLacksIsANodeErrorNamingIt)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    KnobChoice* cameraKnob = getKnob<KnobChoice>(projector, kUSDProjectUVParamCamera);
    ASSERT_TRUE(cameraKnob != NULL);
    cameraKnob->setActiveEntry(ChoiceOption("/Gone", "/Gone", ""));

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error));
    ASSERT_TRUE(projector->hasPersistentMessage());
    EXPECT_TRUE(getMessage(projector).contains(QString::fromUtf8("/Gone"))) << getMessage(projector).toStdString();
}

TEST_F(USDProjectUVTest, ThePatternMatchingNothingPassesTheSceneThroughWithAWarning)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobString>(projector, kUSDProjectUVParamPrimPattern)->setValue("/Nothing");

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    ASSERT_TRUE(bool(stage));
    expectSameUVs(getFlattenedSt(composeNode(card), getNodePath(card)), getFlattenedSt(stage, getNodePath(card)), 0.f);
    ASSERT_TRUE(projector->hasPersistentMessage());
    EXPECT_TRUE(getMessage(projector).contains(QString::fromUtf8("/Nothing"))) << getMessage(projector).toStdString();
}

TEST_F(USDProjectUVTest, TheCameraMenuListsTheCameraInputsCameras)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    USDProjectUV* effect = dynamic_cast<USDProjectUV*>(projector->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    effect->refreshCameraMenu();
    const std::vector<ChoiceOption> entries = getKnob<KnobChoice>(projector, kUSDProjectUVParamCamera)->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)1, entries.size());
    EXPECT_EQ(getNodePath(camera).GetString(), entries[0].id);
}
