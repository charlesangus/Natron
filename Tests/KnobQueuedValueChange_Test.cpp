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
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// A Viewer widget skips writing a dimension whose value does not change, so the dimension gains no
// key. While a render runs, writes are queued and the value the render reads is stale: the skip must
// not compare against it, or a drag that returns to its start leaves the knob at a queued value the
// pointer has already left.

namespace {

class RenderingHolder
    : public KnobHolder {
public:
    explicit RenderingHolder(const AppInstancePtr& app)
        : KnobHolder(app)
        , rendering(false)
    {
    }

    bool rendering;

protected:
    virtual bool canSetValue() const OVERRIDE
    {
        return !rendering;
    }

    virtual void initializeKnobs() OVERRIDE
    {
    }
};

class KnobQueuedValueChangeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        holder = std::make_shared<RenderingHolder>(getApp());
        position = AppManager::createKnob<KnobDouble>(holder.get(), std::string("position"), 2, false);
        ASSERT_NE(nullptr, position);
        position->setValue(100., ViewSpec::all(), 0);
        position->setValue(50., ViewSpec::all(), 1);
    }

    void overlayWrite(double value,
                      int dimension)
    {
        if (!position->isValueChangeAtCurrentTime(value, dimension)) {
            return;
        }
        KeyFrame k;
        position->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, &k);
    }

    std::shared_ptr<RenderingHolder> holder;
    KnobDoublePtr position;
};

} // namespace

TEST_F(KnobQueuedValueChangeTest, OutsideARenderOnlyADifferentValueIsAChange)
{
    EXPECT_FALSE(position->isValueChangeAtCurrentTime(100., 0));
    EXPECT_TRUE(position->isValueChangeAtCurrentTime(101., 0));
    EXPECT_FALSE(position->isValueChangeAtCurrentTime(50., 1));
}

TEST_F(KnobQueuedValueChangeTest, DragBackToTheStartDuringARenderEndsAtTheStart)
{
    holder->rendering = true;
    overlayWrite(101., 0);
    ASSERT_DOUBLE_EQ(100., position->getRawValue(0));

    EXPECT_TRUE(position->isValueChangeAtCurrentTime(100., 0));
    // The other dimension has nothing queued and keeps its value, so it is still skipped.
    EXPECT_FALSE(position->isValueChangeAtCurrentTime(50., 1));

    overlayWrite(100., 0);
    overlayWrite(50., 1);

    holder->rendering = false;
    holder->dequeueValuesSet();
    EXPECT_DOUBLE_EQ(100., position->getRawValue(0));
    EXPECT_DOUBLE_EQ(100., position->getValue(0));
    EXPECT_DOUBLE_EQ(50., position->getValue(1));
}

TEST_F(KnobQueuedValueChangeTest, QueuedKeyOnAnAnimatedDimensionIsAChange)
{
    const double time = position->getCurrentTime();
    KeyFrame k;
    position->setValueAtTime(time, 100., ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, &k);
    ASSERT_TRUE(position->isAnimated(0));
    ASSERT_FALSE(position->isAnimated(1));

    holder->rendering = true;
    overlayWrite(101., 0);
    EXPECT_TRUE(position->isValueChangeAtCurrentTime(100., 0));
    overlayWrite(100., 0);
    overlayWrite(50., 1);

    holder->rendering = false;
    holder->dequeueValuesSet();
    EXPECT_DOUBLE_EQ(100., position->getValueAtTime(time, 0));
    EXPECT_FALSE(position->isAnimated(1));
}
