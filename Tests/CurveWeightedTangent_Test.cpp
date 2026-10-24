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
#include <memory>

#include <QColor>
#include <QString>
#include <QUndoStack>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/Interpolation.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/CurveEditorUndoRedo.h"
#include "Gui/CurveGui.h"

NATRON_NAMESPACE_USING

namespace {

const double kThird = Interpolation::kDefaultTangentWeight;

// Three keys at 0, 10 and 20, so the middle key's handles reach into two 10-frame segments.
struct ThreeKeyCurve {
    KnobDoublePtr knob;
    CurvePtr curve;
    CurveGuiPtr curveGui;
    KeyPtr key;

    bool init(const KeyFrame& middle,
              double selectedTime = 10.)
    {
        AppInstancePtr app = appPTR->getTopLevelInstance();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        if (!project) {
            return false;
        }
        knob = AppManager::createKnob<KnobDouble>(project.get(), std::string("WeightedTangent"), 1, false);
        if (!knob) {
            return false;
        }
        curve = knob->getCurve(ViewIdx(0), 0);
        if (!curve) {
            return false;
        }
        curve->addKeyFrame(KeyFrame(0., 0., 0., 0., eKeyframeTypeFree));
        curve->addKeyFrame(middle);
        curve->addKeyFrame(KeyFrame(20., 0., 0., 0., eKeyframeTypeFree));
        curveGui = std::make_shared<KnobCurveGui>(nullptr, curve, KnobIPtr(knob), RotoContextPtr(),
                                                  0, QString::fromUtf8("WeightedTangent"), QColor());
        KeyFrame selected;
        if (!curve->getKeyFrameWithTime(selectedTime, &selected)) {
            return false;
        }
        key = std::make_shared<SelectedKey>(curveGui, selected, false, KeyFrame(), false, KeyFrame());

        return true;
    }

    KeyFrame keyAt(double time) const
    {
        KeyFrame k;
        bool found = curve->getKeyFrameWithTime(time, &k);
        EXPECT_TRUE(found);

        return k;
    }
};

// dx and dy are measured from the handle to its key, as the curve widget passes them: a right
// handle dragged to the right of its key has a negative dx.
MoveTangentCommand*
dragRight(const ThreeKeyCurve& c,
          double handleTimeOffset,
          double handleValueOffset,
          bool forceBreak = false)
{
    return new MoveTangentCommand(nullptr, MoveTangentCommand::eSelectedTangentRight, c.key,
                                  -handleTimeOffset, -handleValueOffset, true, forceBreak);
}

} // namespace

TEST(CurveWeightedTangent, AFreeDragDoublesTheDraggedWeightAndTheOtherSideScalesWithIt)
{
    ThreeKeyCurve c;
    ASSERT_TRUE(c.init(KeyFrame(10., 5., 0., 0., eKeyframeTypeFree)));
    const double midBefore = c.curve->getValueAt(15.);

    QUndoStack undoStack;
    undoStack.push(dragRight(c, 20. / 3., 0.));

    KeyFrame k = c.keyAt(10.);
    EXPECT_EQ(eKeyframeTypeFree, k.getInterpolation());
    EXPECT_NEAR(2. * kThird, k.getRightWeight(), 1e-12);
    EXPECT_NEAR(2. * kThird, k.getLeftWeight(), 1e-12);
    EXPECT_NEAR(0., k.getRightDerivative(), 1e-12);
    EXPECT_NEAR(0., k.getLeftDerivative(), 1e-12);
    EXPECT_GT(c.curve->getValueAt(15.), midBefore + 1e-6);
}

TEST(CurveWeightedTangent, ABrokenDragChangesTheDraggedSideOnly)
{
    ThreeKeyCurve c;
    ASSERT_TRUE(c.init(KeyFrame(10., 5., -1., 2., eKeyframeTypeBroken)));

    QUndoStack undoStack;
    undoStack.push(dragRight(c, 5., 5.));

    KeyFrame k = c.keyAt(10.);
    EXPECT_EQ(eKeyframeTypeBroken, k.getInterpolation());
    EXPECT_NEAR(0.5, k.getRightWeight(), 1e-12);
    EXPECT_NEAR(1., k.getRightDerivative(), 1e-12);
    EXPECT_EQ(kThird, k.getLeftWeight());
    EXPECT_EQ(-1., k.getLeftDerivative());
}

TEST(CurveWeightedTangent, ACtrlDragBreaksTheKeyAndLeavesTheOtherSide)
{
    ThreeKeyCurve c;
    ASSERT_TRUE(c.init(KeyFrame(10., 5., 0., 0., eKeyframeTypeFree, 0.25, 0.25)));

    QUndoStack undoStack;
    undoStack.push(dragRight(c, 5., 5., true));

    KeyFrame k = c.keyAt(10.);
    EXPECT_EQ(eKeyframeTypeBroken, k.getInterpolation());
    EXPECT_NEAR(0.5, k.getRightWeight(), 1e-12);
    EXPECT_EQ(0.25, k.getLeftWeight());
    EXPECT_EQ(0., k.getLeftDerivative());
}

TEST(CurveWeightedTangent, UndoRestoresTheKeyExactly)
{
    ThreeKeyCurve c;
    const KeyFrame original(10., 5., 0.7, 0.7, eKeyframeTypeFree, 0.25, 0.4);
    ASSERT_TRUE(c.init(original));
    const KeyFrame before = c.keyAt(10.);

    QUndoStack undoStack;
    undoStack.push(dragRight(c, 8., -3.));
    ASSERT_NE(before, c.keyAt(10.));

    undoStack.undo();
    EXPECT_TRUE(before == c.keyAt(10.));

    undoStack.redo();
    KeyFrame k = c.keyAt(10.);
    EXPECT_NEAR(0.8, k.getRightWeight(), 1e-12);
    EXPECT_NEAR(0.25 * 0.8 / 0.4, k.getLeftWeight(), 1e-12);
}

TEST(CurveWeightedTangent, ConsecutiveDragStepsUndoInOneStep)
{
    ThreeKeyCurve c;
    ASSERT_TRUE(c.init(KeyFrame(10., 5., 0., 0., eKeyframeTypeFree)));
    const KeyFrame before = c.keyAt(10.);

    QUndoStack undoStack;
    undoStack.push(dragRight(c, 4., 1.));
    undoStack.push(dragRight(c, 6., 2.));
    undoStack.push(dragRight(c, 7., 3.));
    EXPECT_EQ(1, undoStack.count());

    KeyFrame k = c.keyAt(10.);
    EXPECT_NEAR(0.7, k.getRightWeight(), 1e-12);
    EXPECT_NEAR(3. / 7., k.getRightDerivative(), 1e-12);

    undoStack.undo();
    EXPECT_TRUE(before == c.keyAt(10.));
}

TEST(CurveWeightedTangent, AnOuterHandleDragSetsTheSlopeOnly)
{
    ThreeKeyCurve c;
    ASSERT_TRUE(c.init(KeyFrame(10., 5., 0., 0., eKeyframeTypeFree), 0.));

    // The first key's left handle has no segment beyond it to take a weight from.
    QUndoStack undoStack;
    undoStack.push(new MoveTangentCommand(nullptr, MoveTangentCommand::eSelectedTangentLeft, c.key, 2., 1., true));

    KeyFrame k = c.keyAt(0.);
    EXPECT_NEAR(0.5, k.getLeftDerivative(), 1e-12);
    EXPECT_EQ(kThird, k.getLeftWeight());
    EXPECT_EQ(kThird, k.getRightWeight());
}
