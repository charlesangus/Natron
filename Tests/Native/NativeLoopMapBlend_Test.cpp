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

#include <cmath>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativeShapeTestSupport.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Generator/Ramp.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopMapBlend.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using namespace LoopMapBlend;
using NativeShapeTest::at;
using NativeShapeTest::renderRgba;
using NativeShapeTest::resetProject;

namespace {

const double kTime = 1.;
const int kWidth = 256;
const int kHeight = 64;
const int kHalf = kWidth / 2;

RectI
fullWindow()
{
    return RectI(0, 0, kWidth, kHeight);
}

KnobDriverPtr
linearDriver(double start,
             double end)
{
    return std::make_shared<const KnobDriver>(kKnobDriverDefaultVariable, start, end, KnobDriver::eEasingLinear);
}

bool
renderPlane(const NodePtr& node,
            RenderedPlane* plane)
{
    return renderRgba(node, fullWindow(), plane);
}

bool
sameBits(float a,
         float b)
{
    return (a == b) || (std::isnan(a) && std::isnan(b));
}

// Enables LoopMapBlend::TileStats for the scope.
class TileStatsGuard {
public:
    TileStatsGuard()
    {
        TileStats::setEnabled(true);
    }

    ~TileStatsGuard()
    {
        TileStats::setEnabled(false);
    }

    TileStatsGuard(const TileStatsGuard&) = delete;
    TileStatsGuard& operator=(const TileStatsGuard&) = delete;
};

} // namespace

class NativeLoopMapBlendTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject(getApp(), kWidth, kHeight);
    }

    virtual void TearDown() OVERRIDE
    {
        TileStats::setEnabled(false);
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A Constant of value in every channel, over the project format or, when extent is not
    // null, over extent.
    NodePtr createConstant(double value,
                           const RectI& extent = RectI())
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, { value, value, value, value }));
        if (!extent.isNull()) {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
        }

        return node;
    }

    // A Ramp from 0 at p0 to 1 at p1 over extent.
    NodePtr createRamp(const RectI& extent,
                       double p0x,
                       double p0y,
                       double p1x,
                       double p1y)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
        EXPECT_TRUE(setKnobValues(node, kRampParamPoint0, { p0x, p0y }));
        EXPECT_TRUE(setKnobValues(node, kRampParamPoint1, { p1x, p1y }));

        return node;
    }

    // A Grade whose multiply, in every channel, goes from start to end over loop.t.
    NodePtr createDrivenGrade(double start,
                              double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        KnobColorPtr multiply = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGradeParamMultiply)) : KnobColorPtr();

        EXPECT_TRUE(bool(multiply));
        if (!multiply) {
            return NodePtr();
        }
        for (int i = 0; i < multiply->getDimension(); ++i) {
            EXPECT_TRUE(multiply->setDriver(i, linearDriver(start, end)));
        }

        return node;
    }

    // A Blur whose size goes from start to end over loop.t.
    NodePtr createDrivenBlur(double start,
                             double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
        KnobDoublePtr size = node ? std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBlurParamSize)) : KnobDoublePtr();

        EXPECT_TRUE(bool(size));
        if (!size) {
            return NodePtr();
        }
        for (int i = 0; i < size->getDimension(); ++i) {
            EXPECT_TRUE(size->setDriver(i, linearDriver(start, end)));
        }

        return node;
    }

    // A LoopEnd in Map Blend over iterations.
    NodePtr createMapBlendEnd(int iterations,
                              const std::string& interpolation = kLoopEndParamInterpolationLinear)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setChoiceKnob(node, kLoopEndParamMode, kLoopEndParamModeMapBlend));
        EXPECT_TRUE(setIterations(node, iterations));
        EXPECT_TRUE(setChoiceKnob(node, kLoopEndParamInterpolation, interpolation));

        return node;
    }

    // source -> LoopBegin -> body -> end, with map on end's Map input when there is one.
    static bool wireLoop(const NodePtr& source,
                         const NodePtr& begin,
                         const NodePtr& body,
                         const NodePtr& end,
                         const NodePtr& map)
    {
        if (!source || !begin || !body || !end) {
            return false;
        }
        if (!begin->connectInput(source, 0) || !body->connectInput(begin, 0) || !end->connectInput(body, kLoopEndInputSource)) {
            return false;
        }

        return !map || end->connectInput(map, kLoopEndInputMap);
    }

    static bool setIterations(const NodePtr& node,
                              int iterations)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kLoopEndParamIterations));

        if (!knob) {
            return false;
        }
        knob->setValue(iterations);

        return true;
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

    // The red value of the bottom-left pixel of node's output.
    static bool renderedValue(const NodePtr& node,
                              float* value)
    {
        RenderedPlane plane;

        if (!renderPlane(node, &plane)) {
            return false;
        }
        *value = at(plane, 0, 0, 0);

        return true;
    }

    static std::string message(const NodePtr& node,
                               int* type)
    {
        QString text;

        node->getPersistentMessage(&text, type, false);

        return text.toStdString();
    }
};

// Iteration k sits at k / (n - 1): a value on an iteration reads it alone, one between two reads
// both, and the ranges a tile spans give the iterations it needs.
TEST_F(NativeLoopMapBlendTest, TapsAndIterationRanges)
{
    Taps taps = tapsAt(0.5f, 5, eInterpolationLinear);
    ASSERT_EQ(1, taps.count);
    EXPECT_EQ(2, taps.taps[0].iteration);
    EXPECT_EQ(1.f, taps.taps[0].weight);

    taps = tapsAt(0.3f, 3, eInterpolationLinear);
    ASSERT_EQ(2, taps.count);
    EXPECT_EQ(0, taps.taps[0].iteration);
    EXPECT_EQ(1, taps.taps[1].iteration);
    EXPECT_NEAR(0.4f, taps.taps[0].weight, 1e-6f);
    EXPECT_NEAR(0.6f, taps.taps[1].weight, 1e-6f);

    taps = tapsAt(0.3f, 3, eInterpolationNearest);
    ASSERT_EQ(1, taps.count);
    EXPECT_EQ(1, taps.taps[0].iteration);

    // Catmull-Rom over k - 1 .. k + 2: at the first span the held end merges into one tap.
    taps = tapsAt(0.1f, 3, eInterpolationCubic);
    ASSERT_EQ(3, taps.count);
    float sum = 0.f;
    for (int i = 0; i < taps.count; ++i) {
        EXPECT_EQ(i, taps.taps[i].iteration);
        sum += taps.taps[i].weight;
    }
    EXPECT_NEAR(1.f, sum, 1e-6f);
    taps = tapsAt(0.4f, 6, eInterpolationCubic);
    ASSERT_EQ(4, taps.count);
    EXPECT_EQ(1, taps.taps[0].iteration);
    EXPECT_EQ(4, taps.taps[3].iteration);

    taps = tapsAt(0.7f, 1, eInterpolationCubic);
    ASSERT_EQ(1, taps.count);
    EXPECT_EQ(0, taps.taps[0].iteration);

    taps = tapsAt(std::numeric_limits<float>::quiet_NaN(), 5, eInterpolationLinear);
    ASSERT_EQ(1, taps.count);
    EXPECT_EQ(0, taps.taps[0].iteration);
    taps = tapsAt(2.f, 5, eInterpolationLinear);
    ASSERT_EQ(1, taps.count);
    EXPECT_EQ(4, taps.taps[0].iteration);
    EXPECT_EQ(0.f, weightOf(taps, 3));

    MapRange range;
    IterationRange iterations = iterationsOf(range, 5, eInterpolationLinear);
    EXPECT_EQ(0, iterations.first);
    EXPECT_EQ(0, iterations.last);
    range.max = 1.f;
    iterations = iterationsOf(range, 5, eInterpolationLinear);
    EXPECT_EQ(0, iterations.first);
    EXPECT_EQ(4, iterations.last);
    range.min = range.max = 0.3f;
    iterations = iterationsOf(range, 5, eInterpolationCubic);
    EXPECT_EQ(0, iterations.first);
    EXPECT_EQ(3, iterations.last);
    range.min = range.max = 0.5f;
    iterations = iterationsOf(range, 5, eInterpolationCubic);
    EXPECT_EQ(2, iterations.first);
    EXPECT_EQ(2, iterations.last);
    range.min = 0.1f;
    range.max = 0.2f;
    iterations = iterationsOf(range, 5, eInterpolationNearest);
    EXPECT_EQ(0, iterations.first);
    EXPECT_EQ(1, iterations.last);
}

TEST_F(NativeLoopMapBlendTest, KnobsAndLevelSpacing)
{
    NodePtr end = createMapBlendEnd(5);
    ASSERT_TRUE(bool(end));

    KnobChoicePtr interpolation = std::dynamic_pointer_cast<KnobChoice>(end->getKnobByName(kLoopEndParamInterpolation));
    KnobStringPtr levelSpacing = std::dynamic_pointer_cast<KnobString>(end->getKnobByName(kLoopEndParamLevelSpacing));
    ASSERT_TRUE(bool(interpolation));
    ASSERT_TRUE(bool(levelSpacing));
    EXPECT_EQ("Interpolation", interpolation->getLabel());
    EXPECT_EQ("Level Spacing", levelSpacing->getLabel());
    const std::vector<ChoiceOption> options = interpolation->getEntries_mt_safe();
    ASSERT_EQ(3u, options.size());
    EXPECT_EQ(kLoopEndParamInterpolationLinear, options[0].id);
    EXPECT_EQ(kLoopEndParamInterpolationCubic, options[1].id);
    EXPECT_EQ(kLoopEndParamInterpolationNearest, options[2].id);
    EXPECT_TRUE(bool(end->getKnobByName(kMapInputParamChannel)));
    EXPECT_TRUE(bool(end->getKnobByName(kMapInputParamInvert)));
    EXPECT_TRUE(bool(end->getKnobByName(kMapInputParamGamma)));

    EXPECT_EQ("0.25 apart, 5 levels", levelSpacing->getValue());
    ASSERT_TRUE(setIterations(end, 1));
    EXPECT_EQ("One iteration: the Map has no effect.", levelSpacing->getValue());
}

// A Grade gain driven 0 to 1, read at a constant Map of 0.5, is the gain 0.5 exactly; between
// iterations Linear and Cubic both give the linear body's value and Nearest the nearest iteration.
TEST_F(NativeLoopMapBlendTest, ConstantMapReadsTheDrivenValue)
{
    NodePtr source = createConstant(1.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(0., 1.);
    NodePtr end = createMapBlendEnd(5);
    NodePtr half = createConstant(0.5);
    ASSERT_TRUE(wireLoop(source, begin, grade, end, half));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(0.5f, value);

    NodePtr between = createConstant(0.3);
    ASSERT_TRUE(end->replaceInput(between, kLoopEndInputMap));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(0.3f, value, 1e-6f);
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamInterpolation, kLoopEndParamInterpolationCubic));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(0.3f, value, 1e-6f);
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamInterpolation, kLoopEndParamInterpolationNearest));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(0.25f, value);

    // One iteration is t = 0, whatever the Map.
    ASSERT_TRUE(setIterations(end, 1));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(0.f, value);
}

// A missing Map reads 0, so the output is the first iteration, and says so.
TEST_F(NativeLoopMapBlendTest, MissingMapWarnsAndReadsZero)
{
    NodePtr source = createConstant(1.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(1., 2.);
    NodePtr end = createMapBlendEnd(5);
    ASSERT_TRUE(wireLoop(source, begin, grade, end, NodePtr()));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(1.f, value);
    int type = -1;
    const std::string text = message(end, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type) << text;
    EXPECT_NE(std::string::npos, text.find("No Map is connected")) << text;

    NodePtr map = createConstant(1.);
    ASSERT_TRUE(end->connectInput(map, kLoopEndInputMap));
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(2.f, value);
    EXPECT_TRUE(message(end, &type).empty());
}

// The Map's invert and gamma remap it before it picks the iterations.
TEST_F(NativeLoopMapBlendTest, InvertAndGammaRemapTheMap)
{
    NodePtr source = createConstant(1.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(1., 2.);
    NodePtr end = createMapBlendEnd(5);
    NodePtr map = createConstant(0.25);
    ASSERT_TRUE(wireLoop(source, begin, grade, end, map));

    KnobBoolPtr invert = std::dynamic_pointer_cast<KnobBool>(end->getKnobByName(kMapInputParamInvert));
    ASSERT_TRUE(bool(invert));
    invert->setValue(true);
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_EQ(1.75f, value);

    invert->setValue(false);
    ASSERT_TRUE(setKnobValues(end, kMapInputParamGamma, { 2. }));
    MapInputParams params;
    params.exponent = 0.5f;
    ASSERT_TRUE(renderedValue(end, &value));
    EXPECT_NEAR(1. + params.remap(0.25f), value, 1e-5);
}

// A Blur driven 0 to 20 through a Map black on the left half leaves that half as the input was.
TEST_F(NativeLoopMapBlendTest, BlackMapLeavesTheInput)
{
    NodePtr source = createRamp(fullWindow(), 0., 0., (double)kWidth, 0.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr blur = createDrivenBlur(0., 20.);
    NodePtr end = createMapBlendEnd(5);
    NodePtr map = createConstant(1., RectI(kHalf, 0, kWidth, kHeight));
    ASSERT_TRUE(wireLoop(source, begin, blur, end, map));

    RenderedPlane input, output;
    ASSERT_TRUE(renderPlane(source, &input));
    ASSERT_TRUE(renderPlane(end, &output));
    int differing = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kHalf; ++x) {
            for (int c = 0; c < 4; ++c) {
                differing += sameBits(at(input, x, y, c), at(output, x, y, c)) ? 0 : 1;
            }
        }
    }
    EXPECT_EQ(0, differing);
}

// A Map 0 on the left half and a ramp on the right: the left tiles ask for one iteration, the
// right ones for all of them, and the image is what blending every iteration gives, bit for bit,
// under both schedulers.
TEST_F(NativeLoopMapBlendTest, CullsIterationsPerTile)
{
    const int count = 5;
    NodePtr source = createConstant(1.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(0., 1.);
    NodePtr end = createMapBlendEnd(count);
    NodePtr ramp = createRamp(RectI(kHalf, 0, kWidth, kHeight), (double)kHalf, 0., (double)kHalf, (double)kHeight);
    ASSERT_TRUE(wireLoop(source, begin, grade, end, ramp));

    RenderedPlane culled;
    {
        TileStatsGuard guard;
        ASSERT_TRUE(renderPlane(end, &culled));

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, fullWindow(), poolSizes);
        EXPECT_FALSE(m.any) << describe(m);

        const std::vector<TileStats::Entry> entries = TileStats::take();
        int left = 0, right = 0;
        for (const TileStats::Entry& entry : entries) {
            if (entry.tile.x2 <= kHalf) {
                ++left;
                EXPECT_EQ(1, entry.iterations) << "left tile at " << entry.tile.x1 << "," << entry.tile.y1;
            } else {
                ASSERT_GE(entry.tile.x1, kHalf);
                ++right;
                EXPECT_EQ(count, entry.iterations) << "right tile at " << entry.tile.x1 << "," << entry.tile.y1;
            }
        }
        EXPECT_GT(left, 0);
        EXPECT_GT(right, 0);
    }

    // Each iteration alone, read through a constant Map that lands on it.
    std::vector<RenderedPlane> levels(count);
    for (int k = 0; k < count; ++k) {
        NodePtr level = createConstant((double)k / (double)(count - 1));
        ASSERT_TRUE(end->replaceInput(level, kLoopEndInputMap));
        ASSERT_TRUE(renderPlane(end, &levels[k]));
    }
    RenderedPlane mapValues;
    ASSERT_TRUE(renderPlane(ramp, &mapValues));
    int differing = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const Taps taps = tapsAt(at(mapValues, x, y, 3), count, eInterpolationLinear);
            for (int c = 0; c < 4; ++c) {
                float expected = 0.f;
                for (int i = 0; i < taps.count; ++i) {
                    expected += taps.taps[i].weight * at(levels[taps.taps[i].iteration], x, y, c);
                }
                differing += sameBits(expected, at(culled, x, y, c)) ? 0 : 1;
            }
        }
    }
    EXPECT_EQ(0, differing);
}

// A Map-blend LoopEnd whose Map comes from inside another loop's body reads it there: it is not
// that loop's end.
TEST_F(NativeLoopMapBlendTest, AMapReadFromAnotherBodyDoesNotPair)
{
    NodePtr outerSource = createConstant(1.);
    NodePtr outerBegin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr outerGrade = createDrivenGrade(0., 1.);
    NodePtr outerEnd = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
    ASSERT_TRUE(outerEnd && setIterations(outerEnd, 3));
    ASSERT_TRUE(wireLoop(outerSource, outerBegin, outerGrade, outerEnd, NodePtr()));

    NodePtr source = createConstant(1.);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(0., 1.);
    NodePtr end = createMapBlendEnd(5);
    ASSERT_TRUE(wireLoop(source, begin, grade, end, outerGrade));

    EXPECT_EQ(outerEnd->getEffectInstance().get(), LoopEnd::pairedEndOf(outerBegin).get());
    EXPECT_EQ(end->getEffectInstance().get(), LoopEnd::pairedEndOf(begin).get());
    int type = -1;
    EXPECT_TRUE(message(outerEnd, &type).empty()) << message(outerEnd, &type);
}
