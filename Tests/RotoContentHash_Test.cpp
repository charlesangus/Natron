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

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"

NATRON_NAMESPACE_USING

namespace {

const int kManyStrokes = 500;
const int kPointsPerStroke = 64;

CurvePtr
makeStrokeCurve(int seed)
{
    CurvePtr curve = std::make_shared<Curve>();
    for (int i = 0; i < kPointsPerStroke; ++i) {
        curve->addKeyFrame(KeyFrame(i, seed + 0.5 * i));
    }

    return curve;
}

} // namespace

class RotoContentHashTest
    : public BaseTest {
protected:
    RotoContextPtr createRotoContext()
    {
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        if (!roto) {
            return RotoContextPtr();
        }

        return roto->getRotoContext();
    }

    // Keyed at the current time because Bezier::movePointByIndex() ignores a point that has
    // no keyframe at the given time while auto-keying (on by default) is what keys new points.
    BezierPtr makeKeyedSquare(const RotoContextPtr& context,
                              double x,
                              double y,
                              double size)
    {
        return context->makeSquare(x, y, size, context->getTimelineCurrentTime());
    }
};

TEST_F(RotoContentHashTest, MovingAPointAndBackRestoresTheHash)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    ASSERT_TRUE(context->isAutoKeyingEnabled());
    BezierPtr square = makeKeyedSquare(context, 1., 6., 4.);
    ASSERT_TRUE(bool(square));
    const double time = context->getTimelineCurrentTime();

    const U64 initial = context->getContentHash();
    EXPECT_NE(0u, initial);
    EXPECT_EQ(initial, context->getContentHash());

    square->movePointByIndex(0, time, 2., 3.);
    const U64 moved = context->getContentHash();
    EXPECT_NE(initial, moved);

    square->movePointByIndex(0, time, -2., -3.);
    EXPECT_EQ(initial, context->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, MovingAPointChangesTheRotoNodeHash)
{
    getApp()->getProject()->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr square = makeKeyedSquare(context, 1., 6., 4.);
    ASSERT_TRUE(bool(square));
    const double time = context->getTimelineCurrentTime();

    roto->refreshHashNow();
    const U64 initial = roto->getContentHash();
    EXPECT_NE(0u, initial);

    square->movePointByIndex(0, time, 2., 3.);
    roto->refreshHashNow();
    EXPECT_NE(initial, roto->getContentHash());

    square->movePointByIndex(0, time, -2., -3.);
    roto->refreshHashNow();
    EXPECT_EQ(initial, roto->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, AddingAKeyframeChangesTheHash)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr square = makeKeyedSquare(context, 1., 6., 4.);
    ASSERT_TRUE(bool(square));
    const int keyframes = square->getKeyframesCount();

    const U64 initial = context->getContentHash();
    square->setKeyframe(context->getTimelineCurrentTime() + 4.);
    ASSERT_EQ(keyframes + 1, square->getKeyframesCount());
    EXPECT_NE(initial, context->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, ReorderingItemsChangesTheHash)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr first = makeKeyedSquare(context, 1., 6., 4.);
    BezierPtr second = makeKeyedSquare(context, 2., 7., 3.);
    ASSERT_TRUE(bool(first) && bool(second));

    RotoLayerPtr base = context->getOrCreateBaseLayer();
    ASSERT_TRUE(bool(base));
    ASSERT_EQ(0, base->getChildIndex(second));
    ASSERT_EQ(1, base->getChildIndex(first));

    const U64 initial = context->getContentHash();

    base->insertItem(first, 0);
    ASSERT_EQ(0, base->getChildIndex(first));
    const U64 swapped = context->getContentHash();
    EXPECT_NE(initial, swapped);

    base->insertItem(second, 0);
    ASSERT_EQ(0, base->getChildIndex(second));
    EXPECT_EQ(initial, context->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, KnobEditsChangeTheHash)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr square = makeKeyedSquare(context, 1., 6., 4.);
    ASSERT_TRUE(bool(square));
    // A selected item's knobs are slaved to the node's, so edits are made on its own knobs only
    // once it is deselected.
    context->clearSelection(RotoItem::eSelectionReasonOther);

    const U64 initial = context->getContentHash();
    const double opacity = square->getOpacityKnob()->getValue();

    square->getOpacityKnob()->setValue(opacity * 0.5);
    EXPECT_NE(initial, context->getContentHash());

    square->getOpacityKnob()->setValue(opacity);
    EXPECT_EQ(initial, context->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, IdenticalContextsHashEqual)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr a = createRotoContext();
    RotoContextPtr b = createRotoContext();
    ASSERT_TRUE(bool(a) && bool(b));
    ASSERT_TRUE(bool(makeKeyedSquare(a, 1., 6., 4.)));
    ASSERT_TRUE(bool(makeKeyedSquare(b, 1., 6., 4.)));

    EXPECT_EQ(a->getContentHash(), b->getContentHash());

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoContentHashTest, StrokeHashFollowsItsCurves)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));

    // Detached and without internal nodes: only the item's own hash is exercised.
    RotoStrokeItemPtr stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context, std::string("stroke"), RotoLayerPtr());
    const U64 empty = stroke->getContentHash();

    stroke->addStroke(makeStrokeCurve(0), makeStrokeCurve(1), makeStrokeCurve(2));
    const U64 oneStroke = stroke->getContentHash();
    EXPECT_NE(empty, oneStroke);
    EXPECT_EQ(oneStroke, stroke->getContentHash());

    CurvePtr x, y, p;
    stroke->removeLastStroke(&x, &y, &p);
    EXPECT_EQ(empty, stroke->getContentHash());

    getApp()->getProject()->reset(false, true);
}

// Not a pass/fail check on timing: prints how long hashing many strokes takes cold and memoized.
TEST_F(RotoContentHashTest, HashingManyStrokesTiming)
{
    getApp()->getProject()->reset(false, true);

    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));

    std::vector<RotoStrokeItemPtr> strokes;
    strokes.reserve(kManyStrokes);
    for (int i = 0; i < kManyStrokes; ++i) {
        RotoStrokeItemPtr stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context, std::string("stroke") + std::to_string(i), RotoLayerPtr());
        stroke->addStroke(makeStrokeCurve(i), makeStrokeCurve(i + 1), makeStrokeCurve(i + 2));
        strokes.push_back(stroke);
    }

    typedef std::chrono::steady_clock Clock;
    U64 cold = 0;
    const Clock::time_point coldStart = Clock::now();
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        cold ^= strokes[i]->getContentHash() + i;
    }
    const Clock::time_point coldEnd = Clock::now();

    U64 warm = 0;
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        warm ^= strokes[i]->getContentHash() + i;
    }
    const Clock::time_point warmEnd = Clock::now();

    EXPECT_EQ(cold, warm);

    const double coldMs = std::chrono::duration<double, std::milli>(coldEnd - coldStart).count();
    const double warmMs = std::chrono::duration<double, std::milli>(warmEnd - coldEnd).count();
    std::printf("[ RotoContentHash ] %d strokes x %d points: cold %.3f ms, memoized %.3f ms\n",
                kManyStrokes, kPointsPerStroke, coldMs, warmMs);

    strokes.clear();
    getApp()->getProject()->reset(false, true);
}
