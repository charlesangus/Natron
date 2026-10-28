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

#include <memory>
#include <vector>

#include <QThreadPool>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/ViewIdx.h"

#include "Gui/KnobGui.h"

NATRON_NAMESPACE_USING

namespace {

class RotoItemKnobKeyframesTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);

        CreateNodeArgs args(PLUGINID_NATRON_ROTO, _app->getProject());
        _node = _app->createNode(args);
        ASSERT_TRUE(bool(_node));
        RotoContextPtr context = _node->getRotoContext();
        ASSERT_TRUE(bool(context));
        _shape = context->makeSquare(1., 6., 4., context->getTimelineCurrentTime());
        ASSERT_TRUE(bool(_shape));
        _opacity = _shape->getOpacityKnob();
        ASSERT_TRUE(bool(_opacity));
    }

    virtual void TearDown() OVERRIDE
    {
        _opacity.reset();
        _shape.reset();
        _node.reset();
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    int keyCount() const
    {
        return _opacity->getCurve(ViewIdx(0), 0)->getKeyFramesCount();
    }

    AppInstancePtr _app;
    NodePtr _node;
    BezierPtr _shape;
    KnobDoublePtr _opacity;
};

} // namespace

// Undoing a dope sheet or curve editor delete puts item-knob keys back through these helpers, and
// a shape's knobs have no holder to reach the app's timeline indicators through.
TEST_F(RotoItemKnobKeyframesTest, KeyframeHelpersRestoreKeysOnAKnobWithNoHolder)
{
    ASSERT_EQ(nullptr, _opacity->getHolder());
    _opacity->setValueAtTime(10., 0.5, ViewSpec::all(), 0);
    _opacity->setValueAtTime(20., 0.25, ViewSpec::all(), 0);
    ASSERT_EQ(2, keyCount());

    CurvePtr curve = _opacity->getCurve(ViewIdx(0), 0);
    KeyFrame first;
    KeyFrame second;
    ASSERT_TRUE(curve->getKeyFrameWithTime(10., &first));
    ASSERT_TRUE(curve->getKeyFrameWithTime(20., &second));

    KnobGui::removeKnobKeyframe(_opacity, 10., 0, ViewIdx(0));
    ASSERT_EQ(1, keyCount());
    KnobGui::setKnobKeyframe(_opacity, 10., first, 0, ViewIdx(0));
    ASSERT_EQ(2, keyCount());
    EXPECT_DOUBLE_EQ(0.5, _opacity->getValueAtTime(10., 0, ViewIdx(0)));

    std::vector<KeyFrame> keys;
    keys.push_back(first);
    keys.push_back(second);
    KnobGui::removeKnobKeyframes(_opacity, keys, 0, ViewIdx(0));
    ASSERT_EQ(0, keyCount());
    KnobGui::setKnobKeyframes(_opacity, keys, 0, ViewIdx(0));
    ASSERT_EQ(2, keyCount());
    EXPECT_DOUBLE_EQ(0.25, _opacity->getValueAtTime(20., 0, ViewIdx(0)));

    KnobGui::setKnobKeyframe(_opacity, 15., 0, ViewIdx(0));
    EXPECT_EQ(3, keyCount());
}
