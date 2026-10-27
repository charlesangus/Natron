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

#include "Global/Macros.h"

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

RectI
fullWindow()
{
    return RectI(0, 0, kCountingTestSize, kCountingTestSize);
}

// The red value of the bottom-left pixel of node's output.
bool
renderedValue(const NodePtr& node,
              float* value)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, fullWindow(), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, true /*byPassCache*/)) {
        ADD_FAILURE() << error;

        return false;
    }
    if (planes.empty() || planes[0].pixels.empty()) {
        ADD_FAILURE() << "no pixel was read back";

        return false;
    }
    *value = planes[0].pixels[0];

    return true;
}

// Renders node's RGBA through renderRoI, which (unlike renderNodePlanesDirect) also accepts a
// black identity with no region of its own.
EffectInstance::RenderRoIRetCode
renderRoIDirect(const NodePtr& node,
                std::map<ImageLayerDesc, ImagePtr>* planes)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameArgs(kTime,
                                       ViewIdx(0),
                                       true /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       abortInfo,
                                       node,
                                       0 /*textureIndex*/,
                                       node->getApp()->getTimeLine().get(),
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       RenderStatsPtr());
    EffectInstancePtr effect = node->getEffectInstance();
    const RectI window = fullWindow();
    const RectD canonicalWindow = window.toCanonical_noClipping(0, effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
    FrameRequestMap request;

    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonicalWindow, node, request) == eStatusFailed) {
        return EffectInstance::eRenderRoIRetCodeFailed;
    }
    frameArgs.updateNodesRequest(request);

    EffectInstance::RenderRoIArgs args(kTime,
                                       RenderScale::identity,
                                       0,
                                       ViewIdx(0),
                                       false /*byPassCache*/,
                                       window,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kTime);
    try {
        return effect->renderRoI(args, planes);
    } catch (...) {
        return EffectInstance::eRenderRoIRetCodeFailed;
    }
}

bool
allZero(const std::map<ImageLayerDesc, ImagePtr>& planes)
{
    for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        const RectI bounds = image->getBounds();
        const int nComps = (int)image->getComponentsCount();
        Image::ReadAccess access(image.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const float* pix = (const float*)access.pixelAt(bounds.x1, y);
            for (int i = 0; pix && (i < bounds.width() * nComps); ++i) {
                if (pix[i] != 0.f) {
                    return false;
                }
            }
        }
    }

    return true;
}

KnobDriverPtr
linearDriver(const std::string& variable,
             double start,
             double end)
{
    return std::make_shared<const KnobDriver>(variable, start, end, KnobDriver::eEasingLinear);
}
} // namespace

class NativeLoopBlockTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createConstant(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        KnobColorPtr color = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kConstantParamColor)) : KnobColorPtr();

        EXPECT_TRUE(bool(color));
        if (!color) {
            return NodePtr();
        }
        for (int i = 0; i < color->getDimension(); ++i) {
            color->setValue(value, ViewSpec::all(), i);
        }

        return node;
    }

    // A Grade whose multiply, in every channel, goes from start to end over variable.
    NodePtr createDrivenGrade(const std::string& variable,
                              double start,
                              double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        KnobColorPtr multiply = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGradeParamMultiply)) : KnobColorPtr();

        EXPECT_TRUE(bool(multiply));
        if (!multiply) {
            return NodePtr();
        }
        for (int i = 0; i < multiply->getDimension(); ++i) {
            EXPECT_TRUE(multiply->setDriver(i, linearDriver(variable, start, end)));
        }

        return node;
    }

    NodePtr createLoopBegin(const std::string& name)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
        KnobStringPtr nameKnob = node ? std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kLoopBeginParamName)) : KnobStringPtr();

        EXPECT_TRUE(bool(nameKnob));
        if (nameKnob && !name.empty()) {
            nameKnob->setValue(name);
        }

        return node;
    }

    NodePtr createLoopEnd(int iterations,
                          const std::string& operation)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));

        EXPECT_TRUE(node && setIterations(node, iterations));
        KnobChoicePtr operationKnob = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamOperation)) : KnobChoicePtr();
        EXPECT_TRUE(bool(operationKnob));
        if (operationKnob) {
            operationKnob->setValueFromID(operation, 0);
        }

        return node;
    }

    static bool setIntKnob(const NodePtr& node,
                           const char* name,
                           int value)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValue(value);

        return true;
    }

    static bool setIterations(const NodePtr& node,
                              int iterations)
    {
        return setIntKnob(node, kLoopEndParamIterations, iterations);
    }

    // The node renders black, as a black identity, and shows text in a message of type type.
    void expectBlackWithMessage(const NodePtr& node,
                                MessageTypeEnum type,
                                const QString& text)
    {
        std::map<ImageLayerDesc, ImagePtr> planes;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderRoIDirect(node, &planes));
        EXPECT_TRUE(allZero(planes));

        EffectInstancePtr effect = node->getEffectInstance();
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -3;
        EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, fullWindow(), ViewIdx(0), &inputTime, &inputView, &inputNb));
        EXPECT_EQ(-1, inputNb);

        QString message;
        int messageType = -1;
        node->getPersistentMessage(&message, &messageType, false);
        EXPECT_EQ((int)type, messageType) << message.toStdString();
        EXPECT_TRUE(message.contains(text)) << message.toStdString();
    }
};

TEST_F(NativeLoopBlockTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_LOOPBEGIN, PLUGIN_MAJOR_NATRON_LOOPBEGIN, "LoopBegin");
    expectNativeIdentity(PLUGINID_NATRON_LOOPEND, PLUGIN_MAJOR_NATRON_LOOPEND, "LoopEnd");

    KnobIntPtr maxIterations = std::dynamic_pointer_cast<KnobInt>(createLoopEnd(1, kLoopEndParamOperationMean)->getKnobByName(kLoopEndParamMaxIterations));
    ASSERT_TRUE(bool(maxIterations));
    EXPECT_EQ(kLoopEndDefaultMaxIterations, maxIterations->getValue());
    EXPECT_EQ(4096, maxIterations->getMaximum(0));
}

// t runs 0, 1/4, ..., 1 over five iterations, so the gain runs 1 to 2 and averages 1.5.
TEST_F(NativeLoopBlockTest, MeanOfADrivenGainIsItsMidpoint)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr grade = createDrivenGrade(kKnobDriverDefaultVariable, 1., 2.);
    NodePtr end = createLoopEnd(5, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && grade && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(grade->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1.5, value, 1e-5);

    // Sum, min and max over the same gains.
    KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(end->getKnobByName(kLoopEndParamOperation));
    ASSERT_TRUE(bool(operation));
    operation->setValueFromID(kLoopEndParamOperationSum, 0);
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(7.5, value, 1e-5);
    operation->setValueFromID(kLoopEndParamOperationMin, 0);
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1., value, 1e-5);
    operation->setValueFromID(kLoopEndParamOperationMax, 0);
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(2., value, 1e-5);

    // One iteration is t = 0.
    operation->setValueFromID(kLoopEndParamOperationMean, 0);
    ASSERT_TRUE(setIterations(end, 1));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1., value, 1e-5);
}

// A body node that reads no loop variable has one output for every iteration, so it renders once.
TEST_F(NativeLoopBlockTest, AnUndrivenBodyRendersOnce)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr end = createLoopEnd(5, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && body && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    // The counting body adds its value 1 to the constant 1.
    EXPECT_NEAR(2., value, 1e-5);
    EXPECT_EQ(1, CountingTestRegistry::renders(body));
}

// Inside an inner loop of 3 nested in an outer loop of 2, loop.t is the inner loop's and
// loops.outer.t the outer's. The inner gain 1 to 2 averages 1.5; the outer gain is 1 then 3,
// summed by the outer loop: 1.5 * (1 + 3) = 6. Had the outer gain read the inner t, each outer
// iteration would be (1*1 + 1.5*2 + 2*3) / 3 and the total 20/3; had it read nothing, 3.
TEST_F(NativeLoopBlockTest, NestedLoopsSeeBothLoopsVariables)
{
    NodePtr constant = createConstant(1.);
    NodePtr outerBegin = createLoopBegin("outer");
    NodePtr innerBegin = createLoopBegin("inner");
    NodePtr innerGain = createDrivenGrade(kKnobDriverDefaultVariable, 1., 2.);
    NodePtr outerGain = createDrivenGrade("loops.outer.t", 1., 3.);
    NodePtr innerEnd = createLoopEnd(3, kLoopEndParamOperationMean);
    NodePtr outerEnd = createLoopEnd(2, kLoopEndParamOperationSum);
    ASSERT_TRUE(constant && outerBegin && innerBegin && innerGain && outerGain && innerEnd && outerEnd);
    ASSERT_TRUE(outerBegin->connectInput(constant, 0));
    ASSERT_TRUE(innerBegin->connectInput(outerBegin, 0));
    ASSERT_TRUE(innerGain->connectInput(innerBegin, 0));
    ASSERT_TRUE(outerGain->connectInput(innerGain, 0));
    ASSERT_TRUE(innerEnd->connectInput(outerGain, 0));
    ASSERT_TRUE(outerEnd->connectInput(innerEnd, 0));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(outerEnd, &value));
    EXPECT_NEAR(6., value, 1e-4);

    QString message;
    int type = -1;
    innerEnd->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
    outerEnd->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}

TEST_F(NativeLoopBlockTest, ALoopEndWithoutALoopBeginIsBlack)
{
    NodePtr constant = createConstant(1.);
    NodePtr end = createLoopEnd(3, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && end);
    ASSERT_TRUE(end->connectInput(constant, 0));

    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("No LoopBegin was found above this LoopEnd."));
}

TEST_F(NativeLoopBlockTest, ALoopBeginSharedByTwoLoopEndsIsAnError)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr first = createLoopEnd(3, kLoopEndParamOperationMean);
    NodePtr second = createLoopEnd(3, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && first && second);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(first->connectInput(begin, 0));
    ASSERT_TRUE(second->connectInput(begin, 0));

    expectBlackWithMessage(first, eMessageTypeError, QString::fromUtf8("is reached from more than one LoopEnd"));
    expectBlackWithMessage(second, eMessageTypeError, QString::fromUtf8("is reached from more than one LoopEnd"));
}

TEST_F(NativeLoopBlockTest, ZeroIterationsIsBlackWithAWarning)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr end = createLoopEnd(0, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(end->connectInput(begin, 0));

    expectBlackWithMessage(end, eMessageTypeWarning, QString::fromUtf8("Iterations is 0"));

    // A valid count takes the message back.
    ASSERT_TRUE(setIterations(end, 2));
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1., value, 1e-5);
    QString message;
    int type = -1;
    end->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}

TEST_F(NativeLoopBlockTest, MoreIterationsThanTheCapIsAnError)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && end);
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamMaxIterations, 5));
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(end->connectInput(begin, 0));

    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("10 iterations is more than Max Iterations (5)."));
}

TEST_F(NativeLoopBlockTest, InvalidOrDuplicateLoopNamesAreErrors)
{
    NodePtr constant = createConstant(1.);
    NodePtr first = createLoopBegin(std::string());
    NodePtr second = createLoopBegin(std::string());
    NodePtr end = createLoopEnd(2, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && first && second && end);
    ASSERT_TRUE(second->connectInput(constant, 0));
    ASSERT_TRUE(end->connectInput(second, 0));

    // A new LoopBegin takes the first free name.
    KnobStringPtr firstName = std::dynamic_pointer_cast<KnobString>(first->getKnobByName(kLoopBeginParamName));
    KnobStringPtr secondName = std::dynamic_pointer_cast<KnobString>(second->getKnobByName(kLoopBeginParamName));
    ASSERT_TRUE(firstName && secondName);
    EXPECT_NE(firstName->getValue(), secondName->getValue());

    secondName->setValue(firstName->getValue());
    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("is also named"));

    secondName->setValue(std::string("1st"));
    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("is not valid"));
}
