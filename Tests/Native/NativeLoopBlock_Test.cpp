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

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
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
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
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
              float* value,
              bool byPassCache = true)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, fullWindow(), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, byPassCache)) {
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

// The bottom-left pixel of node's RGBA output.
bool
renderedPixel(const NodePtr& node,
              float pixel[4])
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, fullWindow(), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, true /*byPassCache*/)) {
        ADD_FAILURE() << error;

        return false;
    }
    if (planes.empty() || (planes[0].pixels.size() < 4)) {
        ADD_FAILURE() << "no pixel was read back";

        return false;
    }
    std::copy(planes[0].pixels.begin(), planes[0].pixels.begin() + 4, pixel);

    return true;
}

// Restores the scheduler's bytes budget on destruction.
class BytesBudgetGuard {
public:
    explicit BytesBudgetGuard(std::size_t budget)
        : _scheduler(appPTR->getRenderScheduler())
        , _saved(_scheduler->getBytesBudget())
    {
        _scheduler->setBytesBudgetForTests(budget);
    }

    ~BytesBudgetGuard()
    {
        _scheduler->setBytesBudgetForTests(_saved);
    }

    BytesBudgetGuard(const BytesBudgetGuard&) = delete;
    BytesBudgetGuard& operator=(const BytesBudgetGuard&) = delete;

private:
    RenderScheduler* _scheduler;
    std::size_t _saved;
};

bool
setForceCaching(const NodePtr& node,
                bool enabled)
{
    KnobBool* knob = node ? dynamic_cast<KnobBool*>(node->getKnobByName("forceCaching").get()) : NULL;

    EXPECT_TRUE(knob != NULL) << "the node has no forceCaching knob";
    if (!knob) {
        return false;
    }
    knob->setValue(enabled);

    return node->isForceCachingEnabled() == enabled;
}

// The frames node asks of its input 0 at kTime.
std::set<double>
framesNeeded(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    FramesNeededMap frames = effect->getFramesNeeded_public(effect->getRenderHash(), kTime, ViewIdx(0), 0);
    std::set<double> needed;

    for (const RangeD& range : frames[0][ViewIdx(0)]) {
        for (double frame = range.min; frame <= range.max; frame += 1.) {
            needed.insert(frame);
        }
    }

    return needed;
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

    // A Grade whose red, green and blue multiply go from start to end over loop.t, its alpha left alone.
    NodePtr createDrivenColorGrade(double start,
                                   double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        KnobColorPtr multiply = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGradeParamMultiply)) : KnobColorPtr();

        EXPECT_TRUE(bool(multiply));
        if (!multiply) {
            return NodePtr();
        }
        EXPECT_EQ(4, multiply->getDimension());
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(multiply->setDriver(i, linearDriver(kKnobDriverDefaultVariable, start, end)));
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

    static bool setChoiceKnob(const NodePtr& node,
                              const char* name,
                              const std::string& id)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValueFromID(id, 0);

        return knob->getActiveEntry().id == id;
    }

    static bool setStopCondition(const NodePtr& node,
                                 const std::string& expression)
    {
        KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kLoopEndParamStopCondition));

        if (!knob) {
            return false;
        }
        knob->setValue(expression);

        return true;
    }

    // A counting body whose value goes from 0 to 1 over loop.t, so every iteration is its own render.
    NodePtr createDrivenCounter()
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
        KnobDoublePtr value = node ? std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("value")) : KnobDoublePtr();

        EXPECT_TRUE(bool(value));
        if (!value) {
            return NodePtr();
        }
        EXPECT_TRUE(value->setDriver(0, linearDriver(kKnobDriverDefaultVariable, 0., 1.)));

        return node;
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

// A Python expression reads loop.t through the loop object: it gives 10 * t at each iteration,
// reads loop.t and nothing else, and the mean over the loop is the midpoint.
TEST_F(NativeLoopBlockTest, AnExpressionReadsTheLoopVariable)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin("outer");
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr end = createLoopEnd(3, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && grade && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(grade->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));

    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    // Set inside the loop, so the check it runs outside a render reads the preview iteration.
    ASSERT_NO_THROW(multiply->setExpression(0, "loop.t * 10", false, true));
    std::string error;
    EXPECT_TRUE(multiply->isExpressionValid(0, &error)) << error;

    RenderContextReadSet expected;
    expected.add(RenderContextName::loop("t"));
    EXPECT_TRUE(multiply->getRFVReadSet() == expected);

    const double values[] = { 0., 5., 10. };
    for (int k = 0; k < 3; ++k) {
        ActionContextScope scope(true, LoopEnd::iterationContext(RenderContext::empty(), "outer", k, 3, 0, 1, kTime));
        EXPECT_NEAR(values[k], multiply->getValueAtTime(kTime, 0), 1e-9) << "iteration " << k;
    }

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(5., value, 1e-4);
}

// Inside a nested loop an expression reads both loops: 1 + loop.t + 2 * loops.outer.t, its
// inner mean 1.5 + 2 * loops.outer.t, summed over the outer loop's t of 0 and 1, is 5.
TEST_F(NativeLoopBlockTest, AnExpressionReadsAnOuterLoopsVariables)
{
    NodePtr constant = createConstant(1.);
    NodePtr outerBegin = createLoopBegin("outer");
    NodePtr innerBegin = createLoopBegin("inner");
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr innerEnd = createLoopEnd(3, kLoopEndParamOperationMean);
    NodePtr outerEnd = createLoopEnd(2, kLoopEndParamOperationSum);
    ASSERT_TRUE(constant && outerBegin && innerBegin && grade && innerEnd && outerEnd);
    ASSERT_TRUE(outerBegin->connectInput(constant, 0));
    ASSERT_TRUE(innerBegin->connectInput(outerBegin, 0));
    ASSERT_TRUE(grade->connectInput(innerBegin, 0));
    ASSERT_TRUE(innerEnd->connectInput(grade, 0));
    ASSERT_TRUE(outerEnd->connectInput(innerEnd, 0));

    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    ASSERT_NO_THROW(multiply->setExpression(0, "1 + loop.t + loops.outer.t * 2", false, true));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(outerEnd, &value));
    EXPECT_NEAR(5., value, 1e-4);
}

// An inner loop's stop condition reads the outer loop's iteration: the inner loop runs once in
// the outer's first iteration and twice in its second, so the sums add to 3.
TEST_F(NativeLoopBlockTest, AStopConditionReadsAnOuterLoopsVariables)
{
    NodePtr constant = createConstant(1.);
    NodePtr outerBegin = createLoopBegin("outer");
    NodePtr innerBegin = createLoopBegin("inner");
    NodePtr innerEnd = createLoopEnd(10, kLoopEndParamOperationSum);
    NodePtr outerEnd = createLoopEnd(2, kLoopEndParamOperationSum);
    ASSERT_TRUE(constant && outerBegin && innerBegin && innerEnd && outerEnd);
    ASSERT_TRUE(outerBegin->connectInput(constant, 0));
    ASSERT_TRUE(innerBegin->connectInput(outerBegin, 0));
    ASSERT_TRUE(innerEnd->connectInput(innerBegin, 0));
    ASSERT_TRUE(outerEnd->connectInput(innerEnd, 0));
    ASSERT_TRUE(setStopCondition(innerEnd, "loop.iteration >= loops.outer.iteration + 1"));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(outerEnd, &value));
    EXPECT_NEAR(3., value, 1e-5);

    QString message;
    int type = -1;
    innerEnd->getPersistentMessage(&message, &type, false);
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

// Iterations of red 1, 2 and 3 under alpha 0.5. Over puts each iteration over the ones before:
// 3 + (2 + 1 * 0.5) * 0.5 = 4.25; reversed, 1 + (2 + 3 * 0.5) * 0.5 = 2.75. Under is Over with
// the order reversed. Alpha is 0.875 in every case. Both schedulers give the same bits.
TEST_F(NativeLoopBlockTest, OverAndUnderFoldInOrder)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr grade = createDrivenColorGrade(1., 3.);
    NodePtr end = createLoopEnd(3, kLoopEndParamOperationOver);
    ASSERT_TRUE(constant && begin && grade && end);
    KnobColorPtr color = std::dynamic_pointer_cast<KnobColor>(constant->getKnobByName(kConstantParamColor));
    ASSERT_TRUE(bool(color));
    color->setValue(0.5, ViewSpec::all(), 3);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(grade->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));
    KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(end->getKnobByName(kLoopEndParamOperation));
    KnobBoolPtr reverseOrder = std::dynamic_pointer_cast<KnobBool>(end->getKnobByName(kLoopEndParamReverseOrder));
    ASSERT_TRUE(bool(operation));
    ASSERT_TRUE(bool(reverseOrder));
    EXPECT_EQ(std::string("Reverse Order"), reverseOrder->getLabel());

    struct Case {
        const char* operation;
        bool reverse;
        float red;
    };
    const Case cases[] = {
        { kLoopEndParamOperationOver, false, 4.25f },
        { kLoopEndParamOperationOver, true, 2.75f },
        { kLoopEndParamOperationUnder, false, 2.75f },
        { kLoopEndParamOperationUnder, true, 4.25f },
    };
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(c.operation) + (c.reverse ? " reversed" : ""));
        operation->setValueFromID(c.operation, 0);
        reverseOrder->setValue(c.reverse);

        float pixel[4] = { -1.f, -1.f, -1.f, -1.f };
        ASSERT_TRUE(renderedPixel(end, pixel));
        EXPECT_NEAR(c.red, pixel[0], 1e-5);
        EXPECT_NEAR(0.875, pixel[3], 1e-5);

        std::vector<int> unplanned;
        const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, fullWindow(), poolSizes, std::function<void()>(), 0.f, &unplanned);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplanned.size());
        for (std::size_t i = 0; i < unplanned.size(); ++i) {
            // Every step found the previous one and its iteration in the frame's store.
            EXPECT_EQ(0, unplanned[i]) << "pool " << poolSizes[i];
        }
    }
}

// 64 iterations of a float body: the task graph folds them in a chain, so with a budget of a few
// images the iterations never all wait in the frame's store at once, and the result is the same
// bits as the Legacy render's serial fold at every pool size. The iterations all read the same
// image, so each is admitted against the budget rather than run as soon as that image is there.
TEST_F(NativeLoopBlockTest, AFoldChainStaysWithinTheBytesBudget)
{
    const int width = 1024;
    const int height = 576;
    const int iterations = 64;
    const std::size_t imageBytes = (std::size_t)width * height * 4 * sizeof(float);
    const std::size_t budget = 6 * imageBytes;

    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr grade = createDrivenGrade(kKnobDriverDefaultVariable, 1., 2.);
    NodePtr end = createLoopEnd(iterations, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && grade && end);
    KnobChoicePtr extent = std::dynamic_pointer_cast<KnobChoice>(constant->getKnobByName(kNativeGeneratorParamExtent));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(constant->getKnobByName(kNativeGeneratorParamSize));
    ASSERT_TRUE(bool(extent));
    ASSERT_TRUE(bool(size));
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    size->setValue((double)width, ViewSpec::all(), 0);
    size->setValue((double)height, ViewSpec::all(), 1);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(grade->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));

    BytesBudgetGuard guard(budget);
    RenderScheduler* scheduler = appPTR->getRenderScheduler();
    scheduler->resetPeakBytesInFlight();

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplanned;
    const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, RectI(0, 0, width, height), poolSizes, std::function<void()>(), 0.f, &unplanned);
    EXPECT_FALSE(m.any) << describe(m);
    ASSERT_EQ(poolSizes.size(), unplanned.size());
    for (std::size_t i = 0; i < unplanned.size(); ++i) {
        EXPECT_EQ(0, unplanned[i]) << "pool " << poolSizes[i];
    }
    // A branch is admitted only while the stores fit the budget, except one admitted when nothing
    // else runs, which may go over by its own output.
    EXPECT_GT(scheduler->getPeakBytesInFlight(), (std::size_t)0);
    EXPECT_LE(scheduler->getPeakBytesInFlight(), budget + imageBytes) << "peak of " << ((double)scheduler->getPeakBytesInFlight() / (double)imageBytes) << " images";
}

// Over the frames 2 before to 2 after the current frame 1, a Constant whose value is the frame
// number gives the frames -1 to 3: their mean is 1, their sum 5, their minimum -1, their maximum 3.
TEST_F(NativeLoopBlockTest, FramesIterateOverARelativeFrameRange)
{
    NodePtr constant = createConstant(0.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr end = createLoopEnd(1, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && end);
    KnobColorPtr color = std::dynamic_pointer_cast<KnobColor>(constant->getKnobByName(kConstantParamColor));
    ASSERT_TRUE(bool(color));
    for (int frame = -1; frame <= 3; ++frame) {
        for (int i = 0; i < color->getDimension(); ++i) {
            color->setValueAtTime((double)frame, (double)frame, ViewSpec::all(), i);
        }
    }
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(end->connectInput(begin, 0));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamIterateOver, kLoopEndParamIterateOverFrames));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameStart, -2));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameEnd, 2));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameStep, 1));

    // The body is asked for every iteration's frame.
    EffectInstancePtr effect = end->getEffectInstance();
    FramesNeededMap frames = effect->getFramesNeeded_public(effect->getRenderHash(), kTime, ViewIdx(0), 0);
    ASSERT_EQ(1u, frames.count(0));
    std::set<double> needed;
    for (const RangeD& range : frames[0][ViewIdx(0)]) {
        for (double frame = range.min; frame <= range.max; frame += 1.) {
            needed.insert(frame);
        }
    }
    if (appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph) {
        EXPECT_EQ(std::set<double>({ -1., 0., 1., 2., 3. }), needed);
    } else {
        // Under no fold step, the last step's iteration.
        EXPECT_EQ(std::set<double>({ 3. }), needed);
    }

    struct Case {
        const char* operation;
        float value;
    };
    const Case cases[] = {
        { kLoopEndParamOperationMean, 1.f },
        { kLoopEndParamOperationSum, 5.f },
        { kLoopEndParamOperationMin, -1.f },
        { kLoopEndParamOperationMax, 3.f },
    };
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (const Case& c : cases) {
        SCOPED_TRACE(c.operation);
        ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamOperation, c.operation));
        float value = -10.f;
        ASSERT_TRUE(renderedValue(end, &value));
        EXPECT_NEAR(c.value, value, 1e-5);

        std::vector<int> unplanned;
        const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, fullWindow(), poolSizes, std::function<void()>(), 0.f, &unplanned);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplanned.size());
        for (std::size_t i = 0; i < unplanned.size(); ++i) {
            EXPECT_EQ(0, unplanned[i]) << "pool " << poolSizes[i];
        }
    }

    // Frame Step 2 keeps -1, 1 and 3.
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamOperation, kLoopEndParamOperationSum));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameStep, 2));
    float value = -10.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(3., value, 1e-5);

    // An empty range is black, with a warning.
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameEnd, -3));
    expectBlackWithMessage(end, eMessageTypeWarning, QString::fromUtf8("Frame End is before Frame Start"));
}

// A body that is the same at every frame has one output for all the iterated frames.
TEST_F(NativeLoopBlockTest, FramesOfAStillBodyRenderOnce)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr end = createLoopEnd(1, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && body && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamIterateOver, kLoopEndParamIterateOverFrames));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(2., value, 1e-5);
    EXPECT_EQ(1, CountingTestRegistry::renders(body));
}

// Single Pass at iteration 2 of 5 is the body at t = 2/4: a gain of 1 to 2 gives 1.5, unfolded.
TEST_F(NativeLoopBlockTest, SinglePassOutputsOneIteration)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr grade = createDrivenGrade(kKnobDriverDefaultVariable, 1., 2.);
    NodePtr end = createLoopEnd(5, kLoopEndParamOperationSum);
    ASSERT_TRUE(constant && begin && grade && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(grade->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeSinglePass));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamSinglePassIteration, 2));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1.5, value, 1e-5);

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplanned;
    const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, fullWindow(), poolSizes, std::function<void()>(), 0.f, &unplanned);
    EXPECT_FALSE(m.any) << describe(m);

    // The stop condition does not apply to a single pass.
    ASSERT_TRUE(setStopCondition(end, "True"));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1.5, value, 1e-5);

    ASSERT_TRUE(setIntKnob(end, kLoopEndParamSinglePassIteration, 5));
    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("past the last iteration"));
}

// The loop ends before the first iteration the stop condition holds for: 3 of 10 iterations
// render, t being 0, 1/9 and 2/9.
TEST_F(NativeLoopBlockTest, AStopConditionEndsTheLoop)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createDrivenCounter();
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && body && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >= 3"));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(3, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1. / 9., value, 1e-5);

    QString message;
    int type = -1;
    end->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplanned;
    const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, fullWindow(), poolSizes, std::function<void()>(), 0.f, &unplanned);
    EXPECT_FALSE(m.any) << describe(m);

    // Holding before the first iteration leaves nothing to fold.
    ASSERT_TRUE(setStopCondition(end, "loop.frame == frame"));
    expectBlackWithMessage(end, eMessageTypeWarning, QString::fromUtf8("before its first iteration"));
}

// A stop condition that raises at iteration 2 ends the loop there and shows its error; the two
// iterations before it still render.
TEST_F(NativeLoopBlockTest, ARaisingStopConditionEndsTheLoopWithItsError)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createDrivenCounter();
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    ASSERT_TRUE(constant && begin && body && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(setStopCondition(end, "1 / (2 - loop.iteration) > 100"));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(2, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1. / 18., value, 1e-5);

    QString message;
    int type = -1;
    end->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("ZeroDivisionError"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("iteration 2"))) << message.toStdString();

    // Fixing the expression takes the error back.
    ASSERT_TRUE(setStopCondition(end, "False"));
    ASSERT_TRUE(renderedValue(end, &value));
    end->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();

    // A syntax error stops the loop before its first iteration.
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >"));
    expectBlackWithMessage(end, eMessageTypeError, QString::fromUtf8("SyntaxError"));
}

// The stop condition reads another node's parameter: editing it moves the LoopEnd's hash, so the
// region of definition, the frames asked of the body and the cached output follow the new stop.
TEST_F(NativeLoopBlockTest, EditingAParameterTheStopConditionReadsRefreshesTheLoop)
{
    NodePtr constant = createConstant(0.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr end = createLoopEnd(1, kLoopEndParamOperationSum);
    NodePtr control = createConstant(2.);
    ASSERT_TRUE(constant && begin && end && control);
    KnobColorPtr color = std::dynamic_pointer_cast<KnobColor>(constant->getKnobByName(kConstantParamColor));
    KnobChoicePtr extent = std::dynamic_pointer_cast<KnobChoice>(constant->getKnobByName(kNativeGeneratorParamExtent));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(constant->getKnobByName(kNativeGeneratorParamSize));
    KnobColorPtr stopAt = std::dynamic_pointer_cast<KnobColor>(control->getKnobByName(kConstantParamColor));
    ASSERT_TRUE(bool(color));
    ASSERT_TRUE(bool(extent));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(stopAt));
    // At frame f, the body is f in every channel and 10 * (f + 2) pixels wide.
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    for (int frame = -1; frame <= 3; ++frame) {
        for (int i = 0; i < color->getDimension(); ++i) {
            color->setValueAtTime((double)frame, (double)frame, ViewSpec::all(), i);
        }
        size->setValueAtTime((double)frame, 10. * (frame + 2), ViewSpec::all(), 0);
        size->setValueAtTime((double)frame, 10., ViewSpec::all(), 1);
    }
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(end->connectInput(begin, 0));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamIterateOver, kLoopEndParamIterateOverFrames));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameStart, -2));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameEnd, 2));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamFrameStep, 1));
    ASSERT_TRUE(setForceCaching(end, true));
    const std::string controlVar = getApp()->getAppIDString() + "." + control->getFullyQualifiedName();
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >= " + controlVar + ".color.getValue(0)"));

    EffectInstancePtr effect = end->getEffectInstance();
    const bool legacy = appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph;
    appPTR->clearAllCaches();

    // Stopping at iteration 2 keeps the frames -1 and 0.
    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_NEAR(20., rod.width(), 1e-6);
    // Under the task graph and no fold step, the last step's iteration.
    EXPECT_EQ(legacy ? std::set<double>({ -1., 0. }) : std::set<double>({ 0. }), framesNeeded(end));
    float value = -10.f;
    ASSERT_TRUE(renderedValue(end, &value, false /*byPassCache*/));
    EXPECT_NEAR(-1., value, 1e-5);

    // Stopping at iteration 4 keeps the frames -1 to 2.
    const U64 hash = effect->getRenderHash();
    stopAt->setValue(4., ViewSpec::all(), 0);
    EXPECT_NE(hash, effect->getRenderHash());
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_NEAR(40., rod.width(), 1e-6);
    EXPECT_EQ(legacy ? std::set<double>({ -1., 0., 1., 2. }) : std::set<double>({ 2. }), framesNeeded(end));
    ASSERT_TRUE(renderedValue(end, &value, false /*byPassCache*/));
    EXPECT_NEAR(2., value, 1e-5);

    appPTR->clearAllCaches();
}

// With a stop condition and nothing it reads edited, a second render is the cached output; editing
// what it reads renders the loop again, to the new stop.
TEST_F(NativeLoopBlockTest, AStopConditionsOutputIsCachedUntilWhatItReadsChanges)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createDrivenCounter();
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    NodePtr control = createConstant(3.);
    ASSERT_TRUE(constant && begin && body && end && control);
    KnobColorPtr stopAt = std::dynamic_pointer_cast<KnobColor>(control->getKnobByName(kConstantParamColor));
    ASSERT_TRUE(bool(stopAt));
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(setForceCaching(end, true));
    const std::string controlVar = getApp()->getAppIDString() + "." + control->getFullyQualifiedName();
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >= " + controlVar + ".color.getValue(0)"));
    appPTR->clearAllCaches();

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value, false /*byPassCache*/));
    EXPECT_EQ(3, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1. / 9., value, 1e-5);

    ASSERT_TRUE(renderedValue(end, &value, false /*byPassCache*/));
    EXPECT_EQ(3, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1. / 9., value, 1e-5);

    // Five iterations, t from 0 to 4/9.
    CountingTestRegistry::reset();
    stopAt->setValue(5., ViewSpec::all(), 0);
    ASSERT_TRUE(renderedValue(end, &value, false /*byPassCache*/));
    EXPECT_EQ(5, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 2. / 9., value, 1e-5);

    appPTR->clearAllCaches();
}

// The stop condition reads a context variable as a knob expression does: rfv.n comes from a Set
// below the LoopEnd, and changing it moves where the loop stops.
TEST_F(NativeLoopBlockTest, AStopConditionReadsContextVariablesFromASetBelow)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createDrivenCounter();
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(constant && begin && body && end && set);
    EffectInstancePtr setEffect = set->getEffectInstance();
    KnobDoublePtr variable = setEffect->createDoubleKnob("n", "n", 1);
    KnobGroupPtr variables = std::dynamic_pointer_cast<KnobGroup>(set->getKnobByName(kReverseFlowVariableSetParamVariables));
    ASSERT_TRUE(bool(variables));
    ASSERT_TRUE(variables->addKnob(variable));
    setEffect->recreateUserKnobs(false);
    variable->setValue(3.);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(set->connectInput(end, 0));
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >= rfv.n"));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(set, &value));
    EXPECT_EQ(3, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1. / 9., value, 1e-5);

    QString message;
    int type = -1;
    end->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();

    CountingTestRegistry::reset();
    variable->setValue(5.);
    ASSERT_TRUE(renderedValue(set, &value));
    EXPECT_EQ(5, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 2. / 9., value, 1e-5);
}

// A node is named bare in the stop condition, as in a knob expression.
TEST_F(NativeLoopBlockTest, AStopConditionReadsAParameterByBareNodeName)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin(std::string());
    NodePtr body = createDrivenCounter();
    NodePtr end = createLoopEnd(10, kLoopEndParamOperationMean);
    NodePtr control = createConstant(4.);
    ASSERT_TRUE(constant && begin && body && end && control);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(body, 0));
    ASSERT_TRUE(setStopCondition(end, "loop.iteration >= " + control->getScriptName_mt_safe() + ".color.getValue(0)"));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(4, CountingTestRegistry::renders(body));
    EXPECT_NEAR(1. + 1.5 / 9., value, 1e-5);

    QString message;
    int type = -1;
    end->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}
