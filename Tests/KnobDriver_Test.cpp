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

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

RenderContextPtr
contextWith(const RenderContextName& name,
            const RenderContextValue& value)
{
    return RenderContext::Builder().set(name, value).build();
}

// knob's dimension 0 at frame 1, read as the holder's effect does while it renders under context.
double
doubleUnder(const KnobColorPtr& knob,
            const RenderContextPtr& context)
{
    ActionContextScope scope(true, context);

    return knob->getValueAtTime(1., 0);
}

int
intUnder(const KnobIntPtr& knob,
         const RenderContextPtr& context)
{
    ActionContextScope scope(true, context);

    return knob->getValue(0);
}

KnobDriverPtr
makeDriver(const std::string& variable,
           double start,
           double end,
           KnobDriver::EasingEnum easing,
           const Curve* curve = NULL)
{
    return std::make_shared<const KnobDriver>(variable, start, end, easing, curve);
}

// 0 -> 0, 0.5 -> 0.8, 1 -> 1, linear between.
void
fillEasingCurve(Curve* curve)
{
    curve->addKeyFrame(KeyFrame(0., 0., 0., 0., eKeyframeTypeLinear));
    curve->addKeyFrame(KeyFrame(0.5, 0.8, 0., 0., eKeyframeTypeLinear));
    curve->addKeyFrame(KeyFrame(1., 1., 0., 0., eKeyframeTypeLinear));
}
} // anonymous namespace

TEST_F(BaseTest, KnobDriverReadsTheEasedLerpUnderItsVariable)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));
    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    multiply->setValue(0.75, ViewSpec::all(), 0);

    ASSERT_TRUE(multiply->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 1., 2., KnobDriver::eEasingLinear)));
    ASSERT_TRUE(multiply->hasDrivers());

    const RenderContextName t = RenderContextName::loop("t");
    EXPECT_DOUBLE_EQ(1., doubleUnder(multiply, contextWith(t, 0.)));
    EXPECT_DOUBLE_EQ(2., doubleUnder(multiply, contextWith(t, 1.)));
    EXPECT_DOUBLE_EQ(1.5, doubleUnder(multiply, contextWith(t, 0.5)));
    // Linear easing extrapolates.
    EXPECT_DOUBLE_EQ(3., doubleUnder(multiply, contextWith(t, 2.)));

    // Its own value without a context, under one lacking the variable, or holding it as text.
    EXPECT_DOUBLE_EQ(0.75, multiply->getValueAtTime(1., 0));
    EXPECT_DOUBLE_EQ(0.75, multiply->getValue(0));
    EXPECT_DOUBLE_EQ(0.75, doubleUnder(multiply, contextWith(RenderContextName::loop("iteration"), (std::int64_t)3)));
    EXPECT_DOUBLE_EQ(0.75, doubleUnder(multiply, contextWith(t, std::string("half"))));
    // The other dimensions have no driver.
    {
        ActionContextScope scope(true, contextWith(t, 1.));
        EXPECT_DOUBLE_EQ(1., multiply->getValueAtTime(1., 1));
    }

    // Smooth easing: 3t^2 - 2t^3, the midpoint unchanged, t clamped to [0, 1].
    ASSERT_TRUE(multiply->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 1., 2., KnobDriver::eEasingSmooth)));
    EXPECT_DOUBLE_EQ(1.5, doubleUnder(multiply, contextWith(t, 0.5)));
    EXPECT_DOUBLE_EQ(1.15625, doubleUnder(multiply, contextWith(t, 0.25)));
    EXPECT_DOUBLE_EQ(2., doubleUnder(multiply, contextWith(t, 2.)));

    // Curve easing maps t through the curve.
    Curve easing;
    fillEasingCurve(&easing);
    ASSERT_TRUE(multiply->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 1., 2., KnobDriver::eEasingCurve, &easing)));
    EXPECT_DOUBLE_EQ(1., doubleUnder(multiply, contextWith(t, 0.)));
    EXPECT_NEAR(1.8, doubleUnder(multiply, contextWith(t, 0.5)), 1e-12);
    EXPECT_NEAR(1.4, doubleUnder(multiply, contextWith(t, 0.25)), 1e-12);
    EXPECT_DOUBLE_EQ(2., doubleUnder(multiply, contextWith(t, 1.)));

    // An outer loop's variable, and an integer one.
    ASSERT_TRUE(multiply->setDriver(0, makeDriver("loops.outer.iteration", 0., 1., KnobDriver::eEasingLinear)));
    EXPECT_DOUBLE_EQ(4., doubleUnder(multiply, contextWith(RenderContextName::loops("outer", "iteration"), (std::int64_t)4)));
    EXPECT_DOUBLE_EQ(0.75, doubleUnder(multiply, contextWith(RenderContextName::loop("iteration"), (std::int64_t)4)));

    ASSERT_TRUE(multiply->setDriver(0, KnobDriverPtr()));
    EXPECT_FALSE(multiply->hasDrivers());
    EXPECT_DOUBLE_EQ(0.75, doubleUnder(multiply, contextWith(t, 1.)));
}

TEST_F(BaseTest, KnobDriverRoundsForIntKnobsAndIsRefusedByOthers)
{
    NodePtr loopEnd = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
    ASSERT_TRUE(bool(loopEnd));
    KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(loopEnd->getKnobByName(kLoopEndParamIterations));
    KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(loopEnd->getKnobByName(kLoopEndParamOperation));
    ASSERT_TRUE(iterations && operation);

    ASSERT_TRUE(iterations->setDriver(0, makeDriver("count", 0., 10., KnobDriver::eEasingLinear)));
    EXPECT_EQ(3, intUnder(iterations, contextWith(RenderContextName::plain("count"), 0.26)));
    // Clamped to the knob's range, whose minimum is 0.
    EXPECT_EQ(0, intUnder(iterations, contextWith(RenderContextName::plain("count"), -1.)));

    EXPECT_FALSE(operation->canHaveDriver());
    EXPECT_FALSE(operation->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 0., 1., KnobDriver::eEasingLinear)));
    EXPECT_FALSE(bool(operation->getDriver(0)));
    EXPECT_FALSE(iterations->setDriver(1, makeDriver(kKnobDriverDefaultVariable, 0., 1., KnobDriver::eEasingLinear)));

    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));
    KnobBoolPtr clampBlack = std::dynamic_pointer_cast<KnobBool>(grade->getKnobByName(kGradeParamClampBlack));
    ASSERT_TRUE(bool(clampBlack));
    EXPECT_FALSE(clampBlack->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 0., 1., KnobDriver::eEasingLinear)));
}

TEST_F(BaseTest, KnobDriverIsAReadSourceInTheContentHash)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));
    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    EXPECT_FALSE(multiply->hasContextReadSources());
    const U64 undriven = multiply->getContentHash();

    ASSERT_TRUE(multiply->setDriver(2, makeDriver("loops.outer.t", 1., 2., KnobDriver::eEasingLinear)));
    EXPECT_TRUE(multiply->hasContextReadSources());
    EXPECT_TRUE(multiply->getContextReadSet().contains(RenderContextName::loops("outer", "t")));
    EXPECT_FALSE(multiply->getContextReadSet().contains(RenderContextName::loop("t")));
    const U64 driven = multiply->getContentHash();
    EXPECT_NE(undriven, driven);

    ASSERT_TRUE(multiply->setDriver(2, makeDriver("loops.outer.t", 1., 3., KnobDriver::eEasingLinear)));
    EXPECT_NE(driven, multiply->getContentHash());
    EXPECT_TRUE(multiply->getContextReadSet().contains(RenderContextName::loops("outer", "t")));

    ASSERT_TRUE(multiply->setDriver(2, KnobDriverPtr()));
    EXPECT_FALSE(multiply->hasContextReadSources());
    EXPECT_EQ(undriven, multiply->getContentHash());
}

TEST_F(BaseTest, KnobDriverRoundTripsThroughAProject)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));
    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    Curve easing;
    fillEasingCurve(&easing);
    ASSERT_TRUE(multiply->setDriver(0, makeDriver(kKnobDriverDefaultVariable, 1., 2., KnobDriver::eEasingLinear)));
    ASSERT_TRUE(multiply->setDriver(3, makeDriver("loops.outer.t", 0.25, 4., KnobDriver::eEasingCurve, &easing)));
    const std::string gradeName = grade->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("knob-driver.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    multiply.reset();
    grade.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    grade = project->getNodeByName(gradeName);
    ASSERT_TRUE(bool(grade));
    multiply = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));

    const KnobDriverPtr first = multiply->getDriver(0);
    ASSERT_TRUE(bool(first));
    EXPECT_EQ(std::string(kKnobDriverDefaultVariable), first->getVariable());
    EXPECT_DOUBLE_EQ(1., first->getStart());
    EXPECT_DOUBLE_EQ(2., first->getEnd());
    EXPECT_EQ(KnobDriver::eEasingLinear, first->getEasing());
    EXPECT_FALSE(multiply->getDriver(1));
    EXPECT_FALSE(multiply->getDriver(2));
    const KnobDriverPtr last = multiply->getDriver(3);
    ASSERT_TRUE(bool(last));
    EXPECT_EQ(std::string("loops.outer.t"), last->getVariable());
    EXPECT_DOUBLE_EQ(0.25, last->getStart());
    EXPECT_DOUBLE_EQ(4., last->getEnd());
    EXPECT_EQ(KnobDriver::eEasingCurve, last->getEasing());
    EXPECT_NEAR(0.8, last->ease(0.5), 1e-12);
    EXPECT_NEAR(0.4, last->ease(0.25), 1e-12);

    EXPECT_DOUBLE_EQ(1.5, doubleUnder(multiply, contextWith(RenderContextName::loop("t"), 0.5)));
    EXPECT_DOUBLE_EQ(1., multiply->getValueAtTime(1., 0));
    EXPECT_TRUE(multiply->getContextReadSet().contains(RenderContextName::loops("outer", "t")));
}
