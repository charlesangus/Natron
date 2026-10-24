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

#include <gtest/gtest.h>

#include <QString>
#include <QDir>

#include "Engine/ContentHasher.h"
#include "Engine/Curve.h"

NATRON_NAMESPACE_USING

TEST(KeyFrame,
     Basic)
{
    KeyFrame k;

    k.setValue(10.);
    EXPECT_EQ( 10., k.getValue() );
    k.setTime(50.);
    EXPECT_EQ( 50., k.getTime() );
    k.setLeftDerivative(-1.);
    EXPECT_EQ( -1., k.getLeftDerivative() );
    k.setRightDerivative(-2.);
    EXPECT_EQ( -2., k.getRightDerivative() );
    k.setInterpolation(eKeyframeTypeCatmullRom);
    EXPECT_EQ( eKeyframeTypeCatmullRom, k.getInterpolation() );

    KeyFrame k1(10., 20.);
    EXPECT_NE(k, k1);
    EXPECT_TRUE( KeyFrame_compare_time() (k1, k) );
    EXPECT_TRUE( k1.getTime() < k.getTime() );
    k1.setValue(10.);
    k1.setTime(50.);
    k1.setLeftDerivative(-1.);
    k1.setRightDerivative(-2.);
    k1.setInterpolation(eKeyframeTypeCatmullRom);
    EXPECT_EQ(k, k1);
}

TEST(Curve, Basic)
{
    Curve c;

    // empty curve
    EXPECT_FALSE( c.isAnimated() );

    // one keyframe
    EXPECT_TRUE( c.addKeyFrame( KeyFrame(0., 5.) ) );
    EXPECT_EQ( 5., c.getValueAt(0.) );
    EXPECT_EQ( 5., c.getValueAt(10.) );
    EXPECT_EQ( 5., c.getValueAt(-10.) );
    EXPECT_TRUE( c.isAnimated() );

    // two keyframes
    EXPECT_FALSE( c.addKeyFrame( KeyFrame(0., 10.) ) ); // keyframe already exists, replacing it
    EXPECT_TRUE( c.addKeyFrame( KeyFrame(1., 20.) ) );
    EXPECT_EQ( 10., c.getValueAt(0.) );
    EXPECT_EQ( 20., c.getValueAt(1.) );
    EXPECT_EQ( 10., c.getValueAt(-10.) ); // before first keyframe
    EXPECT_EQ( 20., c.getValueAt(10.) ); // after last keyframe
    EXPECT_EQ( 15., c.getValueAt(0.5) ); // middle
    // derivative
    EXPECT_EQ( 10., c.getDerivativeAt(0.) );
    EXPECT_EQ( 10., c.getDerivativeAt(0.5) ); // middle
    EXPECT_EQ( 10., c.getDerivativeAt(0.99) );
    EXPECT_EQ( 0., c.getDerivativeAt(1.) ); // from last keyframe, it's constant

    // integrate
    EXPECT_EQ( 100., c.getIntegrateFromTo(-10., 0.) );
    EXPECT_EQ( 15., c.getIntegrateFromTo(0., 1.) );
    EXPECT_EQ( 200., c.getIntegrateFromTo(1., 11.) );
    EXPECT_EQ( 315., c.getIntegrateFromTo(-10., 11.) );

    KeyFrameSet ks = c.getKeyFrames_mt_safe();

    c.clearKeyFrames();
    // empty curve
    EXPECT_FALSE( c.isAnimated() );

    // two keyframes, constant interpolation
    EXPECT_TRUE( c.addKeyFrame( KeyFrame(0., 10., 0., 0., eKeyframeTypeConstant) ) ); // keyframe already exists, replacing it
    EXPECT_TRUE( c.addKeyFrame( KeyFrame(1., 20., 0., 0., eKeyframeTypeConstant) ) );
    EXPECT_EQ( 10., c.getValueAt(0.) );
    EXPECT_EQ( 20., c.getValueAt(1.) );
    EXPECT_EQ( 10., c.getValueAt(-10.) ); // before first keyframe
    EXPECT_EQ( 20., c.getValueAt(10.) ); // after last keyframe
    EXPECT_EQ( 10., c.getValueAt(0.5) ); // middle
    // derivative
    EXPECT_EQ( 0., c.getDerivativeAt(0.) );
    EXPECT_EQ( 0., c.getDerivativeAt(0.5) ); // middle
    EXPECT_EQ( 0., c.getDerivativeAt(0.99) );
    EXPECT_EQ( 0., c.getDerivativeAt(1.) ); // from last keyframe, it's constant
    // integrate
    EXPECT_EQ( 100., c.getIntegrateFromTo(-10., 0.) );
    EXPECT_EQ( 10., c.getIntegrateFromTo(0., 1.) );
    EXPECT_EQ( 200., c.getIntegrateFromTo(1., 11.) );
    EXPECT_EQ( 310., c.getIntegrateFromTo(-10., 11.) );

    KeyFrame k2(1., 20.);
}

TEST(KeyFrame, WeightsOnlyOnFreeAndBrokenKeys)
{
    KeyFrame smooth(0., 1.);
    smooth.setLeftWeight(0.5);
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, smooth.getLeftWeight());

    KeyFrame free(0., 1., 2., 2., eKeyframeTypeFree, 0.25, 0.75);
    EXPECT_EQ(0.25, free.getLeftWeight());
    EXPECT_EQ(0.75, free.getRightWeight());

    free.setRightWeight(5.);
    EXPECT_EQ(Interpolation::kMaxTangentWeight, free.getRightWeight());
    free.setRightWeight(-1.);
    EXPECT_EQ(Interpolation::kMinTangentWeight, free.getRightWeight());
    free.setRightWeight(std::nan(""));
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, free.getRightWeight());

    KeyFrame other(free);
    EXPECT_EQ(free, other);
    other.setLeftWeight(0.5);
    EXPECT_NE(free, other);

    other.setInterpolation(eKeyframeTypeBroken);
    EXPECT_EQ(0.5, other.getLeftWeight());
    other.setInterpolation(eKeyframeTypeLinear);
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, other.getLeftWeight());
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, other.getRightWeight());
}

namespace {
void
addFreeKeys(Curve& c)
{
    c.addKeyFrame(KeyFrame(0., 0., 1., 1., eKeyframeTypeFree));
    c.addKeyFrame(KeyFrame(10., 5., 0., 0., eKeyframeTypeFree));
    c.addKeyFrame(KeyFrame(20., 0., -1., -1., eKeyframeTypeFree));
}

U64
curveHash(const Curve& c)
{
    ContentHasher h;
    c.appendContentHash(h);

    return h.finalize();
}
} // anon namespace

TEST(Curve, WeightedKeyChangesSegmentsNotKeys)
{
    Curve c;
    addFreeKeys(c);
    const double before5 = c.getValueAt(5.);
    const double before15 = c.getValueAt(15.);
    const double integralBefore = c.getIntegrateFromTo(0., 20.);
    const U64 hashBefore = curveHash(c);

    const KeyFrame key = c.setKeyFrameTangents(0., 0., 0.8, 0.8, 1);
    EXPECT_EQ(0.8, key.getLeftWeight());
    EXPECT_EQ(0.8, key.getRightWeight());
    EXPECT_EQ(eKeyframeTypeFree, key.getInterpolation());

    EXPECT_EQ(0., c.getValueAt(0.));
    EXPECT_EQ(5., c.getValueAt(10.));
    EXPECT_EQ(0., c.getValueAt(20.));
    EXPECT_NE(before5, c.getValueAt(5.));
    EXPECT_NE(before15, c.getValueAt(15.));
    // Longer flat handles at the peak keep the curve nearer the peak for longer.
    EXPECT_GT(c.getValueAt(5.), before5);
    EXPECT_GT(c.getIntegrateFromTo(0., 20.), integralBefore);
    EXPECT_NEAR(0., c.getDerivativeAt(10.), 1e-9);

    // Only the weights changed, so only they can have moved the hash.
    EXPECT_NE(hashBefore, curveHash(c));
    c.setKeyFrameTangents(0., 0., Interpolation::kDefaultTangentWeight, Interpolation::kDefaultTangentWeight, 1);
    EXPECT_EQ(hashBefore, curveHash(c));
    EXPECT_EQ(before5, c.getValueAt(5.));
    EXPECT_EQ(before15, c.getValueAt(15.));
}

TEST(Curve, LeavingFreeResetsWeights)
{
    Curve c;
    addFreeKeys(c);
    c.setKeyFrameTangents(0., 0., 0.6, 0.2, 1);

    const KeyFrame smooth = c.setKeyFrameInterpolation(eKeyframeTypeSmooth, 1);
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, smooth.getLeftWeight());
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, smooth.getRightWeight());

    // Weights given to a key that is not Free or Broken do not hold.
    const KeyFrame stillSmooth = c.setKeyFrameTangents(0., 0., 0.6, 0.2, 1);
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, stillSmooth.getLeftWeight());
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, stillSmooth.getRightWeight());
}

TEST(Curve, CopiesKeepWeights)
{
    Curve c;
    addFreeKeys(c);
    c.setKeyFrameTangents(0., 0., 0.6, 0.2, 1);
    const U64 hash = curveHash(c);

    Curve cloned;
    cloned.clone(c);
    Curve assigned;
    assigned = c;
    Curve copied(c);

    for (const Curve* other : { &cloned, &assigned, &copied }) {
        KeyFrame key;
        ASSERT_TRUE(other->getKeyFrameWithIndex(1, &key));
        EXPECT_EQ(0.6, key.getLeftWeight());
        EXPECT_EQ(0.2, key.getRightWeight());
        EXPECT_EQ(c.getValueAt(5.), other->getValueAt(5.));
        EXPECT_EQ(hash, curveHash(*other));
    }
}
