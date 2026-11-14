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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QObject>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Curve.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::vector<std::pair<double, double>> Keys;

Keys
keysOf(const KnobIPtr& knob,
       int dimension = 0)
{
    Keys keys;
    CurvePtr curve = knob->getCurve(ViewSpec(0), dimension, true);
    if (!curve) {
        return keys;
    }
    const KeyFrameSet set = curve->getKeyFrames_mt_safe();
    for (KeyFrameSet::const_iterator it = set.begin(); it != set.end(); ++it) {
        keys.push_back(std::make_pair(it->getTime(), it->getValue()));
    }

    return keys;
}

// Keeps what is recorded and the step each record was made in, the way the window's history would.
class StepRecorder
    : public UndoRecorderI {
public:
    StepRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , replaying(false)
        , commands()
        , serials()
    {
    }

    virtual void openStep(const std::string& /*text*/) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
        }
    }

    virtual void closeStep() OVERRIDE FINAL
    {
        if (depth > 0) {
            --depth;
        }
    }

    virtual bool isCapturing() const OVERRIDE FINAL
    {
        return (depth > 0) && !replaying;
    }

    virtual std::uint64_t getCaptureSerial() const OVERRIDE FINAL
    {
        return serial;
    }

    virtual bool isReplaying() const OVERRIDE FINAL
    {
        return replaying;
    }

    virtual void record(const UndoCommandPtr& command,
                        const UndoTouched& /*touched*/) OVERRIDE FINAL
    {
        commands.push_back(command);
        serials.push_back(serial);
    }

    int depth;
    std::uint64_t serial;
    bool replaying;
    std::vector<UndoCommandPtr> commands;
    std::vector<std::uint64_t> serials;
};

} // namespace

class RotoSelectionProxyTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_recorder) {
            _recorder->commands.clear();
            getApp()->setUndoRecorder(UndoRecorderPtr());
            _recorder.reset();
        }
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    RotoContextPtr makeContext(const char* pluginID)
    {
        _node = createNode(QString::fromUtf8(pluginID));

        return _node ? _node->getRotoContext() : RotoContextPtr();
    }

    template <typename K>
    std::shared_ptr<K> proxy(const char* name) const
    {
        return std::dynamic_pointer_cast<K>(_node->getKnobByName(name));
    }

    // A stroke built the way the paint tool builds one: nodes first, then inserted into its layer.
    RotoStrokeItemPtr makeStroke(const RotoContextPtr& context,
                                 const char* name,
                                 double y)
    {
        RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, name, RotoLayerPtr()));
        stroke->createNodes();
        context->addItem(context->getLayers().front(), 0, stroke, RotoItem::eSelectionReasonOther);
        for (int i = 0; i < 8; ++i) {
            stroke->appendPoint(i == 0, RotoPoint(4. + 2. * i, y, 0.5, 1. + 0.1 * i));
        }
        stroke->setStrokeFinished();

        return stroke;
    }

    BezierPtr makeSquare(const RotoContextPtr& context,
                         double x)
    {
        return context->makeSquare(x, 0., 10., context->getTimelineCurrentTime());
    }

    std::shared_ptr<StepRecorder> installRecorder()
    {
        _recorder = std::make_shared<StepRecorder>();
        getApp()->setUndoRecorder(_recorder);

        return _recorder;
    }

    // Undoes, then redoes, everything recorded, the way the history replays one entry.
    void undoRecorded()
    {
        UndoReplayGuard replay(getApp());
        _recorder->replaying = true;
        for (std::vector<UndoCommandPtr>::reverse_iterator it = _recorder->commands.rbegin(); it != _recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
        _recorder->replaying = false;
    }

    void redoRecorded()
    {
        UndoReplayGuard replay(getApp());
        _recorder->replaying = true;
        for (std::vector<UndoCommandPtr>::iterator it = _recorder->commands.begin(); it != _recorder->commands.end(); ++it) {
            (*it)->redo();
        }
        _recorder->replaying = false;
    }

    NodePtr _node;
    std::shared_ptr<StepRecorder> _recorder;
};

// Selecting, adding to, removing from and clearing the selection never write an item: each stroke
// keeps its own frame, and the node's knob shows the last-selected one.
TEST_F(RotoSelectionProxyTest, StrokesKeepTheirLifeTimeFrameThroughSelection)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTOPAINT);
    ASSERT_TRUE(bool(context));
    ASSERT_FALSE(context->getLayers().empty());
    RotoStrokeItemPtr first = makeStroke(context, "First", 10.);
    RotoStrokeItemPtr second = makeStroke(context, "Second", 40.);
    ASSERT_TRUE(first && second);
    first->getLifeTimeFrameKnob()->setValue(5);
    second->getLifeTimeFrameKnob()->setValue(12);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    KnobIntPtr shown = proxy<KnobInt>("lifeTimeFrame");
    ASSERT_TRUE(bool(shown));

    auto expectOwn = [&](const char* step) {
        EXPECT_EQ(5, first->getLifeTimeFrameKnob()->getValue()) << step;
        EXPECT_EQ(12, second->getLifeTimeFrameKnob()->getValue()) << step;
    };

    context->select(first, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(5, shown->getValue());
    expectOwn("select first");
    context->select(second, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(12, shown->getValue());
    expectOwn("add second");
    context->deselect(second, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(5, shown->getValue());
    expectOwn("deselect second");
    context->select(second, RotoItem::eSelectionReasonOther);
    context->deselect(first, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(12, shown->getValue());
    expectOwn("deselect first");
    context->clearSelection(RotoItem::eSelectionReasonOther);
    expectOwn("clear");

    context->select(second, RotoItem::eSelectionReasonOther);
    context->select(first, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(5, shown->getValue());
    expectOwn("second then first");
    context->clearSelection(RotoItem::eSelectionReasonOther);
    expectOwn("clear again");
}

// Curves keyed differently keep every key through any order of selection changes, and the node's
// knob shows the last-selected curve's keys.
TEST_F(RotoSelectionProxyTest, BeziersKeepTheirOwnOpacityKeysThroughSelection)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr a = makeSquare(context, 0.);
    BezierPtr b = makeSquare(context, 30.);
    ASSERT_TRUE(a && b);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    a->getOpacityKnob()->setValueAtTime(1., 0.2, ViewSpec::all(), 0);
    a->getOpacityKnob()->setValueAtTime(10., 0.8, ViewSpec::all(), 0);
    b->getOpacityKnob()->setValueAtTime(5., 0.5, ViewSpec::all(), 0);
    const Keys aKeys = keysOf(a->getOpacityKnob());
    const Keys bKeys = keysOf(b->getOpacityKnob());
    ASSERT_EQ(2u, aKeys.size());
    ASSERT_EQ(1u, bKeys.size());
    KnobDoublePtr shown = proxy<KnobDouble>("opacity");
    ASSERT_TRUE(bool(shown));

    auto expectOwn = [&](const char* step) {
        EXPECT_EQ(aKeys, keysOf(a->getOpacityKnob())) << step;
        EXPECT_EQ(bKeys, keysOf(b->getOpacityKnob())) << step;
    };

    context->select(a, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(aKeys, keysOf(shown));
    expectOwn("select a");
    context->select(b, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(bKeys, keysOf(shown));
    expectOwn("add b");
    context->deselect(b, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(aKeys, keysOf(shown));
    expectOwn("deselect b");
    context->select(b, RotoItem::eSelectionReasonOther);
    context->deselect(a, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(bKeys, keysOf(shown));
    expectOwn("deselect a");
    context->select(a, RotoItem::eSelectionReasonOther);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    expectOwn("clear");
}

// An edit on the node's knob reaches every selected item and nothing else. An animated item takes
// a key and keeps its other keys; a static item takes a static value.
TEST_F(RotoSelectionProxyTest, ProxyEditsReachEverySelectedItemOnly)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr animated = makeSquare(context, 0.);
    BezierPtr still = makeSquare(context, 30.);
    BezierPtr outside = makeSquare(context, 60.);
    ASSERT_TRUE(animated && still && outside);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    const double now = context->getTimelineCurrentTime();
    const double away = now + 20.;
    animated->getOpacityKnob()->setValueAtTime(away, 0.1, ViewSpec::all(), 0);
    KnobDoublePtr shown = proxy<KnobDouble>("opacity");
    ASSERT_TRUE(bool(shown));

    context->select(animated, RotoItem::eSelectionReasonOther);
    context->select(still, RotoItem::eSelectionReasonOther);
    ASSERT_FALSE(shown->isAnimated(0));

    shown->setValue(0.4);
    EXPECT_DOUBLE_EQ(0.4, still->getOpacityKnob()->getValue());
    EXPECT_FALSE(still->getOpacityKnob()->isAnimated(0));
    EXPECT_DOUBLE_EQ(0.4, animated->getOpacityKnob()->getValueAtTime(now));
    EXPECT_DOUBLE_EQ(0.1, animated->getOpacityKnob()->getValueAtTime(away));
    EXPECT_EQ(2u, keysOf(animated->getOpacityKnob()).size());
    EXPECT_DOUBLE_EQ(1., outside->getOpacityKnob()->getValue());

    // Keys set on the node's knob, then one of them removed.
    const double k1 = now + 5.;
    const double k2 = now + 9.;
    shown->setValueAtTime(k1, 0.6, ViewSpec::all(), 0);
    shown->setValueAtTime(k2, 0.7, ViewSpec::all(), 0);
    KeyFrame key;
    EXPECT_TRUE(still->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k1, &key));
    EXPECT_DOUBLE_EQ(0.6, key.getValue());
    EXPECT_TRUE(still->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k2, &key));
    EXPECT_DOUBLE_EQ(0.7, key.getValue());
    EXPECT_TRUE(animated->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k2, &key));
    EXPECT_DOUBLE_EQ(0.7, key.getValue());
    EXPECT_TRUE(animated->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(away, &key));
    EXPECT_FALSE(outside->getOpacityKnob()->isAnimated(0));

    KnobIPtr(shown)->deleteValueAtTime(eCurveChangeReasonInternal, k1, ViewSpec::all(), 0, true);
    EXPECT_FALSE(still->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k1, &key));
    EXPECT_FALSE(animated->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k1, &key));
    EXPECT_TRUE(still->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(k2, &key));
    EXPECT_TRUE(animated->getOpacityKnob()->getCurve(ViewSpec(0), 0, true)->getKeyFrameWithTime(away, &key));
    EXPECT_FALSE(outside->getOpacityKnob()->isAnimated(0));
    EXPECT_DOUBLE_EQ(1., outside->getOpacityKnob()->getValue());

    // Once deselected, an item no longer takes the node knob's edits.
    context->clearSelection(RotoItem::eSelectionReasonOther);
    shown->setValue(0.9);
    EXPECT_DOUBLE_EQ(0.7, still->getOpacityKnob()->getValueAtTime(k2));
    EXPECT_DOUBLE_EQ(1., outside->getOpacityKnob()->getValue());
}

// A write to one selected item's own knob changes that item only and reads back; the node's knob
// follows it only when it is the item the knob shows.
TEST_F(RotoSelectionProxyTest, AWriteToOneSelectedItemChangesOnlyIt)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr a = makeSquare(context, 0.);
    BezierPtr b = makeSquare(context, 30.);
    ASSERT_TRUE(a && b);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(a, RotoItem::eSelectionReasonOther);
    context->select(b, RotoItem::eSelectionReasonOther);
    KnobDoublePtr shown = proxy<KnobDouble>("opacity");
    ASSERT_TRUE(bool(shown));

    a->getOpacityKnob()->setValue(0.3);
    EXPECT_DOUBLE_EQ(0.3, a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(1., b->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(1., shown->getValue());

    b->getOpacityKnob()->setValue(0.7);
    EXPECT_DOUBLE_EQ(0.7, b->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(0.3, a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(0.7, shown->getValue());

    context->clearSelection(RotoItem::eSelectionReasonOther);
    EXPECT_DOUBLE_EQ(0.3, a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(0.7, b->getOpacityKnob()->getValue());
}

// A write to one selected item does not come back to it, or reach another, through the node's
// knob.
TEST_F(RotoSelectionProxyTest, AnItemWriteDoesNotEchoThroughTheNodeKnob)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr a = makeSquare(context, 0.);
    BezierPtr b = makeSquare(context, 30.);
    ASSERT_TRUE(a && b);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(a, RotoItem::eSelectionReasonOther);
    context->select(b, RotoItem::eSelectionReasonOther);

    int aChanges = 0;
    int bChanges = 0;
    QObject::connect(a->getOpacityKnob()->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged,
                     [&aChanges](ViewSpec, int, int) { ++aChanges; });
    QObject::connect(b->getOpacityKnob()->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged,
                     [&bChanges](ViewSpec, int, int) { ++bChanges; });

    // b is the item the node's knob shows.
    b->getOpacityKnob()->setValue(0.25);
    EXPECT_EQ(1, bChanges);
    EXPECT_EQ(0, aChanges);
    a->getOpacityKnob()->setValue(0.5);
    EXPECT_EQ(1, aChanges);
    EXPECT_EQ(1, bChanges);

    // One edit on the node's knob writes each selected item once.
    proxy<KnobDouble>("opacity")->setValue(0.75);
    EXPECT_EQ(2, aChanges);
    EXPECT_EQ(2, bChanges);
    EXPECT_DOUBLE_EQ(0.75, a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(0.75, b->getOpacityKnob()->getValue());
    context->clearSelection(RotoItem::eSelectionReasonOther);
}

// The curves of a selected layer are not edited by the node's knobs: only the layer is.
TEST_F(RotoSelectionProxyTest, ACurveSelectedThroughItsLayerIsNotEdited)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr inside = makeSquare(context, 0.);
    RotoLayerPtr layer = context->addLayer();
    ASSERT_TRUE(inside && layer);
    layer->addItem(inside);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(layer, RotoItem::eSelectionReasonOther);

    proxy<KnobDouble>("opacity")->setValue(0.2);
    EXPECT_DOUBLE_EQ(1., inside->getOpacityKnob()->getValue());
    context->clearSelection(RotoItem::eSelectionReasonOther);
}

// An edit on the node's knob, made in one step, records each selected item's own knob in that step:
// undoing the step gives each item back its own curve, and redoing it reapplies the edit.
TEST_F(RotoSelectionProxyTest, AForwardedEditIsOneUndoStep)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr a = makeSquare(context, 0.);
    BezierPtr b = makeSquare(context, 30.);
    BezierPtr outside = makeSquare(context, 60.);
    ASSERT_TRUE(a && b && outside);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    a->getOpacityKnob()->setValueAtTime(1., 0.2, ViewSpec::all(), 0);
    a->getOpacityKnob()->setValueAtTime(10., 0.8, ViewSpec::all(), 0);
    b->getOpacityKnob()->setValueAtTime(5., 0.5, ViewSpec::all(), 0);
    const Keys aKeys = keysOf(a->getOpacityKnob());
    const Keys bKeys = keysOf(b->getOpacityKnob());
    context->select(a, RotoItem::eSelectionReasonOther);
    context->select(b, RotoItem::eSelectionReasonOther);
    KnobDoublePtr shown = proxy<KnobDouble>("opacity");
    ASSERT_TRUE(bool(shown));
    ASSERT_EQ(bKeys, keysOf(shown));

    std::shared_ptr<StepRecorder> recorder = installRecorder();
    {
        UndoStepGuard step(getApp(), "Set opacity");
        // What a pushed knob command or a Python write records for the node's knob.
        recordKnobEdit(shown);
        shown->setValueAtTime(7., 0.9, ViewSpec::all(), 0);
    }
    Keys aEdited = aKeys;
    aEdited.insert(aEdited.begin() + 1, std::make_pair(7., 0.9));
    Keys bEdited = bKeys;
    bEdited.push_back(std::make_pair(7., 0.9));
    EXPECT_EQ(aEdited, keysOf(a->getOpacityKnob()));
    EXPECT_EQ(bEdited, keysOf(b->getOpacityKnob()));
    EXPECT_FALSE(outside->getOpacityKnob()->isAnimated(0));

    // The node's knob and the two selected items, all in the one step.
    ASSERT_EQ(3u, recorder->commands.size());
    for (std::size_t i = 0; i < recorder->serials.size(); ++i) {
        EXPECT_EQ(recorder->serials.front(), recorder->serials[i]);
    }

    undoRecorded();
    EXPECT_EQ(aKeys, keysOf(a->getOpacityKnob()));
    EXPECT_EQ(bKeys, keysOf(b->getOpacityKnob()));
    EXPECT_EQ(bKeys, keysOf(shown));

    redoRecorded();
    EXPECT_EQ(aEdited, keysOf(a->getOpacityKnob()));
    EXPECT_EQ(bEdited, keysOf(b->getOpacityKnob()));
    EXPECT_EQ(bEdited, keysOf(shown));
    EXPECT_FALSE(outside->getOpacityKnob()->isAnimated(0));
    context->clearSelection(RotoItem::eSelectionReasonOther);
}

// While the history replays an entry, an edit of the node's knob is not forwarded: the entry holds
// the selected items' own records.
TEST_F(RotoSelectionProxyTest, AReplayedEditIsNotForwarded)
{
    RotoContextPtr context = makeContext(PLUGINID_NATRON_ROTO);
    ASSERT_TRUE(bool(context));
    BezierPtr a = makeSquare(context, 0.);
    BezierPtr b = makeSquare(context, 30.);
    ASSERT_TRUE(a && b);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(a, RotoItem::eSelectionReasonOther);
    context->select(b, RotoItem::eSelectionReasonOther);
    KnobDoublePtr shown = proxy<KnobDouble>("opacity");
    ASSERT_TRUE(bool(shown));

    std::shared_ptr<StepRecorder> recorder = installRecorder();
    recorder->replaying = true;
    shown->setValue(0.3);
    recorder->replaying = false;
    EXPECT_DOUBLE_EQ(1., a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(1., b->getOpacityKnob()->getValue());
    EXPECT_TRUE(recorder->commands.empty());

    // Edits after the replay are forwarded again, from what the node's knob then shows.
    shown->setValue(0.6);
    EXPECT_DOUBLE_EQ(0.6, a->getOpacityKnob()->getValue());
    EXPECT_DOUBLE_EQ(0.6, b->getOpacityKnob()->getValue());
    context->clearSelection(RotoItem::eSelectionReasonOther);
}
