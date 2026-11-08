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
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/qnamespace.h>

#include "Engine/ColorWheelMath.h"
#include "Engine/GlobalModeStroke.h"

NATRON_NAMESPACE_USING

namespace {

const double kEps = 1e-9;

GlobalModeValueStrokeSpec
spec(GlobalModeKindEnum kind,
     std::vector<double> increments,
     bool isInt = false,
     int lock = kGlobalModeLockAll)
{
    GlobalModeValueStrokeSpec s;

    s.kind = kind;
    s.increments = increments;
    s.isInt = isInt;
    s.lockDimension = lock;

    return s;
}

std::vector<double>
strokeOnce(const std::vector<double>& start,
           const GlobalModeValueStrokeSpec& s,
           double dx,
           double dy,
           double gearFactor = 1.)
{
    GlobalModeValueStroke stroke;

    stroke.begin(start);
    stroke.addMove(dx, dy, gearFactor);

    return stroke.evaluate(s, GlobalModeValueRates());
}

} // namespace

TEST(GlobalModeStroke, TranslateMapsXAndUpToOneUnitPerPixel)
{
    const std::vector<double> v = strokeOnce({ 10., 20. }, spec(eGlobalModeKindTranslate, { 0.1, 0.1 }), 5., -3.);

    ASSERT_EQ(2u, v.size());
    EXPECT_NEAR(15., v[0], kEps);
    EXPECT_NEAR(23., v[1], kEps);
}

TEST(GlobalModeStroke, GenericSpatial2DDragsAsTranslate)
{
    const std::vector<double> v = strokeOnce({ 0., 0. }, spec(eGlobalModeKindGenericSpatial2D, { 1., 1. }), -4., 6.);

    EXPECT_NEAR(-4., v[0], kEps);
    EXPECT_NEAR(-6., v[1], kEps);
}

TEST(GlobalModeStroke, RotateRightOrUpTurnsCounterClockwise)
{
    EXPECT_NEAR(2.5, strokeOnce({ 0. }, spec(eGlobalModeKindRotate, { 0.1 }), 10., 0.)[0], kEps);
    EXPECT_NEAR(2.5, strokeOnce({ 0. }, spec(eGlobalModeKindRotate, { 0.1 }), 0., -10.)[0], kEps);
    EXPECT_NEAR(-2.5, strokeOnce({ 0. }, spec(eGlobalModeKindRotate, { 0.1 }), -10., 0.)[0], kEps);
    EXPECT_NEAR(-2.5, strokeOnce({ 0. }, spec(eGlobalModeKindRotate, { 0.1 }), 0., 10.)[0], kEps);
}

TEST(GlobalModeStroke, ClockwisePositiveRotateStillTurnsCounterClockwise)
{
    GlobalModeValueStrokeSpec clockwise = spec(eGlobalModeKindRotate, { 0.1 });

    clockwise.clockwisePositive = true;
    EXPECT_NEAR(-2.5, strokeOnce({ 0. }, clockwise, 10., 0.)[0], kEps);
    EXPECT_NEAR(-2.5, strokeOnce({ 0. }, clockwise, 0., -10.)[0], kEps);
    EXPECT_NEAR(2.5, strokeOnce({ 0. }, clockwise, -10., 0.)[0], kEps);
}

TEST(GlobalModeStroke, ScaleIsUniformAndMultiplicative)
{
    const std::vector<double> grown = strokeOnce({ 2., 2. }, spec(eGlobalModeKindScale, { 0.01, 0.01 }), 100., 0.);
    const double factor = std::pow(1.005, 100.);

    EXPECT_NEAR(2. * factor, grown[0], kEps);
    EXPECT_EQ(grown[0], grown[1]);

    GlobalModeValueStroke stroke;
    stroke.begin({ 2., 2. });
    stroke.addMove(100., 0., 1.);
    stroke.addMove(-100., 0., 1.);
    const std::vector<double> back = stroke.evaluate(spec(eGlobalModeKindScale, { 0.01, 0.01 }), GlobalModeValueRates());
    EXPECT_NEAR(2., back[0], kEps);

    const std::vector<double> shrunk = strokeOnce({ 2., 2. }, spec(eGlobalModeKindScale, { 0.01, 0.01 }), -100., 0.);
    EXPECT_NEAR(2. / factor, shrunk[0], kEps);
    EXPECT_GT(shrunk[0], 0.);
}

TEST(GlobalModeStroke, SizeIsUniformAndAddsTheIncrementPerPixel)
{
    const std::vector<double> v = strokeOnce({ 3., 3. }, spec(eGlobalModeKindSize, { 0.1, 0.1 }), 4., -6.);

    EXPECT_NEAR(4., v[0], kEps);
    EXPECT_NEAR(4., v[1], kEps);
}

TEST(GlobalModeStroke, SkewMapsXToSkewXAndUpToSkewY)
{
    const std::vector<double> v = strokeOnce({ 0., 0. }, spec(eGlobalModeKindSkew, { 0.01, 0.02 }), 10., -5.);

    EXPECT_NEAR(0.1, v[0], kEps);
    EXPECT_NEAR(0.1, v[1], kEps);
}

TEST(GlobalModeStroke, GenericMovesEveryDimensionWithEitherAxis)
{
    const std::vector<double> v = strokeOnce({ 1., 2., 3. }, spec(eGlobalModeKindGeneric, { 0.5, 0.5, 0.5 }), 2., -2.);

    EXPECT_NEAR(3., v[0], kEps);
    EXPECT_NEAR(4., v[1], kEps);
    EXPECT_NEAR(5., v[2], kEps);
}

TEST(GlobalModeStroke, ColourAndIneligibleLeaveTheStart)
{
    EXPECT_EQ(std::vector<double>({ 1., 2. }), strokeOnce({ 1., 2. }, spec(eGlobalModeKindColor, { 1., 1. }), 50., 50.));
    EXPECT_EQ(std::vector<double>({ 1., 2. }), strokeOnce({ 1., 2. }, spec(eGlobalModeKindIneligible, { 1., 1. }), 50., 50.));
}

TEST(GlobalModeStroke, GearScalesEachMoveAsItHappens)
{
    GlobalModeValueStroke stroke;

    stroke.begin({ 0. });
    stroke.addMove(10., 0., gear(false, false));
    stroke.addMove(10., 0., gear(true, false));
    stroke.addMove(10., 0., gear(false, true));
    stroke.addMove(100., 0., gear(true, true));

    EXPECT_NEAR(10. + 1. + 40. + 1., stroke.getTotalDx(), kEps);
    EXPECT_NEAR(52., stroke.evaluate(spec(eGlobalModeKindTranslate, { 1. }), GlobalModeValueRates())[0], kEps);
}

TEST(GlobalModeStroke, LockMovesOnlyThatDimension)
{
    const std::vector<double> x = strokeOnce({ 0., 0. }, spec(eGlobalModeKindTranslate, { 1., 1. }, false, 0), 5., -7.);
    EXPECT_NEAR(5., x[0], kEps);
    EXPECT_NEAR(0., x[1], kEps);

    const std::vector<double> y = strokeOnce({ 0., 0. }, spec(eGlobalModeKindTranslate, { 1., 1. }, false, 1), 5., -7.);
    EXPECT_NEAR(0., y[0], kEps);
    EXPECT_NEAR(7., y[1], kEps);

    const std::vector<double> size = strokeOnce({ 1., 1. }, spec(eGlobalModeKindSize, { 1., 1. }, false, 1), 3., 0.);
    EXPECT_NEAR(1., size[0], kEps);
    EXPECT_NEAR(4., size[1], kEps);

    const std::vector<double> skewY = strokeOnce({ 0., 0. }, spec(eGlobalModeKindSkew, { 0.01, 0.01 }, false, 1), 10., -10.);
    EXPECT_NEAR(0., skewY[0], kEps);
    EXPECT_NEAR(0.1, skewY[1], kEps);
}

TEST(GlobalModeStroke, LockBeyondTheDimensionCountMovesEveryDimension)
{
    const std::vector<double> v = strokeOnce({ 1., 1. }, spec(eGlobalModeKindSize, { 1., 1. }, false, 2), 3., 0.);

    EXPECT_NEAR(4., v[0], kEps);
    EXPECT_NEAR(4., v[1], kEps);
}

TEST(GlobalModeStroke, IntKnobsAccumulateTravelAndStepByWholeUnits)
{
    GlobalModeValueStroke stroke;
    const GlobalModeValueStrokeSpec intSize = spec(eGlobalModeKindSize, { 1. }, true);

    stroke.begin({ 5. });
    stroke.addMove(0.4, 0., 1.);
    EXPECT_EQ(5., stroke.evaluate(intSize, GlobalModeValueRates())[0]);
    stroke.addMove(0.4, 0., 1.);
    EXPECT_EQ(5., stroke.evaluate(intSize, GlobalModeValueRates())[0]);
    stroke.addMove(0.4, 0., 1.);
    EXPECT_EQ(6., stroke.evaluate(intSize, GlobalModeValueRates())[0]);
    stroke.addMove(-2.4, 0., 1.);
    EXPECT_EQ(4., stroke.evaluate(intSize, GlobalModeValueRates())[0]);

    const std::vector<double> scaled = strokeOnce({ 10. }, spec(eGlobalModeKindScale, { 1. }, true), 100., 0.);
    EXPECT_EQ(std::round(10. * std::pow(1.005, 100.)), scaled[0]);
}

TEST(GlobalModeStroke, LoneKnobKeysLockDimensions)
{
    GlobalModeValueKeyAction a = classifyGlobalModeValueKey(Qt::Key_Q, false, 2);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(0, a.dimension);

    a = classifyGlobalModeValueKey(Qt::Key_W, false, 2);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(1, a.dimension);

    a = classifyGlobalModeValueKey(Qt::Key_E, false, 3);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(2, a.dimension);

    a = classifyGlobalModeValueKey(Qt::Key_R, false, 4);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(3, a.dimension);

    EXPECT_EQ(eGlobalModeValueKeyLockAll, classifyGlobalModeValueKey(Qt::Key_A, false, 2).action);

    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_E, false, 2).action);
    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_W, false, 1).action);
    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_T, false, 4).action);
    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_1, false, 4).action);
}

TEST(GlobalModeStroke, TransformGroupKeysSwitchMembersAndLockAxes)
{
    const struct {
        int key;
        GlobalModeGroupMemberEnum member;
    } switches[] = {
        { Qt::Key_Q, eGlobalModeGroupMemberTranslate },
        { Qt::Key_W, eGlobalModeGroupMemberRotate },
        { Qt::Key_E, eGlobalModeGroupMemberScale },
        { Qt::Key_R, eGlobalModeGroupMemberSkew },
        { Qt::Key_T, eGlobalModeGroupMemberCenter },
    };
    for (const auto& s : switches) {
        const GlobalModeValueKeyAction a = classifyGlobalModeValueKey(s.key, true, 1);
        EXPECT_EQ(eGlobalModeValueKeySwitchMember, a.action);
        EXPECT_EQ(s.member, a.member);
    }

    GlobalModeValueKeyAction a = classifyGlobalModeValueKey(Qt::Key_1, true, 2);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(0, a.dimension);

    a = classifyGlobalModeValueKey(Qt::Key_2, true, 2);
    EXPECT_EQ(eGlobalModeValueKeyLockDimension, a.action);
    EXPECT_EQ(1, a.dimension);

    EXPECT_EQ(eGlobalModeValueKeyLockAll, classifyGlobalModeValueKey(Qt::Key_A, true, 2).action);

    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_2, true, 1).action);
    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_3, true, 3).action);
    EXPECT_EQ(eGlobalModeValueKeyNone, classifyGlobalModeValueKey(Qt::Key_Y, true, 2).action);
}
