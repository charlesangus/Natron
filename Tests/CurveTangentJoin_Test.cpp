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
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/CurveEditorUndoRedo.h"
#include "Gui/CurveGui.h"

NATRON_NAMESPACE_USING

namespace {

// A Project is a KnobHolder with a real (background) AppInstance behind it, the same fixture
// ColorKnobUndoMerge_Test uses, so the knob's beginChanges()/endChanges() have a holder to call.
KnobDoublePtr
makeDoubleKnob()
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!project) {
        return KnobDoublePtr();
    }

    return AppManager::createKnob<KnobDouble>(project.get(), std::string("Join"), 1, false);
}

} // namespace

TEST(CurveTangentJoin, OneToOneGivesOne)
{
    EXPECT_NEAR(1., joinedDerivative(1., 1.), 1e-9);
}

TEST(CurveTangentJoin, AFlatAndANearlyVerticalSlopeGiveAbout45Degrees)
{
    EXPECT_NEAR(1., joinedDerivative(0., 1e6), 1e-3);
}

TEST(CurveTangentJoin, OppositeSlopesCancelOut)
{
    EXPECT_NEAR(0., joinedDerivative(-1., 1.), 1e-9);
}

TEST(CurveTangentJoin, CommandRoundTripRestoresTheBrokenStateOnUndo)
{
    KnobDoublePtr knob = makeDoubleKnob();
    ASSERT_TRUE(bool(knob));

    CurvePtr curve = knob->getCurve(ViewIdx(0), 0);
    ASSERT_TRUE(bool(curve));
    curve->addKeyFrame(KeyFrame(0., 0., -4., 1., eKeyframeTypeBroken));

    CurveGuiPtr curveGui = std::make_shared<KnobCurveGui>(nullptr, curve, KnobIPtr(knob), RotoCurveContextPtr(),
                                                          0, QString::fromUtf8("Join"), QColor());

    KeyFrameSet keys = curve->getKeyFrames_mt_safe();
    ASSERT_EQ(std::size_t(1), keys.size());
    KeyPtr key = std::make_shared<SelectedKey>(curveGui, *keys.begin(), false, KeyFrame(), false, KeyFrame());

    SelectedKeys selection;
    selection[curveGui].push_back(key);

    QUndoStack undoStack;
    undoStack.push(new JoinTangentsCommand(nullptr, selection));

    double joined = joinedDerivative(-4., 1.);
    KeyFrameSet afterJoin = curve->getKeyFrames_mt_safe();
    ASSERT_EQ(std::size_t(1), afterJoin.size());
    EXPECT_NEAR(joined, afterJoin.begin()->getLeftDerivative(), 1e-9);
    EXPECT_NEAR(joined, afterJoin.begin()->getRightDerivative(), 1e-9);
    EXPECT_EQ(eKeyframeTypeFree, afterJoin.begin()->getInterpolation());

    undoStack.undo();

    KeyFrameSet afterUndo = curve->getKeyFrames_mt_safe();
    ASSERT_EQ(std::size_t(1), afterUndo.size());
    EXPECT_NEAR(-4., afterUndo.begin()->getLeftDerivative(), 1e-9);
    EXPECT_NEAR(1., afterUndo.begin()->getRightDerivative(), 1e-9);
    EXPECT_EQ(eKeyframeTypeBroken, afterUndo.begin()->getInterpolation());
}

TEST(CurveTangentJoin, AnUnbrokenKeyIsLeftAlone)
{
    KnobDoublePtr knob = makeDoubleKnob();
    ASSERT_TRUE(bool(knob));

    CurvePtr curve = knob->getCurve(ViewIdx(0), 0);
    ASSERT_TRUE(bool(curve));
    curve->addKeyFrame(KeyFrame(0., 0., 2., 2., eKeyframeTypeFree));

    CurveGuiPtr curveGui = std::make_shared<KnobCurveGui>(nullptr, curve, KnobIPtr(knob), RotoCurveContextPtr(),
                                                          0, QString::fromUtf8("Join"), QColor());

    KeyFrameSet keys = curve->getKeyFrames_mt_safe();
    KeyPtr key = std::make_shared<SelectedKey>(curveGui, *keys.begin(), false, KeyFrame(), false, KeyFrame());

    SelectedKeys selection;
    selection[curveGui].push_back(key);

    QUndoStack undoStack;
    undoStack.push(new JoinTangentsCommand(nullptr, selection));

    // Nothing was Broken, so the command had nothing to join: the undo stack still records
    // the gesture (so Edit > Undo has something to point at), but the curve is untouched.
    KeyFrameSet afterPush = curve->getKeyFrames_mt_safe();
    ASSERT_EQ(std::size_t(1), afterPush.size());
    EXPECT_NEAR(2., afterPush.begin()->getLeftDerivative(), 1e-9);
    EXPECT_NEAR(2., afterPush.begin()->getRightDerivative(), 1e-9);
    EXPECT_EQ(eKeyframeTypeFree, afterPush.begin()->getInterpolation());
}
